#pragma once

#include "scene.hpp"
#include "window_inventory.hpp"
#include <memory>
#include <optional>
#include <string>
#include <windows.h>

namespace snow
{
inline constexpr UINT message_desktop = WM_APP + 4;
inline constexpr UINT message_foreground = WM_APP + 5;

struct Display {
	HMONITOR monitor{};
	RECT bounds{};
	float scale = 1;
};

Rect screen_rect(RECT rect) noexcept;
std::vector<Display> enumerate_displays();

// EnumWindows membership plus direct reconciliation of known live handles.
// No DWM/COM calls; safe for the renderer's short-lived ordering snapshot too.
std::vector<WindowOrder>
enumerate_window_order(std::span<const HWND> required = {});

bool on_battery() noexcept;

struct WindowTraits;
bool browser_status_bubble(HWND window, const WindowTraits &traits, Rect bounds,
                           float scale) noexcept;
std::optional<Rect> foreground_fullscreen_fast(HWND window) noexcept;

struct MotionSample {
	HWND window{};
	SurfaceId id{};
	DWORD process{};
	RECT insets{};
	float scale = 1;
	bool zoomed = false;
};

struct DesktopSnapshot {
	std::vector<SurfaceSnapshot> surfaces;
	std::vector<Blocker> blockers;
	std::vector<HWND> popups; // Eligible native UI; renderer revalidates live Z
	                          // order above applications.
	std::vector<HWND> taskbars; // Shell bands are ordering anchors even before
	                            // a tooltip appears.
	std::optional<GUID> desktop;
	bool desktop_changed = false;
	bool full_scan = false;
	std::optional<MotionSample> motion;
	std::optional<Rect> fullscreen_window;
};

class DesktopTracker {
  public:
	explicit DesktopTracker(HWND control);
	~DesktopTracker();
	DesktopTracker(const DesktopTracker &) = delete;
	DesktopTracker &operator=(const DesktopTracker &) = delete;
	std::optional<DesktopSnapshot> snapshot(bool force = false);
	bool dirty() const noexcept;
	bool moving() const noexcept;
	std::uint64_t full_scans() const noexcept;
	std::uint64_t geometry_samples() const noexcept;
	double last_scan_ms() const noexcept;
	double peak_scan_ms() const noexcept;
	std::string error() const;
	bool hooks_active() const noexcept;
	std::size_t tracked_windows() const noexcept;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
}
