#include "desktop.hpp"
#include "win_util.hpp"
#include "window_policy.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <exception>
#include <mutex>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <thread>
#include <unordered_map>

namespace snow
{
Rect screen_rect(RECT r) noexcept
{
	return {static_cast<float>(r.left), static_cast<float>(r.top),
	        static_cast<float>(r.right), static_cast<float>(r.bottom)};
}

namespace
{
void add_known_window(std::vector<WindowOrder> &order, HWND window)
{
	const auto handle = reinterpret_cast<std::uintptr_t>(window);
	if (!window ||
	    std::any_of(order.begin(), order.end(),
	                [&](auto entry) { return entry.window == handle; }) ||
	    !IsWindow(window)) {
		return;
	}
	const bool topmost =
		(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	const auto neighbour = [&](UINT direction) -> std::uintptr_t {
		HWND current = window;
		// A whole run can be absent. Find an observed neighbour, not just
		// an adjacent missing HWND; otherwise recovery would depend on map
		// order.
		for (unsigned n = 0; n < 256; ++n) {
			current = GetWindow(current, direction);
			if (!current || current == window) {
				break;
			}
			const auto id = reinterpret_cast<std::uintptr_t>(current);
			if (std::any_of(order.begin(), order.end(), [&](auto entry) {
					return entry.window == id && entry.topmost == topmost;
				})) {
				return id;
			}
		}
		return 0;
	};
	const auto next = neighbour(GW_HWNDNEXT);
	recover_window(order, {handle, topmost}, next ? 0 : neighbour(GW_HWNDPREV),
	               next);
}
}

std::vector<WindowOrder> enumerate_window_order(std::span<const HWND> required)
{
	constexpr std::size_t limit = 8192;
	std::vector<WindowOrder> order;
	// The callback cannot allocate or throw through the Win32 boundary.
	order.reserve(limit);
	const auto callback = [](HWND window, LPARAM parameter) -> BOOL {
		auto &items = *reinterpret_cast<std::vector<WindowOrder> *>(parameter);
		if (items.size() == items.capacity()) {
			return FALSE;
		}
		items.push_back(
			{reinterpret_cast<std::uintptr_t>(window),
		     (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0});
		return TRUE;
	};
	EnumWindows(callback, reinterpret_cast<LPARAM>(&order));
	for (HWND window : required) {
		add_known_window(order, window);
	}
	// Shell windows can move between bands when Start/Quick Settings activate.
	// They are not necessarily reachable from GetTopWindow(nullptr).
	add_known_window(order, FindWindowW(L"Shell_TrayWnd", nullptr));
	HWND secondary = nullptr;
	for (unsigned n = 0; n < 32; ++n) {
		secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd",
		                          nullptr);
		if (!secondary) {
			break;
		}
		add_known_window(order, secondary);
	}
	add_known_window(order, GetForegroundWindow());
	return order;
}

std::vector<WindowOrder> popup_window_order(std::span<const HWND> candidates)
{
	// Only the live prefix down to the published anchors matters on the UI
	// thread. Keep EnumWindows and shell discovery on the desktop worker.
	constexpr std::size_t limit = 128;
	std::vector<WindowOrder> order;
	auto remaining = candidates.size();
	HWND window = GetTopWindow(nullptr);
	while (window && remaining && order.size() < limit) {
		order.push_back(
			{reinterpret_cast<std::uintptr_t>(window),
		     (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0});
		if (std::find(candidates.begin(), candidates.end(), window) !=
		    candidates.end()) {
			--remaining;
		}
		window = GetWindow(window, GW_HWNDNEXT);
	}
	// Explorer can expose an active shell HWND outside that walk. Recover
	// it against the live neighbours as in the worker inventory.
	if (remaining && !window) {
		for (HWND candidate : candidates) {
			add_known_window(order, candidate);
		}
		add_known_window(order, GetForegroundWindow());
	}
	return order;
}

std::vector<Display> enumerate_displays()
{
	struct State {
		std::vector<Display> displays;
		std::exception_ptr error;
	} state;

	const auto callback = [](HMONITOR monitor, HDC, LPRECT,
	                         LPARAM param) -> BOOL {
		auto &s = *reinterpret_cast<State *>(param);
		try {
			MONITORINFO info{sizeof(info)};
			check_win(GetMonitorInfoW(monitor, &info), "GetMonitorInfo");
			UINT x = 96, y = 96;
			if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) {
				x = 96;
			}
			s.displays.push_back(
				{monitor, info.rcMonitor, static_cast<float>(x) / 96});
			return TRUE;
		} catch (...) {
			s.error = std::current_exception();
			return FALSE;
		}
	};
	const auto ok = EnumDisplayMonitors(nullptr, nullptr, callback,
	                                    reinterpret_cast<LPARAM>(&state));
	if (state.error) {
		std::rethrow_exception(state.error);
	}
	check_win(ok, "EnumDisplayMonitors");
	if (state.displays.empty()) {
		throw std::runtime_error("No active displays");
	}
	return std::move(state.displays);
}

WindowTraits window_traits(HWND window, std::wstring_view name, LONG_PTR style,
                           LONG_PTR ex) noexcept
{
	return {name,
	        (style & WS_POPUP) != 0,
	        (style & WS_CAPTION) == WS_CAPTION,
	        (style & WS_THICKFRAME) != 0,
	        (ex & WS_EX_TOOLWINDOW) != 0,
	        (ex & WS_EX_NOACTIVATE) != 0,
	        (ex & WS_EX_APPWINDOW) != 0,
	        GetWindow(window, GW_OWNER) != nullptr,
	        (ex & WS_EX_TRANSPARENT) != 0};
}

bool browser_status_bubble(HWND window, const WindowTraits &traits,
                           Rect bounds) noexcept
{
	if (!traits.class_name.starts_with(L"Chrome_WidgetWin_")) {
		return false;
	}
	RECT owner_bounds{};
	const HWND owner = GetAncestor(window, GA_ROOTOWNER);
	const UINT dpi = GetDpiForWindow(window);
	return owner && owner != window && GetWindowRect(owner, &owner_bounds) &&
	       is_browser_status_bubble(traits, bounds, screen_rect(owner_bounds),
	                                dpi ? static_cast<float>(dpi) / 96 : 1);
}

class DesktopScanner {
  public:
	explicit DesktopScanner(HWND control);
	~DesktopScanner();
	DesktopSnapshot snapshot(bool force = false);
	bool dirty() const noexcept;
	bool moving() const noexcept;
	std::uint64_t full_scans() const noexcept;
	std::uint64_t geometry_samples() const noexcept;
	void event(DWORD event, HWND window, LONG object, LONG child) noexcept;
	bool hooks_active() const noexcept;
	std::size_t tracked_windows() const noexcept;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

namespace
{
thread_local DesktopScanner *active_tracker = nullptr;
thread_local HWND foreground_listener = nullptr;

void CALLBACK foreground_callback(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD,
                                  DWORD)
{
	// This hook belongs to the UI thread, so a slow worker DWM/Shell scan
	// cannot delay the notification. Do not query/render inside the WinEvent
	// callback.
	if (foreground_listener) {
		PostMessageW(foreground_listener, message_foreground, 0, 0);
	}
}

void CALLBACK event_callback(HWINEVENTHOOK, DWORD event, HWND window,
                             LONG object, LONG child, DWORD, DWORD)
{
	if (active_tracker) {
		active_tracker->event(event, window, object, child);
	}
}

bool background(const wchar_t *name)
{
	return std::wcscmp(name, L"Progman") == 0 ||
	       std::wcscmp(name, L"WorkerW") == 0;
}

bool taskbar(const wchar_t *name)
{
	return std::wcscmp(name, L"Shell_TrayWnd") == 0 ||
	       std::wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
}

bool native_popup(HWND window, const wchar_t *name, LONG_PTR style, LONG_PTR ex)
{
	return is_native_popup(window_traits(window, name, style, ex));
}

bool get_bounds(HWND window, RECT &rect)
{
	return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
	                                       &rect, sizeof(rect))) ||
	       GetWindowRect(window, &rect);
}

std::optional<Rect> foreground_fullscreen_bounds() noexcept
{
	HWND window = GetForegroundWindow();
	if (!window || IsIconic(window) || IsZoomed(window)) {
		return {};
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	if (!pid || pid == GetCurrentProcessId()) {
		return {};
	}
	wchar_t name[128]{};
	GetClassNameW(window, name, 128);
	const auto style = GetWindowLongPtrW(window, GWL_STYLE),
			   ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
	if (background(name) || taskbar(name) || is_shell_focus_proxy(name) ||
	    native_popup(window, name, style, ex) ||
	    (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) {
		return {};
	}
	DWORD cloaked = 0;
	DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	RECT bounds{};
	MONITORINFO monitor{sizeof(monitor)};
	if (cloaked || !get_bounds(window, bounds) ||
	    !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
	                     &monitor)) {
		return {};
	}
	const auto &m = monitor.rcMonitor;
	if (bounds.left <= m.left + 2 && bounds.top <= m.top + 2 &&
	    bounds.right >= m.right - 2 && bounds.bottom >= m.bottom - 2) {
		return screen_rect(bounds);
	}
	return {};
}

}

std::optional<Rect> foreground_fullscreen_fast(HWND window) noexcept
{
	if (!window || !IsWindowVisible(window) || IsIconic(window) ||
	    GetPropW(window, L"NonRudeHWND")) {
		return {};
	}
	wchar_t name[128]{};
	GetClassNameW(window, name, 128);
	if (background(name) || taskbar(name) || is_shell_focus_proxy(name)) {
		return {};
	}
	const auto style = GetWindowLongPtrW(window, GWL_STYLE),
			   ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
	const auto traits = window_traits(window, name, style, ex);
	RECT client{};
	POINT origin{};
	MONITORINFO monitor{sizeof(monitor)};
	// User32 only: no DWM, Shell RPC, enumeration, SendMessage or worker lock.
	if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin) ||
	    !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
	                     &monitor)) {
		return {};
	}
	OffsetRect(&client, origin.x, origin.y);
	const auto bounds = screen_rect(client);
	return fullscreen_client(traits, IsZoomed(window) != FALSE, bounds,
	                         screen_rect(monitor.rcMonitor))
	           ? std::optional<Rect>{bounds}
	           : std::nullopt;
}

