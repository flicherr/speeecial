#pragma once

#include "core.hpp"

namespace snow
{
struct PopupStackEntry {
	std::uintptr_t window{};
	Rect bounds;
	bool popup = false;
};

// Entries must follow LIVE front-to-back order, excluding our overlays,
// hidden/minimized windows and the desktop/taskbar background. A popup below
// an application cannot be an anchor for a monitor-sized snow window: doing
// so would hide the entire scene behind that application.
inline std::uintptr_t popup_anchor(std::span<const PopupStackEntry> stack,
                                   Rect monitor) noexcept
{
	std::uintptr_t anchor = 0;
	for (const auto &w : stack) {
		if (!w.bounds.valid() || !w.bounds.intersects(monitor)) {
			continue;
		}
		if (!w.popup) {
			break;
		}
		anchor = w.window;
	}
	return anchor;
}
}
