#include "app.hpp"
#include "localization.hpp"
#include "tray_icon.hpp"
#include "version.hpp"
#include <algorithm>
#include <commctrl.h>
#include <cstring>
#include <dbt.h>
#include <fstream>
#include <powrprof.h>
#include <psapi.h>
#include <shellapi.h>
#include <sstream>
#include <wtsapi32.h>

namespace snow
{
namespace
{
constexpr auto control_class = L"speeecial.Control.1.0";
constexpr UINT message_tray = WM_APP + 1;
constexpr UINT message_policy = WM_APP + 3;
constexpr UINT_PTR timer_frame = 1, timer_policy = 2, timer_rebuild = 3, timer_desktop = 4, timer_menu_tip = 5;
constexpr UINT icon_id = 1;

bool dark_taskbar() noexcept
{
	DWORD light = 1, bytes = sizeof(light);
	RegGetValueW(
		HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
	return light == 0;
}

enum Command : UINT {
	cmd_pause = 100,
	cmd_clear,
	cmd_accumulate,
	cmd_melt,
	cmd_fullscreen,
	cmd_battery,
	cmd_autostart,
	cmd_retry,
	cmd_diagnostics,
	cmd_folder,
	cmd_exit,
	cmd_windows,
	cmd_collapse,
	cmd_shake,
	cmd_cursor,
	cmd_capture,
	cmd_stop,
	cmd_update_check,
	cmd_update_auto,
	cmd_update_install,
	cmd_update_page,
	cmd_density = 200,
	cmd_wind = 220,
	cmd_fps = 240,
	cmd_layer = 310,
	cmd_height = 330,
	cmd_theme = 500,
	cmd_language = 510,
	cmd_flake_size = 520
};
}

App::App(HINSTANCE instance) : instance_(instance), settings_(store_.load()) {}

App::~App()
{
	closing_ = true;
	updater_.reset();
	close_menu_tip();
	tracker_.reset();
	if (power_notification_) {
		UnregisterPowerSettingNotification(power_notification_);
	}
	if (window_) {
		WTSUnRegisterSessionNotification(window_);
		KillTimer(window_, timer_frame);
		KillTimer(window_, timer_policy);
		KillTimer(window_, timer_rebuild);
		KillTimer(window_, timer_desktop);
	}
	renderer_.reset();
	if (tray_added_) {
		NOTIFYICONDATAW data{sizeof(data)};
		data.hWnd = window_;
		data.uID = icon_id;
		Shell_NotifyIconW(NIM_DELETE, &data);
	}
	if (window_) {
		DestroyWindow(window_);
	}
	if (icon_) {
		DestroyIcon(icon_);
	}
}

int App::run()
{
	WNDCLASSEXW wc{sizeof(wc)};
	wc.hInstance = instance_;
	wc.lpfnWndProc = window_proc;
	wc.lpszClassName = control_class;
	check_win(RegisterClassExW(&wc) != 0, "Register control window");
	// Hidden TOP-LEVEL window, not HWND_MESSAGE: it must receive TaskbarCreated.
	window_ = CreateWindowExW(WS_EX_TOOLWINDOW, control_class, L"speeecial (snow desktop)", WS_OVERLAPPED, 0, 0, 0, 0,
	                          nullptr, nullptr, instance_, this);
	if (!window_) {
		check_win(FALSE, "Create control window");
	}
	taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
	add_tray_icon();
	tracker_ = std::make_unique<DesktopTracker>(window_);
	last_desktop_ = std::chrono::steady_clock::now();
	WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
	power_notification_ =
	    RegisterPowerSettingNotification(window_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
	if (!SetTimer(window_, timer_policy, 1000, nullptr)) {
		check_win(FALSE, "Create policy timer");
	}
	measurement_start_ = std::chrono::steady_clock::now();
	rebuild();
	if (tray_added_ && renderer_) {
		acknowledge_update_startup();
	}
	store_.log("speeecial (snow desktop) " SPEEECIAL_VERSION " started");
	// If already enabled, follow this executable after an upgrade/move.
	try {
		if (!update_startup_pending() && SettingsStore::autostart()) {
			SettingsStore::set_autostart(true);
		}
	} catch (const std::exception &error) {
		store_.log(error.what());
	}
	try {
		updater_ = std::make_unique<Updater>(window_, store_.directory());
	} catch (const std::exception &error) {
		report_error(error.what(), false);
	}
	MSG message{};
	bool quit = false;
	int exit_code = 0;
	while (!quit) {
		const auto batch_start = std::chrono::steady_clock::now();
		// WinEvent/input traffic must not starve rendering: WM_TIMER alone is
		// low priority. Bound message batches and service the frame deadline.
		for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
			if (message.message == WM_QUIT) {
				quit = true;
				exit_code = static_cast<int>(message.wParam);
				break;
			}
			TranslateMessage(&message);
			DispatchMessageW(&message);
			if (std::chrono::steady_clock::now() - batch_start > std::chrono::milliseconds(3)) {
				break;
			}
		}
		if (quit) {
			break;
		}
		try {
			tick();
		} catch (const std::exception &e) {
			report_error(e.what(), true);
		}
		DWORD timeout = INFINITE;
		if (timer_fps_) {
			const auto remaining =
			    std::chrono::ceil<std::chrono::milliseconds>(next_frame_ - std::chrono::steady_clock::now()).count();
			timeout = static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 0, 1000));
		}
		if (MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
			check_win(FALSE, "Wait for messages/frame deadline");
		}
	}
	store_.log("speeecial stopped");
	return exit_code;
}

