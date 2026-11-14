#pragma once

#include "core.hpp"
#include "desktop.hpp"
#include <memory>
#include <string>

namespace snow
{
inline constexpr UINT message_rebuild = WM_APP + 2;

class Renderer {
  public:
	Renderer(HINSTANCE instance, HWND control,
	         const std::vector<Display> &displays, Settings settings,
	         Landscape &landscape);
	~Renderer();
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	void rebuild_scene(const std::vector<Display> &displays, Settings settings);
	void set_settings(Settings settings);
	bool set_capture(bool show) noexcept;
	void synchronize_desktop(const DesktopSnapshot &snapshot,
	                         bool defer_to_frame = false);
	void refresh_foreground(bool force = false);
	void update_policy(bool suspended, bool hide_effects);
	void tick(float dt);
	void clear_snow();
	void hide() noexcept;
	void check_device() const;
	bool needs_animation() const noexcept;
	std::size_t particle_count() const noexcept;
	std::string description() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
}
