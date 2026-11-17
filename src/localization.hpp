#pragma once

#include "core.hpp"
#include <array>

namespace snow
{
enum class Text {
	particles,
	graphics_unavailable,
	effects,
	off,
	light,
	normal,
	blizzard,
	snowfall,
	placement,
	in_front,
	behind,
	calm,
	gusty,
	wind,
	banks,
	on_windows,
	on_taskbar,
	collapse,
	shake,
	melt,
	max_depth,
	thin,
	normal_depth,
	deep,
	clear,
	cursor,
	application,
	system_theme,
	light_theme,
	dark_theme,
	theme,
	performance,
	fullscreen_pause,
	battery_pause,
	capture,
	startup,
	updates,
	checking,
	check_updates,
	automatic_updates,
	update_to,
	downloading,
	restarting,
	available,
	download_manually,
	releases,
	diagnostics,
	rebuild,
	save_report,
	settings_folder,
	pause,
	show_effects,
	hide_effects,
	stop,
	resume,
	freeze,
	exit,
	language,
	system_language,
	flake_size,
	snowfall_level,
	count
};

struct Translation {
	const wchar_t *russian;
	const wchar_t *english;
};

inline constexpr std::array<Translation, static_cast<std::size_t>(Text::count)>
	translations{{
		{L" частиц · лимит ", L" particles · limit "},
		{L"Графика недоступна — подробности в журнале",
         L"Graphics unavailable — see the log"},
		{L"ЭФФЕКТЫ", L"EFFECTS"},
		{L"Выключен", L"Off"},
		{L"Слабый", L"Light"},
		{L"Обычный", L"Normal"},
		{L"Метель", L"Blizzard"},
		{L"Снегопад", L"Snowfall"},
		{L"Расположение", L"Placement"},
		{L"Поверх окон", L"In front of windows"},
		{L"За окнами", L"Behind windows"},
		{L"Штиль", L"Calm"},
		{L"Порывистый", L"Gusty"},
		{L"Ветер", L"Wind"},
		{L"Сугробы", L"Snowbanks"},
		{L"На верхних гранях окон", L"On window top edges"},
		{L"На панели задач", L"On the taskbar"},
		{L"Обвал при закрытии и сворачивании",
         L"Collapse on close and minimize"},
		{L"Осыпание при резком перемещении",
         L"Shake off during sharp movements"},
		{L"Постепенное таяние", L"Gradual melting"},
		{L"Максимальная толщина", L"Maximum depth"},
		{L"Тонкий — 12", L"Thin — 12"},
		{L"Обычный — 22", L"Normal — 22"},
		{L"Пышный — 40", L"Deep — 40"},
		{L"Очистить весь снег", L"Clear all snow"},
		{L"Реакция на курсор", L"Cursor interaction"},
		{L"ПРИЛОЖЕНИЕ", L"APPLICATION"},
		{L"Системная", L"System"},
		{L"Светлая", L"Light"},
		{L"Тёмная", L"Dark"},
		{L"Тема", L"Theme"},
		{L"Производительность", L"Performance"},
		{L"Пауза в полноэкранных приложениях", L"Pause in fullscreen apps"},
		{L"Пауза при работе от батареи", L"Pause on battery"},
		{L"Эффекты на снимках экрана", L"Effects in screenshots"},
		{L"Автозапуск с Windows", L"Start with Windows"},
		{L"Обновления", L"Updates"},
		{L"Проверка…", L"Checking…"},
		{L"Проверить обновления", L"Check for updates"},
		{L"Проверять автоматически", L"Check automatically"},
		{L"Обновить до ", L"Update to "},
		{L"Загрузка… ", L"Downloading… "},
		{L"Перезапуск…", L"Restarting…"},
		{L"Доступна ", L"Available: "},
		{L" — скачать вручную", L" — download manually"},
		{L"Релизы на GitHub", L"Releases on GitHub"},
		{L"Диагностика", L"Diagnostics"},
		{L"Пересоздать графику", L"Recreate graphics"},
		{L"Записать диагностику", L"Save diagnostics"},
		{L"Папка настроек и журнала", L"Settings and log folder"},
		{L"Пауза", L"Pause"},
		{L"Показать эффекты", L"Show effects"},
		{L"Скрыть эффекты", L"Hide effects"},
		{L"Остановить", L"Stop"},
		{L"Продолжить", L"Resume"},
		{L"Заморозить снег", L"Freeze snow"},
		{L"Выход", L"Exit"},
		{L"Язык / Language", L"Language / Язык"},
		{L"Системный", L"System"},
		{L"Средний размер снежинок", L"Average snowflake size"},
		{L"Уровень снегопада", L"Snowfall level"},
	}};

constexpr Language resolve_language(Language selected,
                                    bool system_russian) noexcept
{
	if (selected == Language::russian || selected == Language::english) {
		return selected;
	}
	return system_russian ? Language::russian : Language::english;
}

constexpr const wchar_t *localized(Text text, Language language) noexcept
{
	const auto &entry = translations[static_cast<std::size_t>(text)];
	return language == Language::russian ? entry.russian : entry.english;
}
}
