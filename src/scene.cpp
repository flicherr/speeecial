#include "scene.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace snow
{
namespace
{
bool work_area_maximized(const Blocker &window, Rect taskbar) noexcept
{
	// GetWindowRect can include a maximized resize border beyond rcWork. That
	// sliver must not erase the cap; a fullscreen window covers the bar's
	// height.
	return window.maximized && taskbar.valid() &&
	       !(window.rect.top <= taskbar.top + 1 &&
	         window.rect.bottom >= taskbar.bottom - 1);
}
}

bool SnowSurface::enabled(const Settings &s) const noexcept
{
	return snapshot.state == SurfaceState::visible && snapshot.accepts_snow &&
	       support_visible && !foreground_pending &&
	       (snapshot.kind == SurfaceKind::window ? s.windows : s.accumulate);
}

Rect SnowSurface::snow_bounds() const noexcept
{
	return {snapshot.rect.left,
	        snapshot.rect.top - std::ceil(snow.max_height()) - 2,
	        snapshot.rect.right, snapshot.rect.top};
}

const SnowSurface *Landscape::find(SurfaceId id) const noexcept
{
	const auto i = surfaces_.find(id);
	return i == surfaces_.end() ? nullptr : &i->second;
}

bool Landscape::occluded(Rect rect, int before_z,
                         Rect supporting_body) const noexcept
{
	for (const auto &b : blockers_) {
		const bool work_area_window = work_area_maximized(b, supporting_body);
		if (!b.native_popup && !work_area_window && b.z_order < before_z &&
		    b.rect.intersects(rect)) {
			return true;
		}
	}
	return false;
}

bool Landscape::covered(Rect rect, int before_z) const
{
	if (!rect.valid()) {
		return true;
	}
	std::vector<Rect> pieces{rect}, next;
	for (const auto &b : blockers_) {
		if (b.native_popup || b.z_order >= before_z ||
		    !b.rect.intersects(rect)) {
			continue;
		}
		next.clear();
		for (auto p : pieces) {
			auto r = intersect(p, b.rect);
			if (!r.valid()) {
				next.push_back(p);
				continue;
			}
			for (auto part : {Rect{p.left, p.top, p.right, r.top},
			                  Rect{p.left, r.bottom, p.right, p.bottom},
			                  Rect{p.left, r.top, r.left, r.bottom},
			                  Rect{r.right, r.top, p.right, r.bottom}}) {
				if (part.valid()) {
					next.push_back(part);
				}
			}
		}
		pieces.swap(next);
		if (pieces.empty()) {
			return true;
		}
		if (pieces.size() > 256) {
			return false; // Bound pathological region fragmentation.
		}
	}
	return false;
}

bool Landscape::foreground_covers(const SurfaceSnapshot &s) const noexcept
{
	if (s.kind != SurfaceKind::taskbar || !foreground_cover_) {
		return false;
	}
	const auto &f = *foreground_cover_;
	return f.left <= s.rect.left + 2 && f.top <= s.rect.top + 2 &&
	       f.right >= s.rect.right - 2 && f.bottom >= s.rect.bottom - 2;
}

bool Landscape::begin_foreground_transition(std::optional<Rect> fullscreen)
{
	foreground_cover_ =
		fullscreen && fullscreen->valid() ? fullscreen : std::nullopt;
	bool changed = false;
	for (auto &[id, s] : surfaces_) {
		(void)id;
		const bool pending = foreground_covers(s.snapshot) &&
		                     s.support_visible &&
		                     s.snapshot.state == SurfaceState::visible;
		if (s.foreground_pending != pending) {
			s.foreground_pending = pending;
			update_mask(s);
			changed = true;
		}
	}
	return changed;
}

void Landscape::update_mask(SnowSurface &surface)
{
	if (!surface.support_visible || surface.foreground_pending) {
		surface.visible_regions.clear();
		surface.mask_signature = ~std::uint64_t{0};
		return;
	}
	std::uint64_t hash = 1469598103934665603ull;
	const auto strip = surface.snow_bounds();
	auto mix = [&](float v) {
		hash ^= std::bit_cast<std::uint32_t>(v);
		hash *= 1099511628211ull;
	};
	mix(strip.width());
	mix(strip.height());
	for (const auto &b : blockers_) {
		if (b.native_popup ||
		    (surface.snapshot.kind == SurfaceKind::taskbar &&
		     work_area_maximized(b, surface.snapshot.rect)) ||
		    b.z_order >= surface.snapshot.z_order ||
		    !strip.intersects(b.rect)) {
			continue;
		}
		const auto r = intersect(strip, b.rect);
		mix(r.left - strip.left);
		mix(r.top - strip.top);
		mix(r.right - strip.left);
		mix(r.bottom - strip.top);
	}
	if (surface.mask_signature == hash) {
		return;
	}
	surface.mask_signature = hash;
	// Cached rectangular mask: geometry changes do not repaint D2D surfaces.
	std::vector<Rect> pieces{strip}, next;
	for (const auto &b : blockers_) {
		if (b.native_popup ||
		    (surface.snapshot.kind == SurfaceKind::taskbar &&
		     work_area_maximized(b, surface.snapshot.rect)) ||
		    b.z_order >= surface.snapshot.z_order ||
		    !strip.intersects(b.rect)) {
			continue;
		}
		next.clear();
		for (auto p : pieces) {
			const auto r = intersect(p, b.rect);
			if (!r.valid()) {
				next.push_back(p);
				continue;
			}
			for (auto part : {Rect{p.left, p.top, p.right, r.top},
			                  Rect{p.left, r.bottom, p.right, p.bottom},
			                  Rect{p.left, r.top, r.left, r.bottom},
			                  Rect{r.right, r.top, p.right, r.bottom}}) {
				if (part.valid()) {
					next.push_back(part);
				}
			}
		}
		pieces.swap(next);
		if (pieces.empty()) {
			break;
		}
		if (pieces.size() > 64) {
			pieces.clear();
			break;
		}
	}
	surface.visible_regions.clear();
	for (auto r : pieces) {
		surface.visible_regions.push_back(
			{r.left - strip.left, r.top - strip.top, r.right - strip.left,
		     r.bottom - strip.top});
	}
}

void Landscape::release(SnowSurface &surface, float fraction, float vx,
                        float vy, bool emit)
{
	if (surface.snow.empty()) {
		return;
	}
	if (emit) {
		emit_snow(surface, fraction, vx, vy, 0, surface.snow.width());
	}
	surface.snow.retain(1 - fraction);
}

void Landscape::emit_snow(const SnowSurface &surface, float fraction, float vx,
                          float vy, float left, float right)
{
	left = std::clamp(left, 0.0f, surface.snow.width());
	right = std::clamp(right, 0.0f, surface.snow.width());
	if (right > left && !surface.snow.empty()) {
		if (emissions_.capacity() < max_debris) {
			emissions_.reserve(max_debris);
		}
		const auto &s = surface.snapshot;
		const auto count = std::clamp(
			static_cast<int>((right - left) / (18 * s.scale)), 6, 120);
		auto random = [&](float lo, float hi) {
			return std::uniform_real_distribution<float>(lo, hi)(random_);
		};
		for (int i = 0; i < count && emissions_.size() < max_debris; ++i) {
			const float x =
				left + (static_cast<float>(i) + random(0.05f, 0.95f)) /
						   static_cast<float>(count) * (right - left);
			const float h = surface.snow.height_at(x);
			if (h < 0.15f) {
				continue;
			}
			const float gx = s.rect.left + x,
						gy = s.rect.top - h * random(0.25f, 0.9f);
			if (occluded({gx - 0.5f, gy - 0.5f, gx + 0.5f, gy + 0.5f},
			             s.z_order)) {
				continue;
			}
			emissions_.push_back(
				{gx, gy, vx * random(0.55f, 1.2f) + random(-55, 55) * s.scale,
			     vy + random(-55, 35) * s.scale,
			     std::clamp(h * random(0.22f, 0.8f), 2 * s.scale, 16 * s.scale),
			     std::clamp(h * fraction * random(0.3f, 0.8f), 0.2f,
			                5 * s.scale),
			     s.id, random(0, 0.10f)});
		}
	}
}

void Landscape::synchronize(std::span<const SurfaceSnapshot> snapshots,
                            std::vector<Blocker> blockers,
                            const Settings &settings, float elapsed,
                            bool animate)
{
	++revision_;
	if (std::isfinite(elapsed) && elapsed > 0) {
		clock_ += std::min(elapsed, 3600.0f);
	}
	const auto can_detach = [&](const SnowSurface &surface) {
		// Some applications hide their HWND shortly before destroying it.
		// Retain that brief transition, but never shed snow from another
		// virtual desktop.
		const bool recent_hide =
			surface.snapshot.state == SurfaceState::hidden &&
			clock_ - surface.last_visible <= 0.5f;
		return animate && settings.windows && settings.collapse &&
		       surface.snapshot.accepts_snow &&
		       surface.snapshot.kind == SurfaceKind::window &&
		       (surface.snapshot.state == SurfaceState::visible || recent_hide);
	};
	// Use the old geometry/masks for detachments. Minimized HWNDs may already
	// report (-32000,-32000), and destroyed HWNDs cannot be queried at all.
	for (const auto &s : snapshots) {
		auto it = surfaces_.find(s.id);
		if (s.state == SurfaceState::closed) {
			if (it != surfaces_.end()) {
				auto &old = it->second;
				release(old, 1, 0, 25 * old.snapshot.scale, can_detach(old));
				surfaces_.erase(it);
			}
			continue;
		}
		if (!s.rect.valid() || !std::isfinite(s.scale) || s.scale <= 0 ||
		    s.scale > 8) {
			continue;
		}
		if (it == surfaces_.end()) {
			if (surfaces_.size() >= max_surfaces) {
				continue;
			}
			SnowSurface made;
			made.snapshot = s;
			made.foreground_pending = foreground_covers(s);
			made.snow.resize(s.rect.width(), s.scale,
			                 static_cast<float>(settings.height));
			it = surfaces_.emplace(s.id, std::move(made)).first;
		} else {
			auto &old = it->second;
			const bool was_visible =
				old.snapshot.state == SurfaceState::visible;
			if (!s.accepts_snow) {
				if (!old.snow.empty()) {
					old.snow.clear();
				}
			} else if (old.snapshot.state != SurfaceState::minimized &&
			           s.state == SurfaceState::minimized) {
				release(old, 1, 0, 25 * s.scale, can_detach(old));
			} else if (animate && old.enabled(settings) && was_visible &&
			           s.state == SurfaceState::visible &&
			           s.kind == SurfaceKind::window && settings.shake &&
			           elapsed > 0.005f && elapsed < 0.3f &&
			           clock_ - old.last_shed > 0.35f) {
				const float dx = s.rect.left - old.snapshot.rect.left,
							dy = s.rect.top - old.snapshot.rect.top;
				if (std::hypot(dx, dy) / elapsed > 700 * s.scale &&
				    std::hypot(dx, dy) < 800 * s.scale &&
				    std::abs(s.rect.width() - old.snapshot.rect.width()) < 2) {
					release(old, 0.16f,
					        std::clamp(dx / elapsed * 0.12f, -200 * s.scale,
					                   200 * s.scale),
					        -18 * s.scale, animate && old.enabled(settings));
					old.last_shed = clock_;
				}
			}
			// A rigid move carries the snow with the window. A width change at
			// the same DPI keeps the overlapping span in desktop coordinates,
			// so either resize edge can cut off snow or expose a clean area.
			const bool resized = s.scale == old.snapshot.scale &&
			                     s.rect.width() != old.snapshot.rect.width();
			const float offset =
				resized ? s.rect.left - old.snapshot.rect.left : 0;
			if (resized && s.state == SurfaceState::visible && s.accepts_snow &&
			    can_detach(old)) {
				emit_snow(old, 1, 0, 25 * s.scale, 0, offset);
				emit_snow(old, 1, 0, 25 * s.scale, offset + s.rect.width(),
				          old.snow.width());
			}
			old.snow.resize(s.rect.width(), s.scale,
			                static_cast<float>(settings.height), true, offset);
			old.snapshot = s;
		}
		it->second.seen = revision_;
		if (s.state == SurfaceState::visible) {
			it->second.last_visible = clock_;
		} else if (s.state == SurfaceState::suspended ||
		           s.state == SurfaceState::minimized) {
			it->second.last_visible = -100;
		}
	}
	// Disappearance from a snapshot is NOT proof of closure. Never emit here.
	for (auto i = surfaces_.begin(); i != surfaces_.end();) {
		if (i->second.seen != revision_) {
			i = surfaces_.erase(i);
		} else {
			++i;
		}
	}
	blockers_ = std::move(blockers);
	for (auto &[id, s] : surfaces_) {
		(void)id;
		// Test the physical bar, not its cap protruding into a maximized app's
		// work area. This is independent of the optional fullscreen auto-pause.
		s.support_visible = s.snapshot.kind != SurfaceKind::taskbar ||
		                    !covered(s.snapshot.rect, s.snapshot.z_order);
		// Acknowledgement from the authoritative snapshot: physical hiding now
		// owns visibility. Later taskbar hover/reveal may show the cap again
		// even when the fullscreen application's foreground HWND has not
		// changed.
		if (!s.support_visible || s.snapshot.state != SurfaceState::visible) {
			s.foreground_pending = false;
		}
		update_mask(s);
	}
}

std::optional<Collision> Landscape::collide(float x0, float y0, float x1,
                                            float y1, const Settings &settings,
                                            SurfaceId ignore) const noexcept
{
	if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
	    !std::isfinite(y1) || y1 <= y0) {
		return {};
	}
	std::optional<Collision> nearest;
	for (const auto &[id, s] : surfaces_) {
		if (id == ignore || !s.enabled(settings)) {
			continue;
		}
		const auto &r = s.snapshot.rect;
		if (y0 >= r.top || y1 < r.top - s.snow.max_height() ||
		    std::max(x0, x1) < r.left || std::min(x0, x1) > r.right) {
			continue;
		}
		float t = std::clamp((r.top - y0) / (y1 - y0), 0.0f, 1.0f),
			  x = x0 + (x1 - x0) * t, top = r.top;
		for (int k = 0; k < 3; ++k) {
			top = r.top - s.snow.height_at(x - r.left);
			t = (top - y0) / (y1 - y0);
			x = x0 + (x1 - x0) * t;
		}
		if (t < 0 || t > 1 || x < r.left || x > r.right ||
		    (nearest && t >= nearest->t)) {
			continue;
		}
		if (occluded({x - 0.1f, top - 0.1f, x + 0.1f, top + 0.1f},
		             s.snapshot.z_order,
		             s.snapshot.kind == SurfaceKind::taskbar ? s.snapshot.rect
		                                                     : Rect{})) {
			continue;
		}
		nearest = Collision{id, x - r.left, top, t};
	}
	return nearest;
}

