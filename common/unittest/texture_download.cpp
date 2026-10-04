/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Texture pack download (texture_download.h): manifest parsing, SHA-256
 * check, zip directory and extraction plan (malicious names), versions.
 */

#include <cstring>
#include "texture_download.h"

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Rebirth texture download
#include <boost/test/unit_test.hpp>

using namespace dcx;
using namespace dcx::texture_download;

namespace {

const std::string good_sha{"7d3431f517115da20b29d23579f3620926881670def74df37c600d6e93cf04dd"};

std::string manifest_with(const std::string &mission, const std::string &url, const std::string &sha = good_sha, const std::string &extra = {})
{
	return R"({ "format": 1, "packs": { ")" + mission + R"(": { "version": 1, "url": ")" + url + R"(", "size": 44894890, "sha256": ")" + sha + R"(", "files": 163)" + extra + " } } }";
}

const std::string corona_url{"https://github.com/nburri/d2xx-ai-textures/releases/download/textures-corona-v1/corona-v1.zip"};

std::optional<manifest> parse(const std::string &text)
{
	std::string error;
	auto m{parse_manifest(text, error)};
	BOOST_TEST_MESSAGE(error);
	return m;
}

/* A zip archive in memory, stored entries, as tools/build_pack.py makes it. */
struct test_zip
{
	struct file
	{
		std::string name;
		std::string data;
		std::uint16_t method;
		std::string local_name;	/* if not empty: differs from the directory */
		file(std::string n, std::string d, const std::uint16_t m = 0, std::string l = {}) :
			name{std::move(n)}, data{std::move(d)}, method{m}, local_name{std::move(l)}
		{
		}
	};
	std::vector<file> files;
	std::vector<std::uint8_t> bytes;
	static void put16(std::vector<std::uint8_t> &v, const unsigned x)
	{
		v.push_back(x & 0xff);
		v.push_back((x >> 8) & 0xff);
	}
	static void put32(std::vector<std::uint8_t> &v, const std::uint32_t x)
	{
		put16(v, x & 0xffff);
		put16(v, x >> 16);
	}
	static void put(std::vector<std::uint8_t> &v, const std::string &s)
	{
		v.insert(v.end(), s.begin(), s.end());
	}
	const std::vector<std::uint8_t> &build()
	{
		bytes.clear();
		std::vector<std::uint8_t> cd;
		for (auto &f : files)
		{
			const std::uint32_t offset(bytes.size());
			const auto crc{crc32_update(0, std::span{reinterpret_cast<const std::uint8_t *>(f.data.data()), f.data.size()})};
			const auto &lname{f.local_name.empty() ? f.name : f.local_name};
			put32(bytes, 0x04034b50);
			put16(bytes, 10);
			put16(bytes, 0);
			put16(bytes, f.method);
			put32(bytes, 0);
			put32(bytes, crc);
			put32(bytes, f.data.size());
			put32(bytes, f.data.size());
			put16(bytes, lname.size());
			put16(bytes, 0);
			put(bytes, lname);
			put(bytes, f.data);
			put32(cd, 0x02014b50);
			put16(cd, 20);
			put16(cd, 10);
			put16(cd, 0);
			put16(cd, f.method);
			put32(cd, 0);
			put32(cd, crc);
			put32(cd, f.data.size());
			put32(cd, f.data.size());
			put16(cd, f.name.size());
			put16(cd, 0);
			put16(cd, 0);
			put16(cd, 0);
			put16(cd, 0);
			put32(cd, 0);
			put32(cd, offset);
			put(cd, f.name);
		}
		const std::uint32_t cd_offset(bytes.size());
		bytes.insert(bytes.end(), cd.begin(), cd.end());
		put32(bytes, 0x06054b50);
		put16(bytes, 0);
		put16(bytes, 0);
		put16(bytes, files.size());
		put16(bytes, files.size());
		put32(bytes, cd.size());
		put32(bytes, cd_offset);
		put16(bytes, 0);
		return bytes;
	}
	read_at_function reader() const
	{
		return [this](const std::uint64_t offset, const std::span<std::uint8_t> out) {
			if (offset > bytes.size() || out.size() > bytes.size() - offset)
				return false;
			std::memcpy(out.data(), bytes.data() + offset, out.size());
			return true;
		};
	}
	std::optional<std::vector<zip_entry>> directory(std::string &error) const
	{
		return read_zip_directory(bytes.size(), reader(), error);
	}
};

const std::string png{"\x89PNG\r\n\x1a\nrest of a picture", 26};

/* The plan for a zip with these names (each a small PNG). */
std::optional<std::vector<extract_item>> plan_for(const std::vector<std::string> &names, const unsigned expected, std::string &error)
{
	test_zip z;
	for (auto &n : names)
		z.files.push_back({n, png});
	z.build();
	const auto dir{z.directory(error)};
	if (!dir)
		return std::nullopt;
	return plan_extraction(*dir, "corona", expected, error);
}

bool refused(const std::vector<std::string> &names, const unsigned expected)
{
	std::string error;
	const auto plan{plan_for(names, expected, error)};
	BOOST_TEST_MESSAGE(error);
	return !plan && !error.empty();
}

}