void App::add_tray_icon()
{
	update_tray_icon(updater_ && updater_->view().update_available());
	check_win(icon_ != nullptr, "Load tray icon");
	NOTIFYICONDATAW data{sizeof(data)};
	data.hWnd = window_;
	data.uID = icon_id;
	data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
	data.uCallbackMessage = message_tray;
	data.hIcon = icon_;
	lstrcpynW(data.szTip, L"speeecial (snow desktop)", static_cast<int>(std::size(data.szTip)));
	tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
	if (tray_added_) {
		data.uVersion = NOTIFYICON_VERSION_4;
		Shell_NotifyIconW(NIM_SETVERSION, &data);
	}
	// If Explorer is not ready during logon, the 1-second policy timer retries.
}

void App::update_tray_icon(bool update_available) noexcept
{
	icon_update_requested_ = update_available;
	try {
		const SIZE size{GetSystemMetrics(SM_CXSMICON),
		                GetSystemMetrics(SM_CYSMICON)};
		const bool dark = dark_taskbar();
		if (icon_ && icon_size_.cx == size.cx && icon_size_.cy == size.cy &&
		    icon_update_available_ == update_available && icon_dark_ == dark) {
			icon_dirty_ = false;
			return;
		}
		const auto replacement =
			load_tray_icon(instance_, size.cx, size.cy, update_available, dark);
		if (tray_added_) {
			NOTIFYICONDATAW data{sizeof(data)};
			data.hWnd = window_;
			data.uID = icon_id;
			data.uFlags = NIF_ICON;
			data.hIcon = replacement;
			if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
				tray_added_ = false;
			}
		}
		const auto previous = icon_;
		icon_ = replacement;
		icon_size_ = size;
		icon_update_available_ = update_available;
		icon_dark_ = dark;
		icon_dirty_ = false;
		if (previous) {
			DestroyIcon(previous);
		}
	} catch (const std::exception &error) {
		// Keep the old icon and retry on the policy timer, never by rebuilding
		// the snow renderer or allocating GDI objects in the frame loop.
		icon_dirty_ = true;
		store_.log(error.what());
	} catch (...) {
		icon_dirty_ = true;
		store_.log("Could not update the tray icon");
	}
}

