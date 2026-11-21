#include "app.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	try {
		if (auto result = snow::run_update_mode()) {
			return *result;
		}
		snow::UniqueHandle instance_mutex(
			CreateMutexW(nullptr, FALSE, L"Local\\speeecial.Instance.1.0"));
		const auto error = GetLastError();
		snow::check_win(instance_mutex.get() != nullptr,
		                "Create instance mutex");
		if (error == ERROR_ALREADY_EXISTS) {
			return 0;
		}
		snow::ComApartment com;
		snow::App app(instance);
		return app.run();
	} catch (const std::exception &error) {
		const auto text = snow::widen(error.what());
		MessageBoxW(nullptr, text.c_str(), L"speeecial — ошибка запуска",
		            MB_OK | MB_ICONERROR);
		return 1;
	}
}
