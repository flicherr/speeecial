#pragma once

#include "core.hpp"
#include <map>
#include <optional>

namespace snow
{
enum class SurfaceKind { window, taskbar };
enum class SurfaceState {
	visible,
	hidden,
	suspended, // Other virtual desktop, DWM cloaking, or uncertain visibility.
	minimized,
	closed
};

struct SurfaceSnapshot {
	SurfaceId id{};
	Rect
		rect{}; // top is the bank baseline; left/right exclude rounded corners.
	float scale = 1;
	int z_order = 0;
	SurfaceKind kind = SurfaceKind::window;
	SurfaceState state = SurfaceState::visible;
	bool accepts_snow = true;
};

struct Blocker {
	Rect rect;
	int z_order{};
	bool maximized = false;
	bool native_popup =
		false; // DWM handles its alpha/rounded shape above the overlay.
};

struct SnowSurface {
	SurfaceSnapshot snapshot;
	SnowBank snow;
	std::uint64_t mask_signature = ~std::uint64_t{0}, seen = 0;
	std::vector<Rect>
		visible_regions; // Local to snow_bounds(); compositor clips.
	float last_shed = -10;
	float last_visible = -100;
	bool support_visible =
		true; // A taskbar HWND may exist underneath a fullscreen app.
	bool foreground_pending =
		false; // Hide immediately until the shell snapshot catches up.
	bool enabled(const Settings &settings) const noexcept;
	Rect snow_bounds() const noexcept;
};

struct Collision {
	SurfaceId id{};
	float x{}, y{}, t{};
};

class Landscape {
  public:
	void synchronize(std::span<const SurfaceSnapshot> surfaces,
	                 std::vector<Blocker> blockers, const Settings &settings,
	                 float elapsed, bool animate);
	void update(float dt, const Settings &settings);
	bool begin_foreground_transition(std::optional<Rect> fullscreen);
	std::optional<Collision> collide(float x0, float y0, float x1, float y1,
	                                 const Settings &settings,
	                                 SurfaceId ignore = 0) const noexcept;
	void deposit(const Collision &hit, float amount) noexcept;
	bool occluded(Rect rect, int before_z = 0x7fffffff,
	              Rect supporting_body = {}) const noexcept;
	bool covered(Rect rect, int before_z) const;

	std::span<const Blocker> blockers() const noexcept
	{
		return blockers_;
	}

	const std::map<SurfaceId, SnowSurface> &surfaces() const noexcept
	{
		return surfaces_;
	}

	const SnowSurface *find(SurfaceId id) const noexcept;

	std::span<const Emission> emissions() const noexcept
	{
		return emissions_;
	}

	void consume_emissions() noexcept
	{
		emissions_.clear();
	}

	void clear_snow() noexcept;
	bool needs_melting(const Settings &settings) const noexcept;

	std::uint64_t revision() const noexcept
	{
		return revision_;
	}

  private:
	bool foreground_covers(const SurfaceSnapshot &surface) const noexcept;
	void release(SnowSurface &surface, float fraction, float vx, float vy,
	             bool emit);
	void update_mask(SnowSurface &surface);
	std::map<SurfaceId, SnowSurface> surfaces_;
	std::vector<Blocker> blockers_;
	std::vector<Emission> emissions_;
	std::optional<Rect> foreground_cover_;
	std::uint64_t revision_ = 0;
	float clock_ = 0;
	std::mt19937 random_{0x434C554Du};
};
}
