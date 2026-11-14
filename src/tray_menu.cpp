#include "tray_menu.hpp"
#include "win_util.hpp"
#include <algorithm>
#include <array>
#include <commctrl.h>
#include <cstdlib>
#include <oleacc.h>
#include <shellscalingapi.h>
#include <vector>

namespace snow
{
namespace
{
template <class T> struct Gdi {
	T value{};

	~Gdi()
	{
		if (value) {
			DeleteObject(value);
		}
	}

	Gdi() = default;
	Gdi(const Gdi &) = delete;
	Gdi &operator=(const Gdi &) = delete;
};

struct MenuHandle {
	HMENU value{};

	~MenuHandle()
	{
		if (value) {
			DestroyMenu(value);
		}
	}
};

struct IconHandle {
	HICON value{};

	~IconHandle()
	{
		if (value) {
			DestroyIcon(value);
		}
	}
};

struct ScreenDC {
	HDC value = GetDC(nullptr);

	~ScreenDC()
	{
		if (value) {
			ReleaseDC(nullptr, value);
		}
	}
};

struct SavedDC {
	HDC dc;
	int state;

	explicit SavedDC(HDC value) : dc(value), state(SaveDC(value)) {}

	~SavedDC()
	{
		if (state) {
			RestoreDC(dc, state);
		}
	}
};
enum class Role { item, header, section, separator };

struct Entry {
	MSAAMENUINFO accessibility{}; // Must be first, with no virtual table.
	std::wstring label, value, spoken;
	HMENU owner{}, child{};
	MenuGlyph glyph{};
	MenuMark mark{};
	Role role{};
	UINT width{}, height{};
};

struct Palette {
	COLORREF background, text, muted, accent, hover, active, line, track, thumb,
		texture;
};

Palette palette(bool dark) noexcept
{
	if (dark) {
		return {RGB(23, 23, 28),    RGB(236, 235, 242), RGB(158, 156, 175),
		        RGB(174, 150, 241), RGB(43, 37, 61),    RGB(31, 29, 43),
		        RGB(101, 99, 121),  RGB(75, 76, 89),    RGB(237, 235, 249),
		        RGB(26, 26, 32)};
	}
	return {RGB(247, 247, 252), RGB(36, 36, 49),    RGB(102, 102, 125),
	        RGB(132, 99, 215),  RGB(234, 226, 251), RGB(239, 235, 249),
	        RGB(136, 141, 169), RGB(195, 198, 213), RGB(255, 255, 255),
	        RGB(243, 244, 249)};
}

bool native_accessibility() noexcept
{
	HIGHCONTRASTW contrast{sizeof(contrast)};
	BOOL reader = FALSE;
	SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
	SystemParametersInfoW(SPI_GETSCREENREADER, 0, &reader, 0);
	return (contrast.dwFlags & HCF_HIGHCONTRASTON) || reader;
}

bool dark_preference() noexcept
{
	DWORD light = 1, bytes = sizeof(light);
	RegGetValueW(
		HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
	return light == 0;
}

wchar_t lower(wchar_t c) noexcept
{
	CharLowerBuffW(&c, 1);
	return c;
}
}

struct TrayMenu::Impl {
	// Destruction is in reverse order: menus disappear before data/brush/fonts.
	std::vector<std::unique_ptr<Entry>> entries;
	// Windows SDK RPC headers define small as a macro; use an explicit font
	// name.
	Gdi<HFONT> body, title, small_font;
	Gdi<HBITMAP> texture;
	Gdi<HBRUSH> background;
	IconHandle icon;
	MenuHandle root;
	Palette colors{};
	UINT dpi = 96;
	int width_limit = 1000, height_limit = 1000, body_height = 13;
	bool custom = true;

	int px(int dip) const noexcept
	{
		return MulDiv(dip, static_cast<int>(dpi), 96);
	}

