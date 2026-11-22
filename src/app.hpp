#pragma once

#include "renderer.hpp"
#include "settings.hpp"
#include "tray_menu.hpp"
#include "updater.hpp"
#include "win_util.hpp"
#include <chrono>

namespace snow
{
class App {
  public:
	explicit App(HINSTANCE instance);
	~App();
	int run();
	static LRESULT CALLBACK window_proc(HWND window, UINT message,
	                                    WPARAM wparam, LPARAM lparam) noexcept;

  private:
	LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam);
	void add_tray_icon();
	void update_tray_icon(bool update_available) noexcept;
	void show_menu();
	void update_menu_tip();
	void close_menu_tip() noexcept;
	void command(UINT id);
	void rebuild();
	void update_policy();
	void refresh_desktop(bool force = false);
	void sync_timer();
	void tick();
	void notify(const wchar_t *title, const wchar_t *text,
	            DWORD flags = 0) noexcept;
	void report_error(const std::string &error, bool graphics_error) noexcept;
	void diagnostics();
	void update_finished();

	HINSTANCE instance_;
	HWND window_{};
	HICON icon_{};
	SIZE icon_size_{};
	bool icon_update_available_ = false, icon_dark_ = false;
	bool icon_update_requested_ = false, icon_dirty_ = false;
	HWND menu_tip_{};
	HMENU active_menu_{};
	TrayMenu *styled_menu_{};
	std::wstring menu_detail_;
	ULONGLONG menu_hover_since_ = 0;
	bool menu_tip_visible_ = false;
	HPOWERNOTIFY power_notification_{};
	SettingsStore store_;
	Settings settings_;
	Landscape landscape_;
	std::unique_ptr<DesktopTracker> tracker_;
	std::unique_ptr<Renderer> renderer_;
	std::unique_ptr<Updater> updater_;
	std::uint64_t update_revision_ = 0;
	UINT taskbar_created_{};
	enum class RunMode { running, hidden, stopped };
	RunMode run_mode_ = RunMode::running;
	bool tray_added_ = false, locked_ = false, display_off_ = false;
	bool rebuilding_ = false, menu_open_ = false, closing_ = false;
	bool desktop_scheduled_ = false;
	unsigned graphics_attempts_ = 0;
	std::uint64_t graphics_rebuilds_ = 0;
	ULONGLONG graphics_retry_at_ = 0, graphics_healthy_since_ = 0;
	int timer_fps_ = 0;
	std::string last_error_;
	std::chrono::steady_clock::time_point last_frame_;
	std::chrono::steady_clock::time_point next_frame_;
	std::chrono::steady_clock::time_point last_desktop_;
	std::chrono::steady_clock::time_point measurement_start_;
	std::uint64_t frames_ = 0;
	double measured_fps_ = 0;
	double last_frame_work_ms_ = 0, peak_frame_work_ms_ = 0;
	double peak_frame_interval_ms_ = 0;
	std::uint64_t late_frames_ = 0;
};
}
