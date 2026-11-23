# Contributing to speeecial · Участие в разработке

**English** · [Русский](#русский)

## English

Thank you for your interest in speeecial.

Bug reports, feature ideas, documentation improvements, translations, and code
contributions are welcome. Issues and pull requests can be written in English
or Russian.

**Quick links:**
[Report a bug](https://github.com/flicherr/speeecial/issues/new?template=bug_report.yml) ·
[Suggest a feature](https://github.com/flicherr/speeecial/issues/new?template=feature_request.yml) ·
[Ask a question](https://github.com/flicherr/speeecial/issues/new?template=question.yml)

### Reporting issues and ideas

Read the [English README](README.md) or [Russian README](README.ru.md) and search
existing issues before opening a new one. Pick the matching form in the
[issue chooser](https://github.com/flicherr/speeecial/issues/new/choose):

| Form | Use it for |
|---|---|
| Bug report | Something in the application does not work as expected, including antivirus or SmartScreen warnings |
| Feature request | A specific feature or improvement |
| Question or idea discussion | A usage question, or an idea you would like to talk over first |

**A good bug report includes:**

- the app version (or source commit), and the Windows version and build;
- steps to reproduce, and the expected and actual behavior;
- for graphics or desktop problems: GPU, monitors, display scaling, taskbar,
  and window mode;
- the diagnostics report (see below), plus a screenshot or a short recording
  for visual problems.

To create the report, choose **Diagnostics → Save diagnostics** in the tray
menu. It is saved as `%LOCALAPPDATA%\speeecial\diagnostics.txt`, and
**Diagnostics → Settings and log folder** opens that folder so you can attach
the file to your issue.

If an antivirus or SmartScreen flags speeecial, use the bug report form and
include the antivirus name, the detection name, and the SHA-256 of your file
(`Get-FileHash .\speeecial-windows-x64.exe`).

For larger changes, open an issue first to discuss the use case and approach
before you start implementing.

### Build and test

speeecial is a portable Windows x64 application written in C++20.

**Requirements on Windows:**

- Visual Studio 2022 or Build Tools 2022 with MSVC v143 and the Windows SDK
- CMake 3.24 or newer

Run from the repository root:

```powershell
.\scripts\build.ps1
```

The script configures CMake, builds Release, and runs CTest. Add `-Run` to launch
`build\windows-msvc\Release\speeecial.exe` after the checks pass. Exit any running
copy from its tray menu before rebuilding it.

**Linux.** You can build and test the portable targets (without the Windows
desktop application):

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/native -j 2
ctest --test-dir build/native --output-on-failure
```

More details: [BUILDING.md](docs/BUILDING.md) covers build options and the
Windows menu and graphics tests, and [UPDATES.md](docs/UPDATES.md) covers the
updater tests. Both documents are in Russian.

### Code style

- Follow the root [.clang-format](.clang-format) (clang-format 22.1.8, as
  configured in CI). Keep formatting changes limited to the code you modify. The
  local check with `scripts/check-format.py` is described in
  [BUILDING.md](docs/BUILDING.md#github-actions).
- Keep each change focused on one problem and follow the surrounding code. See
  [ARCHITECTURE.md](docs/ARCHITECTURE.md) for the main components.

### Project rules

Please preserve these when changing the code:

- **Portable and unprivileged.** A single `speeecial.exe` that needs no
  installer or administrator rights, keeps its profile in
  `%LOCALAPPDATA%\speeecial`, and shows snow effects offline.
- **Overlays stay out of the way.** Snow overlays must be transparent to input
  and must not take focus. When changing window order, move only the app's own
  overlays and leave the styles of other applications' windows untouched.
- **The frame loop stays light.** Do full window enumeration and blocking
  DWM/COM queries outside the frame loop, and render from the published window
  snapshot.
- **Antivirus-sensitive areas.** Changes to the registry, autostart, the
  updater, process creation, or system-wide hooks can change how antivirus
  engines classify the executable. Mention such changes in the pull request so
  the build can be scanned before release.

### Pull requests

1. Fork the repository and create a branch for your change.
2. Describe the problem, the resulting behavior, and the related issue. Add
   screenshots or a short recording for visible changes.
3. Run the relevant checks and report the commands and results. Add or update
   tests when you change behavior that can be checked automatically.
   Documentation and issue-template changes do not need an application build.
4. For desktop, focus, window order, Explorer, or DPI changes, also check the
   affected scenarios on an interactive Windows desktop. Where relevant, cover:
   - regular, maximized, and fullscreen windows;
   - taskbar auto-hide;
   - several monitors with different scaling;
   - for graphics recovery changes: repeated recreation, checking that
     snowbanks are preserved and resources do not grow.
5. State which manual scenarios you actually checked and which remain untested.
   Automated tests do not replace interactive Windows validation.

Keep build output and generated release files out of the pull request. Update
the documentation when user-visible behavior or build instructions change.

### License

speeecial is licensed under [GPL-3.0](LICENSE). By submitting a contribution,
you agree that it is licensed under the same terms.

---

## Русский

[English](#english) · **Русский**

Спасибо за интерес к speeecial.

Приветствуются сообщения об ошибках, идеи новых функций, улучшения документации,
переводы и изменения кода. Issues и pull requests можно писать на английском
или русском языке.

**Быстрые ссылки:**
[Сообщить об ошибке](https://github.com/flicherr/speeecial/issues/new?template=bug_report.yml) ·
[Предложить функцию](https://github.com/flicherr/speeecial/issues/new?template=feature_request.yml) ·
[Задать вопрос](https://github.com/flicherr/speeecial/issues/new?template=question.yml)

### Сообщения и предложения

Прочитай [английский README](README.md) или [русский README](README.ru.md) и
поищи похожие issues перед созданием нового. Выбери подходящую форму в
[списке форм](https://github.com/flicherr/speeecial/issues/new/choose):

| Форма | Для чего |
|---|---|
| Bug report | Что-то в приложении работает не так, в том числе предупреждения антивируса или SmartScreen |
| Feature request | Конкретная новая функция или улучшение |
| Question or idea discussion | Вопрос об использовании или идея, которую хочется сначала обсудить |

**Хороший отчёт об ошибке содержит:**

- версию приложения (или коммит исходников), версию и сборку Windows;
- шаги воспроизведения, ожидаемое и фактическое поведение;
- для проблем с графикой или рабочим столом: видеокарту, мониторы, масштаб
  интерфейса, панель задач и режим окна;
- отчёт диагностики (см. ниже), а для визуальных ошибок ещё скриншот или
  короткую запись экрана.

Чтобы создать отчёт, выбери **«Диагностика → Записать диагностику»** в меню
трея. Он сохранится в `%LOCALAPPDATA%\speeecial\diagnostics.txt`, а пункт
**«Диагностика → Папка настроек и журнала»** открывает эту папку, чтобы
приложить файл к issue.

Если антивирус или SmartScreen ругается на speeecial, используй форму Bug report
и укажи название антивируса, имя детекта и SHA-256 файла
(`Get-FileHash .\speeecial-windows-x64.exe`).

Перед началом крупных изменений сначала открой issue и обсуди задачу и подход.

### Сборка и проверка

speeecial — переносимое приложение для Windows x64 на C++20.

**Требования для Windows:**

- Visual Studio 2022 или Build Tools 2022 с MSVC v143 и Windows SDK
- CMake 3.24 или новее

Выполни из корня репозитория:

```powershell
.\scripts\build.ps1
```

Скрипт выполняет конфигурацию CMake, Release-сборку и CTest. Добавь `-Run`,
чтобы после успешных проверок запустить
`build\windows-msvc\Release\speeecial.exe`. Перед пересборкой заверши
запущенную копию приложения через меню трея.

**Linux.** Можно собрать и проверить переносимые цели (без приложения для
рабочего стола Windows):

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/native -j 2
ctest --test-dir build/native --output-on-failure
```

Подробнее: в [BUILDING.md](docs/BUILDING.md) описаны варианты сборки и
Windows-тесты меню и графики, а в [UPDATES.md](docs/UPDATES.md) — тесты
обновления. Оба документа написаны на русском языке.

### Стиль кода

- Следуй корневому [.clang-format](.clang-format) (clang-format 22.1.8, как в
  CI). Не переформатируй код, который не меняешь. Локальная проверка через
  `scripts/check-format.py` описана в
  [BUILDING.md](docs/BUILDING.md#github-actions).
- Одно изменение — одна задача; следуй стилю окружающего кода. Основные
  компоненты описаны в [ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Правила проекта

Сохраняй при изменениях кода:

- **Переносимость и отсутствие привилегий.** Один `speeecial.exe`, которому не
  нужны установщик и права администратора, профиль в
  `%LOCALAPPDATA%\speeecial`, снежные эффекты работают офлайн.
- **Оверлеи не мешают.** Снежные слои пропускают ввод и не перехватывают фокус.
  При изменении Z-порядка перемещай только собственные оверлеи приложения и не
  меняй стили окон других приложений.
- **Кадровый цикл остаётся лёгким.** Полный обход окон и блокирующие запросы
  DWM/COM выполняй вне кадрового цикла, а при отрисовке используй опубликованный
  снимок окон.
- **Области, чувствительные для антивирусов.** Изменения реестра, автозапуска,
  апдейтера, запуска процессов и системных хуков могут изменить то, как
  антивирусы классифицируют исполняемый файл. Упомяни такие изменения в pull
  request, чтобы сборку можно было проверить до релиза.

### Pull requests

1. Создай форк репозитория и ветку для своего изменения.
2. Опиши проблему, итоговое поведение и связанный issue. Для видимых изменений
   добавь скриншоты или короткую запись экрана.
3. Выполни подходящие проверки и укажи команды и результаты. Добавляй или
   обновляй тесты при изменении поведения, которое можно проверить
   автоматически. Для правок документации и форм issues сборка приложения не
   требуется.
4. При изменении рабочего стола, фокуса, Z-порядка, Explorer или DPI проверь
   затронутые сценарии на интерактивном рабочем столе Windows. Где применимо,
   проверь:
   - обычные, развёрнутые и полноэкранные окна;
   - автоскрытие панели задач;
   - несколько мониторов с разным масштабом;
   - для восстановления графики: повторное пересоздание, сохранение сугробов
     и отсутствие роста ресурсов.
5. Укажи, какие ручные сценарии действительно проверены, а какие остались
   непроверенными. Автотесты не заменяют проверку на интерактивном рабочем
   столе Windows.

Не добавляй результаты сборки и сгенерированные релизные файлы в pull request.
Обновляй документацию при изменении поведения для пользователя или инструкций
по сборке.

### Лицензия

speeecial распространяется под лицензией [GPL-3.0](LICENSE). Отправляя вклад,
ты соглашаешься, что он распространяется на тех же условиях.
