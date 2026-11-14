#include "tray_menu.hpp"
#include "win_util.hpp"
#include <algorithm>
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

struct LiveMenu
{
	TrayMenu *menu{};
	HMENU child{};
	bool root_seen{}, child_seen{}, root_shape{}, child_shape{};
	unsigned ticks{};
};

LRESULT CALLBACK live_owner(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
	auto *test = reinterpret_cast<LiveMenu *>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (test) {
		if (message == WM_MEASUREITEM && test->menu->measure(*reinterpret_cast<MEASUREITEMSTRUCT *>(lp))) {
			return TRUE;
		}
		if (message == WM_DRAWITEM && test->menu->draw(*reinterpret_cast<DRAWITEMSTRUCT *>(lp))) {
			return TRUE;
		}
		if (message == WM_TIMER) {
			++test->ticks;
			EnumThreadWindows(
			    GetCurrentThreadId(),
			    [](HWND popup, LPARAM data) -> BOOL {
				    auto &t = *reinterpret_cast<LiveMenu *>(data);
				    MENUBARINFO info{sizeof(info)};
				    wchar_t cls[32]{};
				    GetClassNameW(popup, cls, 32);
				    if (std::wcscmp(cls, L"#32768") != 0 || !IsWindowVisible(popup) ||
					    !GetMenuBarInfo(popup, OBJID_CLIENT, 0, &info)) {
					    return TRUE;
				    }
				    const bool root = info.hMenu == t.menu->handle();
				    if (!root && info.hMenu != t.child) {
					    return TRUE;
				    }
				    RECT rect{};
				    GetWindowRect(popup, &rect);
				    HRGN region = CreateRectRgn(0, 0, 0, 0);
				    const int q = std::max(1, MulDiv(2, static_cast<int>(GetDpiForWindow(popup)), 96));
				    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
				    const bool shape = region && GetWindowRgn(popup, region) != ERROR &&
					                   !PtInRegion(region, w / 3 / q * q + q, 0) && PtInRegion(region, w / 2, h / 2);
				    if (region) {
					    DeleteObject(region);
				    }
				    if (root) {
					    t.root_shape |= shape;
					    if (!t.root_seen) {
						    t.root_seen = true;
						    PostMessageW(popup, WM_KEYDOWN, VK_HOME, 0);
						    PostMessageW(popup, WM_KEYDOWN, VK_RIGHT, 0);
					    }
				    } else {
					    t.child_seen = true;
					    t.child_shape |= shape;
					    PostMessageW(popup, WM_KEYDOWN, VK_RETURN, 0);
				    }
				    return TRUE;
			    },
			    reinterpret_cast<LPARAM>(test));
			if (test->ticks > 20) {
				EndMenu();
			}
			return 0;
		}
	}
	return DefWindowProcW(window, message, wp, lp);
}

void live_frame(MenuTheme theme)
{
	TrayMenu menu(GetModuleHandleW(nullptr), {100, 100}, theme);
	if (!menu.styled()) {
		return;
	}
	menu.header(L"test");
	const auto child = menu.submenu(menu.handle(), L"Снегопад", MenuGlyph::snow);
	menu.item(child, 201, L"Слабый", MenuMark::radio, true);
	menu.item(menu.handle(), 202, L"Выход", MenuMark::none, false, MenuGlyph::exit);
	WNDCLASSW cls{};
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpfnWndProc = live_owner;
	cls.lpszClassName = L"SnowDesk.MenuFrameTest";
	RegisterClassW(&cls);
	HWND owner = CreateWindowW(cls.lpszClassName, L"Menu test", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
	                           cls.hInstance, nullptr);
	check_win(owner != nullptr, "Create live menu owner");
	LiveMenu state{&menu, child};
	SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
	const auto timer = SetTimer(owner, 1, 80, nullptr);
	if (!timer) {
		DestroyWindow(owner);
		throw std::runtime_error("Live menu timer failed");
	}
	const UINT command = menu.track(TPM_RETURNCMD | TPM_NONOTIFY, {100, 100}, owner);
	KillTimer(owner, timer);
	DestroyWindow(owner);
	UnregisterClassW(cls.lpszClassName, cls.hInstance);
	require(state.root_seen && state.child_seen && state.root_shape && state.child_shape,
	        "Live root/submenu pixel regions missing");
	require(command == 201, "Native submenu keyboard command delivery failed");
}

void exercise(MenuTheme theme, Canvas &canvas)
{
	TrayMenu menu(GetModuleHandleW(nullptr), {0, 0}, theme);
	const auto root = menu.handle();
	menu.header(L"test");
	menu.section(root, L"ЭФФЕКТЫ");
	const auto child = menu.submenu(root, L"Снегопад", MenuGlyph::snow, L"Обычный");
	menu.item(child, 201, L"Слабый", MenuMark::radio, false);
	menu.item(child, 202, L"Обычный", MenuMark::radio, true);
	menu.item(root, 114, L"Реакция на курсор", MenuMark::toggle, true, MenuGlyph::cursor);
	menu.separator(root);
	menu.item(root, 100, L"Пауза", MenuMark::action, true, MenuGlyph::pause, L"Показать эффекты");
	menu.item(root, 110, L"Выход", MenuMark::none, false, MenuGlyph::exit);
	require(item(root, 2).hSubMenu == child, "Submenu ownership lost");
	require(item(child, 1).wID == 202 && (item(child, 1).fState & MFS_CHECKED), "Selected radio command lost");
	require(item(root, 3).wID == 114 && (item(root, 3).fState & MFS_CHECKED), "Toggle command lost");
	if (!menu.styled()) {
		require(!(item(root, 3).fType & MFT_OWNERDRAW), "Native fallback still requires custom drawing");
		return;
	}
	const auto mnemonic = menu.character(L'п', root);
	require(HIWORD(mnemonic) == MNC_EXECUTE && LOWORD(mnemonic) == 5, "Cyrillic keyboard selection failed");
	require(HIWORD(menu.character(L'?', root)) == MNC_IGNORE, "Unknown key selects a command");
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
			exercise(theme, canvas);
		}
		live_frame(MenuTheme::light);
		live_frame(MenuTheme::dark);
		const auto process = GetCurrentProcess();
		const auto gdi = GetGuiResources(process, GR_GDIOBJECTS);
		const auto user = GetGuiResources(process, GR_USEROBJECTS);
		for (int i = 0; i < 100; ++i) {
			exercise(i % 2 ? MenuTheme::dark : MenuTheme::light, canvas);
		}
		for (int i = 0; i < 8; ++i) {
			live_frame(i % 2 ? MenuTheme::light : MenuTheme::dark);
		}
		require(GetGuiResources(process, GR_GDIOBJECTS) <= gdi + 2 &&
		            GetGuiResources(process, GR_USEROBJECTS) <= user + 2,
		        "Repeated menu destruction leaks USER/GDI resources");
		std::cout << "PASS menu commands, pixel frames, keyboard submenus, accessibility, DC restoration and USER/GDI "
		             "lifecycle\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
