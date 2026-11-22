#include "updater.hpp"
#include "version.hpp"
#include "win_util.hpp"
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <shellapi.h>
#include <thread>
#include <vector>
#include <winhttp.h>

namespace snow
{
namespace
{
namespace fs = std::filesystem;
constexpr auto stage_prefix = L".speeecial-update-";
HANDLE startup_event = nullptr;

fs::path executable()
{
	std::wstring text(32768, L'\0');
	DWORD count = GetModuleFileNameW(nullptr, text.data(),
	                                 static_cast<DWORD>(text.size()));
	check_win(count && count < text.size(), "Get executable path");
	text.resize(count);
	return text;
}

std::wstring unique_id()
{
	GUID guid{};
	check(CoCreateGuid(&guid), "Create update ID");
	wchar_t text[40]{};
	StringFromGUID2(guid, text, 40);
	return text;
}

std::int64_t unix_time()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
			   std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

std::wstring number(HANDLE handle)
{
	return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
}

HANDLE handle_argument(const wchar_t *text)
{
	std::wstring value(text);
	std::size_t end = 0;
	auto n = std::stoull(value, &end);
	if (end != value.size() || !n || n > UINTPTR_MAX) {
		throw std::runtime_error("Invalid update handle");
	}
	HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(n));
	DWORD flags = 0;
	check_win(GetHandleInformation(handle, &flags), "Validate update handle");
	check_win((flags & HANDLE_FLAG_INHERIT) != 0,
	          "Expected inherited update handle");
	return handle;
}

struct Process {
	PROCESS_INFORMATION info{};

	~Process()
	{
		if (info.hThread) {
			CloseHandle(info.hThread);
		}
		if (info.hProcess) {
			CloseHandle(info.hProcess);
		}
	}
};

void launch(const fs::path &exe, const std::vector<std::wstring> &args,
            const std::vector<HANDLE> &handles, Process &out)
{
	std::wstring command = updates::quote_argument(exe.wstring());
	for (const auto &arg : args) {
		command += L" " + updates::quote_argument(arg);
	}
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = handles.empty() ? sizeof(STARTUPINFOW) : sizeof(si);
	std::vector<unsigned char> attributes;
	if (!handles.empty()) {
		SIZE_T size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		attributes.resize(size);
		si.lpAttributeList =
			reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
		check_win(
			InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size),
			"Initialize update handles");
	}

	struct Cleanup {
		LPPROC_THREAD_ATTRIBUTE_LIST list;

		~Cleanup()
		{
			if (list) {
				DeleteProcThreadAttributeList(list);
			}
		}
	} cleanup{si.lpAttributeList};

	if (!handles.empty()) {
		check_win(UpdateProcThreadAttribute(
					  si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
					  const_cast<HANDLE *>(handles.data()),
					  handles.size() * sizeof(HANDLE), nullptr, nullptr),
		          "Restrict inherited handles");
	}
	check_win(
		CreateProcessW(
			exe.c_str(), command.data(), nullptr, nullptr, !handles.empty(),
			(handles.empty() ? 0 : EXTENDED_STARTUPINFO_PRESENT) |
				CREATE_NO_WINDOW,
			nullptr, exe.parent_path().c_str(), &si.StartupInfo, &out.info),
		"Start update process");
}

HANDLE event()
{
	SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
	auto result = CreateEventW(&security, TRUE, FALSE, nullptr);
	check_win(result != nullptr, "Create update event");
	return result;
}

class Hash {
	BCRYPT_ALG_HANDLE algorithm{};
	BCRYPT_HASH_HANDLE hash{};

  public:
	Hash()
	{
		check(static_cast<HRESULT>(BCryptOpenAlgorithmProvider(
				  &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)),
		      "SHA256 provider");
		auto status =
			BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
		if (status < 0) {
			BCryptCloseAlgorithmProvider(algorithm, 0);
			algorithm = nullptr;
			check(static_cast<HRESULT>(status), "SHA256 hash");
		}
	}

