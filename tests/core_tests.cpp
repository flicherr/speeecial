#include "core.hpp"
#include "localization.hpp"
#include "popup_order.hpp"
#include "scene.hpp"
#include "window_inventory.hpp"
#include "window_policy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
using namespace snow;

void require(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

float total(const SnowBank &bank)
{
	float sum = 0;
	for (float h : bank.heights()) {
		sum += h;
	}
	return sum;
}

SurfaceSnapshot window(SurfaceId id = 1, float top = 100, int z = 5)
{
	return {id, {40, top, 280, top + 100}, 1, z, SurfaceKind::window, SurfaceState::visible};
}

SurfaceSnapshot floor_surface()
{
	return {100, {0, 280, 320, 320}, 1, 0, SurfaceKind::taskbar, SurfaceState::visible};
}

void seed(Landscape &world, SurfaceId id, float amount = 5)
{
	const auto *bank = world.find(id);
	require(bank != nullptr, "Bank missing from setup");
	const float width = bank->snow.width();
	for (float x = 0; x <= width; x += 4) {
		world.deposit({id, x, 0, 0}, amount);
	}
}

void sync(Landscape &world, std::vector<SurfaceSnapshot> surfaces, const Settings &settings = {},
          std::vector<Blocker> blockers = {}, float elapsed = 0.1f, bool animate = true)
{
	world.synchronize(surfaces, std::move(blockers), settings, elapsed, animate);
}

void transient_window_roles()
{
	// These traits deliberately do NOT include TOPMOST: non-topmost UI must work.
	require(is_native_popup({L"tooltips_class32", true}), "Standard tooltip");
	require(is_native_popup({L"TTrayTip", true, false, false, true}), "Unknown bordered tray tooltip");
	require(is_native_popup({L"Chrome_WidgetWin_1", true, false, false, false, true, false, true}),
	        "Chromium status bubble is native UI, not a rectangular blocker");
	require(is_native_popup({L"MiniBinTooltip", true, false, false, false, false, false, true}),
	        "Owned border-only popup without topmost");
	require(is_native_popup({L"MultitaskingViewFrame", true}), "Alt-Tab is not a fullscreen game");
	require(!is_native_popup({L"Chrome_WidgetWin_1", true, true, true, false, false, true}),
	        "Normal browser remains an application window");
	require(!is_native_popup({L"Chrome_WidgetWin_1", true, false, false, false, false, true}),
	        "Borderless app window remains an application");
	require(!is_native_popup({L"#32770", true, true, false, false, false, false, true}),
	        "Captioned owned dialog still supports snow");
	require(!is_native_popup({L"Game", true}), "Borderless unowned game remains eligible for fullscreen pause");
	require(is_native_popup({L"ControlCenterWindow", true, false, false, false, false, true}),
	        "Quick Settings stays native UI even with an app-window style");
	require(is_shell_focus_proxy(L"ForegroundStaging") && !is_native_popup({L"ForegroundStaging", true}),
	        "Shell focus proxy must be skipped, not used as a popup Z-order anchor");
	require(!is_shell_focus_proxy(L"Game") && !is_shell_focus_proxy(L"Chrome_WidgetWin_1"),
	        "Ordinary application windows are not shell focus proxies");
}

void window_inventory_recovery()
{
	std::vector<WindowOrder> order{{10, true}, {20, true}, {30, false}, {40, false}};
	// A known taskbar omitted during a shell transition must precede applications.
	require(recover_window(order, {25, true}, 0, 30) && order[2].window == 25,
	        "Missing topmost taskbar remains above normal applications");
	require(!recover_window(order, {25, true}) && !recover_window(order, {}) && order.size() == 5,
	        "Reconciliation rejects duplicate and null handles");
	// Use live neighbours, even when ranks from the preceding scan were different.
	require(recover_window(order, {15, true}, 10, 20) && order[1].window == 15,
	        "Recovered popup follows the current order");
	require(recover_window(order, {35, false}, 30, 0) && order[5].window == 35,
	        "A missing ordinary window follows its current upper neighbour");
	require(recover_window(order, {45, false}, 0, 0) && order.back().window == 45,
	        "An unanchored normal window is not raised above other applications");
	// A taskbar demoted by Explorer must not inherit its previous topmost rank.
	std::vector<WindowOrder> fullscreen{{10, true}, {30, false}};
	require(recover_window(fullscreen, {25, false}, 10, 0) && fullscreen.back().window == 25,
	        "A demoted taskbar stays below fullscreen content");
}

void incomplete_inventory_preserves_banks()
{
	Settings settings;
	settings.melt = false;
	Landscape world;
	auto app = window();
	auto bar = floor_surface();
	sync(world, {app, bar}, settings);
	seed(world, app.id, 20);
	seed(world, bar.id, 20);
	const float app_snow = total(world.find(app.id)->snow);
	const float bar_snow = total(world.find(bar.id)->snow);
	for (int cycle = 0; cycle < 120; ++cycle) {
		// The foreground/hover HWND can still be a live taskbar absent from
		// enumeration. Rapid flyout changes can omit an ordinary support too.
		std::vector<WindowOrder> order{{200, true}};
		if (cycle % 2 == 0) {
			order.push_back({bar.id, true});
		}
		if (cycle % 3 == 0) {
			order.push_back({app.id, false});
		}
		order.push_back({300, false});
		recover_window(order, {bar.id, true}, 200, app.id);
		recover_window(order, {app.id, false}, bar.id, 300);
		std::vector<SurfaceSnapshot> surfaces;
		std::vector<Blocker> blockers;
		for (std::size_t i = 0; i < order.size(); ++i) {
			const int rank = static_cast<int>(i);
			if (order[i].window == 200) {
				blockers.push_back({{0, 0, 320, 320}, rank, false, true});
			} else if (order[i].window == bar.id || order[i].window == app.id) {
				auto support = order[i].window == bar.id ? bar : app;
				support.z_order = rank;
				surfaces.push_back(support); // Direct HWND queries still report visible.
				blockers.push_back({support.rect, rank});
			}
		}
		sync(world, surfaces, settings, blockers, 0.016f);
		require(world.find(app.id)->enabled(settings) && !world.find(app.id)->visible_regions.empty() &&
		            world.find(bar.id)->enabled(settings) && !world.find(bar.id)->visible_regions.empty(),
		        "Incomplete inventories must not flicker either kind of bank");
		require(total(world.find(app.id)->snow) == app_snow && total(world.find(bar.id)->snow) == bar_snow &&
		            world.emissions().empty(),
		        "Reconciliation preserves accumulated snow without spurious avalanches");
	}
	// Recovery preserves membership, not visibility: real hiding still wins.
	bar.state = SurfaceState::hidden;
	app.state = SurfaceState::suspended;
	sync(world, {app, bar}, settings);
	require(!world.find(app.id)->enabled(settings) && !world.find(bar.id)->enabled(settings),
	        "Actual cloak/visibility state hides recovered surfaces without a grace timer");
	bar.state = app.state = SurfaceState::visible;
	sync(world, {app, bar}, settings);
	require(total(world.find(app.id)->snow) == app_snow && total(world.find(bar.id)->snow) == bar_snow,
	        "Restoring visibility retains both profiles");
}

void popup_order_stays_above_applications()
{
	const Rect monitor{0, 0, 1920, 1080}, app{0, 0, 1920, 1040}, tip{100, 990, 300, 1030};
	// A real tooltip is in front, another visible popup is behind the focused
	// app. The old "last eligible HWND" rule chose 20 and hid ALL snow.
	std::vector<PopupStackEntry> stack{{10, tip, true}, {1, app, false}, {20, tip, true}};
	require(popup_anchor(stack, monitor) == 10, "Do not follow an obscured tooltip below the active app");
	for (unsigned icon = 0; icon < 100; ++icon) {
		const std::uintptr_t current = icon % 2 ? 20 : 10, previous = icon % 2 ? 10 : 20;
		stack = {{current, tip, true}, {1, app, false}, {previous, tip, true}};
		require(popup_anchor(stack, monitor) == current, "Alternating reused tooltip HWNDs must use live order");
	}
	stack = {{1, app, false}, {20, tip, true}};
	require(popup_anchor(stack, monitor) == 0, "No demotion when only background popups remain");
	stack = {{10, tip, true}, {11, tip, true}, {1, app, false}, {20, tip, true}};
	require(popup_anchor(stack, monitor) == 11, "Keep all foreground popups above snow");
	// A normal non-topmost tooltip above its owner stays supported; band flags
	// do not determine whether the source is a safe ordering anchor.
	stack = {{30, {0, 1020, 500, 1040}, true}, {1, app, false}};
	require(popup_anchor(stack, monitor) == 30, "Foreground non-topmost tooltip remains supported");
	require(popup_anchor({}, monitor) == 0, "Dismissal restores the default layer");

	// Every permutation must place an anchor ahead of every intersecting app.
	std::array<unsigned, 5> permutation{0, 1, 2, 3, 4};
	const std::array<PopupStackEntry, 5> windows{
	    PopupStackEntry{1, app, false}, {2, app, false}, {10, tip, true}, {20, tip, true}, {30, tip, true}};
	do {
		stack.clear();
		for (auto n : permutation) {
			stack.push_back(windows[n]);
		}
		const auto anchor = popup_anchor(stack, monitor);
		if (!anchor) {
			require(!stack.front().popup, "A frontmost tooltip must remain available");
			continue;
		}
		const auto selected =
		    std::find_if(stack.begin(), stack.end(), [&](const auto &w) { return w.window == anchor; });
		require(selected != stack.end() && selected->popup, "Anchor must refer to a popup in the current stack");
		require(std::all_of(stack.begin(), selected, [](const auto &w) { return w.popup; }),
		        "No application may precede the chosen anchor");
	} while (std::next_permutation(permutation.begin(), permutation.end()));
}

void popup_order_is_per_monitor()
{
	const Rect left{-1280, 0, 0, 1024}, right{0, 0, 1920, 1080};
	std::vector<PopupStackEntry> stack{{10, {-300, 950, -50, 1000}, true},
	                                   {1, left, false},
	                                   {20, {100, 1000, 400, 1040}, true},
	                                   {2, right, false},
	                                   {30, {-500, 950, -350, 1000}, true}};
	require(popup_anchor(stack, left) == 10, "Negative-coordinate monitor keeps its own front tooltip");
	require(popup_anchor(stack, right) == 20, "An app on another monitor must not reject a local popup");
	stack.insert(stack.begin(), {3, {-500, 0, 500, 800}, false});
	require(popup_anchor(stack, left) == 0 && popup_anchor(stack, right) == 0,
	        "An application spanning both monitors fences both snow layers");
	stack = {{40, {100, 100, 100, 100}, true}, {41, {1920, 0, 2020, 100}, true}, {2, right, false}};
	require(popup_anchor(stack, right) == 0, "Zero-size and off-monitor popups cannot move a snow layer");
}

void settings_and_limits()
{
	Settings s;
	require(s.flake_size == 6,
	        "Default snowflake size preserves the original distribution");
	s.flake_size = -10;
	s.normalize();
	require(s.flake_size == 2, "Snowflake size lower limit");
	s.flake_size = 100;
	s.normalize();
	require(s.flake_size == 12, "Snowflake size upper limit");
	require(s.theme == Theme::system, "System theme must be the default");
	require(s.language == Language::system,
	        "System language must be the default");
	for (bool system_russian : {false, true}) {
		require(resolve_language(Language::system, system_russian) ==
		            (system_russian ? Language::russian : Language::english),
		        "System language chooses Russian or falls back to English");
		for (Language language : {Language::russian, Language::english}) {
			require(resolve_language(language, system_russian) == language,
			        "Manual language must override the system preference");
		}
	}
	for (const auto &entry : translations) {
		require(entry.russian && entry.russian[0] && entry.english &&
		            entry.english[0],
		        "Every menu string must have both translations");
	}
	require(std::wstring(localized(Text::snowfall, Language::english)) ==
	                L"Snowfall" &&
	            std::wstring(localized(Text::snowfall, Language::russian)) ==
	                L"Снегопад",
	        "The selected language must choose the matching menu text");
	for (Language language :
	     {Language::system, Language::russian, Language::english}) {
		s.language = language;
		s.normalize();
		require(s.language == language,
		        "Language selection must survive normalization");
	}
	s.language = static_cast<Language>(999);
	s.normalize();
	require(s.language == Language::system,
	        "Invalid language falls back to system");
	s.density = static_cast<Density>(-9);
	s.wind = static_cast<Wind>(999);
	s.layer = static_cast<Layer>(999);
	s.theme = static_cast<Theme>(999);
	s.height = -1;
	s.fps = 999;
	s.normalize();
	require(s.density == Density::normal && s.wind == Wind::gentle && s.layer == Layer::front && s.height == 22 &&
	            s.fps == 30,
	        "Invalid settings");
	require(s.theme == Theme::system, "Invalid theme must fall back to system");
	for (Theme theme : {Theme::system, Theme::light, Theme::dark}) {
		s.theme = theme;
		s.normalize();
		require(s.theme == theme, "Explicit theme selection must be preserved");
	}
	for (int rate : {20, 30, 60, 144, 240}) {
		s.fps = rate;
		s.normalize();
		require(s.fps == rate,
		        "Supported FPS must survive settings normalization");
	}
	for (std::size_t n = 1; n < 100; ++n) {
		require(particles_per_monitor(Density::heavy, n) * n <= max_particles, "Global snow budget");
	}
	require(particles_per_monitor(Density::off, 1) == 0, "Off must create no flakes");
}

void snowflake_size_and_accumulation()
{
	for (float dpi : {1.0f, 1.5f, 2.0f}) {
		Simulation sim(42);
		sim.configure({0, 0, 320, 320}, dpi, 50, 8);
		const auto original = std::vector<Particle>(sim.particles().begin(),
		                                            sim.particles().end());
		const auto *storage = sim.particles().data();
		for (int mean : {2, 12, 6}) {
			sim.set_flake_size(mean);
			require(sim.particles().data() == storage &&
			            sim.particles().size() == original.size(),
			        "Size adjustment must keep particle count and storage");
			for (std::size_t i = 0; i < original.size(); ++i) {
				const auto &p = sim.particles()[i];
				require(std::abs(p.size - original[i].size * mean / 6.0f) <
				            0.0001f,
				        "Existing snowflakes must immediately follow the size "
				        "setting");
				require(p.x == original[i].x && p.y == original[i].y &&
				            p.speed == original[i].speed,
				        "Resizing snowflakes must not restart their flight");
				require(
					p.size >= mean * dpi * 0.5f - 0.0001f &&
						p.size <= mean * dpi * 1.5f + 0.0001f,
					"Size distribution must respect DPI and the selected mean");
			}
		}
		sim.set_flake_size(12);
		sim.configure({0, 0, 320, 320}, dpi, 60, 8);
		for (const auto &p : sim.particles()) {
			require(p.size >= 6 * dpi && p.size <= 18 * dpi,
			        "New particles must use the selected size after changing "
			        "density");
		}
	}
	const auto first_deposit = [](int mean) {
		Landscape world;
		Settings settings;
		settings.flake_size = mean;
		settings.wind = Wind::calm;
		sync(world, {floor_surface()}, settings);
		Simulation sim(42);
		sim.configure({0, 0, 320, 320}, 1, 1, 0);
		for (int frame = 0; frame < 1000; ++frame) {
			sim.update(1.0f / 30, settings, world, {});
			const float deposited = total(world.find(100)->snow);
			if (deposited > 0) {
				return deposited;
			}
		}
		throw std::runtime_error("Snowflake did not reach the taskbar");
	};
	const float normal = first_deposit(6);
	require(std::abs(first_deposit(12) / normal - 4) < 0.001f,
	        "Doubling the size must quadruple the snow deposited per hit");
	require(std::abs(first_deposit(2) / normal - 1.0f / 9) < 0.001f,
	        "Smaller snowflakes must build banks more slowly");
}

void browser_status_layer_policy()
{
	WindowTraits chrome{L"Chrome_WidgetWin_1",
	                    true,
	                    false,
	                    false,
	                    true,
	                    true,
	                    false,
	                    true,
	                    true};
	const Rect owner{100, 100, 1100, 800};
	require(is_browser_status_bubble(chrome, {100, 775, 700, 800}, owner, 1),
	        "Bottom-left Chrome URL text must stay below snow");
	require(is_browser_status_bubble(chrome, {650, 775, 1101, 800}, owner, 1),
	        "Chrome URL text on the right must stay below snow");
	require(
		is_browser_status_bubble(chrome, {100, 795, 700, 820}, owner, 1),
		"Chrome URL text shifted away from the cursor must stay below snow");
	auto dialog = chrome;
	dialog.input_transparent = false;
	require(!is_browser_status_bubble(dialog, {650, 775, 1101, 800}, owner, 1),
	        "An interactive popup in the same location keeps its priority");
	const Rect maximized{-8, -8, 1928, 1040};
	const Rect inset{55, 1010, 355, 1033};
	require(is_browser_status_bubble(chrome, inset, maximized, 1),
	        "A tab strip beside the web contents can inset the URL bubble");
	auto tooltip = chrome;
	tooltip.tool = false;
	require(!is_browser_status_bubble(tooltip, inset, maximized, 1),
	        "A Chromium tooltip beside the status bubble keeps its priority");
	require(!is_browser_status_bubble(chrome, {100, 200, 700, 225}, owner, 1),
	        "Chrome menus near the top remain native foreground UI");
	require(!is_browser_status_bubble(chrome, {100, 500, 700, 800}, owner, 1),
	        "Large browser panels are not status text");
	chrome.class_name = L"tooltips_class32";
	require(!is_browser_status_bubble(chrome, {100, 775, 700, 800}, owner, 1),
	        "Taskbar tooltips retain priority over snow");
	chrome.class_name = L"Chrome_WidgetWin_1";
	chrome.full_caption = true;
	require(!is_browser_status_bubble(chrome, {100, 775, 700, 800}, owner, 1),
	        "Browser application is not a status bubble");
}

void shell_taskbar_ordering()
{
	const Rect monitor{0, 0, 1920, 1080};
	// Shell taskbars participate even when the snapshot contains no tooltip yet.
	std::vector<PopupStackEntry> stack{{10, {0, 1040, 1920, 1080}, true}, {1, monitor, false}};
	require(popup_anchor(stack, monitor) == 10, "Snow must yield to the visible shell band before hover");
	stack.insert(stack.begin(), {20, {100, 1000, 400, 1040}, true});
	require(popup_anchor(stack, monitor) == 10, "A tooltip above its taskbar stays above the snow layer too");
	stack = {{1, monitor, false}, {10, {0, 1040, 1920, 1080}, true}};
	require(popup_anchor(stack, monitor) == 0, "A taskbar behind a fullscreen game must not drag all snow behind it");
}

void taskbar_geometry_visibility()
{
	const Rect monitor{-1920, 0, 0, 1080};
	require(taskbar_body_on_screen({-1920, 1040, 0, 1080}, monitor, 1), "Visible bottom taskbar");
	require(!taskbar_body_on_screen({-1920, 1078, 0, 1118}, monitor, 1),
	        "Auto-hide reveal sliver is not a visible support");
	require(!taskbar_body_on_screen({-1920, 1080, 0, 1120}, monitor, 1), "Offscreen taskbar must not retain a cap");
	require(taskbar_body_on_screen({-1920, 0, 0, 64}, monitor, 2), "High-DPI top taskbar");
	require(!taskbar_body_on_screen({-1920, 1070, 0, 1140}, monitor, 2), "Auto-hide threshold follows DPI");
	require(!taskbar_body_on_screen({-1920, 0, -1880, 1080}, monitor, 1),
	        "Vertical bar has no horizontal snow support");
}

void hidden_taskbar_keeps_profile_without_rendering()
{
	Settings settings;
	settings.pause_fullscreen = false;
	settings.melt = true;
	Landscape world;
	const auto bar = floor_surface();
	sync(world, {bar}, settings);
	seed(world, bar.id, 20);
	const float saved = total(world.find(bar.id)->snow);
	for (bool maximized : {false, true}) {
		sync(world, {bar}, settings, {{{0, 0, 320, 320}, -1, maximized}});
		const auto *bank = world.find(bar.id);
		require(!bank->enabled(settings) && bank->visible_regions.empty(),
		        "Covered taskbar must not leave floating snow in a game");
		require(!world.collide(160, 0, 160, 320, settings), "Invisible taskbar cannot catch falling snow");
		world.update(0.1f, settings);
		require(total(bank->snow) == saved && world.emissions().empty(),
		        "Hidden taskbar preserves its profile without melting or shedding");
	}
	// Two opaque windows together can hide the bar; a native tooltip cannot.
	sync(world, {bar}, settings, {{{0, 0, 160, 320}, -2}, {{160, 0, 320, 320}, -1}});
	require(!world.find(bar.id)->enabled(settings), "Union coverage hides the support");
	sync(world, {bar}, settings, {{{0, 0, 320, 320}, -1, false, true}});
	require(world.find(bar.id)->enabled(settings), "Native flyout must not remove the snow profile");
	// Ordinary maximization can include an eight-pixel resize border beyond rcWork.
	sync(world, {bar}, settings, {{{0, 0, 320, 288}, -1, true}});
	require(world.find(bar.id)->enabled(settings) && !world.find(bar.id)->visible_regions.empty(),
	        "Maximized work-area window retains the cap above a visible bar");
	require(world.collide(160, 0, 160, 320, settings).has_value(), "Work-area border must not starve the bar");
	auto raised_bar = bar;
	raised_bar.z_order = -2;
	sync(world, {raised_bar}, settings, {{{0, 0, 320, 320}, -1, true}});
	require(world.find(bar.id)->enabled(settings) && total(world.find(bar.id)->snow) == saved,
	        "Raising the taskbar above the fullscreen app restores existing snow");
	auto second = bar;
	second.id = 101;
	second.rect.left += 320;
	second.rect.right += 320;
	sync(world, {bar, second}, settings, {{{0, 0, 320, 320}, -1, true}});
	require(!world.find(bar.id)->enabled(settings) && world.find(second.id)->enabled(settings),
	        "A fullscreen app affects only the taskbar it actually covers");
	sync(world, {bar}, settings);
	require(world.find(bar.id)->enabled(settings) && total(world.find(bar.id)->snow) == saved,
	        "Closing the fullscreen app restores the old profile");
}

void fullscreen_foreground_geometry()
{
	const Rect monitor{-320, 0, 0, 320};
	const WindowTraits game{L"Game", true, false, false, false, false, true};
	require(fullscreen_client(game, false, monitor, monitor), "Borderless fullscreen client on negative-X monitor");
	require(fullscreen_client(game, true, monitor, monitor), "Borderless fullscreen may also have WS_MAXIMIZE");
	require(fullscreen_client(game, false, {-320, 0, 320, 320}, monitor), "Spanning fullscreen client");
	require(!fullscreen_client(game, false, {-320, 0, 0, 280}, monitor), "Work-area-only client is not fullscreen");
	require(!fullscreen_client(game, false, {-320, 1, 0, 320}, monitor),
	        "Outer resize border cannot fake client coverage");
	require(!fullscreen_client({L"App", false, true, true}, true, monitor, monitor),
	        "Ordinary maximized captioned app must preserve the taskbar cap");
	for (const auto name : {L"tooltips_class32", L"MultitaskingViewFrame", L"#32768", L"ControlCenterWindow",
	                        L"ForegroundStaging", L"Windows.UI.Core.CoreWindow"}) {
		require(!fullscreen_client({name, true}, false, monitor, monitor), "Shell popup is not fullscreen content");
	}
	require(!fullscreen_client({L"Overlay", true, false, false, true, true}, false, monitor, monitor),
	        "Nonactivating decorative window is not fullscreen content");
	require(!fullscreen_client(game, false, {}, monitor), "Invalid client geometry is rejected");
	require(fullscreen_client({L"Windows.UI.Core.CoreWindow", true, false, false, false, false, true}, false, monitor,
	                          monitor),
	        "A real fullscreen UWP app must not be excluded with shell CoreWindow flyouts");
}

void shell_flyout_focus_cycle()
{
	const Rect monitor{0, 0, 320, 320};
	const WindowTraits proxy{L"ForegroundStaging", true};
	const WindowTraits start{L"Windows.UI.Core.CoreWindow", true};
	const WindowTraits quick{L"ControlCenterWindow", true, false, false, false, false, true};
	const WindowTraits app{L"App", false, true, true};
	const WindowTraits game{L"Game", true, false, false, false, false, true};
	for (bool auto_pause : {false, true}) {
		Settings settings;
		settings.pause_fullscreen = auto_pause;
		Landscape world;
		const auto bar = floor_surface();
		sync(world, {bar}, settings);
		seed(world, bar.id, 20);
		const auto saved = total(world.find(bar.id)->snow);
		const auto revision = world.find(bar.id)->snow.revision();
		const auto focus = [&](const WindowTraits &w) {
			const auto cover =
			    fullscreen_client(w, w.full_caption, monitor, monitor) ? std::optional<Rect>{monitor} : std::nullopt;
			world.begin_foreground_transition(cover);
		};
		const auto check_bank = [&] {
			const auto *s = world.find(bar.id);
			require(s->enabled(settings) && !s->foreground_pending && !s->visible_regions.empty(),
			        "Visible taskbar keeps its snow without requiring application focus");
			require(total(s->snow) == saved && s->snow.revision() == revision && world.emissions().empty(),
			        "Shell focus changes preserve the bank without clearing or shedding it");
		};
		for (const auto flyout : {start, quick}) {
			for (bool close_with_mouse : {false, true}) {
				focus(proxy); // Opening by taskbar click can stage activation first.
				check_bank();
				focus(flyout);
				// A host rectangle may include transparent space outside the panel.
				sync(world, {bar}, settings, {{monitor, -1, false, is_native_popup(flyout)}});
				check_bank();
				focus(close_with_mouse ? proxy : app);
				for (int scan = 0; scan < 20; ++scan) {
					std::vector<Blocker> blockers;
					if (close_with_mouse && !is_shell_focus_proxy(proxy.class_name)) {
						blockers.push_back({monitor, -1});
					}
					sync(world, {bar}, settings, blockers);
					check_bank(); // Focus does not return to an app in the mouse branch.
				}
				focus(game);
				require(!world.find(bar.id)->enabled(settings), "Actual fullscreen focus still hides snow immediately");
				focus(flyout);
				check_bank(); // Shell focus cancels an unacknowledged fullscreen transition.
			}
		}
	}
}

void foreground_taskbar_transition()
{
	Settings settings;
	settings.pause_fullscreen = false;
	settings.melt = true;
	Landscape world;
	const auto bar = floor_surface();
	auto second = bar;
	second.id = 101;
	second.rect.left -= 320;
	second.rect.right -= 320;
	const auto cap = window();
	const Rect fullscreen{0, 0, 320, 320};
	// Explorer has not lowered the taskbar yet: its snapshot still says visible.
	const std::vector<Blocker> stale{{fullscreen, 10}};
	sync(world, {bar, second, cap}, settings, stale);
	seed(world, bar.id, 20);
	const auto saved = total(world.find(bar.id)->snow);
	const auto revision = world.find(bar.id)->snow.revision();
	for (int cycle = 0; cycle < 20; ++cycle) {
		require(world.begin_foreground_transition(fullscreen), "Foreground event immediately changes bank visibility");
		require(!world.find(bar.id)->enabled(settings) && world.find(bar.id)->visible_regions.empty(),
		        "Cap hides before any desktop snapshot or animation tick");
		require(world.find(second.id)->enabled(settings) && world.find(cap.id)->enabled(settings),
		        "Only the taskbar covered by the foreground fullscreen window is suppressed");
		for (int frame = 0; frame < 6; ++frame) {
			sync(world, {bar, second, cap}, settings, stale);
			require(world.find(bar.id)->foreground_pending && !world.find(bar.id)->enabled(settings),
			        "A stale snapshot must not resurrect a cap over the fullscreen app");
			require(!world.collide(10, 250, 10, 320, settings), "Pending hidden taskbar cannot catch snow");
			world.update(0.1f, settings);
		}
		require(!world.needs_melting(settings), "Only the seeded hidden bank has snow; it must not run the timer");
		sync(world, {bar, second, cap}, settings, {{fullscreen, -1}});
		require(!world.find(bar.id)->foreground_pending && !world.find(bar.id)->enabled(settings),
		        "Actual coverage acknowledges the transition without revealing the cap");
		// Same foreground, but the user reveals the taskbar. No new transition.
		sync(world, {bar, second, cap}, settings, stale);
		require(world.find(bar.id)->enabled(settings) && !world.find(bar.id)->visible_regions.empty(),
		        "Taskbar reveal after acknowledgement restores the cap without changing focus");
		require(world.begin_foreground_transition(fullscreen), "Start a second switch before shell reorder");
		require(world.begin_foreground_transition(std::nullopt) && world.find(bar.id)->enabled(settings),
		        "Returning to an ordinary window cancels an unacknowledged transition");
		require(total(world.find(bar.id)->snow) == saved && world.find(bar.id)->snow.revision() == revision &&
		            world.emissions().empty(),
		        "Repeated switches preserve the entire profile without shedding or melting");
	}
	world.begin_foreground_transition(fullscreen);
	auto created = bar;
	created.id = 102;
	sync(world, {bar, created}, settings, stale);
	require(world.find(created.id)->foreground_pending, "New support inherits the pending foreground coverage");
	sync(world, {bar, created}, settings, {{fullscreen, -1}});
	world.begin_foreground_transition(std::nullopt);
	require(!world.find(bar.id)->enabled(settings), "Clearing pending suppression does not override physical coverage");
}

void bank_bounds_resize_and_melt()
{
	SnowBank b;
	b.resize(100, 1);
	b.deposit(0, 5);
	b.deposit(100, 5);
	b.deposit(50, 8);
	require(b.height_at(0) == 0 && b.height_at(100) == 0, "Endpoints meet baseline");
	require(b.height_at(4) > 0 && b.height_at(96) > 0, "Endpoint deposits feed tapered shoulders");
	const float middle = b.height_at(50);
	b.resize(200, 1, 22, true);
	require(std::abs(b.height_at(50) - middle) < 0.001f,
	        "Resize must preserve snow at its original position");
	require(b.height_at(150) == 0, "Newly exposed width must be clean");
	b.resize(400, 2, 22, true);
	require(std::abs(b.height_at(100) - middle * 2) < 0.01f,
	        "DPI must scale bank height");
	for (int i = 0; i < 1000; ++i) {
		b.deposit(200, 10);
	}
	for (float h : b.heights()) {
		require(h <= 44 && h >= 0, "Bank height cap");
	}
	const float sum = total(b);
	b.deposit(-1, 100);
	b.deposit(500, 100);
	b.deposit(std::numeric_limits<float>::quiet_NaN(), 100);
	require(sum == total(b), "Invalid coordinates must be ignored");
	b.melt(0.1f);
	require(total(b) < sum, "Melting");
	b.retain(0.5f);
	require(total(b) < sum * 0.51f, "Partial shedding");
	b.clear();
	require(b.empty(), "Clear");
}

void curved_bank_shoulders()
{
	for (float dpi : {1.0f, 1.5f, 2.0f, 3.0f}) {
		for (float width : {12.0f, 180.0f, 2051.0f, 5000.0f}) {
			SnowBank bank;
			bank.resize(width * dpi, dpi, 22);
			for (float x = 0; x <= bank.width(); x += dpi) {
				bank.deposit(x, 1000);
			}
			require(bank.height_at(0) == 0 && bank.height_at(bank.width()) == 0, "No vertical end faces");
			const float shoulder = std::min(bank.width() / 2, 22 * dpi * 1.6f);
			float previous = 0;
			for (int i = 1; i <= 100; ++i) {
				const float x = shoulder * static_cast<float>(i) / 100;
				const float h = bank.height_at(x);
				require(h >= previous && h <= bank.max_height(), "Shoulder must rise smoothly without overshoot");
				require(std::abs(h - bank.height_at(bank.width() - x)) < 0.005f, "Symmetric shoulders");
				previous = h;
			}
			require(bank.height_at(shoulder * 0.001f) < bank.max_height() * 0.00001f, "Horizontal baseline tangent");
			const float h = bank.height_at(shoulder / 2);
			for (int i = 0; i < 10; ++i) {
				bank.resize(width * dpi * 2, dpi * 2, 22, true);
				bank.resize(width * dpi, dpi, 22, true);
			}
			require(std::abs(bank.height_at(shoulder / 2) - h) < 0.005f, "DPI round trips must not double-taper");
		}
	}
	Landscape world;
	auto s = window();
	sync(world, {s});
	seed(world, s.id, 1000);
	const auto &bank = world.find(s.id)->snow;
	for (float local : {0.5f, 4.0f, 12.0f, 32.0f, 120.0f, 232.0f, 239.5f}) {
		const float x = s.rect.left + local;
		auto hit = world.collide(x, 0, x, 150, {});
		require(hit && std::abs(hit->y - (s.rect.top - bank.height_at(local))) < 0.001f,
		        "Collision surface must match curved rendered bank");
	}
}

void native_popups_do_not_cut_snow()
{
	Landscape world;
	auto bar = floor_surface();
	bar.z_order = 20;
	auto win = window();
	sync(world, {bar, win});
	seed(world, bar.id);
	seed(world, win.id);
	const auto bar_mask = world.find(bar.id)->mask_signature;
	const auto win_mask = world.find(win.id)->mask_signature;
	const auto profile = total(world.find(bar.id)->snow);
	const std::vector<Blocker> popups = {{{70, 220, 210, 290}, 1, false, true}, {{50, 60, 200, 110}, 0, false, true}};
	for (int i = 0; i < 20; ++i) {
		sync(world, {bar, win}, {}, i % 2 ? popups : std::vector<Blocker>{});
		require(world.find(bar.id)->mask_signature == bar_mask && world.find(win.id)->mask_signature == win_mask,
		        "Tooltip/flyout show-hide must not punch rectangular gaps in banks");
		require(total(world.find(bar.id)->snow) == profile, "Popup must preserve snow profile");
		require(!world.covered({100, 260, 105, 280}, bar.z_order), "Native popup is not a software coverage mask");
		auto hit = world.collide(100, 220, 100, 290, {});
		require(hit && hit->id == bar.id, "Snow still accumulates on taskbar beneath a flyout");
	}
	auto blockers = popups;
	blockers.push_back({{70, 240, 210, 300}, 2});
	sync(world, {bar, win}, {}, blockers);
	require(world.find(bar.id)->mask_signature != bar_mask, "An ordinary window must still clip the bank");
	require(!world.collide(100, 220, 100, 290, {}), "An ordinary foreground window must still block snow");
}

void nearest_surface_and_occlusion()
{
	Landscape world;
	auto high = window(1, 100, 4), low = window(2, 180, 5);
	Settings settings;
	sync(world, {low, high, floor_surface()});
	auto hit = world.collide(100, 0, 100, 270, settings);
	require(hit && hit->id == 1, "Nearest crossed edge wins, independent of map insertion");
	sync(world, {low, high, floor_surface()}, settings, {{{0, 0, 320, 130}, 0}});
	hit = world.collide(100, 0, 100, 270, settings);
	require(hit && hit->id == 2, "Covered upper edge must not catch snow");
	settings.windows = false;
	hit = world.collide(100, 0, 100, 310, settings);
	require(hit && hit->id == 100, "Window accumulation toggle");
	settings.accumulate = false;
	require(!world.collide(100, 0, 100, 310, settings), "Disabled banks cannot collide");
}

void swept_diagonal_collision()
{
	Landscape world;
	sync(world, {window()});
	Settings s;
	auto hit = world.collide(0, 0, 320, 200, s);
	require(hit && hit->x > 110 && hit->x < 130, "Swept diagonal must land at the interpolated X");
	require(!world.collide(0, 0, 20, 200, s), "A segment outside the bank must miss");
	require(!world.collide(100, 150, 100, 50, s), "Upward-moving debris must not land on a roof");
}

void minimize_uses_old_position_once()
{
	Landscape world;
	auto w = window();
	sync(world, {w, floor_surface()});
	seed(world, 1);
	auto minimized = w;
	minimized.state = SurfaceState::minimized;
	minimized.rect = {-32000, -32000, -31760, -31900};
	sync(world, {minimized, floor_surface()});
	require(!world.emissions().empty(), "Minimizing should emit snow");
	for (const auto &e : world.emissions()) {
		require(e.x >= 40 && e.x <= 280 && e.y < 100 && e.y > 0, "Emission must use last visible bounds");
	}
	world.consume_emissions();
	sync(world, {minimized, floor_surface()});
	require(world.emissions().empty(), "Duplicate minimize must not emit twice");
}

void close_and_duplicate_events()
{
	Landscape world;
	auto w = window();
	sync(world, {w});
	seed(world, 1);
	w.state = SurfaceState::closed;
	w.rect = {};
	sync(world, {w});
	require(!world.find(1) && !world.emissions().empty(), "Closed window bank removal");
	world.consume_emissions();
	sync(world, {w});
	require(world.emissions().empty(), "Duplicate close must be harmless");
}

void hidden_desktop_preserves_snow()
{
	Landscape world;
	auto w = window();
	sync(world, {w});
	seed(world, 1);
	const float sum = total(world.find(1)->snow);
	w.state = SurfaceState::suspended;
	sync(world, {w});
	require(world.emissions().empty() && total(world.find(1)->snow) == sum, "Cloaking is not minimization");
	Settings melt;
	melt.melt = true;
	world.update(0.1f, melt);
	require(total(world.find(1)->snow) == sum, "Hidden desktop profiles must remain unchanged");
	w.state = SurfaceState::visible;
	sync(world, {w});
	require(total(world.find(1)->snow) == sum, "Restore hidden bank");
	w.state = SurfaceState::suspended;
	sync(world, {w});
	w.state = SurfaceState::closed;
	sync(world, {w});
	require(world.emissions().empty(), "Closing on an inactive desktop must not create foreground snow");
}

void hide_before_close_and_shell_restart()
{
	Landscape world;
	auto w = window();
	const Blocker partial{{40, 0, 160, 150}, 0};
	const Blocker behind{{40, 0, 280, 150}, 6};
	sync(world, {w}, {}, {partial, behind});
	seed(world, 1);
	w.state = SurfaceState::hidden;
	w.z_order = 100;
	sync(world, {w}, {}, {partial, behind}, 0.6f);
	require(!world.emissions().empty(), "Hiding to the tray must release snow");
	for (const auto &e : world.emissions()) {
		require(e.x >= 160, "Collapse must use the last visible window order");
	}
	world.consume_emissions();
	sync(world, {w}, {}, {partial, behind});
	require(world.emissions().empty(),
	        "A window staying hidden cannot emit twice");
	w.state = SurfaceState::closed;
	sync(world, {w});
	require(world.emissions().empty(),
	        "Closing a hidden window cannot emit twice");

	auto bar = floor_surface();
	sync(world, {bar});
	seed(world, bar.id);
	bar.state = SurfaceState::closed;
	sync(world, {bar});
	require(world.emissions().empty(), "Explorer restart must not create a taskbar avalanche");
}

void overlaps_clip_bursts_and_union()
{
	Landscape world;
	auto w = window();
	Settings s;
	const std::vector<Blocker> blockers = {{{40, 0, 160, 150}, 0}};
	sync(world, {w}, s, blockers);
	seed(world, 1);
	w.state = SurfaceState::closed;
	sync(world, {w}, s, blockers);
	require(!world.emissions().empty(), "Uncovered section must still collapse");
	for (auto e : world.emissions()) {
		require(e.x >= 160, "Covered part of a bank must not burst through a foreground window");
	}
	sync(world, {}, s, {{{0, 0, 50, 100}, 0}, {{50, 0, 100, 100}, 1}});
	require(world.covered({0, 0, 100, 100}, 3), "Union of two blockers must cover a bank");
	require(!world.covered({0, 0, 110, 100}, 3), "Visible remainder must not be dropped");
	require(!world.occluded({0, 0, 100, 100}, 0), "Equal/lower Z cannot occlude a surface");
}

void move_resize_and_shake()
{
	Landscape world;
	Settings s;
	s.shake = false;
	auto w = window();
	sync(world, {w}, s);
	seed(world, 1);
	const float h = world.find(1)->snow.height_at(120);
	w.rect.left += 100;
	w.rect.right += 100;
	sync(world, {w}, s);
	require(world.emissions().empty(), "Rigid move without shedding");
	require(world.find(1)->snow.height_at(120) == h, "Moving should preserve the profile");
	w.rect.bottom += 100;
	sync(world, {w}, s);
	require(world.emissions().empty() &&
	            world.find(1)->snow.height_at(120) == h,
	        "Changing only window height must leave the roof snow intact");
	w.rect.right += 240;
	sync(world, {w}, s);
	require(std::abs(world.find(1)->snow.height_at(120) - h) < 0.01f,
	        "Window growth must not stretch snow");
	require(world.find(1)->snow.height_at(360) == 0,
	        "Window growth exposes a clean span");
	s.shake = true;
	w.rect.left += 80;
	w.rect.right += 80;
	sync(world, {w}, s, {}, 0.05f);
	require(!world.emissions().empty(), "Fast motion should shed snow");
	world.consume_emissions();
	w.rect.left += 80;
	w.rect.right += 80;
	sync(world, {w}, s, {}, 0.05f);
	require(world.emissions().empty(), "Shedding cooldown");
}

void resize_edges_crop_and_extend()
{
	for (float dpi : {1.0f, 1.5f, 2.0f}) {
		for (int edge : {-1, 0, 1}) {
			Landscape world;
			Settings settings;
			// Resize must not depend on the shake setting.
			settings.shake = false;
			auto w = window();
			w.rect = {40 * dpi, 100 * dpi, 520 * dpi, 240 * dpi};
			w.scale = dpi;
			const auto original = w;
			sync(world, {w}, settings);
			seed(world, w.id);
			world.deposit({w.id, 240 * dpi, 0, 0}, 4);
			const float h = world.find(w.id)->snow.height_at(240 * dpi);
			if (edge <= 0) {
				w.rect.left += 80 * dpi;
			}
			if (edge >= 0) {
				w.rect.right -= 80 * dpi;
			}
			sync(world, {w}, settings);
			const float x = original.rect.left + 240 * dpi - w.rect.left;
			require(std::abs(world.find(w.id)->snow.height_at(x) - h) < 0.001f,
			        "Shrinking either edge must preserve the overlapping snow "
			        "profile");
			require(!world.emissions().empty(),
			        "Unsupported snow must fall on resize");
			for (const auto &e : world.emissions()) {
				require(e.x < w.rect.left || e.x > w.rect.right,
				        "Only snow outside the remaining window span may fall");
				require(e.x >= original.rect.left &&
				            e.x <= original.rect.right &&
				            e.y < original.rect.top && e.source == w.id,
				        "Resize debris uses the old roof and source identity");
			}
			world.consume_emissions();
			sync(world, {w}, settings);
			require(world.emissions().empty(),
			        "Duplicate resize must not shed twice");
			w = original;
			sync(world, {w}, settings);
			require(world.emissions().empty(),
			        "Expanding either edge must not shed snow");
			require(std::abs(world.find(w.id)->snow.height_at(240 * dpi) - h) <
			            0.001f,
			        "Expansion must keep existing snow in place");
			if (edge <= 0) {
				require(world.find(w.id)->snow.height_at(40 * dpi) == 0,
				        "Left expansion must not restore cropped snow");
			}
			if (edge >= 0) {
				require(world.find(w.id)->snow.height_at(440 * dpi) == 0,
				        "Right expansion must not restore cropped snow");
			}
			const float local = (edge <= 0 ? 40 : 440) * dpi;
			world.deposit({w.id, local, 0, 0}, 2);
			require(world.find(w.id)->snow.height_at(local) > 0,
			        "Newly exposed span can accumulate fresh snow");
		}
	}
	for (int mode = 0; mode < 4; ++mode) {
		Landscape world;
		Settings settings;
		settings.collapse = mode != 1;
		settings.windows = mode != 2;
		auto w = window();
		const std::vector<Blocker> blockers =
			mode == 3 ? std::vector<Blocker>{{{0, 0, 400, 200}, 0}}
					  : std::vector<Blocker>{};
		sync(world, {w}, settings, blockers);
		seed(world, w.id);
		w.rect.right -= 80;
		sync(world, {w}, settings, blockers, 0.1f, mode != 0);
		require(
			world.emissions().empty(),
			"Resize respects pause, disabled collapse/windows and occlusion");
		w.rect.right += 80;
		sync(world, {w}, settings, blockers);
		require(world.find(w.id)->snow.height_at(200) == 0,
		        "Suppressed animation still discards unsupported snow");
	}
}

void pause_and_disabled_collapse()
{
	Landscape world;
	auto w = window();
	Settings s;
	sync(world, {w}, s);
	seed(world, 1);
	w.state = SurfaceState::minimized;
	sync(world, {w}, s, {}, 0.1f, false);
	require(world.emissions().empty(), "No delayed bursts while paused");
	w.state = SurfaceState::visible;
	sync(world, {w}, s);
	seed(world, 1);
	s.collapse = false;
	w.state = SurfaceState::minimized;
	sync(world, {w}, s);
	require(world.emissions().empty() && world.find(1)->snow.empty(), "Collapse toggle");
}

void fragments_fall_to_taskbar()
{
	Landscape world;
	auto w = window();
	sync(world, {w, floor_surface()});
	seed(world, 1);
	w.state = SurfaceState::closed;
	sync(world, {w, floor_surface()});
	Simulation sim(42);
	Settings settings;
	settings.density = Density::off;
	sim.configure({0, 0, 320, 320}, 1, 0, 64);
	for (auto e : world.emissions()) {
		sim.emit(e);
	}
	world.consume_emissions();
	require(sim.active_debris() > 0, "Closing with snowfall off still animates debris");
	Simulation *destinations[]{&sim};
	for (int i = 0; i < 240; ++i) {
		sim.update(1.0f / 30, settings, world, {});
		sim.transfer_outside(destinations);
	}
	require(total(world.find(100)->snow) > 0, "Collapsed snow must accumulate on the taskbar");
	require(sim.active_debris() == 0, "Fragments must retire");
}

void clear_snow_falls_offscreen()
{
	Landscape world;
	Settings settings;
	settings.density = Density::off;
	settings.wind = Wind::calm;
	settings.collapse = false;
	auto upper = window();
	auto lower = window(2, 200);
	auto hidden = window(3, 50);
	hidden.state = SurfaceState::suspended;
	sync(world, {upper, lower, hidden, floor_surface()}, settings);
	for (SurfaceId id : {1, 2, 3, 100}) {
		seed(world, id);
	}
	Simulation sim(42);
	sim.configure({-1024, 0, 1024, 320}, 1, 0, 256);
	sim.emit({100, 50, 0, -100, 5, 2, upper.id});
	sim.drop_debris();
	require(sim.active_debris() == 1 && sim.debris()[0].fall_through &&
	            sim.debris()[0].vy > 0,
	        "Clearing must keep existing debris and send it downwards");
	world.drop_snow(settings);
	bool upper_emitted = false, lower_emitted = false, bar_emitted = false;
	for (const auto &e : world.emissions()) {
		require(e.fall_through && e.vy > 0 && e.delay == 0,
		        "Cleared banks must fall immediately without landing");
		require(e.source != hidden.id,
		        "Clearing a hidden desktop must not emit foreground snow");
		upper_emitted |= e.source == upper.id;
		lower_emitted |= e.source == lower.id;
		bar_emitted |= e.source == 100;
		sim.emit(e);
	}
	require(upper_emitted && lower_emitted && bar_emitted,
	        "Manual clearing animates window and taskbar banks even with "
	        "collapse off");
	world.consume_emissions();
	const auto active = sim.active_debris();
	require(active > 1, "Clearing with snowfall off must still animate");
	world.drop_snow(settings);
	sim.drop_debris();
	require(world.emissions().empty() && sim.active_debris() == active,
	        "Repeated clearing must not duplicate or erase falling snow");
	Simulation *destinations[]{&sim};
	for (int frame = 0; frame < 240; ++frame) {
		sim.update(1.0f / 30, settings, world, {});
		sim.transfer_outside(destinations);
		for (const auto &[id, surface] : world.surfaces()) {
			(void)id;
			require(surface.snow.empty(),
			        "Cleared snow must not settle on windows or the taskbar");
		}
	}
	require(sim.active_debris() == 0, "Cleared snow must finish falling");
	for (const auto &p : sim.debris()) {
		if (p.generation != 0) {
			require(p.y > sim.bounds().bottom,
			        "Cleared fragments must leave through the bottom of the "
			        "screen");
		}
	}
	world.deposit({upper.id, 120, 0, 0}, 2);
	require(!world.find(upper.id)->snow.empty(),
	        "Fresh snow can accumulate after clearing");
	sync(world, {upper}, settings, {{{0, 0, 320, 320}, 0}});
	world.drop_snow(settings);
	require(world.emissions().empty() && world.find(upper.id)->snow.empty(),
	        "Covered snow clears without bursting through foreground windows");
}

void bounded_fragment_pool_and_source_immunity()
{
	Landscape world;
	sync(world, {window(), floor_surface()});
	Simulation sim(1);
	sim.configure({0, 0, 320, 320}, 1, 0, 16);
	for (int i = 0; i < 10000; ++i) {
		sim.emit({100, 95, 0, 20, 5, 1, 1});
	}
	require(sim.active_debris() == 16, "Fragment pool must remain bounded");
	sim.update(0.1f, Settings{}, world, {});
	require(world.find(1)->snow.empty() && sim.active_debris() == 16, "Fresh debris must not stick back to source");
	sim.clear_debris();
	require(sim.active_debris() == 0, "Clear fragments");
}

void fragments_cross_monitors()
{
	Landscape world;
	Simulation upper(1), lower(2);
	upper.configure({-200, -200, 0, 0}, 1, 0, 8);
	lower.configure({-200, 0, 0, 200}, 1.5f, 0, 8);
	Simulation *destinations[]{&upper, &lower};
	upper.emit({-100, -1, 10, 100, 5, 2, 42, 0, true});
	upper.update(0.03f, {}, world, {});
	lower.update(0.03f, {}, world, {});
	const auto crossing = upper.debris()[0];
	upper.transfer_outside(destinations);
	lower.transfer_outside(destinations);
	require(upper.active_debris() == 0 && lower.active_debris() == 1, "Fragment must cross the monitor seam once");
	const auto &landed = lower.debris()[0];
	require(
		landed.x == crossing.x && landed.y == crossing.y &&
			landed.age == crossing.age && landed.source == 42 &&
			landed.vy == crossing.vy && landed.fall_through,
		"Transfer must preserve position, age, velocity and source immunity");
	lower.clear_debris();
	upper.emit({-100, -1, 0, 200, 3, 1, 42});
	upper.update(0.1f, {}, world, {});
	Simulation *only_upper[]{&upper};
	upper.transfer_outside(only_upper);
	require(upper.active_debris() == 0 && lower.active_debris() == 0,
	        "Fragments leaving the desktop must retire; hidden monitors cannot receive them");
}

void resume_time_and_cursor()
{
	Landscape a_world, b_world;
	Simulation a(7), b(7);
	Settings s;
	a.configure({-1920, 0, 0, 1080}, 1, 20, 16);
	b.configure({-1920, 0, 0, 1080}, 1, 20, 16);
	a.update(3600, s, a_world, {});
	b.update(0.1f, s, b_world, {});
	for (std::size_t i = 0; i < a.particles().size(); ++i) {
		require(a.particles()[i].x == b.particles()[i].x && a.particles()[i].y == b.particles()[i].y,
		        "Resume dt clamp");
	}
	const auto p = a.particles()[0];
	s.cursor = true;
	Cursor c{p.x, p.y, 800, 200, true};
	a.update(0.03f, s, a_world, c);
	s.cursor = false;
	b.update(0.03f, s, b_world, c);
	require(a.particles()[0].x != b.particles()[0].x, "Cursor must influence nearby particles only when enabled");
	const float x = a.particles()[0].x;
	a.update(std::numeric_limits<float>::quiet_NaN(), s, a_world, {});
	require(a.particles()[0].x == x, "NaN dt must be ignored");
}

void stable_identity_and_limits()
{
	Landscape world;
	auto old = window(5);
	sync(world, {old});
	seed(world, 5);
	old.state = SurfaceState::closed;
	sync(world, {old, window(6)});
	require(!world.find(5) && world.find(6)->snow.empty(), "Reused handle with new identity must not inherit snow");
	std::vector<SurfaceSnapshot> many;
	for (std::size_t i = 0; i < max_surfaces + 20; ++i) {
		many.push_back(window(1000 + i));
	}
	sync(world, many);
	require(world.surfaces().size() <= max_surfaces, "Profile budget");
}

void nested_windows_receive_continuous_snow()
{
	Landscape world;
	Settings settings;
	settings.wind = Wind::calm;
	settings.shake = false;
	SurfaceSnapshot outer{1, {0, 40, 320, 265}, 1, 10, SurfaceKind::window, SurfaceState::visible};
	SurfaceSnapshot inner{2, {70, 150, 250, 235}, 1, 5, SurfaceKind::window, SurfaceState::visible};
	sync(world, {outer, inner, floor_surface()}, settings, {{outer.rect, 10}, {inner.rect, 5}});
	Simulation sim(123);
	// All initial flakes start ABOVE the enclosing window. This reproduces
	// starvation even if the old random startup distribution briefly hid it.
	sim.configure({0, -180, 320, -20}, 1, 450, 0);
	sim.configure({0, 0, 320, 320}, 1, 450, 0);
	for (int i = 0; i < 2700; ++i) {
		sim.update(1.0f / 30, settings, world, {});
	}
	require(total(world.find(1)->snow) > 20, "Outer edge must collect snow");
	require(total(world.find(2)->snow) > 600, "Nested foreground edge must not be starved");
	require(total(world.find(100)->snow) > 1000, "Windows above must not starve the taskbar");
}

void maximized_window_has_no_bank()
{
	Landscape world;
	Settings settings;
	auto w = window();
	sync(world, {w, floor_surface()});
	seed(world, 1);
	w.rect = {0, 0, 320, 280};
	w.accepts_snow = false;
	sync(world, {w, floor_surface()}, settings, {{w.rect, -1, true}});
	require(world.find(1)->snow.empty() && !world.find(1)->enabled(settings), "Maximize clears and disables the bank");
	require(world.emissions().empty(), "Maximize is not a close avalanche");
	const auto hit = world.collide(100, -10, 100, 300, settings);
	require(hit && hit->id == 100, "Maximized window must not intercept snow headed for taskbar");
	Simulation sim(11);
	sim.configure({0, -120, 320, -10}, 1, 200, 0);
	sim.configure({0, 0, 320, 320}, 1, 200, 0);
	for (int i = 0; i < 1200; ++i) {
		sim.update(1.0f / 30, settings, world, {});
	}
	require(total(world.find(100)->snow) > 20 && world.find(1)->snow.empty(),
	        "Taskbar snowfall with a maximized window");
	require(!world.find(100)->visible_regions.empty(), "Maximized body must not erase the taskbar cap");
	w.accepts_snow = true;
	w.rect = {40, 100, 280, 200};
	sync(world, {w, floor_surface()});
	require(world.find(1)->enabled(settings) && world.find(1)->snow.empty(), "Restore starts a fresh bank");
}

void clips_follow_geometry_without_changing_snow()
{
	Landscape world;
	Settings settings;
	settings.shake = false;
	auto w = window();
	const Rect blocker{90, 60, 170, 105};
	sync(world, {w}, settings, {{blocker, 0}});
	seed(world, 1);
	const auto revision = world.find(1)->snow.revision();
	const auto signature = world.find(1)->mask_signature;
	const auto *storage = world.find(1)->visible_regions.data();
	const auto original = world.find(1)->visible_regions;
	for (int frame = 0; frame < 600; ++frame) {
		const float dx = static_cast<float>(frame) * 0.5f, dy = static_cast<float>(frame) * 0.25f;
		auto moved = w;
		moved.rect.left += dx;
		moved.rect.right += dx;
		moved.rect.top += dy;
		moved.rect.bottom += dy;
		auto b = blocker;
		b.left += dx;
		b.right += dx;
		b.top += dy;
		b.bottom += dy;
		sync(world, {moved}, settings, {{b, 0}}, 1.0f / 60);
		const auto &surface = *world.find(1);
		require(surface.snow.revision() == revision, "Move-only frames must not dirty/repaint the snow profile");
		require(surface.mask_signature == signature && surface.visible_regions == original &&
		            surface.visible_regions.data() == storage,
		        "Rigid translation must reuse the relative clip mask without allocation");
		require(surface.snapshot.rect == moved.rect, "No accumulated positional lag in the model");
	}
}

void rectangular_clip_mask_matches_occlusion()
{
	Landscape world;
	auto w = window();
	sync(world, {w}, {}, {{{70, 70, 110, 105}, 0}, {{100, 90, 190, 105}, 1}, {{200, 0, 240, 100}, 2}});
	const auto &bank = *world.find(1);
	const auto strip = bank.snow_bounds();
	for (float x = 0.5f; x < strip.width(); x += 2) {
		for (float y = 0.5f; y < strip.height(); y += 2) {
			bool visible = false;
			for (auto r : bank.visible_regions) {
				visible |= r.contains(x, y);
			}
			const float gx = x + strip.left, gy = y + strip.top;
			require(visible == !world.occluded({gx, gy, gx + 0.1f, gy + 0.1f}, w.z_order),
			        "Compositor clip must match collision occlusion");
		}
	}
}

void avalanche_is_irregular_and_rotates()
{
	Landscape world;
	auto w = window();
	sync(world, {w});
	seed(world, 1, 12);
	w.state = SurfaceState::closed;
	sync(world, {w});
	const auto emissions = world.emissions();
	require(emissions.size() > 6, "Avalanche setup");
	float lo = 1000, hi = 0, slow = 1000, fast = -1000;
	for (const auto &e : emissions) {
		lo = std::min(lo, e.size);
		hi = std::max(hi, e.size);
		slow = std::min(slow, e.vx);
		fast = std::max(fast, e.vx);
	}
	require(hi - lo > 3 && fast - slow > 30, "Chunks must vary in size and velocity");
	Simulation sim(42);
	sim.configure({0, 0, 320, 600}, 1, 0, 128);
	for (auto e : emissions) {
		e.delay = 0;
		sim.emit(e);
	}
	const auto before = sim.debris()[0];
	sim.update(0.1f, {}, world, {});
	const auto after = sim.debris()[0];
	require(after.angle != before.angle && after.vy > before.vy, "Chunks must rotate and accelerate under gravity");
	require(sim.debris()[0].aspect != sim.debris()[1].aspect, "Independent chunk silhouettes");
}

void cursor_sweep_is_visible_and_can_lift_snow()
{
	Landscape world;
	Simulation a(7), b(7);
	Settings s;
	s.cursor = true;
	a.configure({0, 0, 1920, 1080}, 1, 1, 0);
	b.configure({0, 0, 1920, 1080}, 1, 1, 0);
	const auto p = a.particles()[0];
	a.update(0.05f, s, world, {p.x + 200, p.y, 8000, 0, true});
	b.update(0.05f, s, world, {});
	require(std::abs(a.particles()[0].x - b.particles()[0].x) > 15,
	        "Fast cursor sweep must affect flakes along the segment, not just its endpoint");
	Simulation up(7);
	up.configure({0, 0, 1920, 1080}, 1, 1, 0);
	up.update(0.05f, s, world, {p.x, p.y - 100, 0, -4000, true});
	require(up.particles()[0].y < p.y - 5, "Upward mouse motion must visibly lift nearby snow");
}

void deterministic_soak()
{
	Landscape world;
	Simulation sim(97);
	Settings s;
	s.wind = Wind::gusts;
	s.melt = true;
	s.cursor = true;
	sim.configure({-320, 0, 320, 360}, 1, 450, 128);
	std::vector<SurfaceSnapshot> surfaces{floor_surface()};
	for (int i = 0; i < 12; ++i) {
		auto w = window(200 + i, 60 + static_cast<float>(i) * 12, i + 2);
		w.rect.left -= 200;
		w.rect.right -= 80;
		surfaces.push_back(w);
	}
	sync(world, surfaces, s);
	const auto *particles = sim.particles().data();
	const auto *debris = sim.debris().data();
	Simulation *destinations[]{&sim};
	for (int frame = 0; frame < 2700; ++frame) {
		if (frame % 30 == 0) {
			auto &w = surfaces[1 + static_cast<std::size_t>(frame / 30) % 12];
			w.state = w.state == SurfaceState::visible ? SurfaceState::minimized : SurfaceState::visible;
			sync(world, surfaces, s, {}, 1.0f);
			for (auto e : world.emissions()) {
				sim.emit(e);
			}
			world.consume_emissions();
		}
		sim.update(1.0f / 30, s, world, {0, 120, 100, 0, true});
		sim.transfer_outside(destinations);
		world.update(1.0f / 30, s);
		for (auto p : sim.particles()) {
			require(std::isfinite(p.x) && std::isfinite(p.y), "Non-finite particle during soak");
		}
		for (const auto &[id, bank] : world.surfaces()) {
			(void)id;
			for (float h : bank.snow.heights()) {
				require(std::isfinite(h) && h >= 0 && h <= bank.snow.max_height(), "Bank invariant during soak");
			}
		}
	}
	require(particles == sim.particles().data() && debris == sim.debris().data(),
	        "Simulation tick must not reallocate fixed particle pools");
}
}

