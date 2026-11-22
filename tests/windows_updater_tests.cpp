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

void startup_acknowledgement()
{
	using namespace snow;
	require(!update_startup_pending(), "Unexpected pending startup");
	UniqueHandle signal(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	check_win(signal.get() != nullptr, "Create startup acknowledgement event");
	check_win(DuplicateHandle(GetCurrentProcess(), signal.get(),
	                          GetCurrentProcess(), &startup_event,
	                          EVENT_MODIFY_STATE, FALSE, 0),
	          "Duplicate startup acknowledgement event");
	require(update_startup_pending(),
	        "Startup must remain pending before signal");
	acknowledge_update_startup();
	require(!update_startup_pending() &&
	            WaitForSingleObject(signal.get(), 0) == WAIT_OBJECT_0,
	        "Successful acknowledgement must clear pending startup");
	startup_event = INVALID_HANDLE_VALUE;
	acknowledge_update_startup();
	require(update_startup_pending(),
	        "Failed acknowledgement must keep startup pending for rollback");
	startup_event = nullptr;
}

void update_indicators()
{
	using namespace snow;
	using Status = Updater::Status;
	Updater::View initial;
	for (auto status :
	     {Status::idle, Status::checking, Status::current, Status::failed}) {
		initial.status = status;
		require(!initial.update_available(),
		        "An unknown release must not show an update indicator");
	}
	initial.status = Status::checking;
	initial.manual = true;
	const auto json =
		R"({"tag_name":"v99.0.0","draft":false,"prerelease":false,"assets":[{"name":"speeecial-windows-x64.exe","state":"uploaded","digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":4096,"browser_download_url":"https://github.com/flicherr/speeecial/releases/download/v99.0.0/speeecial-windows-x64.exe"}]})";
	auto available = check_release(initial, [json] { return json; });
	require(available.status == Status::available &&
	            available.update_available() &&
	            available.release->installable() && available.manual &&
	            !available.busy(),
	        "A confirmed release must enable the update indicator and install");
	for (auto status : {Status::available, Status::checking,
	                    Status::downloading, Status::ready, Status::failed}) {
		available.status = status;
		require(available.update_available(),
		        "A known release must stay marked while busy or after failure");
		require(
			available.busy() ==
				(status == Status::checking || status == Status::downloading ||
		         status == Status::ready),
			"Update indicators must not change which states block commands");
	}
	available.status = Status::checking;
	available.manual = false;
	auto failed_fetch = []() -> std::string {
		throw std::runtime_error("Network unavailable");
	};
	const auto failed = check_release(available, failed_fetch);
	require(failed.status == Status::failed && failed.update_available() &&
	            failed.release->tag == "v99.0.0" && !failed.manual &&
	            !failed.busy(),
	        "A failed recheck must retain the known release and permit retry");
	const auto malformed = check_release(available, [] { return "{broken}"; });
	require(malformed.status == Status::failed &&
	            malformed.update_available() &&
	            malformed.release->tag == "v99.0.0",
	        "Invalid metadata must not erase an earlier confirmed update");
	const auto first_failure = check_release(initial, failed_fetch);
	require(first_failure.status == Status::failed &&
	            !first_failure.update_available() && first_failure.manual,
	        "A first-check failure must not invent a release");
	const auto current = check_release(available, [] {
		return "{\"tag_name\":\"v" SPEEECIAL_VERSION
			   "\",\"draft\":false,\"prerelease\":false,\"assets\":[]}";
	});
	require(current.status == Status::current && !current.update_available() &&
	            !current.busy(),
	        "A successful current-version response must clear the indicator");
	const auto manual = check_release(initial, [] {
		return R"({"tag_name":"v99.0.1","draft":false,"prerelease":false,"assets":[]})";
	});
	require(manual.status == Status::available && manual.update_available() &&
	            !manual.release->installable(),
	        "Manual-download releases must also show the update indicator");
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
		startup_acknowledgement();
		update_indicators();
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