	~Hash()
	{
		if (hash) {
			BCryptDestroyHash(hash);
		}
		if (algorithm) {
			BCryptCloseAlgorithmProvider(algorithm, 0);
		}
	}

	void add(const unsigned char *data, ULONG size)
	{
		check(static_cast<HRESULT>(
				  BCryptHashData(hash, const_cast<PUCHAR>(data), size, 0)),
		      "SHA256 data");
	}

	std::string finish()
	{
		unsigned char bytes[32]{};
		check(static_cast<HRESULT>(BCryptFinishHash(hash, bytes, 32, 0)),
		      "SHA256 finish");
		constexpr char hex[] = "0123456789abcdef";
		std::string result;
		for (auto b : bytes) {
			result += hex[b >> 4];
			result += hex[b & 15];
		}
		return result;
	}
};

void validate_executable(const fs::path &path, const std::string &expected,
                         std::string_view expected_version = {})
{
	UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
	                              nullptr, OPEN_EXISTING,
	                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
	check_win(file.get() != INVALID_HANDLE_VALUE, "Open downloaded executable");
	Hash hash;
	unsigned char buffer[65536];
	DWORD read = 0;
	std::uint64_t total = 0;
	do {
		check_win(ReadFile(file.get(), buffer, sizeof(buffer), &read, nullptr),
		          "Read downloaded executable");
		total += read;
		if (total > updates::max_download) {
			throw std::runtime_error("Update exceeds size limit");
		}
		hash.add(buffer, read);
	} while (read);
	if (hash.finish() != expected) {
		throw std::runtime_error("SHA-256 mismatch; update was not installed");
	}
	LARGE_INTEGER offset{};
	check_win(SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN),
	          "Read PE header");
	IMAGE_DOS_HEADER dos{};
	check_win(ReadFile(file.get(), &dos, sizeof(dos), &read, nullptr),
	          "Read DOS header");
	if (read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
	    dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
	    static_cast<std::uint64_t>(dos.e_lfanew) + sizeof(DWORD) +
	            sizeof(IMAGE_FILE_HEADER) >
	        total) {
		throw std::runtime_error("Invalid update executable");
	}
	offset.QuadPart = dos.e_lfanew;
	check_win(SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN),
	          "Seek PE header");
	DWORD signature = 0;
	IMAGE_FILE_HEADER header{};
	check_win(
		ReadFile(file.get(), &signature, sizeof(signature), &read, nullptr) &&
			read == sizeof(signature),
		"Read PE signature");
	check_win(ReadFile(file.get(), &header, sizeof(header), &read, nullptr) &&
	              read == sizeof(header),
	          "Read PE machine");
	if (signature != IMAGE_NT_SIGNATURE ||
	    header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
	    !(header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
	    (header.Characteristics & IMAGE_FILE_DLL) ||
	    header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
		throw std::runtime_error("Update must be a Windows x64 executable");
	}
	IMAGE_OPTIONAL_HEADER64 optional{};
	check_win(
		ReadFile(file.get(), &optional, sizeof(optional), &read, nullptr) &&
			read == sizeof(optional),
		"Read PE optional header");
	if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
	    (optional.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI &&
	     optional.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI)) {
		throw std::runtime_error("Invalid Windows executable subsystem");
	}
	if (!expected_version.empty()) {
		auto required = updates::version(expected_version);
		DWORD ignored = 0,
			  size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
		if (!required || !size || size > 1024 * 1024) {
			throw std::runtime_error("Missing executable version");
		}
		std::vector<unsigned char> data(size);
		check_win(GetFileVersionInfoW(path.c_str(), 0, size, data.data()),
		          "Read executable version");
		VS_FIXEDFILEINFO *info = nullptr;
		UINT bytes = 0;
		check_win(VerQueryValueW(data.data(), L"\\",
		                         reinterpret_cast<void **>(&info), &bytes),
		          "Read fixed version");
		if (!info || bytes < sizeof(*info) || info->dwSignature != 0xfeef04bd ||
		    updates::Version{HIWORD(info->dwFileVersionMS),
		                     LOWORD(info->dwFileVersionMS),
		                     HIWORD(info->dwFileVersionLS)} != *required ||
		    LOWORD(info->dwFileVersionLS) != 0) {
			throw std::runtime_error(
				"Executable version does not match the release tag");
		}
	}
}

