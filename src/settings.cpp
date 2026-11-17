#include "settings.hpp"
#include "win_util.hpp"
#include <fstream>
#include <iomanip>
#include <shlobj.h>

namespace snow
{
namespace
{
constexpr auto run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
}

SettingsStore::SettingsStore()
{
	PWSTR path = nullptr;
	check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
	                           &path),
	      "SHGetKnownFolderPath");
	try {
		directory_ = std::filesystem::path(path) / L"SnowDesk";
	} catch (...) {
		CoTaskMemFree(path);
		throw;
	}
	CoTaskMemFree(path);
	std::filesystem::create_directories(directory_);
}

Settings SettingsStore::load() const
{
	Settings s;
	const auto file = directory_ / L"settings.ini";
	auto read = [&](const wchar_t *name, int fallback) {
		return static_cast<int>(
			GetPrivateProfileIntW(L"SnowDesk", name, fallback, file.c_str()));
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
	auto write = [&](const wchar_t *name, int value) {
		const auto text = std::to_wstring(value);
		check_win(WritePrivateProfileStringW(L"SnowDesk", name, text.c_str(),
		                                     temporary.c_str()),
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
		const auto file = directory_ / L"snowdesk.log";
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
	DWORD bytes = 0;
	return RegGetValueW(HKEY_CURRENT_USER, run_key, L"SnowDesk", RRF_RT_REG_SZ,
	                    nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}

void SettingsStore::set_autostart(bool enabled)
{
	HKEY key = nullptr;
	auto status = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0,
	                              KEY_SET_VALUE, nullptr, &key, nullptr);
	check(HRESULT_FROM_WIN32(status), "Open autostart registry key");
	if (enabled) {
		std::wstring filename(32768, L'\0');
		const auto length = GetModuleFileNameW(
			nullptr, filename.data(), static_cast<DWORD>(filename.size()));
		if (!length || length >= filename.size()) {
			RegCloseKey(key);
			throw std::runtime_error(
				"Cannot resolve executable path for autostart");
		}
		filename.resize(length);
		const auto command = L"\"" + filename + L"\"";
		status = RegSetValueExW(
			key, L"SnowDesk", 0, REG_SZ,
			reinterpret_cast<const BYTE *>(command.c_str()),
			static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
	} else {
		status = RegDeleteValueW(key, L"SnowDesk");
		if (status == ERROR_FILE_NOT_FOUND) {
			status = ERROR_SUCCESS;
		}
	}
	RegCloseKey(key);
	check(HRESULT_FROM_WIN32(status), "Update autostart registry value");
}
}
