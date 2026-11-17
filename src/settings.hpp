#pragma once

#include "core.hpp"
#include <filesystem>
#include <string>

namespace snow
{
class SettingsStore {
  public:
	SettingsStore();
	explicit SettingsStore(const std::filesystem::path &local);
	void migrate();
	Settings load() const;
	void save(const Settings &settings) const;
	void log(const std::string &message) const noexcept;

	const std::filesystem::path &directory() const noexcept
	{
		return directory_;
	}

	static bool autostart();
	static void set_autostart(bool enabled);

  private:
	std::filesystem::path directory_;
};
}
