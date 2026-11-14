#include "app.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	try {
		// Keep the prototype's singleton identity to prevent two overlays
		// running.
		snow::UniqueHandle instance_mutex(
			CreateMutexW(nullptr, FALSE, L"Local\\SnowDesk.Prototype.0.1"));
		if (!instance_mutex.get()) {
			snow::check_win(FALSE, "Create instance mutex");
		}
		if (GetLastError() == ERROR_ALREADY_EXISTS) {
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
