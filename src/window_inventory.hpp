#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace snow
{
struct WindowOrder {
	std::uintptr_t window{};
	bool topmost = false;
};

// Recover a known live HWND omitted by enumeration. Prefer a current neighbour
// over an old numeric Z rank: ranks from different scans are not comparable.
inline bool recover_window(std::vector<WindowOrder> &order, WindowOrder window,
                           std::uintptr_t previous = 0, std::uintptr_t next = 0)
{
	if (!window.window ||
	    std::any_of(order.begin(), order.end(), [&](auto entry) {
			return entry.window == window.window;
		})) {
		return false;
	}
	const auto same_band = [&](auto entry) {
		return entry.topmost == window.topmost;
	};
	auto position = std::find_if(order.begin(), order.end(), [&](auto entry) {
		return entry.window == next && same_band(entry);
	});
	if (position == order.end()) {
		const auto upper =
			std::find_if(order.begin(), order.end(), [&](auto entry) {
				return entry.window == previous && same_band(entry);
			});
		if (upper != order.end()) {
			position = upper + 1;
		} else if (window.topmost) {
			position = std::find_if(order.begin(), order.end(),
			                        [](auto entry) { return !entry.topmost; });
		}
	}
	order.insert(position, window);
	return true;
}
}