BOOST_AUTO_TEST_CASE(manifest_of_the_release_parses)
{
	const auto m{parse(manifest_with("corona", corona_url))};
	BOOST_TEST_REQUIRE(m.has_value());
	BOOST_TEST(m->packs.size() == 1u);
	const auto p{m->find("corona")};
	BOOST_TEST_REQUIRE(p != nullptr);
	BOOST_TEST(p->version == 1u);
	BOOST_TEST(p->size == 44894890u);
	BOOST_TEST(p->files == 163u);
	BOOST_TEST(p->url == corona_url);
	BOOST_TEST(sha256_hex(p->sha256) == good_sha);
	BOOST_TEST(m->find("other") == nullptr);
	/* Unknown members are fine (later additions). */
	BOOST_TEST(parse(manifest_with("corona", corona_url, good_sha, R"(, "title": "Corona", "tags": [1, true, null])")).has_value());
	BOOST_TEST(parse(R"({"format":1,"packs":{}})").has_value());
}

BOOST_AUTO_TEST_CASE(manifest_refuses_bad_input)
{
	BOOST_TEST(!parse("").has_value());
	BOOST_TEST(!parse("not json").has_value());
	BOOST_TEST(!parse(R"({"format":2,"packs":{}})").has_value());
	BOOST_TEST(!parse(R"({"packs":{}})").has_value());
	BOOST_TEST(!parse(R"({"format":1})").has_value());
	BOOST_TEST(!parse(R"({"format":1,"packs":{}} trailing)").has_value());
	BOOST_TEST(!parse(R"({"format":1,"format":1,"packs":{}})").has_value());
	BOOST_TEST(!parse(R"({"format":1.5,"packs":{}})").has_value());
	/* Mission names that are no plain lower case directory name. */
	for (const char *const bad : {"Corona", "../corona", "a/b", "shared", "", "a b", "corona.mn2"})
		BOOST_TEST(!parse(manifest_with(bad, corona_url)).has_value(), bad);
	/* Links outside the fixed HTTPS base. */
	for (const char *const bad : {
		"http://github.com/nburri/d2xx-ai-textures/releases/download/textures-corona-v1/corona-v1.zip",
		"https://example.com/corona-v1.zip",
		"https://github.com/nburri/d2xx-ai-textures/releases/download/../../other/x.zip",
		"https://github.com/nburri/d2xx-ai-textures/releases/download/textures-corona-v1/corona-v1.zip?x=1",
		"https://github.com/nburri/d2xx-ai-textures/releases/download/",
		"https://github.com/nburri/d2xx-ai-textures/releases/download//evil.com/x.zip",
		"https://github.com/nburri/d2xx-ai-textures/releases/downloadx/a.zip",
	})
		BOOST_TEST(!parse(manifest_with("corona", bad)).has_value(), bad);
	BOOST_TEST(!parse(manifest_with("corona", corona_url, "abc")).has_value());
	BOOST_TEST(!parse(manifest_with("corona", corona_url, std::string(64, 'g'))).has_value());
	BOOST_TEST(!parse(R"({ "format": 1, "packs": { "corona": { "version": 0, "url": ")" + corona_url + R"(", "size": 1, "sha256": ")" + good_sha + R"(", "files": 1 } } })").has_value());
	BOOST_TEST(!parse(R"({ "format": 1, "packs": { "corona": { "version": 1, "url": ")" + corona_url + R"(", "size": 999999999999, "sha256": ")" + good_sha + R"(", "files": 1 } } })").has_value());
	BOOST_TEST(!parse(R"({ "format": 1, "packs": { "corona": { "version": 1, "url": ")" + corona_url + R"(", "size": 1, "sha256": ")" + good_sha + R"(", "files": 99999 } } })").has_value());
	BOOST_TEST(!parse(R"({ "format": 1, "packs": { "corona": { "version": 1, "url": ")" + corona_url + R"(", "sha256": ")" + good_sha + R"(", "files": 1 } } })").has_value());
	/* Deep nesting does not recurse without bound. */
	BOOST_TEST(!parse(std::string(10000, '[') + std::string(10000, ']')).has_value());
	BOOST_TEST(!parse(std::string(max_manifest_bytes + 1, ' ')).has_value());
}