void clean_stage(const fs::path &stage) noexcept
{
	// Never recurse through arbitrary directories or remove unknown files.
	for (auto name :
	     {L"payload.exe", L"previous.exe", L"helper.exe", L"complete.ok"}) {
		DeleteFileW((stage / name).c_str());
	}
	RemoveDirectoryW(stage.c_str());
}

void cleanup_completed(const fs::path &target) noexcept
{
	try {
		std::error_code error;
		for (const auto &entry :
		     fs::directory_iterator(target.parent_path(), error)) {
			const auto name = entry.path().filename().wstring();
			DWORD attributes = GetFileAttributesW(entry.path().c_str());
			if (!name.starts_with(stage_prefix) ||
			    attributes == INVALID_FILE_ATTRIBUTES ||
			    !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
			    (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
				continue;
			}
			// Marker includes the target filename, so parallel portable copies
			// in one directory do not remove each other's rollback files.
			const auto marker_path = entry.path() / L"complete.ok";
			const auto marker_size = fs::file_size(marker_path, error);
			if (error || marker_size > 32768) {
				error.clear();
				continue;
			}
			std::ifstream marker(marker_path, std::ios::binary);
			std::string owner((std::istreambuf_iterator<char>(marker)), {});
			marker.close();
			const auto target_name = target.filename().u8string();
			if (owner ==
			    std::string(reinterpret_cast<const char *>(target_name.data()),
			                target_name.size())) {
				clean_stage(entry.path());
			}
		}
	} catch (...) {
	}
}

struct HttpHandle {
	HINTERNET value{};

	~HttpHandle()
	{
		if (value) {
			WinHttpCloseHandle(value);
		}
	}
};

// All WinHTTP operations, including handle destruction, stay on this worker.
// A synchronous request must NOT be closed concurrently from the UI thread.
void download(const std::wstring &url,
              const std::function<void(const unsigned char *, DWORD)> &sink,
              const std::atomic_bool &cancel)
{
	auto start = GetTickCount64();
	HttpHandle session{WinHttpOpen(
		L"speeecial/" SPEEECIAL_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
	check_win(session.value != nullptr, "Open HTTPS session");
	check_win(WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000),
	          "Set HTTPS timeouts");
	std::wstring current = url;
	for (unsigned redirect = 0; redirect < 6; ++redirect) {
		if (cancel.load() || GetTickCount64() - start > 180000) {
			throw std::runtime_error("Update cancelled or timed out");
		}
		URL_COMPONENTS parts{sizeof(parts)};
		parts.dwHostNameLength = parts.dwUrlPathLength =
			parts.dwExtraInfoLength = parts.dwUserNameLength =
				parts.dwPasswordLength = static_cast<DWORD>(-1);
		check_win(WinHttpCrackUrl(current.c_str(), 0, 0, &parts),
		          "Parse HTTPS URL");
		std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != 443 ||
		    parts.dwUserNameLength || parts.dwPasswordLength ||
		    (host != L"api.github.com" && host != L"github.com" &&
		     host != L"release-assets.githubusercontent.com" &&
		     host != L"objects.githubusercontent.com")) {
			throw std::runtime_error("Unexpected update download host");
		}
		std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
		if (parts.dwExtraInfoLength) {
			path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		}
		HttpHandle connection{
			WinHttpConnect(session.value, host.c_str(), 443, 0)};
		check_win(connection.value != nullptr, "Connect to GitHub");
		HttpHandle request{WinHttpOpenRequest(
			connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
		check_win(request.value != nullptr, "Create GitHub request");
		DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
		                 WINHTTP_DISABLE_AUTHENTICATION;
		check_win(WinHttpSetOption(request.value,
		                           WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
		                           sizeof(disabled)),
		          "Disable automatic redirects");
		DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
		check_win(WinHttpSetOption(request.value,
		                           WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon,
		                           sizeof(autologon)),
		          "Disable ambient credentials");
		const wchar_t *headers =
			host == L"api.github.com"
				? L"Accept: "
		          L"application/vnd.github+json\r\nX-GitHub-Api-Version: "
		          L"2022-11-28\r\n"
				: L"Accept: application/octet-stream\r\n";
		check_win(WinHttpSendRequest(request.value, headers,
		                             static_cast<DWORD>(-1),
		                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0),
		          "Send GitHub request");
		if (cancel.load()) {
			throw std::runtime_error("Update cancelled");
		}
		check_win(WinHttpReceiveResponse(request.value, nullptr),
		          "Receive GitHub response");
		DWORD status = 0, length = sizeof(status);
		check_win(WinHttpQueryHeaders(request.value,
		                              WINHTTP_QUERY_STATUS_CODE |
		                                  WINHTTP_QUERY_FLAG_NUMBER,
		                              WINHTTP_HEADER_NAME_BY_INDEX, &status,
		                              &length, WINHTTP_NO_HEADER_INDEX),
		          "Read HTTP status");
		if (status == 301 || status == 302 || status == 303 || status == 307 ||
		    status == 308) {
			wchar_t location[16384]{};
			length = sizeof(location);
			check_win(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION,
			                              WINHTTP_HEADER_NAME_BY_INDEX,
			                              location, &length,
			                              WINHTTP_NO_HEADER_INDEX),
			          "Read download redirect");
			current = location;
			continue;
		}
		if (status != 200) {
			throw std::runtime_error(
				"GitHub HTTP " + std::to_string(status) +
				(status == 403 || status == 429
			         ? " (rate limit or access denied; retry later)"
			         : ""));
		}
		unsigned char buffer[65536];
		DWORD count = 0;
		do {
			if (cancel.load() || GetTickCount64() - start > 180000) {
				throw std::runtime_error("Update cancelled or timed out");
			}
			check_win(
				WinHttpReadData(request.value, buffer, sizeof(buffer), &count),
				"Read GitHub download");
			if (count) {
				sink(buffer, count);
			}
		} while (count);
		return;
	}
	throw std::runtime_error("Too many update redirects");
}

void report_update_error(const std::wstring &message)
{
	MessageBoxW(nullptr, message.c_str(), L"speeecial — обновление отменено",
	            MB_OK | MB_ICONWARNING);
}

int apply_update(const fs::path &target, HANDLE parent, HANDLE ready,
                 HANDLE commit_event, const std::string &hash,
                 void (*report)(const std::wstring &) = report_update_error)
{
	const auto stage = executable().parent_path();
	if (!stage.filename().wstring().starts_with(stage_prefix) ||
	    target.parent_path() != stage.parent_path() ||
	    _wcsicmp(target.extension().c_str(), L".exe") != 0 ||
	    (GetFileAttributesW(stage.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) {
		throw std::runtime_error("Invalid update staging directory");
	}
	const auto payload = stage / L"payload.exe",
			   backup = stage / L"previous.exe";
	validate_executable(payload, hash);
	check_win(SetEvent(ready), "Signal updater ready");
	HANDLE wait_commit[] = {commit_event, parent};
	if (WaitForMultipleObjects(2, wait_commit, FALSE, 60000) != WAIT_OBJECT_0) {
		return 2;
	}
	if (WaitForSingleObject(parent, 30000) != WAIT_OBJECT_0) {
		return 3;
	}
	bool replaced = false;
	try {
		validate_executable(payload, hash);
		// Keep an independent backup even if replacement itself fails.
		check_win(CopyFileW(target.c_str(), backup.c_str(), TRUE),
		          "Save previous executable");
		check_win(ReplaceFileW(target.c_str(), payload.c_str(), nullptr, 0,
		                       nullptr, nullptr),
		          "Replace executable");
		replaced = true;
		validate_executable(target, hash);
		UniqueHandle started(event());
		Process child;
		launch(target, {L"--snow-updated", number(started.get())},
		       {started.get()}, child);
		HANDLE wait_started[] = {started.get(), child.info.hProcess};
		if (WaitForMultipleObjects(2, wait_started, FALSE, 30000) !=
		    WAIT_OBJECT_0) {
			if (WaitForSingleObject(child.info.hProcess, 0) == WAIT_TIMEOUT) {
				check_win(TerminateProcess(child.info.hProcess, 1),
				          "Stop failed update");
				check_win(WaitForSingleObject(child.info.hProcess, 10000) ==
				              WAIT_OBJECT_0,
				          "Wait for failed update");
			}
			throw std::runtime_error("New version did not confirm startup; "
			                         "restoring previous version");
		}
		std::ofstream marker(stage / L"complete.ok", std::ios::binary);
		const auto name = target.filename().u8string();
		marker.write(reinterpret_cast<const char *>(name.data()),
		             static_cast<std::streamsize>(name.size()));
		return 0;
	} catch (const std::exception &error) {
		// Copy, do not move the backup: recovery material survives any failure.
		if (replaced || !fs::exists(target)) {
			check_win(CopyFileW(backup.c_str(), target.c_str(), FALSE),
			          "Restore previous executable (backup retained in update "
			          "folder)");
		}
		Process previous;
		launch(target, {}, {}, previous);
		report(widen(error.what()));
		return 1;
	}
}

template <class Fetch>
Updater::View check_release(Updater::View result, Fetch &&fetch)
{
	try {
		// Replace a known release only after fetching and parsing succeed.
		result.release = updates::parse_release(fetch(), SPEEECIAL_VERSION);
		result.status = result.release ? Updater::Status::available
		                               : Updater::Status::current;
		result.detail = result.release
		                    ? L"Доступна " + widen(result.release->tag)
		                    : L"Установлена актуальная версия";
	} catch (const std::exception &error) {
		result.status = Updater::Status::failed;
		result.detail = widen(error.what());
	}
	return result;
}
}

struct Updater::State {
	mutable std::mutex mutex;
	std::atomic_bool cancel = false;
	HWND owner{};
	View view;
	fs::path target;
	UniqueHandle commit_event;

	void publish(View next)
	{
		std::lock_guard lock(mutex);
		next.revision = view.revision + 1;
		view = std::move(next);
		if (owner) {
			PostMessageW(owner, message_update, 0, 0);
		}
	}
};

Updater::Updater(HWND owner, const fs::path &directory)
	: state_(std::make_shared<State>()),
	  preferences_(directory / L"updates.ini")
{
	state_->owner = owner;
	state_->target = executable();
	automatic_ = GetPrivateProfileIntW(L"Updates", L"Automatic", 1,
	                                   preferences_.c_str()) != 0;
	// This independent worker owns its state. Shutdown never joins DNS/network
	// waits, and no callback can refer to a destroyed App or HWND.
	auto state = state_;
	std::thread([state] { cleanup_completed(state->target); }).detach();
	wchar_t last[64]{};
	GetPrivateProfileStringW(L"Updates", L"LastAttempt", L"0", last, 64,
	                         preferences_.c_str());
	std::int64_t last_time = 0;
	try {
		last_time = std::stoll(last);
	} catch (...) {
	}
	if (automatic_ && updates::automatic_due(unix_time(), last_time)) {
		check(false);
	}
}

Updater::~Updater()
{
	state_->cancel.store(true);
	std::lock_guard lock(state_->mutex);
	state_->owner = nullptr;
}

Updater::View Updater::view() const
{
	std::lock_guard lock(state_->mutex);
	return state_->view;
}

void Updater::set_automatic(bool enabled)
{
	check_win(WritePrivateProfileStringW(L"Updates", L"Automatic",
	                                     enabled ? L"1" : L"0",
	                                     preferences_.c_str()),
	          "Save update preference");
	automatic_ = enabled;
}

void Updater::check(bool manual)
{
	auto pending = view();
	if (pending.busy()) {
		return;
	}
	const auto now = std::to_wstring(unix_time());
	check_win(WritePrivateProfileStringW(L"Updates", L"LastAttempt",
	                                     now.c_str(), preferences_.c_str()),
	          "Save last update check");
	pending.status = Status::checking;
	pending.manual = manual;
	pending.progress = 0;
	pending.detail.clear();
	state_->publish(pending);
	auto state = state_;
	try {
		std::thread([state, pending] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			auto result = check_release(pending, [&] {
				std::string json;
				download(
					L"https://api.github.com/repos/flicherr/speeecial/releases/"
					L"latest",
					[&](const unsigned char *data, DWORD count) {
						if (json.size() + count > 2 * 1024 * 1024) {
							throw std::runtime_error(
								"Release metadata exceeds limit");
						}
						json.append(reinterpret_cast<const char *>(data),
					                count);
					},
					state->cancel);
				return json;
			});
			state->publish(std::move(result));
		}).detach();
	} catch (...) {
		pending.status = Status::failed;
		pending.detail = L"Не удалось запустить проверку";
		state_->publish(pending);
		throw;
	}
}

void Updater::install()
{
	auto previous = view();
	if (previous.busy() || !previous.release ||
	    !previous.release->installable()) {
		return;
	}
	const auto release = *previous.release;
	auto state = state_;
	previous.status = Status::downloading;
	previous.manual = true;
	previous.progress = 0;
	state_->publish(previous);
	try {
		std::thread([state, release] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			View result;
			result.release = release;
			result.manual = true;
			fs::path stage;
			bool stage_created = false;
			try {
				stage = state->target.parent_path() /
				        (std::wstring(stage_prefix) + unique_id());
				check_win(CreateDirectoryW(stage.c_str(), nullptr),
				          "Create update folder (move the portable app to a "
				          "writable folder or download manually)");
				stage_created = true;
				SetFileAttributesW(stage.c_str(), FILE_ATTRIBUTE_HIDDEN);
				const auto payload = stage / L"payload.exe";
				{
					UniqueHandle file(CreateFileW(
						payload.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
						FILE_ATTRIBUTE_NORMAL, nullptr));
					check_win(file.get() != INVALID_HANDLE_VALUE,
					          "Create update file");
					std::uint64_t total = 0;
					Hash hash;
					download(
						widen(release.url),
						[&](const unsigned char *data, DWORD count) {
							total += count;
							if (total > release.size ||
						        total > updates::max_download) {
								throw std::runtime_error(
									"Unexpected update size");
							}
							DWORD written = 0;
							check_win(WriteFile(file.get(), data, count,
						                        &written, nullptr) &&
						                  written == count,
						              "Write update file");
							hash.add(data, count);
							std::lock_guard lock(state->mutex);
							state->view.progress = static_cast<unsigned>(
								total * 100 / release.size);
						},
						state->cancel);
					if (total != release.size ||
					    hash.finish() != release.sha256) {
						throw std::runtime_error(
							"Update size or SHA-256 mismatch; executable "
						    "unchanged");
					}
					check_win(FlushFileBuffers(file.get()),
					          "Flush downloaded update");
				}
				validate_executable(payload, release.sha256, release.tag);
				if (state->cancel.load()) {
					throw std::runtime_error("Update cancelled");
				}
				const auto helper = stage / L"helper.exe";
				check_win(
					CopyFileW(state->target.c_str(), helper.c_str(), TRUE),
					"Copy updater helper");
				UniqueHandle parent;
				HANDLE duplicated = nullptr;
				check_win(DuplicateHandle(GetCurrentProcess(),
				                          GetCurrentProcess(),
				                          GetCurrentProcess(), &duplicated,
				                          SYNCHRONIZE, TRUE, 0),
				          "Pass parent process handle");
				parent.reset(duplicated);
				UniqueHandle ready(event()), commit_signal(event());
				Process helper_process;
				launch(helper,
				       {L"--snow-apply", state->target.wstring(),
				        number(parent.get()), number(ready.get()),
				        number(commit_signal.get()), widen(release.sha256)},
				       {parent.get(), ready.get(), commit_signal.get()},
				       helper_process);
				HANDLE wait_ready[] = {ready.get(),
				                       helper_process.info.hProcess};
				if (WaitForMultipleObjects(2, wait_ready, FALSE, 15000) !=
				        WAIT_OBJECT_0 ||
				    state->cancel.load()) {
					if (WaitForSingleObject(helper_process.info.hProcess, 0) ==
					    WAIT_TIMEOUT) {
						TerminateProcess(helper_process.info.hProcess, 1);
						WaitForSingleObject(helper_process.info.hProcess, 5000);
					}
					throw std::runtime_error("Update helper could not start");
				}
				{
					std::lock_guard lock(state->mutex);
					HANDLE signal = nullptr;
					check_win(DuplicateHandle(GetCurrentProcess(),
					                          commit_signal.get(),
					                          GetCurrentProcess(), &signal,
					                          EVENT_MODIFY_STATE, FALSE, 0),
					          "Save update commit event");
					state->commit_event.reset(signal);
				}
				result.status = Status::ready;
				result.detail = L"Обновление готово; перезапуск…";
			} catch (const std::exception &error) {
				result.status = Status::failed;
				result.detail = widen(error.what());
				if (stage_created) {
					clean_stage(stage);
				}
			}
			state->publish(std::move(result));
		}).detach();
	} catch (...) {
		previous.status = Status::failed;
		previous.detail = L"Не удалось начать загрузку";
		state_->publish(previous);
		throw;
	}
}

bool Updater::commit()
{
	std::lock_guard lock(state_->mutex);
	if (state_->view.status != Status::ready || !state_->commit_event.get()) {
		return false;
	}
	check_win(SetEvent(state_->commit_event.get()), "Commit update");
	return true;
}

std::optional<int> run_update_mode()
{
	int count = 0;
	auto args = CommandLineToArgvW(GetCommandLineW(), &count);
	check_win(args != nullptr, "Read command line");

	struct Cleanup {
		LPWSTR *args;

		~Cleanup()
		{
			LocalFree(args);
		}
	} cleanup{args};

	if (count < 2) {
		return {};
	}
	if (std::wstring_view(args[1]) == L"--snow-updated") {
		if (count != 3) {
			throw std::runtime_error("Invalid update startup arguments");
		}
		startup_event = handle_argument(args[2]);
		return {};
	}
	if (std::wstring_view(args[1]) != L"--snow-apply") {
		return {};
	}
	if (count != 7) {
		throw std::runtime_error("Invalid updater arguments");
	}
	UniqueHandle parent(handle_argument(args[3])),
		ready(handle_argument(args[4])),
		commit_signal(handle_argument(args[5]));
	std::wstring wide_hash(args[6]);
	std::string hash(wide_hash.begin(), wide_hash.end());
	if (hash.size() != 64 ||
	    hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
		throw std::runtime_error("Invalid update digest");
	}
	return apply_update(fs::path(args[2]), parent.get(), ready.get(),
	                    commit_signal.get(), hash);
}

void acknowledge_update_startup() noexcept
{
	if (startup_event && SetEvent(startup_event)) {
		CloseHandle(startup_event);
		startup_event = nullptr;
	}
}

bool update_startup_pending() noexcept
{
	return startup_event != nullptr;
}
}
