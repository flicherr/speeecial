#include "localization.hpp"
#include "tray_menu.hpp"
#include "win_util.hpp"
#include <iostream>
#include <oleacc.h>

namespace
{
using namespace snow;

void require(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

MENUITEMINFOW item(HMENU menu, UINT position)
{
	MENUITEMINFOW info{sizeof(info)};
	info.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_DATA | MIIM_SUBMENU;
	check_win(GetMenuItemInfoW(menu, position, TRUE, &info), "Read test menu item");
	return info;
}

struct Canvas
{
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bitmap{};
	HGDIOBJ previous{};

	Canvas()
	{
		check_win(dc != nullptr, "Create menu test DC");
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = 4096;
		info.bmiHeader.biHeight = -1024;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		void *pixels = nullptr;
		bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		if (!bitmap) {
			DeleteDC(dc);
			check_win(FALSE, "Create menu test bitmap");
		}
		previous = SelectObject(dc, bitmap);
	}

	~Canvas()
	{
		SelectObject(dc, previous);
		DeleteObject(bitmap);
		DeleteDC(dc);
	}
};

void exercise(MenuTheme theme, Canvas &canvas, Language language)
{
	const auto tr = [language](Text text) { return localized(text, language); };
	TrayMenu menu(GetModuleHandleW(nullptr), {0, 0}, theme);
	const auto root = menu.handle();
	menu.header(L"test");
	menu.section(root, tr(Text::effects));
	const auto child = menu.submenu(root, tr(Text::snowfall), MenuGlyph::snow,
	                                tr(Text::normal));
	menu.slider(
		child, 200, tr(Text::snowfall_level), 0, 3, 2, [](int) {},
		{tr(Text::off), tr(Text::light), tr(Text::normal), tr(Text::blizzard)});
	menu.slider(child, 520, tr(Text::flake_size), 2, 12, 6, [](int) {});
	menu.item(root, 114, tr(Text::cursor), MenuMark::toggle, true,
	          MenuGlyph::cursor);
	menu.separator(root);
	menu.item(root, 100, tr(Text::pause), MenuMark::action, true,
	          MenuGlyph::pause, tr(Text::show_effects));
	menu.item(root, 110, tr(Text::exit), MenuMark::none, false,
	          MenuGlyph::exit);
	int slider_changes = 0;
	menu.slider(root, 520, tr(Text::flake_size), 2, 12, 6,
	            [&](int) { ++slider_changes; });
	require(slider_changes == 0,
	        "Constructing a slider must not change settings");
	require(item(root, 2).hSubMenu == child, "Submenu ownership lost");
	require(GetMenuItemCount(child) == 2,
	        "Snowfall must contain exactly two slider rows");
	require(item(root, 3).wID == 114 && (item(root, 3).fState & MFS_CHECKED), "Toggle command lost");
	if (!menu.styled()) {
		const auto modes = item(child, 0).hSubMenu;
		require(modes && GetMenuItemCount(modes) == 4 &&
		            item(modes, 0).wID == 200 && item(modes, 3).wID == 203 &&
		            (item(modes, 2).fState & MFS_CHECKED),
		        "Native mode choices must preserve all four commands and the "
		        "current mode");
		wchar_t first[64]{}, last[64]{};
		GetMenuStringW(modes, 0, first, 64, MF_BYPOSITION);
		GetMenuStringW(modes, 3, last, 64, MF_BYPOSITION);
		require(std::wstring(first) == tr(Text::off) &&
		            std::wstring(last) == tr(Text::blizzard),
		        "Mode choices must show localized names instead of pixels");
		const auto choices = item(root, 7).hSubMenu;
		require(choices && GetMenuItemCount(choices) == 11,
		        "Native accessibility mode must offer all size choices");
		require(item(choices, 0).wID == 522 && item(choices, 10).wID == 532 &&
		            (item(choices, 4).fState & MFS_CHECKED),
		        "Native size choices must preserve their commands and current "
		        "value");
		require(!(item(root, 3).fType & MFT_OWNERDRAW), "Native fallback still requires custom drawing");
		return;
	}
	const auto mnemonic =
		menu.character(language == Language::russian ? L'п' : L'p', root);
	require(HIWORD(mnemonic) == MNC_EXECUTE && LOWORD(mnemonic) == 5,
	        "Localized keyboard selection failed");
	require(HIWORD(menu.character(L'?', root)) == MNC_IGNORE, "Unknown key selects a command");
	require(
		HIWORD(menu.character(language == Language::russian ? L'у' : L's',
	                          child)) == MNC_SELECT,
		"A slider mnemonic must select the row without executing its command");
	MEASUREITEMSTRUCT slider_size{};
	slider_size.CtlType = ODT_MENU;
	slider_size.itemData = item(root, 7).dwItemData;
	MEASUREITEMSTRUCT normal_size{};
	normal_size.CtlType = ODT_MENU;
	normal_size.itemData = item(root, 3).dwItemData;
	require(menu.measure(slider_size) && menu.measure(normal_size) &&
	            slider_size.itemHeight > normal_size.itemHeight,
	        "The slider must reserve separate lines for its caption and track");
	const auto old_font = GetCurrentObject(canvas.dc, OBJ_FONT);
	const auto old_pen = GetCurrentObject(canvas.dc, OBJ_PEN);
	const auto old_brush = GetCurrentObject(canvas.dc, OBJ_BRUSH);
	const auto old_color = GetTextColor(canvas.dc);
	for (HMENU handle : {root, child}) {
		for (int pos = 0; pos < GetMenuItemCount(handle); ++pos) {
			const auto info = item(handle, static_cast<UINT>(pos));
			const auto *accessible = reinterpret_cast<const MSAAMENUINFO *>(info.dwItemData);
			require(accessible && accessible->dwMSAASignature == MSAA_MENU_SIG, "Accessible menu metadata missing");
			require(accessible->cchWText == wcslen(accessible->pszWText), "Accessible text lifetime invalid");
			MEASUREITEMSTRUCT size{};
			size.CtlType = ODT_MENU;
			size.itemData = info.dwItemData;
			require(menu.measure(size) && size.itemWidth && size.itemHeight, "Owner-drawn item has no hit area");
			DRAWITEMSTRUCT draw{};
			draw.CtlType = ODT_MENU;
			draw.itemData = info.dwItemData;
			draw.hDC = canvas.dc;
			draw.rcItem = {0, 0, static_cast<LONG>(size.itemWidth), static_cast<LONG>(size.itemHeight)};
			for (UINT state : {UINT{0}, UINT{ODS_SELECTED | ODS_FOCUS}, UINT{ODS_CHECKED}, UINT{ODS_DISABLED}}) {
				draw.itemState = state;
				require(menu.draw(draw), "Owner-drawn callback failed");
				require(GetCurrentObject(canvas.dc, OBJ_FONT) == old_font &&
				            GetCurrentObject(canvas.dc, OBJ_PEN) == old_pen &&
				            GetCurrentObject(canvas.dc, OBJ_BRUSH) == old_brush && GetTextColor(canvas.dc) == old_color,
				        "Menu drawing damaged caller DC state");
			}
		}
	}
}
}

int main()
{
	try {
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		Canvas canvas;
		for (auto theme : {MenuTheme::dark, MenuTheme::light, MenuTheme::native}) {
			for (auto language : {Language::russian, Language::english}) {
				exercise(theme, canvas, language);
			}
		}
		const auto process = GetCurrentProcess();
		const auto gdi = GetGuiResources(process, GR_GDIOBJECTS);
		const auto user = GetGuiResources(process, GR_USEROBJECTS);
		for (int i = 0; i < 100; ++i) {
			exercise(i % 2 ? MenuTheme::dark : MenuTheme::light, canvas,
			         i % 3 ? Language::english : Language::russian);
		}
		require(GetGuiResources(process, GR_GDIOBJECTS) <= gdi + 2 &&
		            GetGuiResources(process, GR_USEROBJECTS) <= user + 2,
		        "Repeated menu destruction leaks USER/GDI resources");
		std::cout << "PASS menu commands, accessibility, drawing, DC "
		             "restoration and USER/GDI "
					 "lifecycle\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