void App::notify(const wchar_t *title, const wchar_t *text, DWORD flags) noexcept
{
	if (!tray_added_) {
		return;
	}
	NOTIFYICONDATAW data{sizeof(data)};
	data.hWnd = window_;
	data.uID = icon_id;
	data.uFlags = NIF_INFO;
	data.dwInfoFlags = (flags ? flags : NIIF_INFO) | NIIF_NOSOUND;
	lstrcpynW(data.szInfoTitle, title, static_cast<int>(std::size(data.szInfoTitle)));
	lstrcpynW(data.szInfo, text, static_cast<int>(std::size(data.szInfo)));
	Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::report_error(const std::string &error, bool graphics_error) noexcept
{
	try {
		last_error_ = error;
		store_.log(error);
		if (graphics_error) {
			graphics_healthy_since_ = 0;
			KillTimer(window_, timer_frame);
			timer_fps_ = 0;
			if (renderer_) {
				renderer_->hide();
			}
			renderer_.reset();
			if (graphics_attempts_ < 3) {
				graphics_retry_at_ = GetTickCount64() + (1000ull << graphics_attempts_++);
			} else {
				graphics_retry_at_ = 0;
			}
		}
		const auto text = widen(error);
		notify(L"speeecial: ошибка", text.c_str(), NIIF_ERROR);
	} catch (...) { /* Never unwind through a Win32 callback. */
	}
}

void App::rebuild()
{
	if (rebuilding_ || closing_) {
		return;
	}
	rebuilding_ = true;
	KillTimer(window_, timer_rebuild);
	KillTimer(window_, timer_frame);
	timer_fps_ = 0;
	try {
		const auto displays = enumerate_displays();
		if (renderer_) {
			try {
				renderer_->check_device();
			} catch (...) {
				renderer_.reset();
			}
		}
		if (renderer_) {
			renderer_->rebuild_scene(displays, settings_);
		} else {
			renderer_ = std::make_unique<Renderer>(instance_, window_, displays, settings_, landscape_);
		}
		++graphics_rebuilds_;
		if (!renderer_->set_capture(settings_.show_capture)) {
			settings_.show_capture = true;
			notify(L"speeecial (snow desktop)",
			       L"Windows не разрешила исключить эффекты из захвата. Отображение на снимках оставлено включённым.");
		}
		store_.log(renderer_->description());
		update_policy();
		refresh_desktop(true);
		last_error_.clear();
		graphics_retry_at_ = 0;
		graphics_healthy_since_ = GetTickCount64();
	} catch (const std::exception &error) {
		report_error(error.what(), true);
	}
	rebuilding_ = false;
}

void App::sync_timer()
{
	const bool motion = tracker_ && tracker_->moving() && run_mode_ != RunMode::stopped && !locked_ && !display_off_ &&
	                    !(settings_.pause_battery && on_battery());
	const int desired = renderer_ && (renderer_->needs_animation() || motion) ? settings_.fps : 0;
	if (timer_fps_ == desired) {
		return;
	}
	KillTimer(window_, timer_frame);
	timer_fps_ = 0;
	if (desired) {
		// WM_TIMER is a fallback for the tray menu's nested message loop.
		// The main pump services steady-clock deadlines even under event traffic.
		const UINT interval = static_cast<UINT>((1000 + desired - 1) / desired);
		if (!SetTimer(window_, timer_frame, interval, nullptr)) {
			check_win(FALSE, "Create frame timer");
		}
		timer_fps_ = desired;
		last_frame_ = std::chrono::steady_clock::now();
		next_frame_ = last_frame_ + std::chrono::microseconds(1000000 / desired);
	}
}

void App::update_policy()
{
	if (!renderer_) {
		return;
	}
	renderer_->update_policy(run_mode_ == RunMode::stopped || locked_ || display_off_ ||
	                             (settings_.pause_battery && on_battery()),
	                         run_mode_ == RunMode::hidden);
	sync_timer();
}

void App::refresh_desktop(bool force)
{
	if (!tracker_ || (!force && !tracker_->dirty())) {
		return;
	}
	auto ready = tracker_->snapshot(force);
	if (!ready) {
		return;
	}
	auto &snapshot = *ready;
	const auto now = std::chrono::steady_clock::now();
	const float elapsed = std::chrono::duration<float>(now - last_desktop_).count();
	last_desktop_ = now;
	const bool animate = renderer_ && !snapshot.desktop_changed && run_mode_ != RunMode::stopped && !locked_ &&
	                     !display_off_ && !(settings_.pause_battery && on_battery());
	landscape_.synchronize(snapshot.surfaces, std::move(snapshot.blockers), settings_, elapsed, animate);
	if (renderer_) {
		renderer_->synchronize_desktop(snapshot, timer_fps_ != 0);
		update_policy();
	} else {
		landscape_.consume_emissions();
	}
}

void App::tick()
{
	if (!renderer_ || !timer_fps_) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < next_frame_) {
		return;
	}
	next_frame_ = now + std::chrono::microseconds(1000000 / timer_fps_);
	const float dt = std::chrono::duration<float>(now - last_frame_).count();
	peak_frame_interval_ms_ = std::max(peak_frame_interval_ms_, static_cast<double>(dt) * 1000);
	if (dt * timer_fps_ > 1.75f) {
		++late_frames_;
	}
	last_frame_ = now;
	renderer_->refresh_foreground();
	refresh_desktop();
	if (!renderer_) {
		return;
	}
	renderer_->tick(std::min(dt, 0.1f));
	last_frame_work_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
	peak_frame_work_ms_ = std::max(peak_frame_work_ms_, last_frame_work_ms_);
	++frames_;
	sync_timer();
}

