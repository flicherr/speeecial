#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace snow::updates
{
inline constexpr std::string_view repository = "flicherr/speeecial";
inline constexpr std::string_view asset_name = "speeecial-windows-x64.exe";
inline constexpr std::string_view releases_page =
	"https://github.com/flicherr/speeecial/releases";
inline constexpr std::uint64_t max_download = 128 * 1024 * 1024;
using Version = std::array<unsigned, 3>;
std::optional<Version> version(std::string_view value) noexcept;

struct Release {
	std::string tag, url, sha256;
	std::uint64_t size = 0;

	bool installable() const noexcept
	{
		return !url.empty();
	}
};

// Null means no newer stable release; a newer release without a suitable
// asset remains visible, but is offered only on the GitHub releases page.
std::optional<Release> parse_release(std::string_view json,
                                     std::string_view current);
bool automatic_due(std::int64_t now, std::int64_t last_attempt) noexcept;
std::wstring quote_argument(std::wstring_view argument);
}
