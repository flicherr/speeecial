#include "migration.hpp"
#include "settings.hpp"
#include <fstream>
#include <iostream>

namespace
{
namespace fs = std::filesystem;
using namespace snow;
unsigned checks = 0;

void require(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		throw std::runtime_error(message);
	}
}

template <class F> void rejects(F action, const char *message)
{
	bool failed = false;
	try {
		action();
	} catch (const std::exception &) {
		failed = true;
	}
	require(failed, message);
}

void ini(const fs::path &file, const wchar_t *section, const wchar_t *name,
         const wchar_t *value)
{
	check_win(WritePrivateProfileStringW(section, name, value, file.c_str()),
	          "Write test profile");
	WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
}

std::string contents(const fs::path &file)
{
	std::ifstream input(file, std::ios::binary);
	require(input.good(), "Test file missing");
	return std::string(std::istreambuf_iterator<char>(input), {});
}

void profiles(const fs::path &root)
{
	SettingsStore fresh(root / L"fresh");
	require(fresh.directory().filename() == L"speeecial", "Fresh profile name");
	fresh.migrate();
	ini(fresh.directory() / L"settings.ini.tmp", L"SnowDesk", L"FPS", L"144");
	fresh.save(fresh.load());
	require(contents(fresh.directory() / L"settings.ini").find("SnowDesk") ==
	            std::string::npos,
	        "Fresh settings contain legacy section");

	const auto local = root / L"legacy профиль";
	const auto legacy = local / L"SnowDesk";
	fs::create_directories(legacy);
	const auto file = legacy / L"settings.ini";
	ini(file, L"SnowDesk", L"FPS", L"144");
	ini(file, L"SnowDesk", L"FlakeSize", L"9");
	ini(file, L"SnowDesk", L"Language", L"2");
	ini(file, L"SnowDesk", L"UnknownFutureKey", L"keep-me");
	ini(file, L"Other", L"Preserved", L"42");
	ini(legacy / L"updates.ini", L"Updates", L"Automatic", L"0");
	ini(legacy / L"updates.ini", L"Updates", L"LastAttempt", L"123456789");
	std::ofstream(legacy / L"snowdesk.log") << "legacy journal";
	const auto original = contents(file);
	const auto updates = contents(legacy / L"updates.ini");
	SettingsStore migrated(local);
	const auto settings = migrated.load();
	require(settings.fps == 144 && settings.flake_size == 9 &&
	            settings.language == Language::english,
	        "Legacy settings lost");
	require(migrated.directory() == legacy && contents(file) == original,
	        "Reading before startup acknowledgement must not migrate data");
	migrated.migrate();
	const auto current = local / L"speeecial";
	require(migrated.directory() == current && !fs::exists(legacy),
	        "Legacy directory must be renamed");
	require(contents(current / L"updates.ini") == updates,
	        "Update preferences or throttle timestamp changed");
	require(contents(current / L"speeecial.log") == "legacy journal",
	        "Legacy journal lost");
	require(contents(current / L"settings.ini").find("[SnowDesk]") ==
	            std::string::npos,
	        "Legacy section remains after migration");
	wchar_t value[64]{};
	GetPrivateProfileStringW(L"speeecial", L"UnknownFutureKey", L"", value, 64,
	                         (current / L"settings.ini").c_str());
	require(std::wstring_view(value) == L"keep-me", "Unknown setting lost");
	require(GetPrivateProfileIntW(L"Other", L"Preserved", 0,
	                              (current / L"settings.ini").c_str()) == 42,
	        "Unrelated INI section lost");
	const auto converted = contents(current / L"settings.ini");
	migrated.migrate();
	require(contents(current / L"settings.ini") == converted,
	        "Repeated migration changed settings");
	migrated.save(settings);
	SettingsStore restarted(local);
	require(restarted.load().fps == 144 && restarted.load().flake_size == 9,
	        "Settings did not survive restart");
	const auto pending_local = root / L"pending startup";
	fs::create_directories(pending_local / L"SnowDesk");
	ini(pending_local / L"SnowDesk/settings.ini", L"SnowDesk", L"FPS", L"144");
	SettingsStore pending(pending_local);
	pending.save(pending.load());
	require(GetPrivateProfileIntW(
				L"SnowDesk", L"FPS", 0,
				(pending.directory() / L"settings.ini").c_str()) == 144,
	        "Saving before acknowledgement must remain legacy-compatible");

	const auto conflict = root / L"both";
	fs::create_directories(conflict / L"SnowDesk");
	fs::create_directories(conflict / L"speeecial");
	ini(conflict / L"SnowDesk/settings.ini", L"SnowDesk", L"FPS", L"240");
	ini(conflict / L"speeecial/settings.ini", L"speeecial", L"FPS", L"60");
	const auto old_settings = contents(conflict / L"SnowDesk/settings.ini");
	SettingsStore preferred(conflict);
	preferred.migrate();
	require(preferred.load().fps == 60, "Existing current profile overwritten");
	require(contents(conflict / L"SnowDesk/settings.ini") == old_settings,
	        "Conflicting legacy profile must be retained");

	ini(preferred.directory() / L"settings.ini", L"SnowDesk", L"FPS", L"144");
	ini(preferred.directory() / L"settings.ini", L"SnowDesk", L"FlakeSize",
	    L"8");
	std::ofstream(preferred.directory() / L"snowdesk.log") << "older journal";
	std::ofstream(preferred.directory() / L"speeecial.log") << "newer journal";
	preferred.migrate();
	require(preferred.load().fps == 60 && preferred.load().flake_size == 8,
	        "Section migration must preserve new keys and import missing keys");
	require(contents(preferred.directory() / L"speeecial.log") ==
	                "newer journal" &&
	            contents(preferred.directory() / L"speeecial.previous.0.log") ==
	                "older journal",
	        "Journal name collision lost data");
}

