#include "core.hpp"
#include "scene.hpp"
#include <algorithm>
#include <cmath>

namespace snow
{
void Settings::normalize() noexcept
{
	if (density < Density::off || density > Density::heavy) {
		density = Density::normal;
	}
	if (wind < Wind::calm || wind > Wind::gusts) {
		wind = Wind::gentle;
	}
	if (layer != Layer::front && layer != Layer::behind) {
		layer = Layer::front;
	}
	if (theme != Theme::system && theme != Theme::light &&
	    theme != Theme::dark) {
		theme = Theme::system;
	}
	if (fps != 20 && fps != 30 && fps != 60 && fps != 144 && fps != 240) {
		fps = 30;
	}
	if (height != 12 && height != 22 && height != 40) {
		height = 22;
	}
}

std::size_t particles_per_monitor(Density density,
                                  std::size_t monitors) noexcept
{
	const std::size_t count = density == Density::light    ? 160
	                          : density == Density::normal ? 450
	                          : density == Density::heavy  ? 1000
	                                                       : 0;
	return std::min(count, max_particles / std::max(monitors, std::size_t{1}));
}

bool Rect::valid() const noexcept
{
	return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
	       std::isfinite(bottom) && right > left && bottom > top;
}

bool Rect::contains(float x, float y) const noexcept
{
	return x >= left && x < right && y >= top && y < bottom;
}

bool Rect::intersects(Rect r) const noexcept
{
	return left < r.right && right > r.left && top < r.bottom && bottom > r.top;
}

Rect intersect(Rect a, Rect b) noexcept
{
	return {std::max(a.left, b.left), std::max(a.top, b.top),
	        std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

void SnowBank::resize(float width, float scale, float height, bool preserve,
                      float offset)
{
	width = std::clamp(std::isfinite(width) ? width : 1.0f, 1.0f, 65536.0f);
	scale = std::clamp(std::isfinite(scale) ? scale : 1.0f, 0.5f, 8.0f);
	height = std::clamp(std::isfinite(height) ? height : 22.0f, 4.0f, 64.0f);
	if (!heights_.empty() && width == width_ && scale == scale_ &&
	    height * scale == max_height_ && offset == 0) {
		return;
	}
	const float step = 4 * scale;
	std::vector<float> next(
		static_cast<std::size_t>(std::ceil(width / step)) + 1, 0);
	if (preserve && !heights_.empty()) {
		for (std::size_t i = 0; i < next.size(); ++i) {
			next[i] = std::min(
				height * scale,
				raw_height_at(std::min(static_cast<float>(i) * step, width) *
			                      scale_ / scale +
			                  offset) *
					scale / scale_);
		}
	}
	width_ = width;
	scale_ = scale;
	step_ = step;
	max_height_ = height * scale;
	heights_ = std::move(next);
	++revision_;
}

void SnowBank::clear() noexcept
{
	std::fill(heights_.begin(), heights_.end(), 0);
	++revision_;
}

bool SnowBank::empty() const noexcept
{
	return std::none_of(heights_.begin(), heights_.end(),
	                    [](float h) { return h > 0.02f; });
}

float SnowBank::sample_x(std::size_t i) const noexcept
{
	return std::min(static_cast<float>(i) * step_, width_);
}

float SnowBank::height_at(float x) const noexcept
{
	const float raw = raw_height_at(x);
	// Smoothstep meets the baseline and the body with horizontal tangents.
	// Keep the stored profile untapered, so resize/DPI changes cannot erode it.
	const float shoulder = std::min(width_ * 0.5f, max_height_ * 1.6f);
	const float t = std::clamp(std::min(x, width_ - x) / shoulder, 0.0f, 1.0f);
	return raw > 0 ? raw * t * t * (3 - 2 * t) : 0;
}

float SnowBank::raw_height_at(float x) const noexcept
{
	if (heights_.empty() || !std::isfinite(x) || x < 0 || x > width_) {
		return 0;
	}
	const auto i = std::min(static_cast<std::size_t>(x / step_),
	                        heights_.size() - 1),
			   j = std::min(i + 1, heights_.size() - 1);
	const float t = std::clamp((x - sample_x(i)) /
	                               std::max(sample_x(j) - sample_x(i), 0.001f),
	                           0.0f, 1.0f);
	return heights_[i] + (heights_[j] - heights_[i]) * t;
}

void SnowBank::deposit(float x, float amount) noexcept
{
	if (heights_.empty() || !std::isfinite(x) || !std::isfinite(amount) ||
	    amount <= 0 || x < 0 || x > width_) {
		return;
	}
	const auto center = static_cast<std::ptrdiff_t>(std::lround(x / step_));
	constexpr float weights[] = {0.12f, 0.35f, 0.70f, 1.0f,
	                             0.70f, 0.35f, 0.12f};
	bool changed = false;
	for (std::ptrdiff_t k = -3; k <= 3; ++k) {
		const auto i = center + k;
		if (i < 0 || static_cast<std::size_t>(i) >= heights_.size()) {
			continue;
		}
		auto &h = heights_[static_cast<std::size_t>(i)];
		const float next = std::min(max_height_, h + amount * weights[k + 3]);
		changed |= next != h;
		h = next;
	}
	if (changed) {
		++revision_;
	}
}

void SnowBank::melt(float dt) noexcept
{
	if (!std::isfinite(dt) || dt <= 0) {
		return;
	}
	bool changed = false;
	const float loss = std::min(dt, 0.1f) * max_height_ / 240;
	for (auto &h : heights_) {
		const float next = std::max(0.0f, h - loss);
		changed |= next != h;
		h = next;
	}
	if (changed) {
		++revision_;
	}
}

void SnowBank::retain(float fraction) noexcept
{
	fraction = std::clamp(fraction, 0.0f, 1.0f);
	for (auto &h : heights_) {
		h *= fraction;
	}
	++revision_;
}

Simulation::Simulation(std::uint32_t seed) : random_(seed) {}

float Simulation::random(float lo, float hi)
{
	return std::uniform_real_distribution<float>(lo, hi)(random_);
}

void Simulation::configure(Rect bounds, float scale, std::size_t count,
                           std::size_t slots)
{
	bounds_ = bounds.valid() ? bounds : Rect{0, 0, 1, 1};
	scale_ = std::clamp(std::isfinite(scale) ? scale : 1.0f, 0.5f, 8.0f);
	const auto old = particles_.size();
	particles_.resize(std::min(count, max_particles));
	for (std::size_t i = old; i < particles_.size(); ++i) {
		auto &p = particles_[i];
		p.size = random(3, 9) * scale_;
		p.speed = random(22, 70) * scale_;
		p.opacity = random(0.35f, 0.95f);
		p.phase = random(0, 6.2831853f);
		p.sway = random(3, 15) * scale_;
		p.sprite = static_cast<unsigned>(random_() % 3);
		respawn(p, true);
	}
	debris_.resize(std::min(slots, max_debris));
	next_debris_ = 0;
}

void Simulation::respawn(Particle &p, bool spread)
{
	p.x = random(bounds_.left, bounds_.right);
	p.y = spread ? random(bounds_.top - p.size, bounds_.bottom)
	             : bounds_.top - p.size - random(0, 35) * scale_;
	p.drift = 0;
	p.lift = 0;
}

Debris *Simulation::emit(const Emission &e) noexcept
{
	if (debris_.empty() || !std::isfinite(e.x) || !std::isfinite(e.y)) {
		return nullptr;
	}
	std::size_t slot = next_debris_;
	for (std::size_t i = 0; i < debris_.size(); ++i) {
		const auto c = (next_debris_ + i) % debris_.size();
		if (!debris_[c].active) {
			slot = c;
			break;
		}
	}
	debris_[slot] = {e.x,
	                 e.y,
	                 e.vx,
	                 e.vy,
	                 std::clamp(e.size, 2.0f * scale_, 18.0f * scale_),
	                 std::clamp(e.mass, 0.1f, 6.0f * scale_),
	                 0,
	                 e.source,
	                 generation_++,
	                 true};
	debris_[slot].angle = random(-180, 180);
	debris_[slot].spin = random(-240, 240);
	debris_[slot].aspect = random(0.6f, 1.45f);
	debris_[slot].sprite = random_() % 3;
	debris_[slot].delay = std::clamp(e.delay, 0.0f, 0.15f);
	debris_[slot].fall_through = e.fall_through;
	if (generation_ == 0) {
		++generation_;
	}
	next_debris_ = (slot + 1) % debris_.size();
	return &debris_[slot];
}

void Simulation::transfer_outside(std::span<Simulation *> destinations) noexcept
{
	// Update every monitor first, then transfer. A crossing fragment must not
	// advance twice in one frame, or disappear at an internal monitor edge.
	for (auto &p : debris_) {
		if (!p.active || bounds_.contains(p.x, p.y)) {
			continue;
		}
		for (auto *destination : destinations) {
			if (!destination || destination == this ||
			    !destination->bounds_.contains(p.x, p.y)) {
				continue;
			}
			if (auto *copy = destination->emit(
					{p.x, p.y, p.vx, p.vy, p.size, p.mass, p.source})) {
				const auto generation = copy->generation;
				*copy = p;
				copy->generation = generation;
			}
			p.active = false;
			break;
		}
		if (p.y > bounds_.bottom + p.size || p.x < bounds_.left - 2 * p.size ||
		    p.x > bounds_.right + 2 * p.size) {
			p.active = false;
		}
	}
}

void Simulation::clear_debris() noexcept
{
	for (auto &p : debris_) {
		p.active = false;
	}
}

void Simulation::drop_debris() noexcept
{
	for (auto &p : debris_) {
		if (p.active) {
			p.fall_through = true;
			p.vy = std::max(p.vy, 25 * scale_);
			p.delay = 0;
		}
	}
}

std::size_t Simulation::active_debris() const noexcept
{
	return static_cast<std::size_t>(
		std::count_if(debris_.begin(), debris_.end(),
	                  [](const Debris &p) { return p.active; }));
}

namespace
{
void cursor_force(float x, float y, Cursor c, float scale, float dt, float &vx,
                  float &vy)
{
	if (!c.active) {
		return;
	}
	const float sx = c.x - c.vx * dt, sy = c.y - c.vy * dt;
	const float mx = c.x - sx, my = c.y - sy;
	const float t = std::clamp(((x - sx) * mx + (y - sy) * my) /
	                               std::max(mx * mx + my * my, 0.001f),
	                           0.0f, 1.0f);
	const float dx = x - (sx + t * mx), dy = y - (sy + t * my),
				radius = 145 * scale, d2 = dx * dx + dy * dy;
	if (d2 >= radius * radius) {
		return;
	}
	const float distance = std::sqrt(std::max(d2, 1.0f)),
				weight = 1 - distance / radius;
	const float speed = std::min(std::hypot(c.vx, c.vy), 1800.0f * scale);
	if (speed < 15 * scale) {
		return;
	}
	const float response = (1 - std::exp(-14 * dt)) * weight;
	vx += (std::clamp(c.vx, -1800 * scale, 1800 * scale) * 0.8f +
	       dx / distance * speed * 0.45f - vx) *
	      response;
	vy += (std::clamp(c.vy, -1800 * scale, 1800 * scale) * 0.6f +
	       dy / distance * speed * 0.45f - vy) *
	      response;
}
}

void Simulation::update(float dt, const Settings &settings,
                        Landscape &landscape, Cursor cursor)
{
	if (!std::isfinite(dt) || dt <= 0) {
		return;
	}
	dt = std::min(dt, 0.1f);
	time_ = std::fmod(time_ + dt, 4096.0f);
	float breeze = 0;
	if (settings.wind == Wind::gentle) {
		breeze = 12 * scale_;
	}
	if (settings.wind == Wind::gusts) {
		breeze = (20 + 55 * std::sin(time_ * 0.23f)) * scale_;
	}
	if (!settings.cursor) {
		cursor.active = false;
	}
	const float damping = std::exp(-2.0f * dt);
	for (auto &p : particles_) {
		const float old_x = p.x, old_y = p.y;
		p.phase = std::fmod(p.phase + dt * 0.9f, 6.2831853f);
		p.drift *= damping;
		p.lift *= damping;
		cursor_force(p.x, p.y, cursor, scale_, dt, p.drift, p.lift);
		p.x += (breeze + p.sway * std::sin(p.phase) + p.drift) * dt;
		p.y += std::clamp(p.speed + p.lift, -500 * scale_, 900 * scale_) * dt;
		if (auto hit = landscape.collide(old_x, old_y, p.x, p.y, settings)) {
			landscape.deposit(*hit, p.size * 0.07f);
			// Continue screen-space weather below this edge instead of starving
			// nested foreground windows and the taskbar by restarting at the
			// top.
			const auto *surface = landscape.find(hit->id);
			if (surface && surface->snapshot.kind == SurfaceKind::window) {
				p.x = random(bounds_.left, bounds_.right);
				p.y = surface->snapshot.rect.top + p.size +
				      random(1, 10) * scale_;
			} else {
				respawn(p, false);
			}
			continue;
		}
		if (p.x < bounds_.left - p.size) {
			p.x = bounds_.right + p.size;
		}
		if (p.x > bounds_.right + p.size) {
			p.x = bounds_.left - p.size;
		}
		if (p.y > bounds_.bottom + p.size) {
			respawn(p, false);
		}
	}
	for (auto &p : debris_) {
		if (!p.active) {
			continue;
		}
		const float old_x = p.x, old_y = p.y;
		if (p.delay > 0) {
			p.delay = std::max(0.0f, p.delay - dt);
			continue;
		}
		p.age += dt;
		const float drag = 0.4f + 2.0f * scale_ / std::max(p.size, scale_);
		p.vx += (breeze * 0.6f - p.vx * drag) * dt;
		p.vy += (420 * scale_ - p.vy * drag * 0.25f) * dt;
		p.angle = std::fmod(p.angle + p.spin * dt, 360.0f);
		p.spin *= std::exp(-0.18f * dt);
		if (!p.fall_through) {
			cursor_force(p.x, p.y, cursor, scale_, dt, p.vx, p.vy);
		}
		p.x += p.vx * dt;
		p.y += p.vy * dt;
		// Explicit clearing sends snow off the desktop, past every support.
		if (!p.fall_through) {
			if (auto hit = landscape.collide(old_x, old_y, p.x, p.y, settings,
			                                 p.age < 0.35f ? p.source : 0)) {
				landscape.deposit(*hit, p.mass);
				p.active = false;
				continue;
			}
		}
		if (p.age > 12) {
			p.active = false;
		}
	}
}
}
