#pragma once

#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace snow
{
struct WindowTraits {
	std::wstring_view class_name;
	bool popup = false, full_caption = false, resizable = false;
	bool tool = false, no_activate = false, app_window = false, owned = false;
};

inline bool is_native_popup(const WindowTraits &w) noexcept
{
	const auto n = w.class_name;
	if (n == L"tooltips_class32" || n == L"#32768" || n == L"SysShadow" ||
	    n == L"NotifyIconOverflowWindow" ||
	    n == L"TopLevelWindowForOverflowXamlIsland" ||
	    n == L"XamlExplorerHostIslandWindow" || n == L"TaskListThumbnailWnd" ||
	    n == L"MultitaskingViewFrame" ||
	    n == L"XamlExplorerHostIslandWindow_Win32" ||
	    (n == L"Windows.UI.Core.CoreWindow" && !w.full_caption &&
	     !w.app_window)) {
		return true;
	}
	// Covers Chromium status bubbles and third-party tray tooltips too.
	// Neither topmost nor a known class name is required. Thin borders are OK.
	return w.popup && !w.full_caption && !w.resizable && !w.app_window &&
	       (w.tool || w.no_activate || w.owned);
}

inline bool is_browser_status_bubble(const WindowTraits &w, Rect bubble,
                                     Rect owner, float scale) noexcept
{
	if (!w.class_name.starts_with(L"Chrome_WidgetWin_") ||
	    !is_native_popup(w) || w.full_caption || w.resizable ||
	    !bubble.valid() || !owner.valid() || !std::isfinite(scale) ||
	    scale <= 0) {
		return false;
	}
	const float edge = std::max(2.0f, 12 * scale);
	return bubble.height() <= 48 * scale &&
	       std::abs(bubble.left - owner.left) <= edge &&
	       std::abs(bubble.bottom - owner.bottom) <= edge &&
	       bubble.right <= owner.right + edge;
}

inline bool taskbar_body_on_screen(Rect body, Rect monitor,
                                   float scale) noexcept
{
	const Rect visible = intersect(body, monitor);
	return visible.valid() && visible.height() >= 6 * scale &&
	       body.width() > body.height();
}

inline bool fullscreen_client(const WindowTraits &w, bool maximized,
                              Rect client, Rect monitor) noexcept
{
	// Client coordinates exclude invisible resize borders. A normal maximized
	// captioned window must not suppress taskbar snow, including auto-hide
	// setups.
	return !is_native_popup(w) && !w.tool && !w.no_activate &&
	       !(maximized && w.full_caption) && client.valid() &&
	       monitor.valid() && client.left <= monitor.left &&
	       client.top <= monitor.top && client.right >= monitor.right &&
	       client.bottom >= monitor.bottom;
}
}