BOOST_AUTO_TEST_CASE(sha256_check_of_a_download)
{
	/* What install() does: hash the bytes as they arrive, compare
	 * with the manifest.
	 */
	const std::string data{"abc"};
	sha256 h;
	h.update(std::span{reinterpret_cast<const std::uint8_t *>(data.data()), 1});
	h.update(std::span{reinterpret_cast<const std::uint8_t *>(data.data()) + 1, 2});
	sha256_digest expected{};
	BOOST_TEST_REQUIRE(sha256_from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected));
	BOOST_TEST((h.finish() == expected));
	std::string changed{data};
	changed[1] ^= 1;
	BOOST_TEST((sha256_of(std::span{reinterpret_cast<const std::uint8_t *>(changed.data()), changed.size()}) != expected));
}

BOOST_AUTO_TEST_CASE(crc32_known_value)
{
	const std::string s{"123456789"};
	BOOST_TEST(crc32_update(0, std::span{reinterpret_cast<const std::uint8_t *>(s.data()), s.size()}) == 0xcbf43926u);
	/* In pieces. */
	auto c{crc32_update(0, std::span{reinterpret_cast<const std::uint8_t *>(s.data()), 4})};
	c = crc32_update(c, std::span{reinterpret_cast<const std::uint8_t *>(s.data()) + 4, 5});
	BOOST_TEST(c == 0xcbf43926u);
}

BOOST_AUTO_TEST_CASE(zip_directory_and_plan_of_a_good_pack)
{
	test_zip z;
	z.files.push_back({"textures/", ""});
	z.files.push_back({"textures/corona/", ""});
	z.files.push_back({"textures/corona/rock072.png", png});
	z.files.push_back({"textures/corona/door32#14.png", png + "x"});
	z.build();
	std::string error;
	const auto dir{z.directory(error)};
	BOOST_TEST_REQUIRE(dir.has_value(), error);
	BOOST_TEST(dir->size() == 4u);
	const auto plan{plan_extraction(*dir, "corona", 2, error)};
	BOOST_TEST_REQUIRE(plan.has_value(), error);
	BOOST_TEST((*plan)[0].file_name == "rock072.png");
	BOOST_TEST((*plan)[1].file_name == "door32#14.png");
	const auto &e{(*dir)[(*plan)[1].entry]};
	const auto data{zip_data_offset(e, z.bytes.size(), z.reader(), error)};
	BOOST_TEST_REQUIRE(data.has_value(), error);
	BOOST_TEST(std::string(reinterpret_cast<const char *>(&z.bytes[*data]), e.size) == png + "x");
	BOOST_TEST(crc32_update(0, std::span{&z.bytes[*data], e.size}) == e.crc32);
}

BOOST_AUTO_TEST_CASE(zip_extraction_refuses_malicious_names)
{
	BOOST_TEST(!refused({"textures/corona/a.png"}, 1));
	/* Path traversal, absolute paths, other directories, other types. */
	for (const char *const bad : {
		"../a.png",
		"textures/corona/../../a.png",
		"textures/corona/../other/a.png",
		"textures/corona/..png",
		"/etc/a.png",
		"textures/corona//a.png",
		"textures/corona/sub/a.png",
		"textures/corona\\..\\..\\a.png",
		"textures/corona/a.png\\..\\b.png",
		"C:/a.png",
		"textures/other/a.png",
		"textures/a.png",
		"textures/corona/a.exe",
		"textures/corona/a.png.exe",
		"textures/corona/A.png",
		"textures/corona/a#.png",
		"textures/corona/a#1000.png",
		"textures/corona/.png",
		"textures/corona/a b.png",
		"textures/corona/a:b.png",
		"pack-version.txt",
		"textures/corona/pack-version.txt",
	})
		BOOST_TEST(refused({"textures/corona/a.png", bad}, 2), bad);
}