struct DesktopScanner::Impl {
	struct Record {
		DWORD process{};
		SurfaceSnapshot surface;
		bool destroyed = false, minimizing = false, on_current = true,
			 geometry_dirty = false;
		RECT frame_insets{}, monitor_bounds{};
		bool zoomed = false;
		float inset_scale = 1;
		ULONGLONG last_geometry_event = 0;
		ULONGLONG minimize_at = 0, desktop_checked = 0;
	};

	HWND control;
	std::array<HWINEVENTHOOK, 5> hooks{};
	ComPtr<IVirtualDesktopManager> desktops;
	std::unordered_map<HWND, Record> records;
	SurfaceId next_id = 1;
	bool notified = false, geometry_dirty = false, topology_dirty = true,
		 urgent = true;
	HWND moving_window{};
	ULONGLONG last_scan = 0;
	std::uint64_t scans = 0, samples = 0;
	std::vector<Blocker> blockers;
	std::vector<HWND> popups;
	std::vector<HWND> taskbars;
	std::vector<HWND> ordering_apps;

	struct BlockerEntry {
		std::size_t index;
		bool dirty = false;
	};

	std::unordered_map<HWND, BlockerEntry> blocker_indices;
	std::optional<GUID> current_desktop;
	std::optional<Rect> fullscreen_window;
	ULONGLONG last_probe = 0;

