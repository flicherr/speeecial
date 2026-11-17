# Сборка speeecial

[← Об приложении](../README.md)

Инструкции предназначены для сборки из исходников. Готовый файл приложения — `speeecial.exe`; отдельные ресурсные файлы рядом с ним не требуются.

## Требования

- Windows x64.
- Visual Studio 2022 или Build Tools 2022 с компонентом **«Разработка классических приложений на C++»**: MSVC v143 и Windows SDK.
- CMake 3.24 или новее, доступный в `PATH`.
- Для варианта с Ninja — установленный Ninja и x64 Developer PowerShell.

Проект использует C++20 и системные библиотеки Windows. MSVC Release собирается со статическим CRT (`/MT`); Qt, WebView, SDL и сторонние графические библиотеки не нужны. Архив исходников не содержит компилятор и готовый EXE.

## MSVC

Выполни из корня проекта:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release
```

Результат:

```text
build/windows-msvc/Release/speeecial.exe
```

Запуск:

```powershell
.\build\windows-msvc\Release\speeecial.exe
```

Перед пересборкой запущенного приложения выбери **«Выход»** в трее, чтобы освободить EXE для линковщика.

### Скрипт сборки

Из PowerShell можно выполнить те же шаги одной командой:

```powershell
.\scripts\build.ps1 -Run
```

Скрипт последовательно выполняет конфигурацию, сборку и тесты. С параметром `-Run` запускает приложение только после их успешного завершения.

| Параметр | Назначение |
|---|---|
| `-Run` | Запустить собранное приложение |
| `-Fresh` | Пересоздать кэш CMake и выполнить полную пересборку |
| `-Ninja` | Использовать пресет Ninja вместо генератора Visual Studio |

## Обновление исходников

При добавлении или удалении `.cpp`-файлов повторно выполни конфигурацию CMake, затем сборку. Для перехода со старого состава проекта или восстановления некорректного кэша:

```powershell
cmake --fresh --preset windows-msvc
cmake --build --preset windows-release --clean-first
ctest --preset windows-release
```

Эквивалент через скрипт: `./scripts/build.ps1 -Fresh -Run`.

Настройки приложения лежат в `%LOCALAPPDATA%\SnowDesk` и не зависят от каталога сборки. После перехода с ранней версии запускай `speeecial.exe`; старый `snowdesk.exe` больше не является результатом текущей сборки.

## Ninja и clang-cl

Из **x64 Developer PowerShell**:

```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
ctest --preset ninja-release
.\build\ninja-release\speeecial.exe
```

Для clang-cl укажи компилятор при первой конфигурации:

```powershell
cmake --preset ninja-release -DCMAKE_CXX_COMPILER=clang-cl
```

Нужны Windows SDK и совместимый линковщик. При смене компилятора используй новый каталог сборки или заново создай кэш; не смешивай объектные файлы разных инструментальных цепочек.

## Тесты

По умолчанию `BUILD_TESTING=ON`: собирается `snow_core_tests`, содержащий 40 групп сценариев модели снега. Windows-тесты включаются отдельно.

### Меню

Проверяет команды, состояния, подписи доступности, отрисовку с сохранением GDI-контекста и освобождение USER/GDI-объектов при повторных циклах создания и удаления меню.

```powershell
cmake --preset windows-msvc -DSNOWDESK_WINDOWS_MENU_TESTS=ON
cmake --build --preset windows-release --target snow_windows_menu_tests
ctest --preset windows-release -R '^snow_windows_menu_tests$'
```

Тест не заменяет ручную проверку настоящих всплывающих меню и ввода. При включённой высокой контрастности или системном флаге экранного диктора используется стандартная отрисовка.

### Графика и рабочий стол

Этот тест временно открывает и активирует тестовые окна, создаёт снежные слои и проверяет серии пересоздания графики. Запускай его на обычном интерактивном рабочем столе Windows с аппаратной D3D11, а не в headless CI.

```powershell
cmake --preset windows-msvc -DSNOWDESK_WINDOWS_GRAPHICS_TESTS=ON
cmake --build --preset windows-release --target snow_windows_graphics_tests
ctest --preset windows-release -R '^snow_windows_graphics_tests$'
```

### Переносимое ядро на Linux

На Linux собирается только модель и её тесты, без приложения для рабочего стола:

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/native -j 2
ctest --test-dir build/native --output-on-failure
```

## Ресурсы и версия

- `resources/snowdesk.rc` подключает иконку, манифест и сведения о версии.
- `resources/snowdesk.ico` — встроенная иконка разных размеров.
- `resources/snowdesk.svg` — редактируемый векторный исходник и изображение для документации; во время работы EXE не загружается.
- `resources/snowdesk.manifest` задаёт DPI awareness, совместимость и запуск без повышения прав.

При обновлении номера версии согласуй `CMakeLists.txt`, ресурс версии, манифест и `src/version.hpp` (откуда её берут меню и диагностика). Имена внутренних классов, mutex и каталога настроек `SnowDesk` сохраняются для совместимости.

## Упаковка релизов и обновления

После сборки выполни `scripts/package-release.ps1`. Скрипт подготовит ZIP, отдельный EXE-asset для обновляющего клиента и контрольные суммы. Формат релиза, ограничения и Windows-тесты замены описаны в [UPDATES.md](UPDATES.md). Сетевой модуль использует только системные `winhttp`, `bcrypt` и `version`; устанавливать дополнительные библиотеки не нужно.
