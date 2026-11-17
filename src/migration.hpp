#pragma once

#include "win_util.hpp"
#include <filesystem>

namespace snow::migration
{
// Legacy identifiers are confined to migration.cpp. Keep compatibility for
// users who skip releases, including the original single-instance guard.
class InstanceGuard {
  public:
	InstanceGuard();
	InstanceGuard(const wchar_t *current, const wchar_t *legacy);
	explicit operator bool() const noexcept;

  private:
	UniqueHandle current_, legacy_;
};

std::filesystem::path profile_directory(const std::filesystem::path &local);
void migrate_profile(std::filesystem::path &directory);
const wchar_t *write_section(const std::filesystem::path &directory);
int read_setting(const std::filesystem::path &file, const wchar_t *name,
                 int fallback);
bool autostart(HKEY key);
void set_autostart(HKEY key, bool enabled, const std::wstring &command);
}
