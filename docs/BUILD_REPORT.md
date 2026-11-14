# Проверка сборки и отрисовки — 14 ноября 2026

Версия исходников: **speeecial (snow desktop) 1.0.9**.

| Проверка | Результат |
|---|---|
| CMake Release, Linux / GCC 14.2.0 | Сборка успешна, CTest 1/1, все 37 групп модели пройдены |
| Восстановление неполного списка | 120 циклов с пропусками панели/обычного окна; видимость и профили сохранены |
| Windows-кросс-сборка приложения, Zig 0.13.0 / Clang, x86_64-windows-gnu | Компиляция и линковка успешны |
| Windows-кросс-сборка `snow_windows_graphics_tests` | Компиляция и линковка успешны |
| Конфликт SDK `small → char` | Обе кросс-сборки выполнены с принудительным `-Dsmall=char` |
| Строгие флаги Windows | `-Wall -Wextra -Wpedantic -Werror`; исключены missing-field-initializers и unknown-pragmas для Win32-структур/заголовков |
| MSVC и исполнение Windows-приложения/графических тестов | Не выполнены: среда Linux |
| Настоящие панели Explorer, память и FPS | Требуют проверки на Windows |

Проверочные EXE не входят в исходный архив.

## Что подтверждает трассировка

В предоставленной трассировке 24 снимка за 19 секунд. На 2047–4875 мс окно `Shell_TrayWnd` отсутствует в списке, построенном через `GetTopWindow` / `GW_HWNDNEXT`; в первых снимках этого интервала `WindowFromPoint` всё ещё возвращает тот же HWND панели. На 10250–11797 мс панель также отсутствует в списке, хотя `GetForegroundWindow` возвращает её HWND. Обход не отмечен как обрезанный по лимиту. Вместе с панелью исчезает группа окон оболочки, затем возвращается после активации приложения.

Это доказывает, что отсутствие HWND в таком обходе нельзя считать скрытием или уничтожением окна. Предыдущий вариант 1.0.9 именно так переводила известную поверхность в `suspended`. Первоначальные изменения классификации служебных окон этот путь не закрывали.

Для мерцания оконных сугробов найден тот же путь подавления при пропуске окна: живая цепочка могла измениться во время запросов DWM/COM, а следующий полный опрос исправлял состояние. Данная трассировка подтверждает пропуск панели; весь сценарий быстрого кликанья по трею в ней отдельно не воспроизведён.

## Исправление и проверки

- HWND собираются через `EnumWindows` до дорогих запросов. Известные опоры и плашки, primary/secondary taskbar и foreground сверяются напрямую, даже если отсутствуют в перечислении.
- Состояние поверхности определяется настоящими свойствами окна. Сам пропуск в списке больше не выставляет `suspended`. Закрытие, сворачивание, cloaking, виртуальные столы, реальное скрытие панели и fullscreen сохраняют свои проверки.
- Восстановленные HWND вставляются относительно известных текущих соседей с учётом TOPMOST. Числовой Z-индекс старого снимка не используется; поиск соседей ограничен и не выполняет DWM/COM-запросов. Рендер использует тот же сбор списка при выборе положения слоя относительно плашек.
- Добавлены две переносимые группы: восстановление порядка/отсутствие дублей и цикл сохранения обоих типов сугробов при неполном перечислении. Прежние проверки fullscreen, сворачивания, закрытия и подсказок продолжают проходить.
- Opt-in Windows-тест проверяет реальное перечисление собственных HWND, чередование show/hide, порядок TOPMOST, уничтожение HWND, неизменность чужих стилей и фокуса. Он скомпилирован, но не исполнен в этой среде.

Сырые данные трассировки пользователя в исходный архив не включены.

## Воспроизводимые проверки

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/native -j 2
ctest --test-dir build/native --output-on-failure
```

Windows x64, Developer PowerShell:

```powershell
cmake --preset windows-msvc -DSNOWDESK_WINDOWS_GRAPHICS_TESTS=ON
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
  tests/windows_graphics_tests.cpp src/desktop.cpp src/renderer.cpp src/core.cpp src/scene.cpp \
  -luser32 -lgdi32 -lshell32 -lole32 -ladvapi32 -ldwmapi \
  -lshcore -lwtsapi32 -lpsapi -ld3d11 -ldxgi -ld2d1 -ldcomp -luuid -lcomctl32 \
  -o build/check/snow_windows_graphics_tests.exe
```

Сценарии приёмки — в [VALIDATION.md](VALIDATION.md), раздел «Пропуски окон при переходах оболочки — 1.0.9».