	Impl(HINSTANCE instance, POINT anchor, MenuTheme theme)
	{
		const auto monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
		UINT dx = 96, dy = 96;
		if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) {
			dpi = dx;
		}
		MONITORINFO bounds{sizeof(bounds)};
		if (GetMonitorInfoW(monitor, &bounds)) {
			width_limit =
				std::max(px(120), static_cast<int>(bounds.rcWork.right -
			                                       bounds.rcWork.left) -
			                          px(16));
			height_limit =
				std::max(px(80), static_cast<int>(bounds.rcWork.bottom -
			                                      bounds.rcWork.top) -
			                         px(12));
		}
		custom = theme != MenuTheme::native && !native_accessibility();
		colors = palette(theme == MenuTheme::dark ||
		                 (theme == MenuTheme::automatic && dark_preference()));
		root.value = CreatePopupMenu();
		check_win(root.value != nullptr, "Create tray menu");
		if (!custom) {
			configure(root.value);
			return;
		}
		NONCLIENTMETRICSW metrics{sizeof(metrics)};
		LOGFONTW font{};
		if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
		                               &metrics, 0, dpi)) {
			font = metrics.lfMenuFont;
		} else {
			wcscpy_s(font.lfFaceName, L"Segoe UI");
		}
		font.lfHeight =
			-std::max(px(13), std::abs(static_cast<int>(font.lfHeight)));
		font.lfWeight = FW_NORMAL;
		font.lfQuality = CLEARTYPE_QUALITY;
		body.value = CreateFontIndirectW(&font);
		font.lfHeight = -std::max(
			px(11), std::abs(static_cast<int>(font.lfHeight)) - px(1));
		small_font.value = CreateFontIndirectW(&font);
		font.lfHeight = -std::max(
			px(15),
			std::abs(static_cast<int>(metrics.lfMenuFont.lfHeight)) + px(2));
		font.lfWeight = FW_SEMIBOLD;
		title.value = CreateFontIndirectW(&font);
		// A tiny cached tile, not a full-size menu bitmap or an animation
		// timer.
		BITMAPINFO tile{};
		tile.bmiHeader = {sizeof(BITMAPINFOHEADER),
		                  px(8),
		                  -px(8),
		                  1,
		                  32,
		                  BI_RGB,
		                  0,
		                  0,
		                  0,
		                  0,
		                  0};
		void *pixels = nullptr;
		texture.value = CreateDIBSection(nullptr, &tile, DIB_RGB_COLORS,
		                                 &pixels, nullptr, 0);
		if (texture.value && pixels) {
			for (int y = 0; y < px(8); ++y) {
				for (int x = 0; x < px(8); ++x) {
					const auto c =
						((x / std::max(1, px(4)) + y / std::max(1, px(4))) & 1)
							? colors.texture
							: colors.background;
					static_cast<DWORD *>(pixels)[y * px(8) + x] =
						(static_cast<DWORD>(GetRValue(c)) << 16) |
						(static_cast<DWORD>(GetGValue(c)) << 8) | GetBValue(c);
				}
			}
			background.value = CreatePatternBrush(texture.value);
		}
		if (!background.value) {
			background.value = CreateSolidBrush(colors.background);
		}
		check_win(body.value && title.value && small_font.value &&
		              background.value,
		          "Create tray menu resources");
		icon.value = static_cast<HICON>(LoadImageW(
			instance, MAKEINTRESOURCEW(101), IMAGE_ICON, px(20), px(20), 0));
		ScreenDC screen;
		check_win(screen.value != nullptr, "Measure tray font");
		SavedDC saved(screen.value);
		SelectObject(screen.value, body.value);
		TEXTMETRICW text{};
		if (GetTextMetricsW(screen.value, &text)) {
			body_height = text.tmHeight;
		}
		configure(root.value);
	}

	void configure(HMENU menu)
	{
		MENUINFO info{sizeof(info)};
		info.fMask = MIM_MAXHEIGHT;
		info.cyMax = static_cast<UINT>(height_limit);
		if (custom) {
			info.fMask |= MIM_BACKGROUND | MIM_STYLE;
			info.hbrBack = background.value;
			info.dwStyle = MNS_NOCHECK;
		}
		check_win(SetMenuInfo(menu, &info), "Configure tray menu");
	}

	Entry *find(ULONG_PTR data) const noexcept
	{
		for (const auto &e : entries) {
			if (reinterpret_cast<ULONG_PTR>(e.get()) == data) {
				return e.get();
			}
		}
		return nullptr;
	}

	struct Frame {
		HWND window{};
		HRGN original{}, outline{};
		int width{}, height{};
	};

	std::array<Frame, 32> frames{};
	inline static thread_local Impl *tracking = nullptr;
	bool attaching = false;

	bool owns(HMENU menu) const noexcept
	{
		return menu == root.value ||
		       std::any_of(entries.begin(), entries.end(), [&](const auto &e) {
				   return e->child == menu && menu;
			   });
	}

	Frame *frame(HWND window) noexcept
	{
		for (auto &f : frames) {
			if (f.window == window) {
				return &f;
			}
		}
		return nullptr;
	}

	void forget(Frame &f, bool restore) noexcept
	{
		if (restore && IsWindow(f.window)) {
			RemoveWindowSubclass(f.window, frame_proc,
			                     reinterpret_cast<UINT_PTR>(this));
			if (SetWindowRgn(f.window, f.original, FALSE)) {
				f.original = nullptr;
			}
		}
		if (f.original) {
			DeleteObject(f.original);
		}
		if (f.outline) {
			DeleteObject(f.outline);
		}
		f = {};
	}

	void shape(Frame &f) noexcept
	{
		RECT r{};
		if (!GetWindowRect(f.window, &r)) {
			return;
		}
		const int w = r.right - r.left, h = r.bottom - r.top,
				  q = std::max(1, px(2));
		if (w == f.width && h == f.height) {
			return;
		}
		if (w < 24 * q || h < 10 * q) {
			return;
		}
		const int a = w / 3 / q * q, b = 2 * w / 3 / q * q;
		const POINT outline[]{{3 * q, 0},
		                      {a, 0},
		                      {a, q},
		                      {a + 3 * q, q},
		                      {a + 3 * q, 0},
		                      {b, 0},
		                      {b, q},
		                      {b + 3 * q, q},
		                      {b + 3 * q, 0},
		                      {w - 3 * q, 0},
		                      {w - 3 * q, q},
		                      {w - q, q},
		                      {w - q, 3 * q},
		                      {w, 3 * q},
		                      {w, h - 3 * q},
		                      {w - q, h - 3 * q},
		                      {w - q, h - q},
		                      {w - 3 * q, h - q},
		                      {w - 3 * q, h},
		                      {b + 3 * q, h},
		                      {b + 3 * q, h - q},
		                      {b, h - q},
		                      {b, h},
		                      {a + 3 * q, h},
		                      {a + 3 * q, h - q},
		                      {a, h - q},
		                      {a, h},
		                      {3 * q, h},
		                      {3 * q, h - q},
		                      {q, h - q},
		                      {q, h - 3 * q},
		                      {0, h - 3 * q},
		                      {0, 3 * q},
		                      {q, 3 * q},
		                      {q, q},
		                      {3 * q, q}};
		HRGN region = CreatePolygonRgn(
			outline, static_cast<int>(std::size(outline)), WINDING);
		HRGN copy = CreateRectRgn(0, 0, 0, 0);
		if (!region || !copy ||
		    CombineRgn(copy, region, nullptr, RGN_COPY) == ERROR) {
			if (region) {
				DeleteObject(region);
			}
			if (copy) {
				DeleteObject(copy);
			}
			return;
		}
		// SetWindowRgn sends position messages synchronously. Cache first.
		f.width = w;
		f.height = h;
		if (f.outline) {
			DeleteObject(f.outline);
		}
		f.outline = copy;
		if (!SetWindowRgn(f.window, region, TRUE)) {
			DeleteObject(region);
			DeleteObject(f.outline);
			f.outline = nullptr;
			f.width = f.height = 0;
		}
	}

	void paint_frame(Frame &f) noexcept
	{
		if (!f.outline) {
			return;
		}
		HDC dc = GetWindowDC(f.window);
		if (!dc) {
			return;
		}
		{
			SavedDC saved(dc);
			RECT window{}, client{};
			GetWindowRect(f.window, &window);
			GetClientRect(f.window, &client);
			MapWindowPoints(f.window, nullptr,
			                reinterpret_cast<POINT *>(&client), 2);
			OffsetRect(&client, -window.left, -window.top);
			const RECT bounds{0, 0, f.width, f.height};
			const int clip = SaveDC(dc);
			ExcludeClipRect(dc, client.left, client.top, client.right,
			                client.bottom);
			FillRect(dc, &bounds, background.value);
			if (clip) {
				RestoreDC(dc, clip);
			}
			SetDCBrushColor(dc, colors.line);
			FrameRgn(dc, f.outline,
			         static_cast<HBRUSH>(GetStockObject(DC_BRUSH)),
			         std::max(1, px(1)), std::max(1, px(1)));
			// USER32 may paint native arrows with a different theme. Repaint
			// only their reserved gutter; retain native submenu hit
			// testing/navigation.
			MENUBARINFO menu{sizeof(menu)};
			if (GetMenuBarInfo(f.window, OBJID_CLIENT, 0, &menu) &&
			    owns(menu.hMenu)) {
				for (int i = 0; i < GetMenuItemCount(menu.hMenu); ++i) {
					MENUITEMINFOW info{sizeof(info)};
					info.fMask = MIIM_SUBMENU | MIIM_STATE;
					RECT row{};
					if (!GetMenuItemInfoW(menu.hMenu, static_cast<UINT>(i),
					                      TRUE, &info) ||
					    !info.hSubMenu ||
					    !GetMenuItemRect(nullptr, menu.hMenu,
					                     static_cast<UINT>(i), &row)) {
						continue;
					}
					OffsetRect(&row, -window.left, -window.top);
					if (row.top < client.top || row.bottom > client.bottom) {
						continue; // Leave USER32's scrolling arrows
						          // unobstructed.
					}
					const int cy = (row.top + row.bottom) / 2,
							  x = row.right - px(14);
					RECT gutter{x - px(1), cy - px(6), row.right - px(5),
					            cy + px(7)};
					SetDCBrushColor(dc, colors.hover);
					FillRect(dc, &gutter,
					         (info.fState & MFS_HILITE)
					             ? static_cast<HBRUSH>(GetStockObject(DC_BRUSH))
					             : background.value);
					SetDCBrushColor(dc, (info.fState & MFS_HILITE)
					                        ? colors.accent
					                        : colors.muted);
					for (int n = 0; n < 4; ++n) {
						RECT upper{x + px(n), cy - px(3 - n), x + px(n + 1),
						           cy - px(2 - n)};
						RECT lower{x + px(n), cy + px(3 - n), x + px(n + 1),
						           cy + px(4 - n)};
						FillRect(dc, &upper,
						         static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
						FillRect(dc, &lower,
						         static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
					}
				}
			}
		}
		ReleaseDC(f.window, dc);
	}

	static LRESULT CALLBACK frame_proc(HWND window, UINT message, WPARAM wp,
	                                   LPARAM lp, UINT_PTR id,
	                                   DWORD_PTR data) noexcept
	{
		auto &self = *reinterpret_cast<Impl *>(data);
		if (message == WM_NCDESTROY) {
			RemoveWindowSubclass(window, frame_proc, id);
			if (auto *f = self.frame(window)) {
				self.forget(*f, false);
			}
			return DefSubclassProc(window, message, wp, lp);
		}
		const auto result = DefSubclassProc(window, message, wp, lp);
		if (auto *f = self.frame(window)) {
			if (message == WM_WINDOWPOSCHANGED || message == WM_SHOWWINDOW ||
			    message == WM_PAINT || message == WM_NCPAINT ||
			    message == WM_MOUSEMOVE || message == WM_KEYDOWN) {
				self.shape(*f);
				self.paint_frame(*f);
			}
		}
		return result;
	}

	static LRESULT CALLBACK menu_hook(int code, WPARAM wp, LPARAM lp) noexcept
	{
		auto *self = tracking;
		if (code >= 0 && self && !self->attaching) {
			const auto &msg = *reinterpret_cast<const CWPSTRUCT *>(lp);
			if ((msg.message == WM_WINDOWPOSCHANGED ||
			     msg.message == WM_WINDOWPOSCHANGING ||
			     msg.message == WM_SHOWWINDOW || msg.message == WM_PAINT) &&
			    !self->frame(msg.hwnd)) {
				// Only our own HMENU tree, on the calling thread. Never style
				// Explorer.
				wchar_t name[32]{};
				GetClassNameW(msg.hwnd, name, 32);
				if (std::wcscmp(name, L"#32768") == 0) {
					self->attaching = true;
					MENUBARINFO menu{sizeof(menu)};
					if (GetMenuBarInfo(msg.hwnd, OBJID_CLIENT, 0, &menu) &&
					    self->owns(menu.hMenu)) {
						for (auto &f : self->frames) {
							if (f.window) {
								continue;
							}
							HRGN original = CreateRectRgn(0, 0, 0, 0);
							if (!original) {
								break;
							}
							if (GetWindowRgn(msg.hwnd, original) == ERROR) {
								DeleteObject(original);
								original = nullptr;
							}
							if (SetWindowSubclass(
									msg.hwnd, frame_proc,
									reinterpret_cast<UINT_PTR>(self),
									reinterpret_cast<DWORD_PTR>(self))) {
								f.window = msg.hwnd;
								f.original = original;
							} else if (original) {
								DeleteObject(original);
							}
							break;
						}
					}
					self->attaching = false;
				}
			}
		}
		return CallNextHookEx(nullptr, code, wp, lp);
	}

	UINT track(UINT flags, POINT anchor, HWND owner)
	{
		if (!custom || tracking) {
			return static_cast<UINT>(TrackPopupMenuEx(
				root.value, flags, anchor.x, anchor.y, owner, nullptr));
		}

		struct Session {
			Impl &self;
			HHOOK hook{};

			~Session()
			{
				if (hook) {
					UnhookWindowsHookEx(hook);
				}
				tracking = nullptr;
				for (auto &f : self.frames) {
					if (f.window) {
						self.forget(f, true);
					}
				}
			}
		} session{*this};

		tracking = this;
		session.hook = SetWindowsHookExW(WH_CALLWNDPROC, menu_hook, nullptr,
		                                 GetCurrentThreadId());
		return static_cast<UINT>(TrackPopupMenuEx(root.value, flags, anchor.x,
		                                          anchor.y, owner, nullptr));
	}

	void append(HMENU menu, UINT id, std::wstring label, Role role,
	            MenuGlyph glyph, MenuMark mark, bool checked,
	            std::wstring value = {}, HMENU child = nullptr)
	{
		auto entry = std::make_unique<Entry>();
		auto &e = *entry;
		e.label = std::move(label);
		e.value = std::move(value);
		e.owner = menu;
		e.child = child;
		e.role = role;
		e.glyph = glyph;
		e.mark = mark;
		e.spoken = e.label + (e.value.empty() ? L"" : L", " + e.value);
		e.accessibility = {MSAA_MENU_SIG, static_cast<DWORD>(e.spoken.size()),
		                   e.spoken.data()};
		std::wstring native_text =
			e.label + (e.value.empty() ? L"" : L"\t" + e.value);
		if (custom) {
			ScreenDC screen;
			check_win(screen.value != nullptr, "Measure tray menu");
			SavedDC saved(screen.value);
			SelectObject(
				screen.value,
				role == Role::header
					? title.value
					: (role == Role::section ? small_font.value : body.value));
			SIZE label_size{}, value_size{};
			GetTextExtentPoint32W(screen.value, e.label.c_str(),
			                      static_cast<int>(e.label.size()),
			                      &label_size);
			SelectObject(screen.value, small_font.value);
			GetTextExtentPoint32W(screen.value, e.value.c_str(),
			                      static_cast<int>(e.value.size()),
			                      &value_size);
			// Match the actual text origin, control area and value gap in
			// draw().
			const int left_padding =
				role == Role::header ? 40 : (role == Role::item ? 38 : 12);
			const int right_padding =
				role == Role::item
					? (mark == MenuMark::toggle ? 46 : (child ? 26 : 12))
					: 12;
			const int value_gap = e.value.empty() ? 0 : 12;
			const int content_width =
				static_cast<int>(label_size.cx + value_size.cx) +
				px(left_padding + right_padding + value_gap);
			e.width = static_cast<UINT>(std::min(
				width_limit,
				std::max(px(menu == root.value ? 282 : 208), content_width)));
			e.height = static_cast<UINT>(
				role == Role::header    ? std::max(px(38), body_height + px(12))
				: role == Role::section ? std::max(px(20), body_height + px(3))
				: role == Role::separator
					? px(7)
					: std::max(px(26), body_height + px(8)));
		}
		MENUITEMINFOW info{sizeof(info)};
		info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_STRING;
		info.wID = id;
		info.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
		if (role != Role::item) {
			info.fState |= MFS_DISABLED;
		}
		info.dwTypeData = native_text.data();
		info.fType = role == Role::separator ? MFT_SEPARATOR : MFT_STRING;
		if (mark == MenuMark::radio) {
			info.fType |= MFT_RADIOCHECK;
		}
		if (child) {
			info.fMask |= MIIM_SUBMENU;
			info.hSubMenu = child;
		}
		if (custom) {
			info.fMask |= MIIM_DATA;
			info.fType =
				MFT_OWNERDRAW | (mark == MenuMark::radio ? MFT_RADIOCHECK : 0);
			info.dwItemData = reinterpret_cast<ULONG_PTR>(entry.get());
		}
		// Store before InsertMenuItem: callbacks must always see stable item
		// data.
		entries.push_back(std::move(entry));
		check_win(InsertMenuItemW(menu,
		                          static_cast<UINT>(GetMenuItemCount(menu)),
		                          TRUE, &info),
		          "Add tray item");
	}

	void glyph(HDC dc, MenuGlyph kind, int x, int y) const noexcept
	{
		auto line = [&](int x1, int y1, int x2, int y2) {
			const int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
			const int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
			int error = dx + dy;
			for (;;) {
				RECT dot{x + px(x1), y + px(y1), x + px(x1 + 1),
				         y + px(y1 + 1)};
				FillRect(dc, &dot,
				         static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
				if (x1 == x2 && y1 == y2) {
					break;
				}
				const int twice = 2 * error;
				if (twice >= dy) {
					error += dy;
					x1 += sx;
				}
				if (twice <= dx) {
					error += dx;
					y1 += sy;
				}
			}
		};
		auto box = [&](int x1, int y1, int x2, int y2) {
			line(x1, y1, x2, y1);
			line(x2, y1, x2, y2);
			line(x2, y2, x1, y2);
			line(x1, y2, x1, y1);
		};
		switch (kind) {
		case MenuGlyph::snow:
			line(8, 1, 8, 16);
			line(2, 4, 14, 12);
			line(2, 12, 14, 4);
			break;
		case MenuGlyph::wind:
			line(1, 4, 13, 4);
			line(3, 8, 16, 8);
			line(1, 12, 11, 12);
			line(13, 4, 15, 2);
			break;
		case MenuGlyph::layers:
			box(2, 2, 12, 11);
			line(6, 13, 15, 13);
			line(15, 13, 15, 6);
			break;
		case MenuGlyph::bank:
			line(1, 13, 16, 13);
			line(2, 12, 6, 6);
			line(6, 6, 10, 10);
			line(10, 10, 13, 7);
			line(13, 7, 16, 12);
			break;
		case MenuGlyph::cursor:
			line(4, 1, 4, 13);
			line(4, 1, 14, 9);
			line(4, 13, 8, 9);
			line(8, 9, 14, 9);
			line(9, 10, 12, 16);
			break;
		case MenuGlyph::speed:
			line(3, 13, 3, 9);
			line(8, 13, 8, 5);
			line(13, 13, 13, 1);
			line(1, 15, 16, 15);
			break;
		case MenuGlyph::capture:
			box(1, 4, 16, 14);
			box(5, 2, 11, 4);
			box(6, 7, 11, 12);
			break;
		case MenuGlyph::startup:
			line(3, 13, 13, 3);
			line(5, 3, 13, 3);
			line(13, 3, 13, 11);
			break;
		case MenuGlyph::tools:
			box(2, 3, 15, 13);
			line(5, 6, 8, 8);
			line(8, 8, 5, 10);
			line(10, 10, 13, 10);
			break;
		case MenuGlyph::pause:
			box(4, 2, 6, 15);
			box(11, 2, 13, 15);
			break;
		case MenuGlyph::stop:
			box(3, 3, 14, 14);
			break;
		case MenuGlyph::exit:
			line(7, 2, 3, 2);
			line(3, 2, 3, 15);
			line(3, 15, 7, 15);
			line(7, 8, 16, 8);
			line(12, 4, 16, 8);
			line(16, 8, 12, 12);
			break;
		case MenuGlyph::clear:
			line(4, 4, 13, 13);
			line(13, 4, 4, 13);
			break;
		case MenuGlyph::folder:
			box(1, 5, 16, 14);
			line(1, 5, 1, 2);
			line(1, 2, 7, 2);
			line(7, 2, 10, 5);
			break;
		case MenuGlyph::file:
			box(3, 1, 14, 16);
			line(6, 6, 11, 6);
			line(6, 10, 11, 10);
			break;
		case MenuGlyph::refresh:
			line(12, 3, 5, 3);
			line(5, 3, 2, 6);
			line(2, 6, 2, 12);
			line(2, 12, 5, 15);
			line(5, 15, 12, 15);
			line(12, 15, 15, 12);
			line(14, 1, 14, 6);
			line(9, 6, 14, 6);
			break;
		default:
			break;
		}
	}
};

TrayMenu::TrayMenu(HINSTANCE instance, POINT anchor, MenuTheme theme)
	: impl_(std::make_unique<Impl>(instance, anchor, theme))
{
}

TrayMenu::~TrayMenu() = default;

HMENU TrayMenu::handle() const noexcept
{
	return impl_->root.value;
}

bool TrayMenu::styled() const noexcept
{
	return impl_->custom;
}

UINT TrayMenu::track(UINT flags, POINT anchor, HWND owner)
{
	return impl_->track(flags, anchor, owner);
}

void TrayMenu::header(const std::wstring &version)
{
	impl_->append(handle(), 0, L"speeecial", Role::header, MenuGlyph::none,
	              MenuMark::none, false, version);
}

void TrayMenu::section(HMENU menu, const std::wstring &label)
{
	impl_->append(menu, 0, label, Role::section, MenuGlyph::none,
	              MenuMark::none, false);
}

void TrayMenu::separator(HMENU menu)
{
	impl_->append(menu, 0, {}, Role::separator, MenuGlyph::none, MenuMark::none,
	              false);
}

HMENU TrayMenu::submenu(HMENU parent, const std::wstring &label,
                        MenuGlyph glyph, const std::wstring &value)
{
	MenuHandle child{CreatePopupMenu()};
	check_win(child.value != nullptr, "Create tray submenu");
	impl_->configure(child.value);
	impl_->append(parent, 0, label, Role::item, glyph, MenuMark::none, false,
	              value, child.value);
	const auto result = child.value;
	child.value = nullptr;
	return result;
}

void TrayMenu::item(HMENU menu, UINT id, const std::wstring &label,
                    MenuMark mark, bool checked, MenuGlyph glyph,
                    const std::wstring &value)
{
	impl_->append(menu, id, label, Role::item, glyph, mark, checked, value);
}

bool TrayMenu::measure(MEASUREITEMSTRUCT &item) const noexcept
{
	const auto *entry = impl_->find(item.itemData);
	if (!impl_->custom || item.CtlType != ODT_MENU || !entry) {
		return false;
	}
	item.itemWidth = entry->width;
	item.itemHeight = entry->height;
	return true;
}

bool TrayMenu::draw(const DRAWITEMSTRUCT &item) const noexcept
{
	const auto *e = impl_->find(item.itemData);
	if (!impl_->custom || item.CtlType != ODT_MENU || !e || !item.hDC) {
		return false;
	}
	auto &s = *impl_;
	const auto px = [&](int n) { return s.px(n); };
	const auto &c = s.colors;
	const auto dc = item.hDC;
	SavedDC saved(dc);
	if (!saved.state) {
		return false;
	}
	IntersectClipRect(dc, item.rcItem.left, item.rcItem.top, item.rcItem.right,
	                  item.rcItem.bottom);
	FillRect(dc, &item.rcItem, s.background.value);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SelectObject(dc, GetStockObject(NULL_PEN));
	const bool active = (item.itemState & ODS_CHECKED) != 0;
	const bool hot =
		(item.itemState & ODS_SELECTED) != 0 && e->role == Role::item;
	RECT r = item.rcItem;
	const int cy = (r.top + r.bottom) / 2;
	if (e->role == Role::separator) {
		SetDCBrushColor(dc, c.line);
		for (int x = r.left + px(12); x < r.right - px(12); x += px(5)) {
			RECT dot{x, cy, x + std::max(1, px(1)), cy + std::max(1, px(1))};
			FillRect(dc, &dot, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		}
		return true;
	}
	if (hot || (active && e->mark != MenuMark::toggle)) {
		SetDCBrushColor(dc, hot ? c.hover : c.active);
		const int l = r.left + px(4), t = r.top + px(2), rr = r.right - px(4),
				  b = r.bottom - px(2), q = px(2);
		const POINT outline[]{{l + q, t},  {rr - q, t}, {rr - q, t + q},
		                      {rr, t + q}, {rr, b - q}, {rr - q, b - q},
		                      {rr - q, b}, {l + q, b},  {l + q, b - q},
		                      {l, b - q},  {l, t + q},  {l + q, t + q}};
		Gdi<HRGN> region;
		region.value = CreatePolygonRgn(
			outline, static_cast<int>(std::size(outline)), WINDING);
		if (region.value) {
			FillRgn(dc, region.value,
			        static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
			SetDCBrushColor(dc, hot ? c.accent : c.line);
			FrameRgn(dc, region.value,
			         static_cast<HBRUSH>(GetStockObject(DC_BRUSH)),
			         std::max(1, px(1)), std::max(1, px(1)));
		}
	}
	if (e->role == Role::header) {
		if (s.icon.value) {
			DrawIconEx(dc, r.left + px(12), cy - px(10), s.icon.value, px(20),
			           px(20), 0, nullptr, DI_NORMAL);
		}
		SelectObject(dc, s.title.value);
		SetTextColor(dc, c.text);
		RECT text{r.left + px(40), r.top, r.right - px(70), r.bottom};
		DrawTextW(dc, e->label.c_str(), -1, &text,
		          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
		SelectObject(dc, s.small_font.value);
		SetTextColor(dc, c.muted);
		SIZE name_size{};
		SelectObject(dc, s.title.value);
		GetTextExtentPoint32W(dc, e->label.c_str(),
		                      static_cast<int>(e->label.size()), &name_size);
		SelectObject(dc, s.small_font.value);
		text = {r.left + px(48) + name_size.cx, r.top, r.right - px(12),
		        r.bottom};
		DrawTextW(dc, e->value.c_str(), -1, &text,
		          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
		SetDCBrushColor(dc, c.line);
		for (int x = r.left + px(12); x < r.right - px(12); x += px(5)) {
			RECT dot{x, r.bottom - px(2), x + std::max(1, px(1)),
			         r.bottom - px(1)};
			FillRect(dc, &dot, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		}
		return true;
	}
	if (e->role == Role::section) {
		SelectObject(dc, s.small_font.value);
		SetTextColor(dc, c.muted);
		RECT text{r.left + px(12), r.top + px(2), r.right - px(12), r.bottom};
		DrawTextW(dc, e->label.c_str(), -1, &text,
		          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
		return true;
	}
	const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
	const auto ink = disabled ? c.muted : (active ? c.accent : c.text);
	Gdi<HPEN> pen;
	pen.value = CreatePen(PS_SOLID, std::max(1, px(1)), ink);
	SelectObject(dc, pen.value ? pen.value : GetStockObject(DC_PEN));
	SetDCPenColor(dc, ink);
	SelectObject(dc, GetStockObject(NULL_BRUSH));
	SetDCBrushColor(dc, ink);
	s.glyph(dc, e->glyph, r.left + px(12), cy - px(8));
	if (e->mark == MenuMark::radio) {
		SelectObject(dc, GetStockObject(DC_BRUSH));
		SelectObject(dc, GetStockObject(NULL_PEN));
		SetDCBrushColor(dc, active ? c.accent : c.track);
		const int size = px(active ? 7 : 4);
		RECT mark{r.left + px(20) - size / 2, cy - size / 2,
		          r.left + px(20) + (size + 1) / 2, cy + (size + 1) / 2};
		FillRect(dc, &mark, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
	}
	if (e->mark == MenuMark::toggle) {
		SelectObject(dc, GetStockObject(DC_BRUSH));
		SelectObject(dc, GetStockObject(NULL_PEN));
		const int x = r.right - px(36);
		// Position AND colour encode the state; off must not look like on.
		for (int cell = 0; cell < 5; ++cell) {
			RECT block{x + px(cell * 5), cy - px(3), x + px(cell * 5 + 4),
			           cy + px(4)};
			SetDCBrushColor(dc, active ? c.accent : c.track);
			FillRect(dc, &block, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		}
		const int thumb_x = x + px(active ? 19 : -1);
		RECT thumb{thumb_x, cy - px(5), thumb_x + px(10), cy + px(6)};
		SetDCBrushColor(dc, c.thumb);
		FillRect(dc, &thumb, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		SetDCBrushColor(dc, active ? c.accent : c.muted);
		FrameRect(dc, &thumb, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
	}
	SelectObject(dc, s.body.value);
	SetTextColor(dc, ink);
	int right =
		r.right - px(e->mark == MenuMark::toggle ? 46 : (e->child ? 26 : 12));
	if (!e->value.empty()) {
		SelectObject(dc, s.small_font.value);
		SIZE extent{};
		GetTextExtentPoint32W(dc, e->value.c_str(),
		                      static_cast<int>(e->value.size()), &extent);
		RECT value{std::max(r.left + px(100), right - extent.cx), r.top, right,
		           r.bottom};
		SetTextColor(dc, active ? c.accent : c.muted);
		DrawTextW(dc, e->value.c_str(), -1, &value,
		          DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX |
		              DT_END_ELLIPSIS);
		right = value.left - px(12);
		SelectObject(dc, s.body.value);
		SetTextColor(dc, ink);
	}
	RECT text{r.left + px(38), r.top, right, r.bottom};
	DrawTextW(dc, e->label.c_str(), -1, &text,
	          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
	// USER32 owns the submenu arrow and its activation area.
	if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
		RECT focus{r.left + px(5), r.top + px(2), r.right - px(5),
		           r.bottom - px(2)};
		DrawFocusRect(dc, &focus);
	}
	// Deselect the temporary pen before DeleteObject, then restore the caller
	// DC.
	SelectObject(dc, GetStockObject(DC_PEN));
	return true;
}

LRESULT TrayMenu::character(wchar_t key, HMENU menu) const noexcept
{
	key = lower(key);
	int current = -1, first = -1, next = -1;
	unsigned matches = 0;
	const int count = GetMenuItemCount(menu);
	for (int i = 0; i < count; ++i) {
		MENUITEMINFOW info{sizeof(info)};
		info.fMask = MIIM_DATA | MIIM_STATE;
		if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
			continue;
		}
		if (info.fState & MFS_HILITE) {
			current = i;
		}
	}
	for (int i = 0; i < count; ++i) {
		MENUITEMINFOW info{sizeof(info)};
		info.fMask = MIIM_DATA | MIIM_STATE;
		if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) ||
		    (info.fState & MFS_DISABLED)) {
			continue;
		}
		const auto *e = impl_->find(info.dwItemData);
		if (!e || e->role != Role::item || e->label.empty() ||
		    lower(e->label.front()) != key) {
			continue;
		}
		++matches;
		if (first < 0) {
			first = i;
		}
		if (i > current && next < 0) {
			next = i;
		}
	}
	if (!matches) {
		return MAKELRESULT(0, MNC_IGNORE);
	}
	return MAKELRESULT(next < 0 ? first : next,
	                   matches == 1 ? MNC_EXECUTE : MNC_SELECT);
}
}