	explicit Impl(HWND c) : control(c) {}

	void motion_sample(DesktopSnapshot &snapshot) const
	{
		const auto it = records.find(moving_window);
		if (it == records.end() ||
		    it->second.surface.state != SurfaceState::visible) {
			return;
		}
		const auto &r = it->second;
		const float factor = r.surface.scale / r.inset_scale;
		snapshot.motion = MotionSample{
			moving_window,
			r.surface.id,
			r.process,
			{static_cast<LONG>(std::lround(r.frame_insets.left * factor)),
		     static_cast<LONG>(std::lround(r.frame_insets.top * factor)),
		     static_cast<LONG>(std::lround(r.frame_insets.right * factor)),
		     static_cast<LONG>(std::lround(r.frame_insets.bottom * factor))},
			r.surface.scale,
			r.zoomed};
	}

	std::optional<GUID> desktop_id()
	{
		if (!desktops) {
			return {};
		}
		GUID id{};
		HWND foreground = GetForegroundWindow();
		DWORD pid = 0;
		GetWindowThreadProcessId(foreground, &pid);
		if (foreground && pid != GetCurrentProcessId() &&
		    SUCCEEDED(desktops->GetWindowDesktopId(foreground, &id)) &&
		    !IsEqualGUID(id, GUID_NULL)) {
			return id;
		}
		// A temporary invisible window obtains the current desktop when it is
		// empty or the foreground belongs to the tray menu. Never switch
		// desktops.
		const auto now = GetTickCount64();
		if (now - last_probe >= 1000) {
			last_probe = now;
			HWND probe = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			                             L"STATIC", L"speeecial desktop probe",
			                             WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
			                             GetModuleHandleW(nullptr), nullptr);
			if (probe) {
				const auto hr = desktops->GetWindowDesktopId(probe, &id);
				DestroyWindow(probe);
				if (SUCCEEDED(hr) && !IsEqualGUID(id, GUID_NULL)) {
					return id;
				}
			}
		}
		return current_desktop;
	}
};