void App::show_menu()
{
	if (menu_open_) {
		return;
	}

	struct Flag
	{
		bool &value;

		explicit Flag(bool &v) : value(v)
		{
			value = true;
		}

		~Flag()
		{
			value = false;
		}
	} guard(menu_open_);

	const auto language = resolve_language(
		settings_.language,
		PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN);
	const auto tr = [language](Text text) { return localized(text, language); };
	POINT point{};
	GetCursorPos(&point);
	const int original_size = settings_.flake_size;
	const auto original_density = settings_.density;
	UINT selected = 0;
	{
		const auto theme = settings_.theme == Theme::light ? MenuTheme::light
		                   : settings_.theme == Theme::dark
		                       ? MenuTheme::dark
		                       : MenuTheme::automatic;
		TrayMenu menu(instance_, point, theme);
		const auto root = menu.handle();
		menu.header(SPEEECIAL_VERSION_W);
		menu_detail_ = renderer_
		                   ? std::to_wstring(renderer_->particle_count()) +
		                         tr(Text::particles) +
		                         std::to_wstring(settings_.fps) + L" FPS"
		                   : tr(Text::graphics_unavailable);
		menu.section(root, tr(Text::effects));
		{
			const wchar_t *labels[] = {tr(Text::off), tr(Text::light),
			                           tr(Text::normal), tr(Text::blizzard)};
			const auto current = std::clamp(static_cast<int>(settings_.density), 0, 3);
			const auto sub = menu.submenu(root, tr(Text::snowfall),
			                              MenuGlyph::snow, labels[current]);
			menu.slider(sub, cmd_density, tr(Text::snowfall_level), 0, 3,
			            current,
			            [this](int level) {
							settings_.density = static_cast<Density>(level);
							if (renderer_) {
								renderer_->set_settings(settings_);
							}
							sync_timer();
						},
			            {labels[0], labels[1], labels[2], labels[3]});
			menu.slider(sub, cmd_flake_size, tr(Text::flake_size), 2, 12,
			            settings_.flake_size, [this](int size) {
							settings_.flake_size = size;
							if (renderer_) {
								renderer_->set_settings(settings_);
							}
							sync_timer();
						});
		}
		{
			const auto sub = menu.submenu(
				root, tr(Text::placement), MenuGlyph::layers,
				settings_.layer == Layer::front ? tr(Text::in_front)
												: tr(Text::behind));
			menu.item(sub, cmd_layer, tr(Text::in_front), MenuMark::radio,
			          settings_.layer == Layer::front);
			menu.item(sub, cmd_layer + 1, tr(Text::behind), MenuMark::radio,
			          settings_.layer == Layer::behind);
		}
		{
			const wchar_t *labels[] = {tr(Text::calm), tr(Text::light),
			                           tr(Text::gusty)};
			const auto current = std::clamp(static_cast<int>(settings_.wind), 0, 2);
			const auto sub = menu.submenu(root, tr(Text::wind), MenuGlyph::wind,
			                              labels[current]);
			for (UINT i = 0; i < std::size(labels); ++i) {
				menu.item(sub, cmd_wind + i, labels[i], MenuMark::radio, current == static_cast<int>(i));
			}
		}
		{
			const auto sub =
				menu.submenu(root, tr(Text::banks), MenuGlyph::bank);
			menu.item(sub, cmd_windows, tr(Text::on_windows), MenuMark::toggle,
			          settings_.windows);
			menu.item(sub, cmd_accumulate, tr(Text::on_taskbar),
			          MenuMark::toggle, settings_.accumulate);
			menu.item(sub, cmd_collapse, tr(Text::collapse), MenuMark::toggle,
			          settings_.collapse);
			menu.item(sub, cmd_shake, tr(Text::shake), MenuMark::toggle,
			          settings_.shake);
			menu.item(sub, cmd_melt, tr(Text::melt), MenuMark::toggle,
			          settings_.melt);
			const auto height =
				menu.submenu(sub, tr(Text::max_depth), MenuGlyph::none,
			                 std::to_wstring(settings_.height));
			menu.item(height, cmd_height + 12, tr(Text::thin), MenuMark::radio,
			          settings_.height == 12);
			menu.item(height, cmd_height + 22, tr(Text::normal_depth),
			          MenuMark::radio, settings_.height == 22);
			menu.item(height, cmd_height + 40, tr(Text::deep), MenuMark::radio,
			          settings_.height == 40);
			menu.separator(sub);
			menu.item(sub, cmd_clear, tr(Text::clear), MenuMark::none, false,
			          MenuGlyph::clear);
		}
		menu.item(root, cmd_cursor, tr(Text::cursor), MenuMark::toggle,
		          settings_.cursor, MenuGlyph::cursor);
		menu.section(root, tr(Text::application));
		{
			const wchar_t *labels[] = {tr(Text::system_language), L"Русский",
			                           L"English"};
			const auto current = static_cast<UINT>(settings_.language);
			const auto sub = menu.submenu(root, tr(Text::language),
			                              MenuGlyph::language, labels[current]);
			for (UINT i = 0; i < std::size(labels); ++i) {
				menu.item(sub, cmd_language + i, labels[i], MenuMark::radio,
				          current == i);
			}
		}
		{
			const wchar_t *labels[] = {tr(Text::system_theme),
			                           tr(Text::light_theme),
			                           tr(Text::dark_theme)};
			const auto current = static_cast<UINT>(settings_.theme);
			const auto sub = menu.submenu(root, tr(Text::theme),
			                              MenuGlyph::theme, labels[current]);
			for (UINT i = 0; i < std::size(labels); ++i) {
				menu.item(sub, cmd_theme + i, labels[i], MenuMark::radio,
				          current == i);
			}
		}
		{
			const auto sub =
				menu.submenu(root, tr(Text::performance), MenuGlyph::speed,
			                 std::to_wstring(settings_.fps) + L" FPS");
			for (int rate : {20, 30, 60, 144, 240}) {
				menu.item(sub, cmd_fps + rate, std::to_wstring(rate) + L" FPS", MenuMark::radio, settings_.fps == rate);
			}
			menu.separator(sub);
			menu.item(sub, cmd_fullscreen, tr(Text::fullscreen_pause),
			          MenuMark::toggle, settings_.pause_fullscreen);
			menu.item(sub, cmd_battery, tr(Text::battery_pause),
			          MenuMark::toggle, settings_.pause_battery);
		}
		menu.item(root, cmd_capture, tr(Text::capture), MenuMark::toggle,
		          settings_.show_capture, MenuGlyph::capture);
		menu.item(root, cmd_autostart, tr(Text::startup), MenuMark::toggle,
		          SettingsStore::autostart(), MenuGlyph::startup);
		if (updater_) {
			const auto state = updater_->view();
			const auto sub =
				menu.submenu(root, tr(Text::updates), MenuGlyph::refresh,
			                 state.release ? widen(state.release->tag) : L"",
			                 state.update_available());
			menu.item(sub, cmd_update_check,
			          state.status == Updater::Status::checking
			              ? tr(Text::checking)
			              : tr(Text::check_updates),
			          MenuMark::none, false, MenuGlyph::refresh, L"",
			          !state.busy());
			menu.item(sub, cmd_update_auto, tr(Text::automatic_updates),
			          MenuMark::toggle, updater_->automatic());
			if (state.release && state.release->installable()) {
				auto label = tr(Text::update_to) + widen(state.release->tag);
				if (state.status == Updater::Status::downloading) {
					label = tr(Text::downloading) +
					        std::to_wstring(state.progress) + L"%";
				}
				if (state.status == Updater::Status::ready) {
					label = tr(Text::restarting);
				}
				menu.item(sub, cmd_update_install, label, MenuMark::none, false, MenuGlyph::startup, L"",
				          !state.busy());
			} else if (state.release) {
				menu.item(sub, 0,
				          tr(Text::available) + widen(state.release->tag) +
				              tr(Text::download_manually),
				          MenuMark::none, false, MenuGlyph::none, L"", false);
			}
			menu.separator(sub);
			menu.item(sub, cmd_update_page, tr(Text::releases), MenuMark::none,
			          false, MenuGlyph::file);
		}
		{
			const auto sub =
				menu.submenu(root, tr(Text::diagnostics), MenuGlyph::tools);
			menu.item(sub, cmd_retry, tr(Text::rebuild), MenuMark::none, false,
			          MenuGlyph::refresh);
			menu.item(sub, cmd_diagnostics, tr(Text::save_report),
			          MenuMark::none, false, MenuGlyph::file);
			menu.item(sub, cmd_folder, tr(Text::settings_folder),
			          MenuMark::none, false, MenuGlyph::folder);
		}
		menu.separator(root);
		menu.item(root, cmd_pause, tr(Text::pause), MenuMark::action,
		          run_mode_ == RunMode::hidden, MenuGlyph::pause,
		          run_mode_ == RunMode::hidden ? tr(Text::show_effects)
		                                       : tr(Text::hide_effects));
		menu.item(root, cmd_stop, tr(Text::stop), MenuMark::action,
		          run_mode_ == RunMode::stopped, MenuGlyph::stop,
		          run_mode_ == RunMode::stopped ? tr(Text::resume)
		                                        : tr(Text::freeze));
		menu.item(root, cmd_exit, tr(Text::exit), MenuMark::none, false,
		          MenuGlyph::exit);
		SetForegroundWindow(window_);
		// Item data and GDI resources outlive all callbacks in USER32's modal loop.
		active_menu_ = root;
		styled_menu_ = &menu;

		struct MenuScope
		{
			App &app;

			~MenuScope()
			{
				app.close_menu_tip();
				app.styled_menu_ = nullptr;
			}
		} menu_scope{*this};

		SetTimer(window_, timer_menu_tip, 50, nullptr);
		selected = menu.track(TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, point,
		                      window_);
	}
	if (settings_.flake_size != original_size ||
	    settings_.density != original_density) {
		store_.save(settings_);
	}
	// Release menu resources before executing commands such as graphics rebuild.
	PostMessageW(window_, WM_NULL, 0, 0);
	if (selected) {
		command(selected);
	}
}

