#pragma once

#include "update_core.hpp"
#include <filesystem>
#include <memory>
#include <windows.h>

namespace snow
{
inline constexpr UINT message_update = WM_APP + 6;

class Updater {
  public:
	enum class Status {
		idle,
		checking,
		available,
		current,
		downloading,
		ready,
		failed
	};

	struct View {
		Status status = Status::idle;
		std::optional<updates::Release> release;
		std::wstring detail;
		unsigned progress = 0;
		std::uint64_t revision = 0;
		bool manual = false;

		bool busy() const
		{
			return status == Status::checking ||
			       status == Status::downloading || status == Status::ready;
		}
	};

	Updater(HWND owner, const std::filesystem::path &settings_directory);
	~Updater();
	View view() const;

	bool automatic() const noexcept
	{
		return automatic_;
	}

	void set_automatic(bool enabled);
	void check(bool manual);
	void install();
	// Only called on the UI thread after an explicit Install command and a
	// successful download/helper handshake. The caller then exits normally.
	bool commit();

  private:
	struct State;
	std::shared_ptr<State> state_;
	std::filesystem::path preferences_;
	bool automatic_ = true;
};

// Handle helper mode before COM, singleton, graphics or autostart changes.
std::optional<int> run_update_mode();
void acknowledge_update_startup() noexcept;
}
