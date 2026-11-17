#include "app.hpp"
#include "migration.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	try {
		if (auto result = snow::run_update_mode()) {
			return *result;
		}
		snow::migration::InstanceGuard instance_guard;
		if (!instance_guard) {
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
