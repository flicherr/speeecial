#include "update_core.hpp"
#include <charconv>
#include <map>
#include <stdexcept>
#include <vector>

namespace snow::updates
{
std::optional<Version> version(std::string_view value) noexcept
{
	if (value.starts_with('v')) {
		value.remove_prefix(1);
	}
	Version result{};
	for (unsigned i = 0; i < 3; ++i) {
		auto end = value.find('.');
		if ((i < 2 && end == value.npos) || (i == 2 && end != value.npos)) {
			return {};
		}
		auto part = value.substr(0, end);
		if (part.empty() || (part.size() > 1 && part.front() == '0')) {
			return {};
		}
		auto parsed =
			std::from_chars(part.data(), part.data() + part.size(), result[i]);
		if (parsed.ec != std::errc{} ||
		    parsed.ptr != part.data() + part.size()) {
			return {};
		}
		if (i < 2) {
			value.remove_prefix(end + 1);
		}
	}
	return result;
}

namespace
{
struct Json {
	enum Kind { null, boolean, number, string, object, array } kind = null;

	std::string text;
	std::map<std::string, Json, std::less<>> fields;
	std::vector<Json> items;

	const Json &at(std::string_view key) const
	{
		auto found = fields.find(key);
		if (kind != object || found == fields.end()) {
			throw std::runtime_error("Missing release field");
		}
		return found->second;
	}

	std::string str() const
	{
		if (kind != string) {
			throw std::runtime_error("Invalid release string");
		}
		return text;
	}

	bool flag() const
	{
		if (kind != boolean) {
			throw std::runtime_error("Invalid release flag");
		}
		return text == "true";
	}
};

class Parser {
	std::string_view input;
	std::size_t pos = 0, nodes = 0;

	[[noreturn]] static void bad()
	{
		throw std::runtime_error("Invalid GitHub JSON response");
	}

	void space()
	{
		while (pos < input.size() &&
		       (input[pos] == ' ' || input[pos] == '\r' || input[pos] == '\n' ||
		        input[pos] == '\t')) {
			++pos;
		}
	}

	bool take(char c)
	{
		space();
		if (pos < input.size() && input[pos] == c) {
			++pos;
			return true;
		}
		return false;
	}

	unsigned hex()
	{
		unsigned result = 0;
		for (int i = 0; i < 4; ++i) {
			if (pos == input.size()) {
				bad();
			}
			char c = input[pos++];
			unsigned n;
			if (c >= '0' && c <= '9') {
				n = unsigned(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				n = unsigned(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				n = unsigned(c - 'A' + 10);
			} else {
				bad();
			}
			result = result * 16 + n;
		}
		return result;
	}

	std::string string()
	{
		if (!take('"')) {
			bad();
		}
		std::string out;
		while (pos < input.size()) {
			unsigned char c = static_cast<unsigned char>(input[pos++]);
			if (c == '"') {
				return out;
			}
			if (c < 32) {
				bad();
			}
			if (c != '\\') {
				out += static_cast<char>(c);
				continue;
			}
			if (pos == input.size()) {
				bad();
			}
			char escaped = input[pos++];
			switch (escaped) {
			case '"':
			case '\\':
			case '/':
				out += escaped;
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				unsigned cp = hex();
				if (cp >= 0xd800 && cp <= 0xdbff) {
					if (input.substr(pos, 2) != "\\u") {
						bad();
					}
					pos += 2;
					unsigned low = hex();
					if (low < 0xdc00 || low > 0xdfff) {
						bad();
					}
					cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
				} else if (cp >= 0xdc00 && cp <= 0xdfff) {
					bad();
				}
				if (cp < 0x80) {
					out += char(cp);
				} else if (cp < 0x800) {
					out += char(0xc0 | (cp >> 6));
					out += char(0x80 | (cp & 63));
				} else if (cp < 0x10000) {
					out += char(0xe0 | (cp >> 12));
					out += char(0x80 | ((cp >> 6) & 63));
					out += char(0x80 | (cp & 63));
				} else {
					out += char(0xf0 | (cp >> 18));
					out += char(0x80 | ((cp >> 12) & 63));
					out += char(0x80 | ((cp >> 6) & 63));
					out += char(0x80 | (cp & 63));
				}
				break;
			}
			default:
				bad();
			}
		}
		bad();
	}

	Json value(unsigned depth)
	{
		if (depth > 32 || ++nodes > 50000) {
			bad();
		}
		space();
		if (pos == input.size()) {
			bad();
		}
		Json out;
		if (input[pos] == '"') {
			out.kind = Json::string;
			out.text = string();
			return out;
		}
		if (take('{')) {
			out.kind = Json::object;
			if (take('}')) {
				return out;
			}
			do {
				auto key = string();
				if (!take(':')) {
					bad();
				}
				if (!out.fields.emplace(std::move(key), value(depth + 1))
				         .second) {
					bad();
				}
			} while (take(','));
			if (!take('}')) {
				bad();
			}
			return out;
		}
		if (take('[')) {
			out.kind = Json::array;
			if (take(']')) {
				return out;
			}
			do {
				out.items.push_back(value(depth + 1));
			} while (take(','));
			if (!take(']')) {
				bad();
			}
			return out;
		}
		for (auto literal : {"true", "false", "null"}) {
			std::string_view word(literal);
			if (input.substr(pos, word.size()) == word) {
				pos += word.size();
				out.kind = word == "null" ? Json::null : Json::boolean;
				out.text = word;
				return out;
			}
		}
		const auto start = pos;
		if (input[pos] == '-') {
			++pos;
		}
		if (pos == input.size()) {
			bad();
		}
		auto digit = [&] {
			return pos < input.size() && input[pos] >= '0' && input[pos] <= '9';
		};
		if (input[pos] == '0') {
			++pos;
		} else {
			if (!digit()) {
				bad();
			}
			while (digit()) {
				++pos;
			}
		}
		if (pos < input.size() && input[pos] == '.') {
			++pos;
			if (!digit()) {
				bad();
			}
			while (digit()) {
				++pos;
			}
		}
		if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
			++pos;
			if (pos < input.size() &&
			    (input[pos] == '+' || input[pos] == '-')) {
				++pos;
			}
			if (!digit()) {
				bad();
			}
			while (digit()) {
				++pos;
			}
		}
		out.kind = Json::number;
		out.text = input.substr(start, pos - start);
		return out;
	}