void Landscape::deposit(const Collision &hit, float amount) noexcept
{
	const auto i = surfaces_.find(hit.id);
	if (i != surfaces_.end()) {
		i->second.snow.deposit(hit.x, amount);
	}
}

void Landscape::update(float dt, const Settings &settings)
{
	if (!std::isfinite(dt) || dt <= 0) {
		return;
	}
	dt = std::min(dt, 0.1f);
	if (settings.melt) {
		for (auto &[id, s] : surfaces_) {
			(void)id;
			if (s.enabled(settings)) {
				s.snow.melt(dt);
			}
		}
	}
}

void Landscape::clear_snow() noexcept
{
	for (auto &[id, s] : surfaces_) {
		(void)id;
		s.snow.clear();
	}
	emissions_.clear();
}

void Landscape::drop_snow(const Settings &settings)
{
	// Manual clearing is independent of the automatic collapse setting.
	// Hidden surfaces are cleared without creating snow on another desktop.
	emissions_.clear();
	for (auto &[id, s] : surfaces_) {
		(void)id;
		if (s.enabled(settings)) {
			emit_snow(s, 1, 0, 25 * s.snapshot.scale, 0, s.snow.width());
		}
		s.snow.clear();
	}
	for (auto &e : emissions_) {
		e.fall_through = true;
		e.vy = std::max(e.vy, 25.0f);
		e.delay = 0;
	}
}

bool Landscape::needs_melting(const Settings &settings) const noexcept
{
	if (!settings.melt) {
		return false;
	}
	for (const auto &[id, s] : surfaces_) {
		(void)id;
		if (s.enabled(settings) && !s.snow.empty()) {
			return true;
		}
	}
	return false;
}
}
