#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace snow
{
enum class Density : int { off, light, normal, heavy };
enum class Wind : int { calm, gentle, gusts };
enum class Layer : int { front, behind };
enum class Theme : int { system, light, dark };

struct Settings {
	Density density = Density::normal;
	Wind wind = Wind::gentle;
	Layer layer = Layer::front;
	Theme theme = Theme::system;
	int fps = 30, height = 22;
	bool accumulate = true, windows = true, collapse = true, shake = true;
	bool cursor = false, show_capture = true, melt = false;
	bool pause_fullscreen = true, pause_battery = false;
	void normalize() noexcept;
};

constexpr std::size_t max_particles = 4096, max_debris = 2048,
					  max_surfaces = 128;
std::size_t particles_per_monitor(Density density,
                                  std::size_t monitors) noexcept;

struct Rect {
	float left{}, top{}, right{}, bottom{};

	float width() const noexcept
	{
		return right - left;
	}

	float height() const noexcept
	{
		return bottom - top;
	}

	bool valid() const noexcept;
	bool contains(float x, float y) const noexcept;
	bool intersects(Rect other) const noexcept;
	bool operator==(const Rect &) const = default;
};

Rect intersect(Rect a, Rect b) noexcept;
using SurfaceId = std::uint64_t;

struct Particle {
	float x{}, y{}, speed{}, size{}, opacity{}, phase{}, sway{}, drift{},
		lift{};
	unsigned sprite{};
};

struct Debris {
	float x{}, y{}, vx{}, vy{}, size{}, mass{}, age{};
	SurfaceId source{};
	std::uint32_t generation{};
	bool active = false;
	float angle = 0, spin = 0, aspect = 1, delay = 0;
	unsigned sprite = 0;
	bool fall_through = false;
};

struct Emission {
	float x{}, y{}, vx{}, vy{}, size{}, mass{};
	SurfaceId source{};
	float delay = 0;
	bool fall_through = false;
};

struct Cursor {
	float x{}, y{}, vx{}, vy{};
	bool active = false;
};

class SnowBank {
  public:
	// Preserve logical distances across DPI changes. offset is the new left
	// edge in the old profile's pixels; newly exposed samples stay empty.
	void resize(float width, float scale, float height = 22,
	            bool preserve = false, float offset = 0);
	void clear() noexcept;
	void deposit(float x, float amount) noexcept;
	void melt(float dt) noexcept;
	void retain(float fraction) noexcept;
	float height_at(float x) const noexcept;
	float sample_x(std::size_t index) const noexcept;

	std::span<const float> heights() const noexcept
	{
		return heights_;
	}

	float width() const noexcept
	{
		return width_;
	}

	float max_height() const noexcept
	{
		return max_height_;
	}

	bool empty() const noexcept;

	std::uint64_t revision() const noexcept
	{
		return revision_;
	}

  private:
	float raw_height_at(float x) const noexcept;
	float width_ = 1, step_ = 4, max_height_ = 22, scale_ = 1;
	std::vector<float> heights_;
	std::uint64_t revision_ = 0;
};
class Landscape;

class Simulation {
  public:
	explicit Simulation(std::uint32_t seed = 0x534E4F57u);
	void configure(Rect bounds, float scale, std::size_t count,
	               std::size_t debris_slots);
	void update(float dt, const Settings &settings, Landscape &landscape,
	            Cursor cursor);
	Debris *emit(const Emission &emission) noexcept;
	void transfer_outside(std::span<Simulation *> destinations) noexcept;
	void clear_debris() noexcept;
	void drop_debris() noexcept;

	std::span<const Particle> particles() const noexcept
	{
		return particles_;
	}

	std::span<const Debris> debris() const noexcept
	{
		return debris_;
	}

	std::size_t active_debris() const noexcept;

	Rect bounds() const noexcept
	{
		return bounds_;
	}

  private:
	float random(float lo, float hi);
	void respawn(Particle &particle, bool spread);
	std::mt19937 random_;
	Rect bounds_{0, 0, 1, 1};
	float scale_ = 1, time_ = 0;
	std::size_t next_debris_ = 0;
	std::uint32_t generation_ = 1;
	std::vector<Particle> particles_;
	std::vector<Debris> debris_;
};
}