int main()
{
	const std::pair<const char *, void (*)()> tests[] = {
		{"settings_and_limits", settings_and_limits},
		{"snowflake_size_and_accumulation", snowflake_size_and_accumulation},
		{"transient_window_roles", transient_window_roles},
		{"window_inventory_recovery", window_inventory_recovery},
		{"incomplete_inventory_preserves_banks",
	     incomplete_inventory_preserves_banks},
		{"popup_order_stays_above_applications",
	     popup_order_stays_above_applications},
		{"popup_order_is_per_monitor", popup_order_is_per_monitor},
		{"browser_status_layer_policy", browser_status_layer_policy},
		{"shell_taskbar_ordering", shell_taskbar_ordering},
		{"taskbar_geometry_visibility", taskbar_geometry_visibility},
		{"hidden_taskbar_keeps_profile_without_rendering",
	     hidden_taskbar_keeps_profile_without_rendering},
		{"fullscreen_foreground_geometry", fullscreen_foreground_geometry},
		{"shell_flyout_focus_cycle", shell_flyout_focus_cycle},
		{"foreground_taskbar_transition", foreground_taskbar_transition},
		{"bank_bounds_resize_and_melt", bank_bounds_resize_and_melt},
		{"curved_bank_shoulders", curved_bank_shoulders},
		{"native_popups_do_not_cut_snow", native_popups_do_not_cut_snow},
		{"nearest_surface_and_occlusion", nearest_surface_and_occlusion},
		{"swept_diagonal_collision", swept_diagonal_collision},
		{"minimize_uses_old_position_once", minimize_uses_old_position_once},
		{"close_and_duplicate_events", close_and_duplicate_events},
		{"hidden_desktop_preserves_snow", hidden_desktop_preserves_snow},
		{"hide_before_close_and_shell_restart",
	     hide_before_close_and_shell_restart},
		{"overlaps_clip_bursts_and_union", overlaps_clip_bursts_and_union},
		{"move_resize_and_shake", move_resize_and_shake},
		{"resize_edges_crop_and_extend", resize_edges_crop_and_extend},
		{"pause_and_disabled_collapse", pause_and_disabled_collapse},
		{"fragments_fall_to_taskbar", fragments_fall_to_taskbar},
		{"clear_snow_falls_offscreen", clear_snow_falls_offscreen},
		{"bounded_fragment_pool_and_source_immunity",
	     bounded_fragment_pool_and_source_immunity},
		{"fragments_cross_monitors", fragments_cross_monitors},
		{"resume_time_and_cursor", resume_time_and_cursor},
		{"stable_identity_and_limits", stable_identity_and_limits},
		{"nested_windows_receive_continuous_snow",
	     nested_windows_receive_continuous_snow},
		{"maximized_window_has_no_bank", maximized_window_has_no_bank},
		{"clips_follow_geometry_without_changing_snow",
	     clips_follow_geometry_without_changing_snow},
		{"rectangular_clip_mask_matches_occlusion",
	     rectangular_clip_mask_matches_occlusion},
		{"avalanche_is_irregular_and_rotates",
	     avalanche_is_irregular_and_rotates},
		{"cursor_sweep_is_visible_and_can_lift_snow",
	     cursor_sweep_is_visible_and_can_lift_snow},
		{"deterministic_soak", deterministic_soak}};
	for (auto [name, test] : tests) {
		try {
			test();
			std::cout << "PASS " << name << '\n';
		} catch (const std::exception &e) {
			std::cerr << "FAIL " << name << ": " << e.what() << '\n';
			return 1;
		}
	}
	std::cout << "All " << std::size(tests) << " scenario groups passed.\n";
	return 0;
}
