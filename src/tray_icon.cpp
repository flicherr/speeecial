#include "tray_icon.hpp"
#include "win_util.hpp"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace snow
{
namespace
{
struct Icon {
	HICON value{};

	~Icon()
	{
		if (value) {
			DestroyIcon(value);
		}
	}

	HICON release() noexcept
	{
		return std::exchange(value, nullptr);
	}
};

struct Bitmap {
	HBITMAP value{};

	~Bitmap()
	{
		if (value) {
			DeleteObject(value);
		}
	}
};

struct Surface {
	HDC dc = CreateCompatibleDC(nullptr);
	Bitmap bitmap;
	HGDIOBJ previous{};
	std::uint32_t *pixels{};

	Surface(int width, int height)
	{
		check_win(dc != nullptr, "Create tray icon DC");
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		void *bits = nullptr;
		bitmap.value =
			CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap.value) {
			DeleteDC(dc);
			dc = nullptr;
			check_win(FALSE, "Create tray icon bitmap");
		}
		pixels = static_cast<std::uint32_t *>(bits);
		std::fill_n(pixels, static_cast<std::size_t>(width) * height, 0);
		previous = SelectObject(dc, bitmap.value);
		if (!previous || previous == HGDI_ERROR) {
			DeleteDC(dc);
			dc = nullptr;
			check_win(FALSE, "Select tray icon bitmap");
		}
	}

	~Surface()
	{
		if (dc) {
			if (previous) {
				SelectObject(dc, previous);
			}
			DeleteDC(dc);
		}
	}

	void deselect()
	{
		const auto selected = SelectObject(dc, previous);
		check_win(selected && selected != HGDI_ERROR,
		          "Deselect tray icon bitmap");
		previous = nullptr;
	}
};

void badge(std::uint32_t *pixels, int width, int height, bool dark)
{
	const int size = std::min(width, height);
	const int diameter = std::max(1, (size + 2) / 4);
	const int halo = std::max(1, size / 24);
	const double radius = diameter / 2.0;
	const double outer = radius + halo;
	const double center_x = width - outer;
	const double center_y = outer;
	const int left = std::max(0, width - diameter - 2 * halo);
	const int bottom = std::min(height, diameter + 2 * halo);
	const std::uint32_t surround = dark ? 0x1f242au : 0xe8eef3u;
	constexpr std::uint32_t amber = 0xf2a93bu;
	constexpr unsigned samples = 8, total = samples * samples;
	for (int y = 0; y < bottom; ++y) {
		for (int x = left; x < width; ++x) {
			unsigned inner_count = 0, halo_count = 0;
			for (unsigned sy = 0; sy < samples; ++sy) {
				for (unsigned sx = 0; sx < samples; ++sx) {
					const double dx = x + (sx + 0.5) / samples - center_x;
					const double dy = y + (sy + 0.5) / samples - center_y;
					const double distance = dx * dx + dy * dy;
					if (distance <= radius * radius) {
						++inner_count;
					} else if (distance <= outer * outer) {
						++halo_count;
					}
				}
			}
			const unsigned covered = inner_count + halo_count;
			if (!covered) {
				continue;
			}
			auto &pixel = pixels[static_cast<std::size_t>(y) * width + x];
			std::uint32_t result = 0;
			// Blend into premultiplied BGRA, including the destination alpha.
			// GDI shape drawing alone would leave an invisible or black badge.
			for (unsigned shift : {0u, 8u, 16u, 24u}) {
				const unsigned dot = shift == 24 ? 255 : (amber >> shift) & 255;
				const unsigned ring =
					shift == 24 ? 255 : (surround >> shift) & 255;
				const unsigned old = (pixel >> shift) & 255;
				const unsigned channel =
					(dot * inner_count + ring * halo_count +
				     old * (total - covered) + total / 2) /
					total;
				result |= channel << shift;
			}
			pixel = result;
		}
	}
}
}

HICON load_tray_icon(HINSTANCE instance, int width, int height,
                     bool update_available, bool dark_background)
{
	if (width < 1 || height < 1 || width > 256 || height > 256) {
		throw std::invalid_argument("Invalid tray icon size");
	}
	Icon source{static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101),
	                                          IMAGE_ICON, width, height,
	                                          LR_DEFAULTCOLOR))};
	check_win(source.value != nullptr, "Load tray icon");
	if (!update_available) {
		return source.release();
	}
	Surface image(width, height);
	ICONINFO source_info{};
	check_win(GetIconInfo(source.value, &source_info), "Read tray icon");
	Bitmap source_color{source_info.hbmColor}, source_mask{source_info.hbmMask};
	BITMAPINFO bitmap_info{};
	bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info.bmiHeader.biWidth = width;
	bitmap_info.bmiHeader.biHeight = -height;
	bitmap_info.bmiHeader.biPlanes = 1;
	bitmap_info.bmiHeader.biBitCount = 32;
	check_win(source_color.value &&
	              GetDIBits(image.dc, source_color.value, 0, height,
	                        image.pixels, &bitmap_info,
	                        DIB_RGB_COLORS) == height,
	          "Read tray icon pixels");
	const auto count = static_cast<std::size_t>(width) * height;
	if (std::none_of(image.pixels, image.pixels + count,
	                 [](std::uint32_t pixel) { return pixel >> 24; })) {
		// Also support non-alpha resource frames. Recover opacity from their
		// AND mask rather than turning their transparent background opaque.
		Surface mask(width, height);
		check_win(DrawIconEx(image.dc, 0, 0, source.value, width, height, 0,
		                     nullptr, DI_NORMAL),
		          "Draw tray icon");
		check_win(DrawIconEx(mask.dc, 0, 0, source.value, width, height, 0,
		                     nullptr, DI_MASK),
		          "Draw tray icon mask");
		GdiFlush();
		for (std::size_t index = 0; index < count; ++index) {
			if ((mask.pixels[index] & 0xffffffu) == 0) {
				image.pixels[index] |= 0xff000000u;
			} else {
				image.pixels[index] = 0;
			}
		}
	}
	badge(image.pixels, width, height, dark_background);
	const int mask_stride = ((width + 15) / 16) * 2;
	std::vector<unsigned char> mask_bits(
		static_cast<std::size_t>(mask_stride) * height, 0);
	Bitmap mask{CreateBitmap(width, height, 1, 1, mask_bits.data())};
	check_win(mask.value != nullptr, "Create tray icon mask");
	ICONINFO info{};
	info.fIcon = TRUE;
	info.hbmColor = image.bitmap.value;
	info.hbmMask = mask.value;
	// CreateIconIndirect requires both source bitmaps to be unselected.
	image.deselect();
	const auto result = CreateIconIndirect(&info);
	check_win(result != nullptr, "Create update tray icon");
	return result;
}
}