void failures(const fs::path &root)
{
	const auto local = root / L"retry";
	const auto legacy = local / L"SnowDesk";
	fs::create_directories(legacy);
	ini(legacy / L"settings.ini", L"SnowDesk", L"FPS", L"240");
	SettingsStore retry(local);
	{
		UniqueHandle locked(CreateFileW(legacy.c_str(), FILE_LIST_DIRECTORY,
		                                FILE_SHARE_READ | FILE_SHARE_WRITE,
		                                nullptr, OPEN_EXISTING,
		                                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		check_win(locked.get() != INVALID_HANDLE_VALUE, "Lock test profile");
		rejects([&] { retry.migrate(); },
		        "Locked directory must not be renamed");
		require(retry.directory() == legacy && retry.load().fps == 240 &&
		            !fs::exists(local / L"speeecial"),
		        "Failed rename switched to empty defaults");
	}
	// Interrupt conversion after the rename: the original INI remains valid,
	// and both this process and the next launch must keep the loaded profile.
	fs::create_directory(legacy / L"settings.migration.ini");
	rejects([&] { retry.migrate(); }, "Blocked staging path must fail safely");
	require(retry.directory() == local / L"speeecial" &&
	            retry.load().fps == 240,
	        "Partial migration lost the active profile");
	SettingsStore resumed(local);
	require(resumed.load().fps == 240, "Interrupted migration lost settings");
	fs::remove(resumed.directory() / L"settings.migration.ini");
	resumed.migrate();
	require(resumed.load().fps == 240, "Migration retry lost settings");
	{
		UniqueHandle locked(CreateFileW(
			(resumed.directory() / L"settings.ini").c_str(), GENERIC_READ, 0,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		check_win(locked.get() != INVALID_HANDLE_VALUE, "Lock test settings");
		rejects([&] { resumed.load(); },
		        "Unreadable INI must not use defaults");
	}
	fs::create_directories(root / L"invalid");
	std::ofstream(root / L"invalid/speeecial") << "retain";
	rejects([&] { SettingsStore invalid(root / L"invalid"); },
	        "A file at the profile path must not be treated as empty settings");
}

void registry(const std::wstring &id)
{
	const auto path = L"Software\\speeecial-migration-test-" + id;
	HKEY key = nullptr;
	check(HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
	                                         nullptr, 0, KEY_ALL_ACCESS,
	                                         nullptr, &key, nullptr)),
	      "Create isolated test registry key");
	try {
		require(!migration::autostart(key), "Fresh autostart must be disabled");
		const std::wstring old_command = L"\"C:\\old приложение.exe\"";
		const std::wstring new_command = L"\"D:\\new приложение.exe\"";
		const auto legacy = [&] {
			check(HRESULT_FROM_WIN32(RegSetValueExW(
					  key, L"SnowDesk", 0, REG_SZ,
					  reinterpret_cast<const BYTE *>(old_command.c_str()),
					  static_cast<DWORD>((old_command.size() + 1) *
			                             sizeof(wchar_t)))),
			      "Seed legacy autostart");
		};
		legacy();
		require(migration::autostart(key), "Legacy autostart not detected");
		migration::set_autostart(key, true, new_command);
		DWORD bytes = 0;
		require(RegGetValueW(key, nullptr, L"SnowDesk", RRF_RT_REG_SZ, nullptr,
		                     nullptr, &bytes) == ERROR_FILE_NOT_FOUND,
		        "Legacy registration remains after successful migration");
		wchar_t command[512]{};
		bytes = sizeof(command);
		check(HRESULT_FROM_WIN32(RegGetValueW(key, nullptr, L"speeecial",
		                                      RRF_RT_REG_SZ, nullptr, command,
		                                      &bytes)),
		      "Read new autostart");
		require(command == new_command, "Autostart command corrupted");
		legacy();
		HKEY read_only = nullptr;
		check(HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(),
		                                       0, KEY_READ, &read_only)),
		      "Open read-only test registry key");
		rejects([&] { migration::set_autostart(read_only, true, new_command); },
		        "Registry write failure must be reported");
		RegCloseKey(read_only);
		bytes = 0;
		require(RegGetValueW(key, nullptr, L"SnowDesk", RRF_RT_REG_SZ, nullptr,
		                     nullptr, &bytes) == ERROR_SUCCESS,
		        "Failed migration removed legacy autostart");
		migration::set_autostart(key, false, L"");
		require(!migration::autostart(key),
		        "Disabling must remove both values");
		migration::set_autostart(key, false, L"");
	} catch (...) {
		RegCloseKey(key);
		RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
		throw;
	}
	RegCloseKey(key);
	RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
}

