#include "settings.hpp"
#include "migration.hpp"
#include "win_util.hpp"
#include <fstream>
#include <iomanip>
#include <shlobj.h>

namespace snow
{
namespace
{
constexpr auto run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct RegistryKey {
	HKEY value{};

	~RegistryKey()
	{
		if (value) {
			RegCloseKey(value);
		}
	}
};
}

SettingsStore::SettingsStore()
{
	PWSTR path = nullptr;
	check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
	                           &path),
	      "SHGetKnownFolderPath");
	try {
		directory_ = migration::profile_directory(path);
	} catch (...) {
		CoTaskMemFree(path);
		throw;
	}
	CoTaskMemFree(path);
}

void SettingsStore::migrate()
{
	migration::migrate_profile(directory_);
}

SettingsStore::SettingsStore(const std::filesystem::path &local)
	: directory_(migration::profile_directory(local))
{
}

Settings SettingsStore::load() const
{
	Settings s;
	const auto file = directory_ / L"settings.ini";
	auto read = [&](const wchar_t *name, int fallback) {
		return migration::read_setting(file, name, fallback);
	};
	s.density = static_cast<Density>(read(L"Density", 2));
	s.wind = static_cast<Wind>(read(L"Wind", 1));
	s.fps = read(L"FPS", 30);
	s.flake_size = read(L"FlakeSize", 6);
	s.height = read(L"Height", 22);
	s.layer = static_cast<Layer>(read(L"Layer", 0));
	s.theme = static_cast<Theme>(read(L"Theme", 0));
	s.language = static_cast<Language>(read(L"Language", 0));
	s.windows = read(L"Windows", 1) != 0;
	s.collapse = read(L"Collapse", 1) != 0;
	s.shake = read(L"Shake", 1) != 0;
	s.cursor = read(L"Cursor", 0) != 0;
	s.show_capture = read(L"ShowCapture", 1) != 0;
	s.accumulate = read(L"Accumulate", 1) != 0;
	s.melt = read(L"Melt", 0) != 0;
	s.pause_fullscreen = read(L"PauseFullscreen", 1) != 0;
	s.pause_battery = read(L"PauseBattery", 0) != 0;
	s.normalize();
	return s;
}

void SettingsStore::save(const Settings &s) const
{
	const auto temporary = directory_ / L"settings.ini.tmp";
	// A previous version may have left an unfinished save in another section.
	// Build the new snapshot from scratch instead of carrying stale data over.
	if (!DeleteFileW(temporary.c_str()) &&
	    GetLastError() != ERROR_FILE_NOT_FOUND) {
		check_win(FALSE, "Remove stale settings snapshot");
	}
	auto write = [&](const wchar_t *name, int value) {
		const auto text = std::to_wstring(value);
		check_win(
			WritePrivateProfileStringW(migration::write_section(directory_),
		                               name, text.c_str(), temporary.c_str()),
			"WritePrivateProfileString");
	};
	write(L"Density", static_cast<int>(s.density));
	write(L"Wind", static_cast<int>(s.wind));
	write(L"FPS", s.fps);
	write(L"FlakeSize", s.flake_size);
	write(L"Height", s.height);
	write(L"Layer", static_cast<int>(s.layer));
	write(L"Theme", static_cast<int>(s.theme));
	write(L"Language", static_cast<int>(s.language));
	write(L"Windows", s.windows);
	write(L"Collapse", s.collapse);
	write(L"Shake", s.shake);
	write(L"Cursor", s.cursor);
	write(L"ShowCapture", s.show_capture);
	write(L"Accumulate", s.accumulate);
	write(L"Melt", s.melt);
	write(L"PauseFullscreen", s.pause_fullscreen);
	write(L"PauseBattery", s.pause_battery);
	WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
	check_win(MoveFileExW(temporary.c_str(),
	                      (directory_ / L"settings.ini").c_str(),
	                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
	          "Save settings");
}

void SettingsStore::log(const std::string &message) const noexcept
{
	try {
		const auto file = directory_ / L"speeecial.log";
		std::error_code error;
		const auto size = std::filesystem::file_size(file, error);
		std::ofstream out(file, !error && size > 512 * 1024 ? std::ios::trunc
		                                                    : std::ios::app);
		SYSTEMTIME t{};
		GetLocalTime(&t);
		out << t.wYear << '-' << std::setfill('0') << std::setw(2) << t.wMonth
			<< '-' << std::setw(2) << t.wDay << ' ' << std::setw(2) << t.wHour
			<< ':' << std::setw(2) << t.wMinute << ':' << std::setw(2)
			<< t.wSecond << ' ' << message << '\n';
	} catch (...) { /* Logging must never break the message loop. */
	}
}

bool SettingsStore::autostart()
{
	RegistryKey key;
	const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0,
	                                  KEY_QUERY_VALUE, &key.value);
	if (status == ERROR_FILE_NOT_FOUND) {
		return false;
	}
	check(HRESULT_FROM_WIN32(status), "Open autostart registry key");
	return migration::autostart(key.value);
}

void SettingsStore::set_autostart(bool enabled)
{
	RegistryKey key;
	auto status = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0,
	                              KEY_SET_VALUE, nullptr, &key.value, nullptr);
	check(HRESULT_FROM_WIN32(status), "Open autostart registry key");
	std::wstring command;
	if (enabled) {
		std::wstring filename(32768, L'\0');
		const auto length = GetModuleFileNameW(
			nullptr, filename.data(), static_cast<DWORD>(filename.size()));
		if (!length || length >= filename.size()) {
			throw std::runtime_error(
				"Cannot resolve executable path for autostart");
		}
		filename.resize(length);
		command = L"\"" + filename + L"\"";
	}
	migration::set_autostart(key.value, enabled, command);
}
}