void App::close_menu_tip() noexcept
{
	if (window_) {
		KillTimer(window_, timer_menu_tip);
	}
	if (menu_tip_) {
		DestroyWindow(menu_tip_);
		menu_tip_ = nullptr;
	}
	active_menu_ = nullptr;
	menu_hover_since_ = 0;
	menu_tip_visible_ = false;
}

void App::update_menu_tip()
{
	POINT cursor{};
	RECT header{};
	const bool hover = menu_open_ && active_menu_ && GetCursorPos(&cursor) &&
	                   GetMenuItemRect(nullptr, active_menu_, 0, &header) && PtInRect(&header, cursor);
	if (!hover) {
		menu_hover_since_ = 0;
		if (menu_tip_ && menu_tip_visible_) {
			TOOLINFOW tool{sizeof(tool)};
			tool.hwnd = window_;
			tool.uId = 1;
			SendMessageW(menu_tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
			menu_tip_visible_ = false;
		}
		return;
	}
	const auto now = GetTickCount64();
	if (!menu_hover_since_) {
		menu_hover_since_ = now;
	}
	if (menu_tip_visible_ || now - menu_hover_since_ < 450) {
		return;
	}
	TOOLINFOW tool{sizeof(tool)};
	tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
	tool.hwnd = window_;
	tool.uId = 1;
	tool.lpszText = menu_detail_.data();
	if (!menu_tip_) {
		INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
		if (!InitCommonControlsEx(&controls)) {
			return;
		}
		menu_tip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
		                            TOOLTIPS_CLASSW, nullptr,
		                            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_NOANIMATE | TTS_NOFADE, CW_USEDEFAULT,
		                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
		if (!menu_tip_) {
			return;
		}
		if (!SendMessageW(menu_tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
			DestroyWindow(menu_tip_);
			menu_tip_ = nullptr;
			return;
		}
		SendMessageW(menu_tip_, TTM_SETMAXTIPWIDTH, 0, 600);
	}
	SendMessageW(menu_tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(cursor.x + 12, header.bottom + 4));
	SendMessageW(menu_tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
	// TTM_TRACKPOSITION packs signed 16-bit coordinates. Clamp using full
	// screen coordinates after activation, including negative-origin monitors.
	RECT tip{};
	MONITORINFO monitor{sizeof(monitor)};
	if (GetWindowRect(menu_tip_, &tip) &&
	    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) {
		const LONG width = tip.right - tip.left, height = tip.bottom - tip.top;
		LONG x =
		    std::clamp(cursor.x + 12, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
		LONG y = header.bottom + 4;
		if (y + height > monitor.rcWork.bottom) {
			y = header.top - height - 4;
		}
		y = std::clamp(y, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
		SetWindowPos(menu_tip_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
	}
	menu_tip_visible_ = true;
}

void App::command(UINT id)
{
	bool changed = true;
	if (id >= cmd_flake_size + 2 && id <= cmd_flake_size + 12) {
		settings_.flake_size = static_cast<int>(id - cmd_flake_size);
	} else if (id >= cmd_density && id < cmd_density + 4) {
		settings_.density = static_cast<Density>(id - cmd_density);
	} else if (id >= cmd_wind && id < cmd_wind + 3) {
		settings_.wind = static_cast<Wind>(id - cmd_wind);
	} else if (id == cmd_fps + 20 || id == cmd_fps + 30 || id == cmd_fps + 60 ||
	           id == cmd_fps + 144 || id == cmd_fps + 240) {
		settings_.fps = static_cast<int>(id - cmd_fps);
	} else if (id == cmd_layer || id == cmd_layer + 1) {
		settings_.layer = static_cast<Layer>(id - cmd_layer);
	} else if (id == cmd_height + 12 || id == cmd_height + 22 ||
	           id == cmd_height + 40) {
		settings_.height = static_cast<int>(id - cmd_height);
	} else if (id >= cmd_theme && id < cmd_theme + 3) {
		settings_.theme = static_cast<Theme>(id - cmd_theme);
	} else if (id >= cmd_language && id < cmd_language + 3) {
		settings_.language = static_cast<Language>(id - cmd_language);
	} else {
		switch (id) {
		case cmd_update_check:
			if (updater_) {
				updater_->check(true);
			}
			return;
		case cmd_update_auto:
			if (updater_) {
				updater_->set_automatic(!updater_->automatic());
			}
			return;
		case cmd_update_install:
			if (updater_) {
				updater_->install();
			}
			return;
		case cmd_update_page: {
			const auto page = widen(std::string(updates::releases_page));
			ShellExecuteW(window_, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return;
		}
		case cmd_pause:
			run_mode_ = run_mode_ == RunMode::hidden ? RunMode::running : RunMode::hidden;
			changed = false;
			break;
		case cmd_stop:
			run_mode_ = run_mode_ == RunMode::stopped ? RunMode::running : RunMode::stopped;
			changed = false;
			break;
		case cmd_clear:
			if (renderer_) {
				renderer_->clear_snow();
			} else {
				landscape_.clear_snow();
			}
			changed = false;
			break;
		case cmd_windows:
			settings_.windows = !settings_.windows;
			break;
		case cmd_accumulate:
			settings_.accumulate = !settings_.accumulate;
			break;
		case cmd_collapse:
			settings_.collapse = !settings_.collapse;
			break;
		case cmd_shake:
			settings_.shake = !settings_.shake;
			break;
		case cmd_cursor:
			settings_.cursor = !settings_.cursor;
			break;
		case cmd_capture:
			settings_.show_capture = !settings_.show_capture;
			break;
		case cmd_melt:
			settings_.melt = !settings_.melt;
			break;
		case cmd_fullscreen:
			settings_.pause_fullscreen = !settings_.pause_fullscreen;
			break;
		case cmd_battery:
			settings_.pause_battery = !settings_.pause_battery;
			break;
		case cmd_autostart:
			SettingsStore::set_autostart(!SettingsStore::autostart());
			changed = false;
			break;
		case cmd_retry:
			graphics_attempts_ = 0;
			rebuild();
			return;
		case cmd_diagnostics:
			diagnostics();
			return;
		case cmd_folder:
			ShellExecuteW(window_, L"open", store_.directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return;
		case cmd_exit:
			closing_ = true;
			PostQuitMessage(0);
			return;
		default:
			return;
		}
	}
	if (changed) {
		settings_.normalize();
		try {
			if (renderer_) {
				if (!renderer_->set_capture(settings_.show_capture)) {
					settings_.show_capture = true;
					notify(L"speeecial (snow desktop)",
					       L"Windows не разрешила исключить эффекты из захвата. Отображение на снимках "
					       L"оставлено включённым.");
				}
				renderer_->set_settings(settings_);
			}
			refresh_desktop(true);
		} catch (const std::exception &error) {
			report_error(error.what(), true);
		}
		try {
			store_.save(settings_);
		} catch (const std::exception &error) {
			report_error(error.what(), false);
		}
	}
	update_policy();
}

void App::update_finished()
{
	if (!updater_ || closing_) {
		return;
	}
	const auto state = updater_->view();
	if (state.revision == update_revision_) {
		return;
	}
	update_revision_ = state.revision;
	update_tray_icon(state.update_available());
	switch (state.status) {
	case Updater::Status::available: {
		const auto detail = L"Доступна " + widen(state.release->tag) +
		                    (state.release->installable()
		                         ? L". Установить: меню → Обновления."
		                         : L". EXE для встроенного обновления отсутствует; доступно скачивание на GitHub.");
		notify(L"speeecial — обновление", detail.c_str());
		break;
	}
	case Updater::Status::current:
		if (state.manual) {
			notify(L"speeecial", L"У тебя актуальная версия.");
		}
		break;
	case Updater::Status::failed:
		// Network failures must never enter report_error(graphics_error=true).
		if (state.manual) {
			notify(L"Не удалось обновить speeecial",
			       (state.detail + L". Релизы доступны через меню Обновления.").c_str(), NIIF_WARNING);
		}
		{
			const auto text = state.detail;
			const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
			                                      nullptr, nullptr);
			std::string utf8(static_cast<std::size_t>(count), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr,
			                    nullptr);
			store_.log("Update: " + utf8);
		}
		break;
	case Updater::Status::ready:
		if (updater_->commit()) {
			if (menu_open_) {
				EndMenu();
			}
			closing_ = true;
			PostQuitMessage(0);
		}
		break;
	default:
		break;
	}
}

void App::diagnostics()
{
	PROCESS_MEMORY_COUNTERS_EX memory{};
	memory.cb = sizeof(memory);
	check_win(
	    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)),
	    "Read memory counters");
	std::ostringstream out;
	out << "speeecial (snow desktop) " SPEEECIAL_VERSION "\n";
	out << "Graphics scene builds: " << graphics_rebuilds_ << "\nLast frame CPU wall time (ms): " << last_frame_work_ms_
	    << "\nPeak frame CPU wall time (ms): " << peak_frame_work_ms_
	    << "\nPeak frame interval (ms): " << peak_frame_interval_ms_ << "\nLate frame intervals: " << late_frames_
	    << '\n';
	if (tracker_) {
		out << "Desktop full scans: " << tracker_->full_scans()
		    << "\nFast geometry samples: " << tracker_->geometry_samples()
		    << "\nWorker scan ms (last / peak): " << tracker_->last_scan_ms() << " / " << tracker_->peak_scan_ms()
		    << "\nDesktop worker error: " << tracker_->error() << '\n';
	}
	if (renderer_) {
		out << renderer_->description();
	}
	out << "Target FPS: " << settings_.fps << "\nObserved update rate (last interval): " << measured_fps_
	    << "\nPrivate commit bytes: " << memory.PrivateUsage << "\nWorking set bytes: " << memory.WorkingSetSize
	    << "\nGDI objects: " << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)
	    << "\nUSER objects: " << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)
	    << "\nTracked windows: " << (tracker_ ? tracker_->tracked_windows() : 0)
	    << "\nAll WinEvent hooks active: " << (tracker_ && tracker_->hooks_active())
	    << "\nRun mode (0=running, 1=hidden, 2=stopped): " << static_cast<int>(run_mode_)
	    << "\nSession locked: " << locked_ << "\nDisplay off: " << display_off_ << "\nLast error: " << last_error_
	    << "\nThese process counters do not include all driver/DWM/GPU memory.\n";
	const auto file = store_.directory() / L"diagnostics.txt";
	std::ofstream output(file, std::ios::trunc);
	output << out.str();
	output.close();
	if (!output) {
		throw std::runtime_error("Cannot save diagnostics.txt");
	}
	notify(L"speeecial (snow desktop)",
	       L"Диагностика записана в diagnostics.txt. Папку можно открыть через меню в трее.");
}

LRESULT App::dispatch(UINT message, WPARAM wparam, LPARAM lparam)
{
	if (taskbar_created_ && message == taskbar_created_) {
		tray_added_ = false;
		add_tray_icon();
		PostMessageW(window_, message_rebuild, 0, 0);
		return 0;
	}
	switch (message) {
	case WM_MEASUREITEM:
		if (styled_menu_ && lparam && styled_menu_->measure(*reinterpret_cast<MEASUREITEMSTRUCT *>(lparam))) {
			return TRUE;
		}
		return DefWindowProcW(window_, message, wparam, lparam);
	case WM_DRAWITEM:
		if (styled_menu_ && lparam && styled_menu_->draw(*reinterpret_cast<const DRAWITEMSTRUCT *>(lparam))) {
			return TRUE;
		}
		return DefWindowProcW(window_, message, wparam, lparam);
	case WM_MENUCHAR:
		if (styled_menu_ && styled_menu_->styled()) {
			return styled_menu_->character(static_cast<wchar_t>(LOWORD(wparam)), reinterpret_cast<HMENU>(lparam));
		}
		return DefWindowProcW(window_, message, wparam, lparam);
	case WM_THEMECHANGED:
		update_tray_icon(icon_update_requested_);
		if (menu_open_) {
			EndMenu();
		}
		return 0;
	case message_update:
		update_finished();
		return 0;
	case message_tray:
		if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT ||
		    LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_LBUTTONUP) {
			show_menu();
		}
		return 0;
	case message_rebuild:
	case WM_DISPLAYCHANGE:
		if (message == WM_DISPLAYCHANGE) {
			update_tray_icon(icon_update_requested_);
		}
		if (!rebuilding_) {
			if (renderer_) {
				renderer_->hide();
			}
			KillTimer(window_, timer_frame);
			timer_fps_ = 0;
			SetTimer(window_, timer_rebuild, 250, nullptr);
		}
		return 0;
	case message_foreground:
		if (renderer_) {
			renderer_->refresh_foreground(true);
			sync_timer();
		}
		return 0;
	case message_desktop:
	case WM_SETTINGCHANGE:
		if (message == WM_SETTINGCHANGE) {
			update_tray_icon(icon_update_requested_);
		}
		if (message == WM_SETTINGCHANGE && menu_open_) {
			EndMenu();
		}
		sync_timer();
		if (!timer_fps_ && !desktop_scheduled_) {
			desktop_scheduled_ = true;
			SetTimer(window_, timer_desktop, 16, nullptr);
		}
		return 0;
	case message_policy:
		update_policy();
		return 0;
	case WM_TIMER:
		if (wparam == timer_menu_tip) {
			update_menu_tip();
		} else if (wparam == timer_frame) {
			tick();
		} else if (wparam == timer_rebuild) {
			rebuild();
		} else if (wparam == timer_desktop) {
			KillTimer(window_, timer_desktop);
			desktop_scheduled_ = false;
			update_policy();
			refresh_desktop();
		} else if (wparam == timer_policy) {
			if (icon_dirty_) {
				update_tray_icon(icon_update_requested_);
			}
			if (!tray_added_) {
				add_tray_icon();
			}
			const auto now = std::chrono::steady_clock::now();
			const auto seconds = std::chrono::duration<double>(now - measurement_start_).count();
			measured_fps_ = seconds > 0 ? static_cast<double>(frames_) / seconds : 0;
			frames_ = 0;
			measurement_start_ = now;
			if (!renderer_ && graphics_retry_at_ && GetTickCount64() >= graphics_retry_at_) {
				rebuild();
			}
			if (renderer_) {
				renderer_->refresh_foreground();
				renderer_->check_device();
			}
			if (tray_added_ && renderer_) {
				acknowledge_update_startup();
			}
			update_policy();
			refresh_desktop(); // The scanner owns the one-second reconciliation.
			if (renderer_ && graphics_healthy_since_ && GetTickCount64() - graphics_healthy_since_ >= 15000) {
				graphics_attempts_ = 0;
			}
		}
		return 0;
	case WM_WTSSESSION_CHANGE:
		if (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT || wparam == WTS_REMOTE_DISCONNECT) {
			locked_ = true;
		}
		if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_CONSOLE_CONNECT || wparam == WTS_REMOTE_CONNECT) {
			locked_ = false;
		}
		update_policy();
		return 0;
	case WM_POWERBROADCAST:
		if (wparam == PBT_POWERSETTINGCHANGE && lparam) {
			const auto *setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(lparam);
			if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) &&
			    setting->DataLength == sizeof(DWORD)) {
				DWORD value{};
				std::memcpy(&value, setting->Data, sizeof(value));
				display_off_ = value == 0;
			}
		}
		if (wparam == PBT_APMSUSPEND) {
			display_off_ = true;
		}
		if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
			display_off_ = false;
		}
		update_policy();
		return TRUE;
	case WM_QUERYENDSESSION:
		return TRUE;
	case WM_ENDSESSION:
		if (wparam) {
			closing_ = true;
			PostQuitMessage(0);
		}
		return 0;
	case WM_CLOSE:
		closing_ = true;
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(window_, message, wparam, lparam);
	}
}

LRESULT CALLBACK App::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
	App *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
		app->window_ = window;
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
	}
	if (!app) {
		return DefWindowProcW(window, message, wparam, lparam);
	}
	try {
		return app->dispatch(message, wparam, lparam);
	} catch (const std::exception &error) {
		app->report_error(error.what(),
		                  message == WM_TIMER || message == message_policy || message == WM_SETTINGCHANGE);
	} catch (...) {
		app->report_error("Unexpected error in the window procedure", true);
	}
	return 0;
}
}
