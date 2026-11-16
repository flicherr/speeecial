#include "update_core.hpp"
#include "version.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace snow::updates;

namespace
{
unsigned checks = 0;

void require(bool value, const char *description)
{
	++checks;
	if (!value) {
		throw std::runtime_error(description);
	}
}

template <class F> void rejects(F action, const char *description)
{
	bool rejected = false;
	try {
		action();
	} catch (const std::exception &) {
		rejected = true;
	}
	require(rejected, description);
}

std::string fixture()
{
	return R"({"tag_name":"v1.0.12","draft":false,"prerelease":false,"body":"Snow \u2744 \ud83c\udf28\ufe0f","assets":[{"name":"speeecial-windows-x64.exe","state":"uploaded","digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":4096,"browser_download_url":"https://github.com/flicherr/speeecial-snow-desktop/releases/download/v1.0.12/speeecial-windows-x64.exe"}]})";
}

std::string replace(std::string text, std::string_view before,
                    std::string_view after)
{
	auto at = text.find(before);
	if (at == text.npos) {
		throw std::runtime_error("Bad test fixture");
	}
	text.replace(at, before.size(), after);
	return text;
}

std::string file(const char *name)
{
	std::ifstream stream(std::string(SNOW_SOURCE_DIR) + "/" + name);
	if (!stream) {
		throw std::runtime_error("Missing version file");
	}
	return std::string(std::istreambuf_iterator<char>(stream), {});
}
}

int main()
{
	try {
		require(version("v1.0.11") == Version{1, 0, 11}, "v prefix");
		require(version("1.0.11") > version("1.0.9"), "numeric comparison");
		require(version("1.10.0") > version("1.9.999"), "minor comparison");
		for (auto text :
		     {"", "v", "1.2", "1.2.3.4", "1.2.3-beta", "1.2.3+build", "01.2.3",
		      "1.-2.3", "+1.2.3", "1.2.99999999999999", " 1.2.3", "1.2.3 "}) {
			require(!version(text), "invalid version");
		}
		auto release = parse_release(fixture(), SPEEECIAL_VERSION);
		require(release && release->installable() && release->size == 4096 &&
		            release->sha256.size() == 64,
		        "valid release");
		require(!parse_release(fixture(), "1.0.12"), "equal version");
		require(!parse_release(fixture(), "2.0.0"), "no downgrade");
		require(!parse_release(replace(fixture(), "\"prerelease\":false",
		                               "\"prerelease\":true"),
		                       SPEEECIAL_VERSION),
		        "no prerelease");
		require(!parse_release(
					replace(fixture(), "\"draft\":false", "\"draft\":true"),
					SPEEECIAL_VERSION),
		        "no draft");
		auto manual = [&](std::string json) {
			auto result = parse_release(json, SPEEECIAL_VERSION);
			require(result && !result->installable(), "manual-only release");
		};
		manual(replace(fixture(), "\"name\":\"speeecial-windows-x64.exe\"",
		               "\"name\":\"speeecial-1.0.12-windows-x64.zip\""));
		manual(replace(fixture(), "sha256:", "sha512:"));
		manual(replace(fixture(),
		               "\"sha256:"
		               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		               "aaaaaaaaa\"",
		               "null"));
		for (auto size :
		     {"134217729", "-1", "4096.0", "99999999999999999999999"}) {
			manual(replace(fixture(), "\"size\":4096",
			               std::string("\"size\":") + size));
		}
		for (auto host :
		     {"http://github.com/", "https://github.com.attacker.example/"}) {
			manual(replace(fixture(), "https://github.com/", host));
		}
		manual(replace(fixture(), "/download/v1.0.12/", "/download/v2.0.0/"));
		manual(replace(fixture(), "/flicherr/", "/other/"));
		manual(replace(fixture(), "\"uploaded\"", "\"new\""));
		for (auto text : {"", "null", "[]", "{}", "{broken}",
		                  "{\"tag_name\":true}", "{\"x\":01}", "{\"x\":1.}",
		                  "{\"x\":1e}", "{\"x\":\"\\ud800\"}"}) {
			rejects([&] { parse_release(text, SPEEECIAL_VERSION); },
			        "malformed JSON");
		}
		rejects(
			[&] { parse_release(fixture() + "garbage", SPEEECIAL_VERSION); },
			"trailing input");
		rejects(
			[&] {
				parse_release(replace(fixture(), "\"draft\":false",
			                          "\"draft\":false,\"draft\":true"),
			                  SPEEECIAL_VERSION);
			},
			"duplicate key");
		rejects(
			[&] {
				parse_release(std::string(2 * 1024 * 1024 + 1, ' '),
			                  SPEEECIAL_VERSION);
			},
			"JSON size bound");
		rejects(
			[&] {
				parse_release(std::string(34, '[') + "0" + std::string(34, ']'),
			                  SPEEECIAL_VERSION);
			},
			"JSON depth bound");
		auto duplicated = fixture();
		const auto begin = duplicated.find("[{"), end = duplicated.rfind("]");
		const auto asset = duplicated.substr(begin + 1, end - begin - 1);
		duplicated.insert(end, "," + asset);
		rejects([&] { parse_release(duplicated, SPEEECIAL_VERSION); },
		        "duplicate asset");
		require(automatic_due(100, 0), "initial automatic check");
		require(!automatic_due(1000, 999), "daily throttle");
		require(!automatic_due(86400, 1), "daily boundary below");
		require(automatic_due(86401, 1), "daily boundary");
		require(automatic_due(100, 200), "clock rollback");
		require(!automatic_due(0, 0), "invalid clock");
		require(quote_argument(L"") == L"\"\"", "empty argument");
		require(quote_argument(L"C:\\snow folder\\") ==
		            L"\"C:\\snow folder\\\\\"",
		        "trailing slash");
		require(quote_argument(L"a\"b") == L"\"a\\\"b\"", "embedded quote");
		require(file("CMakeLists.txt").find("VERSION " SPEEECIAL_VERSION) !=
		            std::string::npos,
		        "CMake version consistent");
		require(file("resources/snowdesk.rc")
		                .find("\"FileVersion\", \"" SPEEECIAL_VERSION) !=
		            std::string::npos,
		        "resource version consistent");
		require(file("resources/snowdesk.manifest")
		                .find("version=\"" SPEEECIAL_VERSION ".0\"") !=
		            std::string::npos,
		        "manifest version consistent");
		std::cout << checks << " updater checks passed\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
