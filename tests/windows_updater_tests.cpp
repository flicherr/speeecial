// White-box process tests: compile the actual replacement/rollback engine once
// in this executable, which also acts as the helper. No GitHub/network access.
#include "../src/updater.cpp"
#include <iostream>

namespace
{
void require(bool value, const char *message)
{
	if (!value) {
		throw std::runtime_error(message);
	}
}

std::string digest(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary);
	snow::Hash hash;
	char bytes[65536];
	while (file) {
		file.read(bytes, sizeof(bytes));
		hash.add(reinterpret_cast<unsigned char *>(bytes),
		         static_cast<ULONG>(file.gcount()));
	}
	return hash.finish();
}

void silent_report(const std::wstring &) {}

void wait_process(HANDLE process, DWORD expected)
{
	require(WaitForSingleObject(process, 35000) == WAIT_OBJECT_0,
	        "Helper timeout");
	DWORD code = 99;
	GetExitCodeProcess(process, &code);
	require(code == expected, "Unexpected helper exit code");
}

void scenario(const std::filesystem::path &root,
              const std::filesystem::path &old_fixture,
              const std::filesystem::path &new_fixture, int mode)
{
	using namespace snow;
	namespace fs = std::filesystem;
	const auto directory = root / std::to_wstring(mode);
	fs::create_directory(directory);
	const auto stage = directory / (std::wstring(stage_prefix) + unique_id());
	fs::create_directory(stage);
	const auto target = directory / L"renamed приложение.exe";
	fs::copy_file(old_fixture, target);
	fs::copy_file(new_fixture, stage / L"payload.exe");
	fs::copy_file(executable(), stage / L"helper.exe");
	const auto old_hash = digest(target),
			   new_hash = digest(stage / L"payload.exe");
	validate_executable(stage / L"payload.exe", new_hash, SPEEECIAL_VERSION);
	bool rejected = false;
	try {
		validate_executable(stage / L"payload.exe", new_hash, "99.0.0");
	} catch (...) {
		rejected = true;
	}
	require(rejected, "Wrong embedded version accepted");
	UniqueHandle stop_parent(event());
	Process parent;
	launch(old_fixture, {L"--standin", number(stop_parent.get())},
	       {stop_parent.get()}, parent);
	HANDLE inherited = nullptr;
	check_win(DuplicateHandle(GetCurrentProcess(), parent.info.hProcess,
	                          GetCurrentProcess(), &inherited, SYNCHRONIZE,
	                          TRUE, 0),
	          "Duplicate test parent");
	UniqueHandle parent_handle(inherited), ready(event()),
		commit_signal(event());
	if (mode == 1) {
		SetEnvironmentVariableW(L"SNOW_UPDATE_FIXTURE_FAIL", L"1");
	}
	Process helper;
	launch(stage / L"helper.exe",
	       {L"--snow-apply", target.wstring(), number(parent_handle.get()),
	        number(ready.get()), number(commit_signal.get()),
	        widen(mode == 4 ? std::string(64, '0') : new_hash)},
	       {parent_handle.get(), ready.get(), commit_signal.get()}, helper);
	SetEnvironmentVariableW(L"SNOW_UPDATE_FIXTURE_FAIL", nullptr);
	if (mode == 4) {
		SetEvent(stop_parent.get());
		wait_process(helper.info.hProcess, 9);
		require(digest(target) == old_hash, "Bad digest changed target");
		return;
	}
	HANDLE waiting[] = {ready.get(), helper.info.hProcess};
	require(WaitForMultipleObjects(2, waiting, FALSE, 15000) == WAIT_OBJECT_0,
	        "Helper never became ready");
	require(digest(target) == old_hash, "Target changed before commit");
	UniqueHandle locked;
	if (mode == 2) {
		locked.reset(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ,
		                         nullptr, OPEN_EXISTING, 0, nullptr));
	}
	if (mode == 5) {
		std::ofstream modify(stage / L"payload.exe", std::ios::app);
		modify << "tampered after handshake";
	}
	if (mode != 3) {
		SetEvent(commit_signal.get());
		Sleep(100);
		require(digest(target) == old_hash,
		        "Target changed while parent alive");
	}
	SetEvent(stop_parent.get());
	wait_process(parent.info.hProcess, 0);
	wait_process(helper.info.hProcess, mode == 0 ? 0 : (mode == 3 ? 2 : 1));
	locked.reset();
	require(digest(target) == (mode == 0 ? new_hash : old_hash),
	        "Wrong target after transaction");
	if (mode == 0 || mode == 1 || mode == 2) {
		require(digest(stage / L"previous.exe") == old_hash,
		        "Backup lost or changed");
	}
	if (mode == 0) {
		require(fs::exists(stage / L"complete.ok"),
		        "Successful transaction not marked");
	}
	if (mode == 3) {
		require(!fs::exists(stage / L"previous.exe"),
		        "Cancelled transaction touched backup");
	}
}
}

int wmain(int argc, wchar_t **argv)
{
	try {
		if (argc == 7 && std::wstring_view(argv[1]) == L"--snow-apply") {
			const std::wstring hash(argv[6]);
			return snow::apply_update(
				argv[2], snow::handle_argument(argv[3]),
				snow::handle_argument(argv[4]), snow::handle_argument(argv[5]),
				std::string(hash.begin(), hash.end()), silent_report);
		}
		require(argc == 3, "Pass old and new fixture executables");
		wchar_t temp[32768]{};
		GetTempPathW(32768, temp);
		const auto root = std::filesystem::path(temp) /
		                  (L"speeecial update тест " + snow::unique_id());
		std::filesystem::create_directory(root);
		try {
			for (int mode = 0; mode < 6; ++mode) {
				scenario(root, std::filesystem::absolute(argv[1]),
				         std::filesystem::absolute(argv[2]), mode);
			}
		} catch (...) {
			std::wcerr << L"Test artifacts retained: " << root << L'\n';
			throw;
		}
		// Only delete the unique directory created by this test, after all
		// spawned processes have finished and released their executable files.
		std::error_code error;
		std::filesystem::remove_all(root, error);
		std::cout << "6 Windows update transaction scenarios passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 9;
	}
}