DesktopScanner::DesktopScanner(HWND control)
	: impl_(std::make_unique<Impl>(control))
{
	active_tracker = this;
	CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
	                 __uuidof(IVirtualDesktopManager),
	                 reinterpret_cast<void **>(impl_->desktops.GetAddressOf()));
	const std::pair<DWORD, DWORD> ranges[] = {
		{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
		{EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND},
		{EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER},
		{EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
		{EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}};
	for (std::size_t i = 0; i < std::size(ranges); ++i) {
		impl_->hooks[i] = SetWinEventHook(
			ranges[i].first, ranges[i].second, nullptr, event_callback, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	}
}

DesktopScanner::~DesktopScanner()
{
	active_tracker = nullptr;
	for (auto hook : impl_->hooks) {
		if (hook) {
			UnhookWinEvent(hook);
		}
	}
}

bool DesktopScanner::hooks_active() const noexcept
{
	return std::all_of(impl_->hooks.begin(), impl_->hooks.end(),
	                   [](auto h) { return h != nullptr; });
}

std::size_t DesktopScanner::tracked_windows() const noexcept
{
	return impl_->records.size();
}

bool DesktopScanner::dirty() const noexcept
{
	return impl_->geometry_dirty || impl_->topology_dirty ||
	       impl_->moving_window;
}

bool DesktopScanner::moving() const noexcept
{
	return impl_->moving_window != nullptr;
}

std::uint64_t DesktopScanner::full_scans() const noexcept
{
	return impl_->scans;
}

std::uint64_t DesktopScanner::geometry_samples() const noexcept
{
	return impl_->samples;
}

void DesktopScanner::event(DWORD event, HWND window, LONG object,
                           LONG child) noexcept
{
	if (!window) {
		return;
	}
	if (event >= EVENT_OBJECT_CREATE &&
	    (object != OBJID_WINDOW || child != CHILDID_SELF)) {
		return;
	}
	// OUTOFCONTEXT callbacks run on the registering message-loop thread. Only
	// update existing records here; defer all querying/enumeration/rendering.
	const auto it = impl_->records.find(window);
	if (event == EVENT_SYSTEM_MOVESIZESTART) {
		impl_->moving_window = window;
	}
	if ((event == EVENT_SYSTEM_MOVESIZEEND || event == EVENT_OBJECT_DESTROY) &&
	    impl_->moving_window == window) {
		impl_->moving_window = nullptr;
	}
	if (event == EVENT_OBJECT_LOCATIONCHANGE ||
	    event == EVENT_SYSTEM_MOVESIZESTART ||
	    event == EVENT_SYSTEM_MOVESIZEEND) {
		if (it != impl_->records.end()) {
			it->second.geometry_dirty = true;
			it->second.last_geometry_event = GetTickCount64();
			impl_->geometry_dirty = true;
		}
		if (auto b = impl_->blocker_indices.find(window);
		    b != impl_->blocker_indices.end()) {
			b->second.dirty = true;
			impl_->geometry_dirty = true;
		}
		// A tooltip may be shown with zero bounds before its first layout.
		// Its subsequent location event still has to update native stacking.
		if (std::find(impl_->popups.begin(), impl_->popups.end(), window) !=
		    impl_->popups.end()) {
			impl_->geometry_dirty = true;
		}
		// Unrelated child/control locations do not trigger a full enumeration.
	} else {
		impl_->topology_dirty = true;
		if (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_SHOW ||
		    ((event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY ||
		      event == EVENT_OBJECT_REORDER) &&
		     std::find(impl_->popups.begin(), impl_->popups.end(), window) !=
		         impl_->popups.end())) {
			impl_->urgent = true;
		}
		if (it != impl_->records.end() &&
		    (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE ||
		     event == EVENT_OBJECT_SHOW ||
		     event == EVENT_SYSTEM_MINIMIZESTART ||
		     event == EVENT_SYSTEM_MINIMIZEEND ||
		     event == EVENT_OBJECT_REORDER)) {
			impl_->urgent = true;
		}
	}
	if (it != impl_->records.end()) {
		if (event == EVENT_OBJECT_DESTROY) {
			it->second.destroyed = true;
		}
		if (event == EVENT_SYSTEM_MINIMIZESTART) {
			it->second.minimizing = true;
			it->second.minimize_at = GetTickCount64();
		}
		if (event == EVENT_SYSTEM_MINIMIZEEND) {
			it->second.minimizing = false;
		}
	}
	if (dirty() && !impl_->notified) {
		impl_->notified = true;
		if (impl_->control) {
			PostMessageW(impl_->control, message_desktop, 0, 0);
		}
	}
}

DesktopSnapshot DesktopScanner::snapshot(bool force)
{
	impl_->notified = false;
	DesktopSnapshot result;
	result.surfaces.reserve(max_surfaces + 16);
	result.blockers.reserve(128);
	const auto now = GetTickCount64();
	if (!force && !impl_->urgent && impl_->last_scan &&
	    (!impl_->topology_dirty || now - impl_->last_scan < 250)) {
		// Fast path: sample only dirty/actively dragged windows. Cached frame
		// insets convert GetWindowRect to visible bounds without DWM round
		// trips.
		for (auto &[window, record] : impl_->records) {
			if (!record.destroyed &&
			    (record.geometry_dirty || window == impl_->moving_window)) {
				RECT r{};
				++impl_->samples;
				const UINT current_dpi = GetDpiForWindow(window);
				if ((IsZoomed(window) != FALSE) != record.zoomed ||
				    (current_dpi &&
				     std::abs(static_cast<float>(current_dpi) / 96 -
				              record.surface.scale) > 0.01f)) {
					return snapshot(true); // Insets/DPI changed; a move-only
					                       // translation is insufficient.
				}
				const bool taskbar =
					record.surface.kind == SurfaceKind::taskbar;
				if (!IsIconic(window) && GetWindowRect(window, &r) &&
				    (record.surface.state == SurfaceState::visible ||
				     (taskbar &&
				      record.surface.state == SurfaceState::hidden))) {
					const float factor =
						record.surface.scale / record.inset_scale;
					r.left += static_cast<LONG>(
						std::lround(record.frame_insets.left * factor));
					r.top += static_cast<LONG>(
						std::lround(record.frame_insets.top * factor));
					r.right += static_cast<LONG>(
						std::lround(record.frame_insets.right * factor));
					r.bottom += static_cast<LONG>(
						std::lround(record.frame_insets.bottom * factor));
					const Rect body = screen_rect(r);
					if (body.valid()) {
						if (auto b = impl_->blocker_indices.find(window);
						    b != impl_->blocker_indices.end()) {
							impl_->blockers[b->second.index].rect = body;
							b->second.dirty = false;
						}
						record.surface.rect = body;
						if (taskbar) {
							record.surface.state =
								IsWindowVisible(window) &&
										taskbar_body_on_screen(
											body,
											screen_rect(record.monitor_bounds),
											record.surface.scale)
									? SurfaceState::visible
									: SurfaceState::hidden;
						}
						if (record.surface.kind == SurfaceKind::window) {
							const float inset = std::min(
								8 * record.surface.scale, body.width() / 8);
							record.surface.rect.left += inset;
							record.surface.rect.right -= inset;
							const auto m = screen_rect(record.monitor_bounds);
							record.surface.accepts_snow =
								!IsZoomed(window) &&
								!(body.left <= m.left + 2 &&
							      body.top <= m.top + 2 &&
							      body.right >= m.right - 2 &&
							      body.bottom >= m.bottom - 2);
						}
					}
				}
				record.geometry_dirty = false;
			}
			result.surfaces.push_back(record.surface);
		}
		for (auto &[window, b] : impl_->blocker_indices) {
			if (b.dirty) {
				RECT r{};
				if (get_bounds(window, r)) {
					impl_->blockers[b.index].rect = screen_rect(r);
				}
				b.dirty = false;
			}
		}
		impl_->geometry_dirty = false;
		impl_->taskbars.clear();
		for (const auto &[window, record] : impl_->records) {
			if (record.surface.kind == SurfaceKind::taskbar &&
			    record.surface.state == SurfaceState::visible &&
			    impl_->taskbars.size() < 32) {
				impl_->taskbars.push_back(window);
			}
		}
		result.blockers = impl_->blockers;
		result.popups = impl_->popups;
		result.taskbars = impl_->taskbars;
		result.ordering_apps = impl_->ordering_apps;
		result.desktop = impl_->current_desktop;
		result.fullscreen_window = impl_->fullscreen_window;
		impl_->motion_sample(result);
		return result;
	}
	impl_->last_scan = now;
	++impl_->scans;
	impl_->topology_dirty = false;
	impl_->urgent = false;
	impl_->geometry_dirty = false;
	impl_->blocker_indices.clear();
	result.full_scan = true;
	impl_->fullscreen_window = foreground_fullscreen_bounds();
	result.fullscreen_window = impl_->fullscreen_window;
	const auto desktop = impl_->desktop_id();
	const bool desktop_changed =
		desktop && (!impl_->current_desktop ||
	                !IsEqualGUID(*desktop, *impl_->current_desktop));
	impl_->current_desktop = desktop;
	result.desktop = desktop;
	result.desktop_changed = desktop_changed;
	// Destroy notifications carry the old identity even if Windows reuses HWND.
	for (auto it = impl_->records.begin(); it != impl_->records.end();) {
		DWORD pid = 0;
		GetWindowThreadProcessId(it->first, &pid);
		if (it->second.destroyed || !IsWindow(it->first) ||
		    pid != it->second.process) {
			auto old = it->second.surface;
			old.state = SurfaceState::closed;
			result.surfaces.push_back(old);
			it = impl_->records.erase(it);
		} else {
			++it;
		}
	}
	// Collect handles before DWM/COM queries. A live GW_HWNDNEXT walk can omit
	// shell bands or lose its tail when a flyout changes the window order.
	std::vector<HWND> required;
	required.reserve(impl_->records.size() + impl_->popups.size());
	for (const auto &[window, record] : impl_->records) {
		(void)record;
		required.push_back(window);
	}
	required.insert(required.end(), impl_->popups.begin(), impl_->popups.end());
	const auto order = enumerate_window_order(required);
	int rank = 0;
	for (const auto &entry : order) {
		const HWND window = reinterpret_cast<HWND>(entry.window);
		++rank;
		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);
		if (!pid) {
			continue;
		}
		wchar_t name[128]{};
		GetClassNameW(window, name, 128);
		if (pid == GetCurrentProcessId()) {
			// Our menu and header tooltip must also stay above the overlays.
			if (IsWindowVisible(window) && result.popups.size() < 128 &&
			    (std::wcscmp(name, L"#32768") == 0 ||
			     std::wcscmp(name, L"tooltips_class32") == 0 ||
			     std::wcscmp(name, L"SysShadow") == 0)) {
				result.popups.push_back(window);
			}
			continue;
		}
		// A focus proxy has no painted body. Do not make it a blocker, snow
		// support, or popup anchor just because User32 reports it as visible.
		if (background(name) || is_shell_focus_proxy(name)) {
			continue;
		}
		const auto style = GetWindowLongPtrW(window, GWL_STYLE),
				   ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
		if (style & WS_CHILD) {
			continue;
		}
		const bool is_taskbar = taskbar(name);
		DWORD cloaked = 0;
		DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
		auto it = impl_->records.find(window);
		bool current = true;
		if (it != impl_->records.end()) {
			auto &record = it->second;
			if (record.minimizing && !IsIconic(window) &&
			    now - record.minimize_at > 1000) {
				record.minimizing = false;
			}
			if (impl_->desktops &&
			    (desktop_changed || now - record.desktop_checked > 1000)) {
				BOOL yes = TRUE;
				if (SUCCEEDED(impl_->desktops->IsWindowOnCurrentVirtualDesktop(
						window, &yes))) {
					record.on_current = yes != FALSE;
				}
				record.desktop_checked = now;
			}
			current = record.on_current;
		}
		const bool minimized =
			IsIconic(window) ||
			(it != impl_->records.end() && it->second.minimizing);
		const bool visible =
			IsWindowVisible(window) && !cloaked && current && !minimized;
		const UINT dpi = GetDpiForWindow(window);
		const float scale = dpi ? static_cast<float>(dpi) / 96 : 1;
		const bool recent_motion = it != impl_->records.end() &&
		                           (window == impl_->moving_window ||
		                            now - it->second.last_geometry_event < 200);
		RECT raw{};
		bool bounds_ok = false;
		// DWM and GetWindowRect are separate snapshots. Never infer border
		// thickness from their positional difference while a window is moving.
		if (recent_motion && GetWindowRect(window, &raw)) {
			const auto &record = it->second;
			const float factor = scale / record.inset_scale;
			raw.left += static_cast<LONG>(
				std::lround(record.frame_insets.left * factor));
			raw.top += static_cast<LONG>(
				std::lround(record.frame_insets.top * factor));
			raw.right += static_cast<LONG>(
				std::lround(record.frame_insets.right * factor));
			raw.bottom += static_cast<LONG>(
				std::lround(record.frame_insets.bottom * factor));
			bounds_ok = true;
		} else {
			bounds_ok = get_bounds(window, raw);
		}
		const bool good = bounds_ok && screen_rect(raw).valid();
		const Rect body = screen_rect(raw);
		bool bar_visible = visible;
		if (is_taskbar && good) {
			MONITORINFO info{sizeof(info)};
			if (GetMonitorInfoW(
					MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
					&info)) {
				bar_visible =
					visible && taskbar_body_on_screen(
								   body, screen_rect(info.rcMonitor), scale);
			} else {
				bar_visible = false;
			}
		}
		const auto traits = window_traits(window, name, style, ex);
		const bool popup = !is_taskbar && is_native_popup(traits);
		if (visible && popup && result.popups.size() < 128) {
			result.popups.push_back(window);
		}
		if (is_taskbar && bar_visible && good && result.taskbars.size() < 32) {
			result.taskbars.push_back(window);
		}
		const bool transparent_overlay =
			(ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT);
		if (visible && good && !transparent_overlay &&
		    (!is_taskbar || bar_visible) && result.blockers.size() < 2048) {
			impl_->blocker_indices.emplace(
				window, Impl::BlockerEntry{result.blockers.size()});
			result.blockers.push_back(
				{body, rank, !is_taskbar && IsZoomed(window) != FALSE, popup});
		}
		const bool candidate =
			is_taskbar ||
			(!popup && !(ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) &&
		     !transparent_overlay &&
		     ((style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME) ||
		      (ex & WS_EX_APPWINDOW)) &&
		     body.width() >= 100 * scale && body.height() >= 50 * scale);
		// A borderless fullscreen app can cover the overlay without having a
		// caption, resize border, or WS_EX_APPWINDOW to collect snow on.
		if (visible && good && !is_taskbar && !popup && !transparent_overlay &&
		    !(ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) &&
		    result.ordering_apps.size() < max_surfaces) {
			result.ordering_apps.push_back(window);
		}
		if (it == impl_->records.end()) {
			if (!candidate || !good || !visible ||
			    impl_->records.size() >= max_surfaces - 8) {
				continue;
			}
			Impl::Record record;
			record.process = pid;
			record.surface.id = impl_->next_id++;
			record.surface.kind =
				is_taskbar ? SurfaceKind::taskbar : SurfaceKind::window;
			record.surface.rect = body;
			record.surface.scale = scale;
			it = impl_->records.emplace(window, std::move(record)).first;
		}
		auto &record = it->second;
		record.geometry_dirty = false;
		record.zoomed = IsZoomed(window) != FALSE;
		auto &s = record.surface;
		s.z_order = rank;
		// Cloaking/current-desktop state wins over minimize hints. Switching a
		// virtual desktop must never be interpreted as minimizing every window.
		s.state = !current || cloaked ? SurfaceState::suspended
		          : minimized         ? SurfaceState::minimized
		          : (visible && (!is_taskbar || bar_visible))
		              ? SurfaceState::visible
		              : SurfaceState::hidden;
		if (s.state == SurfaceState::visible && good) {
			s.rect = body;
			s.scale = scale;
			RECT wr{};
			if (!recent_motion && !record.zoomed &&
			    GetWindowRect(window, &wr)) {
				const RECT insets{raw.left - wr.left, raw.top - wr.top,
				                  raw.right - wr.right, raw.bottom - wr.bottom};
				const float limit = 32 * scale;
				if (std::abs(insets.left) <= limit &&
				    std::abs(insets.top) <= limit &&
				    std::abs(insets.right) <= limit &&
				    std::abs(insets.bottom) <= limit) {
					record.frame_insets = insets;
					record.inset_scale = scale;
				}
			}
			MONITORINFO monitor{sizeof(monitor)};
			if (GetMonitorInfoW(
					MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
					&monitor)) {
				record.monitor_bounds = monitor.rcMonitor;
			}
			const auto m = screen_rect(record.monitor_bounds);
			s.accepts_snow =
				is_taskbar ||
				(!IsZoomed(window) &&
			     !(body.left <= m.left + 2 && body.top <= m.top + 2 &&
			       body.right >= m.right - 2 && body.bottom >= m.bottom - 2));
			if (!is_taskbar) {
				const float inset = std::min(8 * scale, body.width() / 8);
				s.rect.left += inset;
				s.rect.right -= inset;
			}
		}
	}
	// Known handles were queried directly even if enumeration omitted them.
	// A failed/missing enumeration entry is not evidence of hidden content.
	// Destruction and handle reuse are reconciled at the start of the next
	// scan.
	for (auto &[window, r] : impl_->records) {
		(void)window;
		result.surfaces.push_back(r.surface);
	}
	impl_->blockers = result.blockers;
	impl_->popups = result.popups;
	impl_->taskbars = result.taskbars;
	impl_->ordering_apps = result.ordering_apps;
	if (impl_->moving_window && !IsWindow(impl_->moving_window)) {
		impl_->moving_window = nullptr;
	}
	impl_->motion_sample(result);
	return result;
}

bool on_battery() noexcept
{
	SYSTEM_POWER_STATUS status{};
	return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}

struct DesktopTracker::Impl {
	HWND control;
	HWINEVENTHOOK foreground_hook{};
	std::thread worker;
	std::atomic<DWORD> thread_id{0};
	std::atomic<bool> stop{false}, requested{true}, pending{false},
		moving{false}, hooks{false};
	std::atomic<std::uint64_t> scans{0}, samples{0};
	std::atomic<std::size_t> tracked{0};
	std::atomic<double> last_ms{0}, peak_ms{0};
	mutable std::mutex mailbox_mutex;
	DesktopSnapshot mailbox, current;
	std::string failure;

	explicit Impl(HWND owner) : control(owner)
	{
		worker = std::thread([this] { run(); });
		foreground_listener = owner;
		// Include our own tray menu activation too: it ends fullscreen focus.
		foreground_hook = SetWinEventHook(
			EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
			foreground_callback, 0, 0, WINEVENT_OUTOFCONTEXT);
	}

	~Impl()
	{
		if (foreground_hook) {
			UnhookWinEvent(foreground_hook);
		}
		foreground_listener = nullptr;
		stop = true;
		wake();
		if (worker.joinable()) {
			worker.join();
		}
	}

	void wake() noexcept
	{
		if (const auto id = thread_id.load()) {
			PostThreadMessageW(id, WM_NULL, 0, 0);
		}
	}

	void run() noexcept
	{
		try {
			ComApartment apartment;
			MSG message{};
			PeekMessageW(&message, nullptr, 0, 0,
			             PM_NOREMOVE); // Create this thread's message queue.
			thread_id = GetCurrentThreadId();
			DesktopScanner scanner(nullptr);
			hooks = scanner.hooks_active();
			ULONGLONG heartbeat = 0, next_motion = 0;
			while (!stop.load()) {
				for (unsigned n = 0;
				     n < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
				     ++n) {
					TranslateMessage(&message);
					DispatchMessageW(&message);
				}
				const auto now = GetTickCount64();
				const bool force =
					requested.exchange(false) || now >= heartbeat;
				if (force || (scanner.dirty() &&
				              (!scanner.moving() || now >= next_motion))) {
					const auto begin = std::chrono::steady_clock::now();
					auto next = scanner.snapshot(force);
					const double elapsed =
						std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - begin)
							.count();
					last_ms = elapsed;
					peak_ms = std::max(peak_ms.load(), elapsed);
					scans = scanner.full_scans();
					samples = scanner.geometry_samples();
					tracked = scanner.tracked_windows();
					moving = scanner.moving();
					bool notify = false;
					{
						std::lock_guard lock(mailbox_mutex);
						// Preserve a desktop transition if two publications
						// coalesce.
						next.desktop_changed |=
							pending.load() && mailbox.desktop_changed;
						if (pending.load()) {
							for (const auto &old : mailbox.surfaces) {
								if (old.state == SurfaceState::closed &&
								    next.surfaces.size() < max_surfaces * 2 &&
								    std::none_of(next.surfaces.begin(),
								                 next.surfaces.end(),
								                 [&](const auto &s) {
													 return s.id == old.id;
												 })) {
									next.surfaces.push_back(old);
								}
							}
						}
						mailbox = std::move(next);
						notify = !pending.exchange(true);
					}
					if (notify) {
						PostMessageW(control, message_desktop, 0, 0);
					}
					if (force) {
						heartbeat = now + 1000;
					}
					next_motion = now + 16;
				}
				const auto current_time = GetTickCount64();
				const auto deadline =
					scanner.moving()
						? std::min(heartbeat, next_motion)
						: (scanner.dirty()
				               ? std::min(heartbeat, current_time + 50)
				               : heartbeat);
				const auto timeout = static_cast<DWORD>(
					deadline > current_time
						? std::min<ULONGLONG>(deadline - current_time, 1000)
						: 0);
				MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT,
				                            MWMO_INPUTAVAILABLE);
			}
		} catch (const std::exception &e) {
			std::lock_guard lock(mailbox_mutex);
			failure = e.what();
			PostMessageW(control, message_desktop, 0, 0);
		}
		thread_id = 0;
	}

	bool resample_motion()
	{
		if (!current.motion || !moving.load()) {
			return false;
		}
		const auto &m = *current.motion;
		DWORD pid = 0;
		GetWindowThreadProcessId(m.window, &pid);
		if (pid != m.process) {
			return false;
		}
		RECT r{};
		if (!IsWindow(m.window) || IsIconic(m.window) ||
		    (IsZoomed(m.window) != FALSE) != m.zoomed) {
			return false;
		}
		const UINT dpi = GetDpiForWindow(m.window);
		if (dpi && std::abs(static_cast<float>(dpi) / 96 - m.scale) > 0.01f) {
			return false;
		}
		if (!GetWindowRect(m.window, &r)) {
			return false;
		}
		r.left += m.insets.left;
		r.top += m.insets.top;
		r.right += m.insets.right;
		r.bottom += m.insets.bottom;
		const auto body = screen_rect(r);
		if (!body.valid()) {
			return false;
		}
		for (auto &s : current.surfaces) {
			if (s.id != m.id) {
				continue;
			}
			auto profile = body;
			if (s.kind == SurfaceKind::window) {
				const auto inset = std::min(8 * s.scale, body.width() / 8);
				profile.left += inset;
				profile.right -= inset;
			}
			if (profile == s.rect) {
				return false;
			}
			s.rect = profile;
			for (auto &b : current.blockers) {
				if (b.z_order == s.z_order) {
					b.rect = body;
					break;
				}
			}
			return true;
		}
		return false;
	}
};

