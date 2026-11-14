#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

int wmain(int argc, wchar_t **argv)
{
	if (argc == 3 && std::wstring_view(argv[1]) == L"--standin") {
		auto handle = reinterpret_cast<HANDLE>(
			static_cast<std::uintptr_t>(std::stoull(argv[2])));
		return WaitForSingleObject(handle, 30000) == WAIT_OBJECT_0 ? 0 : 1;
	}
#ifdef SNOW_UPDATE_NEW_FIXTURE
	wchar_t fail[8]{};
	if (GetEnvironmentVariableW(L"SNOW_UPDATE_FIXTURE_FAIL", fail, 8) &&
	    fail[0] == L'1') {
		return 23;
	}
	constexpr auto marker = L"new-started.txt";
#else
	constexpr auto marker = L"old-started.txt";
#endif
	wchar_t path[32768]{};
	GetModuleFileNameW(nullptr, path, 32768);
	std::ofstream(std::filesystem::path(path).parent_path() / marker)
		<< "started";
	if (argc == 3 && std::wstring_view(argv[1]) == L"--snow-updated") {
		auto handle = reinterpret_cast<HANDLE>(
			static_cast<std::uintptr_t>(std::stoull(argv[2])));
		SetEvent(handle);
		CloseHandle(handle);
	}
	return 0;
}
