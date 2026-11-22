# Сборка speeecial

[← Об приложении](../README.ru.md)

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

Настройки приложения лежат в `%LOCALAPPDATA%\speeecial` и не зависят от каталога сборки. Поведение старых профилей описано в [UPDATES.md](UPDATES.md#профиль-и-автозапуск).

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

По умолчанию `BUILD_TESTING=ON`: собирается `snow_core_tests`, содержащий 40 групп сценариев модели снега, и `snow_update_tests`. На Windows тесты меню, графики и установки обновлений включаются отдельно.

### Меню

Проверяет команды, состояния, подписи доступности, отрисовку с сохранением GDI-контекста и освобождение USER/GDI-объектов при повторных циклах создания и удаления меню.

```powershell
cmake --preset windows-msvc -DSPEEECIAL_WINDOWS_MENU_TESTS=ON
cmake --build --preset windows-release --target snow_windows_menu_tests
ctest --preset windows-release -R '^snow_windows_menu_tests$'
```

Тест не заменяет ручную проверку настоящих всплывающих меню и ввода. При включённой высокой контрастности или системном флаге экранного диктора используется стандартная отрисовка.

### Графика и рабочий стол

Этот тест временно открывает и активирует тестовые окна, создаёт снежные слои и проверяет серии пересоздания графики. Запускай его на обычном интерактивном рабочем столе Windows с аппаратной D3D11, а не в headless CI.

```powershell
cmake --preset windows-msvc -DSPEEECIAL_WINDOWS_GRAPHICS_TESTS=ON
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

- `resources/speeecial.rc.in` — шаблон ресурсов: иконка, манифест и сведения о версии.
- `resources/speeecial.ico` — встроенная иконка разных размеров.
- `resources/speeecial.svg` — редактируемый векторный исходник и изображение для документации; во время работы EXE не загружается.
- `resources/speeecial.manifest.in` — шаблон манифеста: DPI awareness, совместимость и запуск без повышения прав.

Версия задаётся только строкой `SPEEECIAL_VERSION` в `src/version.hpp`. Для нового выпуска измени её и запусти обычную сборку. CMake читает заголовок перед `project()` и автоматически повторяет конфигурацию при его изменении. Файлы `generated/speeecial.rc` и `generated/speeecial.manifest` в каталоге сборки создаются из шаблонов с той же версией; четвёртая часть числовой версии Windows равна нулю. Широкая строка для интерфейса получается из того же макроса, скрипт упаковки также читает заголовок. Сгенерированные файлы вручную не редактируются. Проект, цель приложения, ресурсы, классы окон и профиль используют имя `speeecial`.

## Упаковка релизов и обновления

После сборки выполни `scripts/package-release.ps1`. Скрипт подготовит ZIP и отдельный EXE-asset для обновляющего клиента. Формат релиза, ограничения и Windows-тесты замены описаны в [UPDATES.md](UPDATES.md). Сетевой модуль использует только системные `winhttp`, `bcrypt` и `version`; устанавливать дополнительные библиотеки не нужно.

## GitHub Actions

`ci.yml` запускается для pull request, push в `main` и вручную. Он вызывает общий `build.yml`: на `windows-2022` приложение собирается MSVC Release x64 через `scripts/build.ps1`, включая переносимые тесты, тесты меню и процессов обновления. Затем `scripts/package-release.ps1` проверяет и упаковывает EXE. На `ubuntu-24.04` отдельно собираются и тестируются переносимые цели GCC. Те же задания вызывает `release.yml` для точного коммита тега.

Обычный CI выполняется с правами только на чтение. В релизном запуске Windows-задание дополнительно получает `id-token: write` и `attestations: write`: после упаковки `actions/attest` создаёт build provenance для EXE и ZIP. Linux-задание сохраняет только `contents: read`. Подпись создаётся до передачи файлов следующему заданию; релизный workflow проверяет её перед загрузкой в черновик. Версия Actions закреплена полным SHA, дополнительных секретов для подписи не требуется. Порядок выпуска и проверки описан в [UPDATES.md](UPDATES.md#подготовка-релиза-в-github-actions).

В артефакте `speeecial-windows-x64` находятся только EXE и ZIP; срок хранения — 14 дней. При ошибке сохраняются диагностические артефакты CMake/CTest. Тестовые EXE в пользовательский архив не попадают. Запуск графики, проверка фокуса, Z-порядка, Explorer и DPI остаются ручными сценариями Windows.

`quality.yml` использует `clang-format` **22.1.8** и корневой `.clang-format`. Для pull request проверяются изменённые строки относительно общего предка с базовой веткой, для push — изменения между предыдущим и текущим коммитом, при ручном запуске — последний коммит. Существующее форматирование вне изменений не проверяется. Изменение `.clang-format` или недоступная базовая ревизия включает полную проверку отслеживаемых C++-файлов. Локально ту же проверку можно выполнить через `py -3 scripts/check-format.py` на Windows или `python3 scripts/check-format.py` на Linux с нужной версией `clang-format` в `PATH`.

Отдельное задание `quality.yml` проверяет workflow через `actionlint` 1.7.12. Actions закреплены полными SHA коммитов; `.github/dependabot.yml` раз в неделю предлагает их обновления через PR. Автоматического слияния нет.
