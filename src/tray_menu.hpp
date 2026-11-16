#pragma once

#include <memory>
#include <string>
#include <windows.h>

namespace snow
{
enum class MenuGlyph {
	none,
	snow,
	layers,
	wind,
	bank,
	cursor,
	speed,
	capture,
	startup,
	tools,
	pause,
	stop,
	exit,
	clear,
	folder,
	file,
	refresh,
	theme,
	language
};
enum class MenuMark { none, toggle, radio, action };
enum class MenuTheme { automatic, light, dark, native };

// A native HMENU tree with scoped GDI resources. USER32 retains navigation,
// submenu placement, dismissal, scrolling and command delivery.
class TrayMenu {
  public:
	TrayMenu(HINSTANCE instance, POINT anchor,
	         MenuTheme theme = MenuTheme::automatic);
	~TrayMenu();
	TrayMenu(const TrayMenu &) = delete;
	TrayMenu &operator=(const TrayMenu &) = delete;
	HMENU handle() const noexcept;
	bool styled() const noexcept;
	UINT track(UINT flags, POINT anchor, HWND owner);
	void header(const std::wstring &version);
	void section(HMENU menu, const std::wstring &label);
	void separator(HMENU menu);
	HMENU submenu(HMENU parent, const std::wstring &label,
	              MenuGlyph glyph = MenuGlyph::none,
	              const std::wstring &value = {});
	void item(HMENU menu, UINT id, const std::wstring &label,
	          MenuMark mark = MenuMark::none, bool checked = false,
	          MenuGlyph glyph = MenuGlyph::none, const std::wstring &value = {},
	          bool enabled = true);
	bool measure(MEASUREITEMSTRUCT &item) const noexcept;
	bool draw(const DRAWITEMSTRUCT &item) const noexcept;
	LRESULT character(wchar_t key, HMENU menu) const noexcept;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
}
