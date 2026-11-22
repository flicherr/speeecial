#include "tray_icon.hpp"
#include "win_util.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
using namespace snow;

void require(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

struct Icon {
	HICON value{};

	~Icon()
	{
		if (value) {
			DestroyIcon(value);
		}
	}
};

struct Canvas {
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bitmap{};
	HGDIOBJ previous{};
	std::uint32_t *pixels{};
	int width, height;

	Canvas(int w, int h, std::uint32_t background = 0) : width(w), height(h)
	{
		check_win(dc != nullptr, "Create icon test DC");
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		void *bits = nullptr;
		bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap) {
			DeleteDC(dc);
			dc = nullptr;
			check_win(FALSE, "Create icon test bitmap");
		}
		pixels = static_cast<std::uint32_t *>(bits);
		std::fill_n(pixels, static_cast<std::size_t>(width) * height,
		            background);
		previous = SelectObject(dc, bitmap);
	}

	~Canvas()
	{
		if (dc) {
			SelectObject(dc, previous);
			DeleteDC(dc);
		}
		if (bitmap) {
			DeleteObject(bitmap);
		}
	}

	std::vector<std::uint32_t> draw(HICON icon)
	{
		check_win(
			DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL),
			"Render icon test");
		GdiFlush();
		return {pixels, pixels + static_cast<std::size_t>(width) * height};
	}
};

void exercise(int width, int height, bool dark)
{
	const auto instance = GetModuleHandleW(nullptr);
	Icon original{static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101),
	                                            IMAGE_ICON, width, height, 0))};
	Icon plain{load_tray_icon(instance, width, height, false, dark)};
	Icon marked{load_tray_icon(instance, width, height, true, dark)};
	require(original.value && plain.value && marked.value,
	        "An icon is missing");
	Canvas raw_original(width, height), raw_plain(width, height),
		raw_marked(width, height);
	const auto original_pixels = raw_original.draw(original.value);
	const auto plain_pixels = raw_plain.draw(plain.value);
	const auto marked_pixels = raw_marked.draw(marked.value);
	require(plain_pixels == original_pixels,
	        "The unmarked icon must match the resource exactly");
	unsigned amber_pixels = 0, changed_pixels = 0;
	const int size = std::min(width, height);
	const int diameter = std::max(1, (size + 2) / 4);
	const int halo = std::max(1, size / 24);
	const int left = width - diameter - 2 * halo;
	const int bottom = diameter + 2 * halo;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const auto index = static_cast<std::size_t>(y) * width + x;
			const auto pixel = marked_pixels[index];
			const auto alpha = pixel >> 24;
			require((pixel & 255) <= alpha && ((pixel >> 8) & 255) <= alpha &&
			            ((pixel >> 16) & 255) <= alpha,
			        "The generated icon must use premultiplied alpha");
			if (pixel == 0xfff2a93bu) {
				++amber_pixels;
			}
			if (pixel != plain_pixels[index]) {
				++changed_pixels;
				if (x < left || y >= bottom) {
					std::cerr << "Changed pixel " << width << 'x' << height
							  << " at " << x << ',' << y << ": " << std::hex
							  << plain_pixels[index] << " -> " << pixel
							  << std::dec << '\n';
				}
				require(x >= left && y < bottom,
				        "The badge changed pixels outside its corner");
			}
		}
	}
	require(amber_pixels && changed_pixels, "No visible amber dot was drawn");
	require(marked_pixels.front() == 0 && marked_pixels.back() == 0,
	        "Icon corners must remain transparent");
	for (auto background : {0xfff7fafcu, 0xff171b1fu}) {
		Canvas plain_canvas(width, height, background),
			marked_canvas(width, height, background);
		const auto before = plain_canvas.draw(plain.value);
		const auto after = marked_canvas.draw(marked.value);
		require(after.front() == before.front() &&
		            after.back() == before.back(),
		        "The marked icon must not introduce a background rectangle");
		for (int y = bottom; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const auto index = static_cast<std::size_t>(y) * width + x;
				require((after[index] & 0xffffffu) ==
				            (before[index] & 0xffffffu),
				        "The original star changed when compositing a badge");
			}
		}
	}
}

void invalid_size()
{
	for (const SIZE size : {SIZE{0, 16}, SIZE{16, -1}, SIZE{257, 24}}) {
		bool rejected = false;
		try {
			Icon icon{load_tray_icon(GetModuleHandleW(nullptr), size.cx,
			                         size.cy, true, false)};
		} catch (const std::invalid_argument &) {
			rejected = true;
		}
		require(rejected, "Invalid icon dimensions were accepted");
	}
}
}

int main()
{
	try {
		for (const SIZE size : {SIZE{16, 16}, SIZE{20, 20}, SIZE{24, 24},
		                        SIZE{32, 32}, SIZE{48, 48}, SIZE{24, 20}}) {
			for (bool dark : {false, true}) {
				exercise(size.cx, size.cy, dark);
			}
		}
		invalid_size();
		const auto process = GetCurrentProcess();
		const auto gdi = GetGuiResources(process, GR_GDIOBJECTS);
		const auto user = GetGuiResources(process, GR_USEROBJECTS);
		for (int iteration = 0; iteration < 200; ++iteration) {
			Icon icon{load_tray_icon(
				GetModuleHandleW(nullptr), 16 + iteration % 3 * 4,
				16 + iteration % 3 * 4, true, iteration % 2 != 0)};
		}
		require(GetGuiResources(process, GR_GDIOBJECTS) <= gdi + 2 &&
		            GetGuiResources(process, GR_USEROBJECTS) <= user + 2,
		        "Recreating the update icon leaks GDI or USER resources");
		std::cout << "PASS tray icon sizes, amber corner, alpha, unchanged "
					 "resource and USER/GDI lifecycle\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