  public:
	explicit Parser(std::string_view text) : input(text)
	{
		if (text.size() > 2 * 1024 * 1024) {
			bad();
		}
	}

	Json parse()
	{
		auto out = value(0);
		space();
		if (pos != input.size()) {
			bad();
		}
		return out;
	}
};
}

std::optional<Release> parse_release(std::string_view json,
                                     std::string_view current)
{
	auto root = Parser(json).parse();
	if (root.at("draft").flag() || root.at("prerelease").flag()) {
		return {};
	}
	auto tag = root.at("tag_name").str();
	auto latest = version(tag), running = version(current);
	if (!latest || !running) {
		throw std::runtime_error(
			"Unsupported release version (expected vX.Y.Z)");
	}
	if (*latest <= *running) {
		return {};
	}
	Release release;
	release.tag = tag;
	const auto &assets = root.at("assets");
	if (assets.kind != Json::array) {
		throw std::runtime_error("Invalid release assets");
	}
	bool found = false;
	for (const auto &asset : assets.items) {
		if (asset.at("name").str() != asset_name) {
			continue;
		}
		if (found) {
			throw std::runtime_error("Ambiguous update asset");
		}
		found = true;
		if (asset.at("state").str() != "uploaded") {
			continue;
		}
		auto digest = asset.at("digest");
		if (digest.kind == Json::null) {
			continue;
		}
		auto hash = digest.str();
		if (!hash.starts_with("sha256:") || hash.size() != 71) {
			continue;
		}
		hash.erase(0, 7);
		bool valid = true;
		for (char &c : hash) {
			if (c >= 'A' && c <= 'F') {
				c = char(c - 'A' + 'a');
			}
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				valid = false;
			}
		}
		if (!valid) {
			continue;
		}
		const auto &size = asset.at("size");
		std::uint64_t bytes = 0;
		auto parsed = std::from_chars(
			size.text.data(), size.text.data() + size.text.size(), bytes);
		if (size.kind != Json::number || parsed.ec != std::errc{} ||
		    parsed.ptr != size.text.data() + size.text.size() || bytes < 1024 ||
		    bytes > max_download) {
			continue;
		}
		auto url = asset.at("browser_download_url").str();
		const auto expected = "https://github.com/" + std::string(repository) +
		                      "/releases/download/" + tag + "/" +
		                      std::string(asset_name);
		if (url != expected) {
			continue;
		}
		release.url = url;
		release.sha256 = hash;
		release.size = bytes;
	}
	return release;
}

bool automatic_due(std::int64_t now, std::int64_t last) noexcept
{
	return now > 0 && (last <= 0 || now < last || now - last >= 24 * 60 * 60);
}

std::wstring quote_argument(std::wstring_view argument)
{
	std::wstring result = L"\"";
	unsigned slashes = 0;
	for (wchar_t c : argument) {
		if (c == L'\\') {
			++slashes;
			continue;
		}
		result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		slashes = 0;
		result += c;
	}
	result.append(slashes * 2, L'\\');
	result += L'"';
	return result;
}
}
