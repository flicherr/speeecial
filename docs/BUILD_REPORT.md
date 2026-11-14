# Результаты подготовки — 14 ноября 2026

Версия исходников: **speeecial (snow desktop) 1.0.8**.

| Проверка | Результат |
|---|---|
| CMake Release, Linux / GCC 14.2.0 | Сборка успешна, CTest 1/1, все 34 группы модели пройдены |
| Windows-кросс-сборка приложения, Zig 0.13.0 / Clang, x86_64-windows-gnu | Компиляция и линковка успешны |
| Windows-кросс-сборка `snow_windows_menu_tests` | Компиляция и линковка успешны |
| Конфликт SDK `small → char` | Обе кросс-сборки выполняются с принудительным `-Dsmall=char`; поле называется `small_font` |
| Строгие флаги Windows | `-Wall -Wextra -Wpedantic -Werror`; исключены missing-field-initializers и unknown-pragmas для Win32-структур/заголовков |
| Ядро, renderer, desktop, settings и тесты | Не изменены относительно 1.0.7 |
| Документация | Относительные ссылки и изображения проверены, XML/JSON разобраны |
| MSVC, запуск Windows-приложения/тестов | **Не выполнены: среда Linux** |
| Реальная отрисовка меню, память/FPS | **Требуют проверки на Windows** |

Предупреждения о повторном dllexport при сборке libunwind относятся к runtime Zig, а не к исходникам проекта. Проверочные EXE не входят в исходный архив. Успешная кросс-сборка не заменяет исполнение на Windows; прежняя ошибка имени `small` дополнительно проверяется макросом командной строки.

## Компактное меню

| Размер при стандартных метриках | 1.0.7 | 1.0.8 |
|---|---:|---:|
| Строка пункта | 34 DIP | 26 DIP |
| Заголовок | 54 DIP | 38 DIP |
| Название группы | 26 DIP | 20 DIP |
| Разделитель | 13 DIP | 7 DIP |
| Минимальная ширина главного меню | 324 DIP | 282 DIP |
| Минимальная ширина подменю | 248 DIP | 208 DIP |
| Значок заголовка | 28 DIP | 20 DIP |

У главного меню 12 командных строк, две группы, заголовок и разделитель. Базовая высота содержимого уменьшена с 527 до 397 DIP, примерно на 25%; это расчёт по параметрам компоновки, не измерение снимка Windows. Внешняя рамка и дополнительные системные области в расчёт не входят. При крупном системном шрифте строки расширяются по его метрикам.

Основной шрифт и все команды сохранены. Уменьшены поля, интервалы, заголовок и переключатели. Ширина рассчитывается из измеренных строк и тех же отступов, которые использует отрисовка. Архитектура нативного HMENU, доступность, управление с клавиатуры и владение GDI-ресурсами не менялись. Новых зависимостей и фоновой работы нет.

## Документация

README описывает приложение и повседневное использование. Инструкции MSVC/Ninja/clang-cl, тесты и обновление исходников перенесены в [BUILDING.md](BUILDING.md), устройство программы — в [ARCHITECTURE.md](ARCHITECTURE.md). В README используются локальные изображения и относительные ссылки без привязки к неизвестному адресу репозитория.

`menu-preview.svg` и `menu-preview.png` — эскизы актуальной компоновки, не снимки работающего Windows-приложения. PNG предназначен для просмотра в README; исходный SVG сохранён. Настоящую отрисовку, размеры областей клика, строки подменю и масштабирование нужно проверить по [VALIDATION.md](VALIDATION.md).

## Воспроизводимые проверки

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/native -j 2
ctest --test-dir build/native --output-on-failure
```

Windows x64, Developer PowerShell:

```powershell
cmake --preset windows-msvc -DSNOWDESK_WINDOWS_MENU_TESTS=ON
cmake --build --preset windows-release
ctest --preset windows-release
```

Кросс-сборка с Zig 0.13.0 в PATH:

```bash
mkdir -p build/check
zig rc /i resources /fo build/check/speeecial.res resources/snowdesk.rc
zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -DNDEBUG \
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Dsmall=char \
  -Isrc -Iresources -Wall -Wextra -Wpedantic -Werror \
  -Wno-missing-field-initializers -Wno-unknown-pragmas \
  -Wl,--subsystem,windows -municode \
  src/main.cpp src/app.cpp src/desktop.cpp src/renderer.cpp \
  src/settings.cpp src/core.cpp src/scene.cpp src/tray_menu.cpp \
  build/check/speeecial.res \
  -luser32 -lgdi32 -lshell32 -lole32 -ladvapi32 -ldwmapi \
  -lshcore -lwtsapi32 -lpsapi -ld3d11 -ldxgi -ld2d1 -ldcomp -luuid -lcomctl32 \
  -o build/check/speeecial.exe

zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -DNDEBUG \
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Dsmall=char \
  -Isrc -Wall -Wextra -Wpedantic -Werror \
  -Wno-missing-field-initializers -Wno-unknown-pragmas \
  tests/windows_menu_tests.cpp src/tray_menu.cpp build/check/speeecial.res \
  -luser32 -lgdi32 -ladvapi32 -lshcore -o build/check/snow_windows_menu_tests.exe
```

Документация API: [Owner-drawn menu accessibility](https://learn.microsoft.com/en-us/windows/win32/winauto/exposing-owner-drawn-menu-items), [MENUINFO](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo), [WM_MENUCHAR](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuchar).

Сценарии приёмки — в [VALIDATION.md](VALIDATION.md).