BOOST_AUTO_TEST_CASE(zip_extraction_refuses_bad_archives)
{
	/* Fewer or more pictures than the manifest says. */
	BOOST_TEST(refused({"textures/corona/a.png", "textures/corona/b.png"}, 3));
	BOOST_TEST(refused({"textures/corona/a.png", "textures/corona/b.png"}, 1));
	/* The same name twice, also in another case (Windows). */
	BOOST_TEST(refused({"textures/corona/a.png", "textures/corona/a.png"}, 2));
	std::string error;
	/* Compressed entries are not supported. */
	{
		test_zip z;
		z.files.push_back({"textures/corona/a.png", png, 8});
		z.build();
		const auto dir{z.directory(error)};
		BOOST_TEST_REQUIRE(dir.has_value());
		BOOST_TEST(!plan_extraction(*dir, "corona", 1, error).has_value());
	}
	/* Too large. */
	{
		std::vector<zip_entry> dir{{"textures/corona/a.png", 0, 0, 0, max_png_bytes + 1, max_png_bytes + 1, 0}};
		BOOST_TEST(!plan_extraction(dir, "corona", 1, error).has_value());
	}
	/* Encrypted. */
	{
		std::vector<zip_entry> dir{{"textures/corona/a.png", 0, 1, 0, 10, 10, 0}};
		BOOST_TEST(!plan_extraction(dir, "corona", 1, error).has_value());
	}
	/* The local header names another file than the directory. */
	{
		test_zip z;
		z.files.push_back({"textures/corona/a.png", png, 0, "../../evil.png"});
		z.build();
		const auto dir{z.directory(error)};
		BOOST_TEST_REQUIRE(dir.has_value());
		BOOST_TEST(!zip_data_offset((*dir)[0], z.bytes.size(), z.reader(), error).has_value());
	}
	/* Not a zip, truncated, bad mission name. */
	{
		test_zip z;
		z.bytes.assign(100, 0x41);
		BOOST_TEST(!z.directory(error).has_value());
		z.files.push_back({"textures/corona/a.png", png});
		z.build();
		z.bytes.resize(z.bytes.size() - 5);
		BOOST_TEST(!z.directory(error).has_value());
		z.build();
		const auto dir{z.directory(error)};
		BOOST_TEST_REQUIRE(dir.has_value());
		BOOST_TEST(!plan_extraction(*dir, "../x", 1, error).has_value());
		/* A directory entry pointing past the end. */
		auto moved{*dir};
		moved[0].local_header_offset = z.bytes.size();
		BOOST_TEST(!zip_data_offset(moved[0], z.bytes.size(), z.reader(), error).has_value());
	}
}

BOOST_AUTO_TEST_CASE(texture_names)
{
	BOOST_TEST(valid_texture_file_name("rock072.png"));
	BOOST_TEST(valid_texture_file_name("door32#0.png"));
	BOOST_TEST(valid_texture_file_name("bluegoal#999.png"));
	BOOST_TEST(!valid_texture_file_name("rock072.PNG"));
	BOOST_TEST(!valid_texture_file_name("rock072"));
	BOOST_TEST(!valid_texture_file_name("a#b.png"));
	BOOST_TEST(valid_mission_key("corona"));
	BOOST_TEST(valid_mission_key("my-mission_2"));
	BOOST_TEST(!valid_mission_key("Corona"));
	BOOST_TEST(!valid_mission_key(".."));
	BOOST_TEST(!valid_mission_key(std::string(41, 'a')));
}

BOOST_AUTO_TEST_CASE(versions_and_markers)
{
	pack_info p;
	p.mission = "corona";
	p.version = 2;
	BOOST_TEST_REQUIRE(sha256_from_hex(good_sha, p.sha256));
	const auto marker{format_marker(p)};
	BOOST_TEST(parse_marker(marker, "corona").value_or(0) == 2u);
	BOOST_TEST(!parse_marker(marker, "coron").has_value());
	BOOST_TEST(!parse_marker(marker, "other").has_value());
	BOOST_TEST(!parse_marker("", "corona").has_value());
	BOOST_TEST(!parse_marker("d2xx-ai-textures corona x", "corona").has_value());
	BOOST_TEST(!parse_marker("d2xx-ai-textures corona 0 abc", "corona").has_value());
	BOOST_TEST(parse_marker("d2xx-ai-textures corona 10", "corona").value_or(0) == 10u);
	BOOST_TEST((decide(p, local_state::missing, 0) == pack_action::download));
	BOOST_TEST((decide(p, local_state::downloaded, 1) == pack_action::download));
	BOOST_TEST((decide(p, local_state::downloaded, 2) == pack_action::none));
	/* A newer local version (an older manifest): keep it. */
	BOOST_TEST((decide(p, local_state::downloaded, 3) == pack_action::none));
	/* Installed by hand: never replaced. */
	BOOST_TEST((decide(p, local_state::manual, 0) == pack_action::keep_manual));
}
