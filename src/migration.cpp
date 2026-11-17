#include "migration.hpp"
#include <algorithm>
#include <vector>

namespace snow::migration
{
namespace
{
constexpr auto legacy_name = L"SnowDesk";
constexpr auto legacy_mutex = L"Local\\SnowDesk.Prototype.0.1";
constexpr auto legacy_log = L"snowdesk.log";
constexpr auto current_name = L"speeecial";

void readable(const std::filesystem::path &file)
{
	if (std::filesystem::exists(file)) {
		UniqueHandle handle(CreateFileW(
			file.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		check_win(handle.get() != INVALID_HANDLE_VALUE,
		          "Read settings for migration");
	}
}

bool acquire(UniqueHandle &handle, const wchar_t *name)
{
	handle.reset(CreateMutexW(nullptr, FALSE, name));
	const auto error = GetLastError();
	check_win(handle.get() != nullptr, "Create instance mutex");
	return error != ERROR_ALREADY_EXISTS;
}

std::vector<std::wstring> section(const std::filesystem::path &file,
                                  const wchar_t *name)
{
	std::vector<wchar_t> buffer(1024);
	for (;;) {
		const auto count = GetPrivateProfileSectionW(
			name, buffer.data(), static_cast<DWORD>(buffer.size()),
			file.c_str());
		if (count < buffer.size() - 2) {
			break;
		}
		if (buffer.size() >= 1024 * 1024) {
			throw std::runtime_error(
				"Settings section exceeds migration limit");
		}
		buffer.resize(buffer.size() * 2);
	}
	std::vector<std::wstring> result;
	for (const auto *entry = buffer.data(); *entry;
	     entry += wcslen(entry) + 1) {
		result.emplace_back(entry);
	}
	return result;
}

void migrate_section(const std::filesystem::path &file)
{
	if (!std::filesystem::exists(file)) {
		return;
	}
	readable(file);
	const auto legacy = section(file, legacy_name);
	if (legacy.empty()) {
		return;
	}
	const auto current = section(file, current_name);
	const auto temporary = file.parent_path() / L"settings.migration.ini";
	check_win(CopyFileW(file.c_str(), temporary.c_str(), FALSE),
	          "Copy settings for migration");
	try {
		for (const auto &entry : legacy) {
			const auto separator = entry.find(L'=');
			if (separator == std::wstring::npos || separator == 0) {
				throw std::runtime_error("Invalid legacy settings entry");
			}
			const auto key = entry.substr(0, separator);
			const bool exists = std::any_of(
				current.begin(), current.end(), [&](const auto &value) {
					return CompareStringOrdinal(
							   value.c_str(),
							   static_cast<int>(value.find(L'=')), key.c_str(),
							   static_cast<int>(key.size()),
							   TRUE) == CSTR_EQUAL;
				});
			if (!exists) {
				check_win(WritePrivateProfileStringW(
							  current_name, key.c_str(),
							  entry.c_str() + separator + 1, temporary.c_str()),
				          "Migrate settings entry");
			}
		}
		check_win(WritePrivateProfileStringW(legacy_name, nullptr, nullptr,
		                                     temporary.c_str()),
		          "Remove legacy settings section");
		// The profile-cache flush does not report success through its return
		// value. Flush the actual file before atomically replacing the profile.
		WritePrivateProfileStringW(nullptr, nullptr, nullptr,
		                           temporary.c_str());
		{
			UniqueHandle staged(CreateFileW(
				temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			check_win(staged.get() != INVALID_HANDLE_VALUE,
			          "Open migrated settings for flush");
			check_win(FlushFileBuffers(staged.get()),
			          "Flush migrated settings");
		}
		check_win(
			MoveFileExW(temporary.c_str(), file.c_str(),
		                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
			"Commit settings migration");
		WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
	} catch (...) {
		DeleteFileW(temporary.c_str());
		throw;
	}
}

bool registered(HKEY key, const wchar_t *name)
{
	DWORD bytes = 0;
	const auto status = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr,
	                                 nullptr, &bytes);
	if (status == ERROR_FILE_NOT_FOUND) {
		return false;
	}
	check(HRESULT_FROM_WIN32(status), "Read autostart registry value");
	return true;
}

void unregister(HKEY key, const wchar_t *name)
{
	const auto status = RegDeleteValueW(key, name);
	if (status != ERROR_FILE_NOT_FOUND) {
		check(HRESULT_FROM_WIN32(status), "Remove autostart registry value");
	}
}
}

InstanceGuard::InstanceGuard()
	: InstanceGuard(L"Local\\speeecial.Instance.1.0", legacy_mutex)
{
}

InstanceGuard::InstanceGuard(const wchar_t *current, const wchar_t *legacy)
{
	if (!acquire(legacy_, legacy) || !acquire(current_, current)) {
		current_.reset();
		legacy_.reset();
	}
}

InstanceGuard::operator bool() const noexcept
{
	return current_.get() && legacy_.get();
}

std::filesystem::path profile_directory(const std::filesystem::path &local)
{
	const auto current = local / current_name, legacy = local / legacy_name;
	if (std::filesystem::exists(current)) {
		if (!std::filesystem::is_directory(current)) {
			throw std::runtime_error("Settings path is not a directory");
		}
		return current;
	}
	if (std::filesystem::exists(legacy)) {
		if (!std::filesystem::is_directory(legacy)) {
			throw std::runtime_error("Legacy settings path is not a directory");
		}
		return legacy;
	}
	std::filesystem::create_directories(current);
	return current;
}

void migrate_profile(std::filesystem::path &directory)
{
	if (directory.filename() == legacy_name) {
		const auto current = directory.parent_path() / current_name;
		std::filesystem::rename(directory, current);
		directory = current;
	}
	migrate_section(directory / L"settings.ini");
	const auto log = directory / legacy_log;
	if (std::filesystem::exists(log)) {
		auto target = directory / L"speeecial.log";
		for (unsigned index = 0; std::filesystem::exists(target); ++index) {
			target = directory / (L"speeecial.previous." +
			                      std::to_wstring(index) + L".log");
		}
		std::filesystem::rename(log, target);
	}
}

int read_setting(const std::filesystem::path &file, const wchar_t *name,
                 int fallback)
{
	readable(file);
	wchar_t value[64]{};
	GetPrivateProfileStringW(current_name, name, L"\x01", value,
	                         static_cast<DWORD>(std::size(value)),
	                         file.c_str());
	const auto section_name = value[0] == L'\x01' ? legacy_name : current_name;
	return static_cast<int>(
		GetPrivateProfileIntW(section_name, name, fallback, file.c_str()));
}

const wchar_t *write_section(const std::filesystem::path &directory)
{
	// Until the rename succeeds, settings must remain readable by an older
	// executable, including one restored by the portable updater.
	return directory.filename() == legacy_name ? legacy_name : current_name;
}

bool autostart(HKEY key)
{
	return registered(key, current_name) || registered(key, legacy_name);
}

void set_autostart(HKEY key, bool enabled, const std::wstring &command)
{
	if (enabled) {
		check(HRESULT_FROM_WIN32(RegSetValueExW(
				  key, current_name, 0, REG_SZ,
				  reinterpret_cast<const BYTE *>(command.c_str()),
				  static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))),
		      "Write autostart registry value");
		// Never remove the working registration before the replacement exists.
		unregister(key, legacy_name);
	} else {
		unregister(key, legacy_name);
		unregister(key, current_name);
	}
}
}