DesktopTracker::DesktopTracker(HWND control)
	: impl_(std::make_unique<Impl>(control))
{
}

DesktopTracker::~DesktopTracker() = default;

std::optional<DesktopSnapshot> DesktopTracker::snapshot(bool force)
{
	if (force) {
		impl_->requested = true;
		impl_->wake();
	}
	bool fresh = false;
	{
		// A frame never waits for enumeration, Shell RPC, DWM, or the worker.
		std::unique_lock lock(impl_->mailbox_mutex, std::try_to_lock);
		if (lock.owns_lock() && impl_->pending.exchange(false)) {
			impl_->current = std::move(impl_->mailbox);
			fresh = true;
		}
	}
	fresh |= impl_->resample_motion();
	if (!fresh) {
		return {};
	}
	auto result = impl_->current;
	impl_->current.desktop_changed = false;
	impl_->current.full_scan = false;
	return result;
}

bool DesktopTracker::dirty() const noexcept
{
	return impl_->pending.load() || impl_->moving.load();
}

bool DesktopTracker::moving() const noexcept
{
	return impl_->moving.load();
}

bool DesktopTracker::hooks_active() const noexcept
{
	return impl_->hooks.load() && impl_->foreground_hook != nullptr;
}

std::uint64_t DesktopTracker::full_scans() const noexcept
{
	return impl_->scans.load();
}

std::uint64_t DesktopTracker::geometry_samples() const noexcept
{
	return impl_->samples.load();
}

std::size_t DesktopTracker::tracked_windows() const noexcept
{
	return impl_->tracked.load();
}

double DesktopTracker::last_scan_ms() const noexcept
{
	return impl_->last_ms.load();
}

double DesktopTracker::peak_scan_ms() const noexcept
{
	return impl_->peak_ms.load();
}

std::string DesktopTracker::error() const
{
	std::unique_lock lock(impl_->mailbox_mutex, std::try_to_lock);
	return lock.owns_lock() ? impl_->failure : std::string{};
}

}
