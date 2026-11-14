#include "renderer.hpp"
#include "win_util.hpp"
#include "window_policy.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <psapi.h>

namespace
{
using namespace snow;

std::uint64_t private_bytes()
{
	PROCESS_MEMORY_COUNTERS_EX memory{};
	memory.cb = sizeof(memory);
	check_win(
	    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)),
	    "Memory counters");
	return memory.PrivateUsage;
}

void pump()
{
	const auto end = GetTickCount64() + 50;
	MSG m{};
	do {
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	} while (GetTickCount64() < end);
}

bool above(HWND upper, HWND lower)
{
	for (HWND w = upper; w; w = GetWindow(w, GW_HWNDNEXT)) {
		if (w == lower) {
			return true;
		}
	}
	return false;
}

void window_inventory_membership()
{
	struct Windows
	{
		HWND normal{}, topmost{};

		~Windows()
		{
			if (topmost) {
				DestroyWindow(topmost);
			}
			if (normal) {
				DestroyWindow(normal);
			}
		}
	} windows;

	constexpr DWORD common = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
	windows.normal = CreateWindowExW(common, L"STATIC", L"Inventory test", WS_POPUP, 40, 40, 180, 80, nullptr, nullptr,
	                                 GetModuleHandleW(nullptr), nullptr);
	windows.topmost = CreateWindowExW(common | WS_EX_TOPMOST, L"STATIC", L"Inventory test topmost", WS_POPUP, 40, 40,
	                                  180, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	check_win(windows.normal && windows.topmost, "Create inventory test windows");
	ShowWindow(windows.normal, SW_SHOWNOACTIVATE);
	ShowWindow(windows.topmost, SW_SHOWNOACTIVATE);
	const HWND foreground = GetForegroundWindow();
	const auto normal_style = GetWindowLongPtrW(windows.normal, GWL_EXSTYLE);
	const auto top_style = GetWindowLongPtrW(windows.topmost, GWL_EXSTYLE);
	for (int i = 0; i < 32; ++i) {
		ShowWindow(windows.topmost, i % 2 ? SW_HIDE : SW_SHOWNOACTIVATE);
		const HWND required[]{windows.topmost, windows.normal, windows.topmost, nullptr};
		const auto order = enumerate_window_order(required);
		for (HWND window : {windows.normal, windows.topmost}) {
			if (std::count_if(order.begin(), order.end(), [&](auto entry) {
				    return entry.window == reinterpret_cast<std::uintptr_t>(window);
			    }) != 1) {
				throw std::runtime_error("Live HWND membership must be unique and independent of visibility");
			}
		}
		const auto normal = std::find_if(order.begin(), order.end(), [&](auto entry) {
			return entry.window == reinterpret_cast<std::uintptr_t>(windows.normal);
		});
		const auto topmost = std::find_if(order.begin(), order.end(), [&](auto entry) {
			return entry.window == reinterpret_cast<std::uintptr_t>(windows.topmost);
		});
		if (topmost >= normal || !topmost->topmost || normal->topmost ||
		    GetWindowLongPtrW(windows.normal, GWL_EXSTYLE) != normal_style ||
		    GetWindowLongPtrW(windows.topmost, GWL_EXSTYLE) != top_style || GetForegroundWindow() != foreground) {
			throw std::runtime_error("Inventory changed foreground, styles, or topmost ordering");
		}
	}
	const HWND destroyed = windows.topmost;
	check_win(DestroyWindow(destroyed), "Destroy inventory test window");
	windows.topmost = nullptr;
	const auto order = enumerate_window_order(std::span<const HWND>(&destroyed, 1));
	if (std::any_of(order.begin(), order.end(),
	                [&](auto entry) { return entry.window == reinterpret_cast<std::uintptr_t>(destroyed); })) {
		throw std::runtime_error("Reconciliation resurrected a destroyed HWND");
	}
}

void popup_order(Renderer &renderer)
{
	for (DWORD band : {DWORD{0}, DWORD{WS_EX_TOPMOST}}) {
		HWND popup = CreateWindowExW(band | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
		                             L"Popup ordering regression", WS_POPUP | WS_BORDER, 100, 100, 240, 40, nullptr,
		                             nullptr, GetModuleHandleW(nullptr), nullptr);
		check_win(popup != nullptr, "Create test popup");

		struct WindowGuard
		{
			HWND w;

			~WindowGuard()
			{
				DestroyWindow(w);
			}
		} guard{popup};

		ShowWindow(popup, SW_SHOWNOACTIVATE);
		DesktopSnapshot snapshot;
		snapshot.popups = {popup};
		renderer.synchronize_desktop(snapshot);
		bool checked = false;
		for (HWND w = FindWindowExW(nullptr, nullptr, L"SnowDesk.Overlay.1.0", nullptr); w;
		     w = FindWindowExW(nullptr, w, L"SnowDesk.Overlay.1.0", nullptr)) {
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			RECT bounds{}, popup_bounds{};
			GetWindowRect(w, &bounds);
			GetWindowRect(popup, &popup_bounds);
			if (pid != GetCurrentProcessId() || !screen_rect(bounds).intersects(screen_rect(popup_bounds))) {
				continue;
			}
			checked = true;
			if (!above(popup, w)) {
				throw std::runtime_error("Overlay covers a native popup");
			}
		}
		if (!checked) {
			throw std::runtime_error("No overlay intersects the test popup");
		}
		if ((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_TOPMOST) != band) {
			throw std::runtime_error("Popup's original topmost flag was changed");
		}
		ShowWindow(popup, SW_HIDE);
		renderer.synchronize_desktop({});
		for (HWND w = FindWindowExW(nullptr, nullptr, L"SnowDesk.Overlay.1.0", nullptr); w;
		     w = FindWindowExW(nullptr, w, L"SnowDesk.Overlay.1.0", nullptr)) {
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			if (pid == GetCurrentProcessId() && !(GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
				throw std::runtime_error("Overlay did not restore its topmost band");
			}
		}
	}
}

void popup_switching(Renderer &renderer)
{
	struct Windows
	{
		std::vector<HWND> handles;
		HWND foreground = GetForegroundWindow();

		~Windows()
		{
			for (auto w : handles) {
				DestroyWindow(w);
			}
			if (IsWindow(foreground)) {
				SetForegroundWindow(foreground);
			}
		}
	} windows;

	const auto make_window = [&](DWORD ex, DWORD style, const wchar_t *title) {
		HWND w = CreateWindowExW(ex, L"STATIC", title, style, 100, 100, 400, 250, nullptr, nullptr,
		                         GetModuleHandleW(nullptr), nullptr);
		check_win(w != nullptr, "Create Z-order regression window");
		windows.handles.push_back(w);
		return w;
	};
	const HWND app = make_window(0, WS_OVERLAPPEDWINDOW, L"Focused application regression");
	const HWND stale =
	    make_window(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP | WS_BORDER, L"Visible popup BEHIND the focused app");
	const HWND tips[]{make_window(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP | WS_BORDER, L"First icon"),
	                  make_window(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP | WS_BORDER, L"Next icon")};
	constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
	ShowWindow(app, SW_SHOW);
	SetForegroundWindow(app);
	if (GetForegroundWindow() != app) {
		throw std::runtime_error("Interactive Z-order test requires foreground access");
	}
	ShowWindow(stale, SW_SHOWNOACTIVATE);
	check_win(SetWindowPos(stale, app, 0, 0, 0, 0, flags), "Place stale popup below focused app");

	const auto verify = [&](HWND tip) {
		bool checked = false;
		for (HWND w = FindWindowExW(nullptr, nullptr, L"SnowDesk.Overlay.1.0", nullptr); w;
		     w = FindWindowExW(nullptr, w, L"SnowDesk.Overlay.1.0", nullptr)) {
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			RECT bounds{}, app_bounds{};
			GetWindowRect(w, &bounds);
			GetWindowRect(app, &app_bounds);
			if (pid != GetCurrentProcessId() || !screen_rect(bounds).intersects(screen_rect(app_bounds))) {
				continue;
			}
			checked = true;
			if (!IsWindowVisible(w) || !above(w, app)) {
				throw std::runtime_error("Entire snow layer disappeared behind the focused application");
			}
			if (tip && !above(tip, w)) {
				throw std::runtime_error("Current tooltip did not stay above snow");
			}
			if (!tip && !(GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
				throw std::runtime_error("Background/stale popup prevented restoring the topmost band");
			}
		}
		if (!checked || GetForegroundWindow() != app) {
			throw std::runtime_error("Missing test overlay or rendering stole foreground focus");
		}
	};

	for (unsigned icon = 0; icon < 20; ++icon) {
		if (icon == 10) {
			ShowWindow(app, SW_MAXIMIZE);
			check_win(SetWindowPos(stale, app, 0, 0, 0, 0, flags), "Keep stale popup behind maximized app");
		}
		const HWND current = tips[icon % 2], previous = tips[(icon + 1) % 2];
		const bool topmost = icon % 4 >= 2;
		ShowWindow(previous, SW_HIDE);
		check_win(SetWindowPos(current, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags),
		          "Choose tooltip band");
		check_win(SetWindowPos(current, HWND_TOP, 0, 0, 0, 0, flags | SWP_SHOWWINDOW), "Show next icon tooltip");
		DesktopSnapshot snapshot;
		// Deliberately alternate outdated enumeration order. One stale visible
		// popup and one hidden, reused tooltip must not drag the overlay down.
		snapshot.popups =
		    icon % 2 ? std::vector<HWND>{current, stale, previous} : std::vector<HWND>{previous, stale, current};
		for (unsigned repeat = 0; repeat < 3; ++repeat) {
			renderer.synchronize_desktop(snapshot);
			verify(current);
		}
		if (((GetWindowLongPtrW(current, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost ||
		    (GetWindowLongPtrW(stale, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
			throw std::runtime_error("Renderer changed a source window's topmost flag");
		}
	}
	for (HWND tip : tips) {
		ShowWindow(tip, SW_HIDE);
	}
	DesktopSnapshot stale_snapshot;
	stale_snapshot.popups = {stale, tips[0], tips[1], app}; // Also simulate a reused HWND with a new role.
	renderer.synchronize_desktop(stale_snapshot);
	verify(nullptr);
	renderer.synchronize_desktop({});
	verify(nullptr);
}

void shell_layer(Renderer &renderer, const Display &display)
{
	const auto instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW cls{sizeof(cls)};
	cls.lpfnWndProc = DefWindowProcW;
	cls.hInstance = instance;
	cls.lpszClassName = L"Shell_SecondaryTrayWnd"; // Local test class, never the real Explorer HWND.
	if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		check_win(FALSE, "Register test taskbar class");
	}

	struct Windows
	{
		std::vector<HWND> handles;
		HWND foreground = GetForegroundWindow();

		~Windows()
		{
			for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
				DestroyWindow(*it);
			}
			if (IsWindow(foreground)) {
				SetForegroundWindow(foreground);
			}
		}
	} windows;

	const auto &b = display.bounds;
	const LONG bar_top = b.top + (b.bottom - b.top) / 2;
	const HWND app = CreateWindowExW(0, L"STATIC", L"Taskbar hover foreground", WS_OVERLAPPEDWINDOW, b.left + 100,
	                                 b.top + 100, 400, 250, nullptr, nullptr, instance, nullptr);
	check_win(app != nullptr, "Create shell test application");
	windows.handles.push_back(app);
	const HWND bar =
	    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"Synthetic taskbar",
		                WS_POPUP, b.left, bar_top, b.right - b.left, 48, nullptr, nullptr, instance, nullptr);
	check_win(bar != nullptr, "Create synthetic taskbar");
	windows.handles.push_back(bar);
	ShowWindow(app, SW_SHOW);
	SetForegroundWindow(app);
	ShowWindow(bar, SW_SHOWNOACTIVATE);
	std::vector<HWND> overlays;
	for (HWND w = FindWindowExW(nullptr, nullptr, L"SnowDesk.Overlay.1.0", nullptr); w;
	     w = FindWindowExW(nullptr, w, L"SnowDesk.Overlay.1.0", nullptr)) {
		DWORD pid = 0;
		GetWindowThreadProcessId(w, &pid);
		RECT bounds{};
		GetWindowRect(w, &bounds);
		if (pid == GetCurrentProcessId() && screen_rect(bounds).intersects(screen_rect(b))) {
			overlays.push_back(w);
			if (GetPropW(w, L"NonRudeHWND") != reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)) ||
			    IsWindowEnabled(w)) {
				throw std::runtime_error(
				    "Decorative overlay can interfere with shell fullscreen detection or hit testing");
			}
			// Reproduce the old failure: snow above the shell, before any tooltip.
			check_win(SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
			          "Raise test overlay");
		}
	}
	if (overlays.empty()) {
		throw std::runtime_error("No overlay for shell ordering test");
	}
	DesktopSnapshot snapshot;
	snapshot.taskbars = {bar};
	renderer.synchronize_desktop(snapshot);
	for (auto w : overlays) {
		if (!above(bar, w) || !above(w, app)) {
			throw std::runtime_error("Expected taskbar > snow > active application before hover");
		}
	}
	const HWND tip =
	    CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Inactive taskbar tooltip",
		                WS_POPUP | WS_BORDER, b.left + 150, bar_top - 40, 240, 40, bar, nullptr, instance, nullptr);
	check_win(tip != nullptr, "Create taskbar-owned tooltip");
	windows.handles.push_back(tip);
	ShowWindow(tip, SW_SHOWNOACTIVATE);
	// Intentionally do not tell the renderer about the tooltip: it should already
	// be above snow by virtue of its shell owner, without needing taskbar focus.
	for (auto w : overlays) {
		if (!above(tip, w) || GetForegroundWindow() != app) {
			throw std::runtime_error("Taskbar-owned tooltip requires focus or a later popup scan");
		}
	}
	const HWND hit = WindowFromPoint({b.left + 100, bar_top + 24});
	if (GetAncestor(hit, GA_ROOT) != bar) {
		throw std::runtime_error("Taskbar hit test intercepted by a decorative layer");
	}
	ShowWindow(tip, SW_HIDE);
	check_win(SetWindowPos(bar, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
	          "Demote synthetic taskbar");
	renderer.synchronize_desktop(snapshot);
	for (auto w : overlays) {
		if (!(GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
			throw std::runtime_error("Snow followed a demoted taskbar into the ordinary window band");
		}
	}
	renderer.synchronize_desktop({});
}

void fullscreen_transition(const Display &display)
{
	struct Windows
	{
		std::vector<HWND> handles;
		HWND foreground = GetForegroundWindow();

		~Windows()
		{
			for (auto w : handles) {
				DestroyWindow(w);
			}
			if (IsWindow(foreground)) {
				SetForegroundWindow(foreground);
			}
		}
	} windows;

	const auto instance = GetModuleHandleW(nullptr);
	const auto b = display.bounds;
	const auto make = [&](DWORD style, int x, int y, int width, int height) {
		HWND w = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"Foreground latency regression", style, x, y, width,
		                         height, nullptr, nullptr, instance, nullptr);
		check_win(w != nullptr, "Create foreground regression window");
		windows.handles.push_back(w);
		return w;
	};
	const HWND game = make(WS_POPUP, b.left, b.top, b.right - b.left, b.bottom - b.top);
	const HWND app = make(WS_OVERLAPPEDWINDOW, b.left + 50, b.top + 50, 400, 250);
	ShowWindow(game, SW_SHOWNOACTIVATE);
	ShowWindow(app, SW_SHOWMAXIMIZED);
	if (!foreground_fullscreen_fast(game) || foreground_fullscreen_fast(app)) {
		throw std::runtime_error("Fullscreen fast path must distinguish client coverage from ordinary maximization");
	}
	ShowWindow(app, SW_RESTORE);
	check_win(SetForegroundWindow(app), "Activate ordinary regression window");
	if (GetForegroundWindow() != app) {
		throw std::runtime_error("Foreground regression requires an interactive desktop");
	}
	Settings settings;
	settings.density = Density::off;
	settings.pause_fullscreen = false;
	Landscape world;
	auto rect = screen_rect(b);
	rect.top = rect.bottom - 48 * display.scale;
	const SurfaceSnapshot bar{77, rect, display.scale, 0, SurfaceKind::taskbar, SurfaceState::visible};
	DesktopSnapshot stale;
	stale.surfaces = {bar};
	stale.blockers = {{screen_rect(b), 10}};
	world.synchronize(stale.surfaces, stale.blockers, settings, 0.1f, false);
	for (float x = 0; x < rect.width(); x += 4) {
		world.deposit({bar.id, x, 0, 0}, 20);
	}
	const auto revision = world.find(bar.id)->snow.revision();
	Renderer renderer(instance, nullptr, {display}, settings, world);
	renderer.update_policy(false, false);
	renderer.synchronize_desktop(stale);
	const auto commits = [&] {
		const auto description = renderer.description();
		const std::string label = "Frame/static commits: ";
		return std::stoull(description.substr(description.find(label) + label.size()));
	};
	for (int cycle = 0; cycle < 10; ++cycle) {
		if (renderer.needs_animation()) {
			throw std::runtime_error("Regression must use a static scene without a frame timer");
		}
		check_win(SetForegroundWindow(game), "Return to fullscreen regression window");
		const auto before = commits();
		renderer.refresh_foreground(true);
		if (!world.find(bar.id)->foreground_pending || world.find(bar.id)->enabled(settings) ||
		    commits() != before + 1) {
			throw std::runtime_error("Foreground switch must submit cap hiding without a snapshot or animation tick");
		}
		world.synchronize(stale.surfaces, stale.blockers, settings, 0.1f, false);
		renderer.synchronize_desktop(stale);
		if (world.find(bar.id)->enabled(settings)) {
			throw std::runtime_error("Stale worker snapshot restored fullscreen taskbar snow");
		}
		world.synchronize(stale.surfaces, {{screen_rect(b), -1}}, settings, 0.1f, false);
		renderer.synchronize_desktop(stale);
		world.synchronize(stale.surfaces, stale.blockers, settings, 0.1f, false);
		renderer.synchronize_desktop(stale);
		if (!world.find(bar.id)->enabled(settings)) {
			throw std::runtime_error("Revealed taskbar is suppressed again by unchanged foreground sampling");
		}
		check_win(SetForegroundWindow(app), "Return to ordinary regression window");
		renderer.refresh_foreground(true);
		if (!world.find(bar.id)->enabled(settings) || world.find(bar.id)->snow.revision() != revision) {
			throw std::runtime_error("Foreground transitions changed the saved profile");
		}
	}
	for (const auto name : {L"ForegroundStaging", L"ControlCenterWindow"}) {
		WNDCLASSEXW cls{sizeof(cls)};
		cls.hInstance = instance;
		cls.lpfnWndProc = DefWindowProcW;
		cls.lpszClassName = name;
		check_win(RegisterClassExW(&cls) != 0, "Register shell focus regression class");

		struct ShellWindow
		{
			HINSTANCE instance;
			const wchar_t *name;
			HWND window{};

			~ShellWindow()
			{
				if (window) {
					DestroyWindow(window);
				}
				UnregisterClassW(name, instance);
			}
		} shell{instance, name};

		shell.window = CreateWindowExW(WS_EX_APPWINDOW, name, L"Synthetic shell focus host", WS_POPUP, b.left, b.top,
		                               b.right - b.left, b.bottom - b.top, nullptr, nullptr, instance, nullptr);
		check_win(shell.window != nullptr, "Create shell focus host");
		ShowWindow(shell.window, SW_SHOWNOACTIVATE);
		if (foreground_fullscreen_fast(shell.window)) {
			throw std::runtime_error("Screen-sized shell host was mistaken for a fullscreen application");
		}
		check_win(SetForegroundWindow(game), "Start fullscreen transition before shell activation");
		renderer.refresh_foreground(true);
		check_win(SetForegroundWindow(shell.window), "Leave focus on shell host instead of an application");
		renderer.refresh_foreground(true);
		auto shell_snapshot = stale;
		if (is_native_popup({name, true, false, false, false, false, true})) {
			shell_snapshot.popups = {shell.window};
		}
		for (int sample = 0; sample < 10; ++sample) {
			world.synchronize(shell_snapshot.surfaces, shell_snapshot.blockers, settings, 0.1f, false);
			renderer.synchronize_desktop(shell_snapshot);
			if (!world.find(bar.id)->enabled(settings) || world.find(bar.id)->foreground_pending ||
			    world.find(bar.id)->snow.revision() != revision || GetForegroundWindow() != shell.window) {
				throw std::runtime_error("Shell focus latched bank hiding, changed snow, or stole activation");
			}
		}
		check_win(SetForegroundWindow(app), "Finish shell focus regression");
	}
}

void run_series(bool reuse_device, const std::vector<Display> &displays, Landscape &landscape)
{
	Settings settings;
	settings.density = Density::heavy;
	std::unique_ptr<Renderer> renderer;
	std::uint64_t baseline = 0, peak = 0;
	DWORD user_baseline = 0, gdi_baseline = 0;
	const auto instance = GetModuleHandleW(nullptr);
	for (unsigned i = 0; i < 25; ++i) {
		if (reuse_device && renderer) {
			renderer->rebuild_scene(displays, settings);
		} else {
			renderer.reset();
			renderer = std::make_unique<Renderer>(instance, nullptr, displays, settings, landscape);
		}
		renderer->update_policy(false, false);
		if (i == 0) {
			popup_order(*renderer);
			popup_switching(*renderer);
			shell_layer(*renderer, displays.front());
		}
		for (int frame = 0; frame < 5; ++frame) {
			renderer->tick(1.0f / 30);
			pump();
		}
		const auto bytes = private_bytes();
		std::cout << (reuse_device ? "scene" : "device") << ',' << i << ',' << bytes << '\n';
		if (i == 4) {
			baseline = bytes;
			user_baseline = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
			gdi_baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
		}
		if (i >= 5) {
			peak = std::max(peak, bytes);
		}
	}
	const auto user_now = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
	const auto gdi_now = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
	if (peak > baseline + 32ull * 1024 * 1024 || user_now > user_baseline + 4 || gdi_now > gdi_baseline + 4) {
		throw std::runtime_error("Rebuild resource growth exceeds the regression budget after warm-up");
	}
}
}

int main()
{
	try {
		check_win(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
		          "Enable test DPI awareness");
		ComApartment apartment;
		window_inventory_membership();
		const auto displays = enumerate_displays();
		fullscreen_transition(displays.front());
		Landscape landscape;
		std::vector<SurfaceSnapshot> surfaces;
		SurfaceId id = 1;
		for (const auto &display : displays) {
			auto r = screen_rect(display.bounds);
			r.top = r.bottom - 50 * display.scale;
			surfaces.push_back({id++, r, display.scale, 0, SurfaceKind::taskbar, SurfaceState::visible});
		}
		landscape.synchronize(surfaces, {}, {}, 0.1f, false);
		for (const auto &s : surfaces) {
			for (float x = 0; x < s.rect.width(); x += 4) {
				landscape.deposit({s.id, x, 0, 0}, 100);
			}
		}
		run_series(true, displays, landscape);
		run_series(false, displays, landscape);
		std::cout << "PASS window inventory, foreground visibility, popup ordering, scene/device rebuild memory and "
		             "USER/GDI budgets\n";
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "FAIL " << e.what() << '\n';
		return 1;
	}
}