void instances(const std::wstring &id)
{
	const auto current = L"Local\\speeecial.test.current." + id;
	const auto legacy = L"Local\\speeecial.test.legacy." + id;
	{
		UniqueHandle old(CreateMutexW(nullptr, FALSE, legacy.c_str()));
		migration::InstanceGuard guard(current.c_str(), legacy.c_str());
		require(!guard, "A running legacy instance must block startup");
	}
	{
		UniqueHandle running(CreateMutexW(nullptr, FALSE, current.c_str()));
		migration::InstanceGuard guard(current.c_str(), legacy.c_str());
		require(!guard, "A running current instance must block startup");
	}
	{
		migration::InstanceGuard guard(current.c_str(), legacy.c_str());
		require(static_cast<bool>(guard),
		        "First instance did not acquire guards");
		migration::InstanceGuard duplicate(current.c_str(), legacy.c_str());
		require(!duplicate, "Duplicate instance not blocked");
		UniqueHandle old(CreateMutexW(nullptr, FALSE, legacy.c_str()));
		require(old.get() && GetLastError() == ERROR_ALREADY_EXISTS,
		        "Legacy executable must also see the running new instance");
	}
	migration::InstanceGuard restarted(current.c_str(), legacy.c_str());
	require(static_cast<bool>(restarted), "Instance guard was not released");
}
}

int main()
{
	const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" +
	                std::to_wstring(GetTickCount64());
	const auto root =
		fs::temp_directory_path() / (L"speeecial migration тест " + id);
	try {
		require(fs::create_directory(root), "Test directory must be new");
		require(fs::canonical(root).parent_path() ==
		            fs::canonical(fs::temp_directory_path()),
		        "Test directory must remain inside the temporary directory");
		profiles(root);
		failures(root);
		registry(id);
		instances(id);
		// Only remove the unique directory created above; never user profiles.
		fs::remove_all(root);
		std::cout << checks << " Windows migration checks passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		std::wcerr << L"Test artifacts retained: " << root << L'\n';
		return 1;
	}
}
