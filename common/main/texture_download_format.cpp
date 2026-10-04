/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Texture pack download: manifest, zip directory, names and versions
 * (texture_download.h).  No network, no files: unit tested.
 */

#include <algorithm>
#include <charconv>
#include <set>
#include "texture_download.h"

namespace dcx::texture_download {

namespace {

/* A small JSON reader for the manifest: objects, arrays, strings
 * (ASCII; \u escapes below 0x80), non-negative integers, true, false,
 * null.  Fractions and exponents are refused; the manifest has none.
 */
struct json_value
{
	enum class type : std::uint8_t
	{
		null,
		boolean,
		integer,
		string,
		array,
		object,
	};
	type t{type::null};
	bool b{};
	std::uint64_t n{};
	std::string s;
	/* Array elements, or the values of an object's members (names in
	 * keys).
	 */
	std::vector<json_value> items;
	std::vector<std::string> keys;
	const json_value *member(const std::string_view key) const
	{
		for (std::size_t i = 0; i < keys.size(); ++i)
			if (keys[i] == key)
				return &items[i];
		return nullptr;
	}
};

class json_reader
{
	std::string_view text;
	std::size_t pos{};
	unsigned depth{};
	void skip_space()
	{
		while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
			++pos;
	}
	bool literal(const std::string_view word)
	{
		if (text.substr(pos, word.size()) != word)
			return false;
		pos += word.size();
		return true;
	}
	bool string(std::string &out)
	{
		if (pos >= text.size() || text[pos] != '"')
			return false;
		++pos;
		for (;;)
		{
			if (pos >= text.size())
				return false;
			const char c{text[pos++]};
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x80)
				return false;
			if (c != '\\')
			{
				out += c;
				continue;
			}
			if (pos >= text.size())
				return false;
			switch (const char e{text[pos++]})
			{
				case '"':
				case '\\':
				case '/':
					out += e;
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
				case 'u':
				{
					if (pos + 4 > text.size())
						return false;
					unsigned code{};
					const auto r{std::from_chars(text.data() + pos, text.data() + pos + 4, code, 16)};
					if (r.ec != std::errc{} || r.ptr != text.data() + pos + 4 || code < 0x20 || code >= 0x80)
						return false;
					pos += 4;
					out += static_cast<char>(code);
					break;
				}
				default:
					return false;
			}
		}
	}
public:
	explicit json_reader(const std::string_view t) :
		text{t}
	{
	}
	bool value(json_value &v)
	{
		skip_space();
		if (pos >= text.size())
			return false;
		const char c{text[pos]};
		if (c == '{' || c == '[')
		{
			if (++depth > 16)
				return false;
			++pos;
			const bool object{c == '{'};
			v.t = object ? json_value::type::object : json_value::type::array;
			skip_space();
			if (pos < text.size() && text[pos] == (object ? '}' : ']'))
			{
				++pos;
				--depth;
				return true;
			}
			for (;;)
			{
				skip_space();
				if (object)
				{
					std::string key;
					if (!string(key))
						return false;
					skip_space();
					if (pos >= text.size() || text[pos] != ':')
						return false;
					++pos;
					json_value member;
					if (!value(member))
						return false;
					if (v.member(key))
						return false;	/* the same key twice */
					v.keys.push_back(std::move(key));
					v.items.push_back(std::move(member));
				}
				else
				{
					json_value item;
					if (!value(item))
						return false;
					v.items.push_back(std::move(item));
				}
				skip_space();
				if (pos >= text.size())
					return false;
				if (text[pos] == ',')
				{
					++pos;
					continue;
				}
				if (text[pos] != (object ? '}' : ']'))
					return false;
				++pos;
				--depth;
				return true;
			}
		}
		if (c == '"')
		{
			v.t = json_value::type::string;
			return string(v.s);
		}
		if (c >= '0' && c <= '9')
		{
			const auto r{std::from_chars(text.data() + pos, text.data() + text.size(), v.n)};
			if (r.ec != std::errc{})
				return false;
			pos = r.ptr - text.data();
			if (pos < text.size() && (text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'))
				return false;
			v.t = json_value::type::integer;
			return true;
		}
		if (literal("true"))
		{
			v.t = json_value::type::boolean;
			v.b = true;
			return true;
		}
		if (literal("false"))
		{
			v.t = json_value::type::boolean;
			return true;
		}
		if (literal("null"))
			return true;
		return false;
	}
	bool at_end()
	{
		skip_space();
		return pos == text.size();
	}
};

bool key_char(const char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool integer_member(const json_value &o, const std::string_view key, const std::uint64_t lowest, const std::uint64_t highest, std::uint64_t &out)
{
	const auto v{o.member(key)};
	if (!v || v->t != json_value::type::integer || v->n < lowest || v->n > highest)
		return false;
	out = v->n;
	return true;
}

/* A download link below the fixed base, without anything that could
 * lead elsewhere.
 */
bool usable_url(const std::string_view url)
{
	if (url.size() > 300 || !url.starts_with(release_base_url) || url.size() == release_base_url.size())
		return false;
	const auto rest{url.substr(release_base_url.size())};
	if (rest.find("..") != rest.npos || rest.starts_with('/'))
		return false;
	for (const char c : rest)
		if (!(key_char(c) || (c >= 'A' && c <= 'Z') || c == '.' || c == '/'))
			return false;
	return true;
}

std::uint16_t le16(const std::uint8_t *const p)
{
	return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t *const p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

char lower_ascii(const char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}

const pack_info *manifest::find(const std::string_view mission) const
{
	for (auto &p : packs)
		if (p.mission == mission)
			return &p;
	return nullptr;
}

bool valid_mission_key(const std::string_view key)
{
	if (key.empty() || key.size() > 40 || key == "shared")
		return false;
	return std::all_of(key.begin(), key.end(), key_char);
}

bool valid_texture_file_name(const std::string_view name)
{
	if (!name.ends_with(".png"))
		return false;
	auto stem{name.substr(0, name.size() - 4)};
	if (const auto hash{stem.find('#')}; hash != stem.npos)
	{
		const auto frame{stem.substr(hash + 1)};
		if (frame.empty() || frame.size() > 3 || !std::all_of(frame.begin(), frame.end(), [](const char c) { return c >= '0' && c <= '9'; }))
			return false;
		stem = stem.substr(0, hash);
	}
	return !stem.empty() && stem.size() <= 40 && std::all_of(stem.begin(), stem.end(), key_char);
}

std::optional<manifest> parse_manifest(const std::string_view text, std::string &error)
{
	if (text.size() > max_manifest_bytes)
	{
		error = "manifest too large";
		return std::nullopt;
	}
	json_value root;
	json_reader reader{text};
	if (!reader.value(root) || !reader.at_end() || root.t != json_value::type::object)
	{
		error = "manifest is not valid JSON";
		return std::nullopt;
	}
	std::uint64_t format{};
	if (!integer_member(root, "format", 1, UINT32_MAX, format))
	{
		error = "manifest has no format";
		return std::nullopt;
	}
	if (format != 1)
	{
		error = "manifest format " + std::to_string(format) + " is newer than this game understands";
		return std::nullopt;
	}
	const auto packs{root.member("packs")};
	if (!packs || packs->t != json_value::type::object)
	{
		error = "manifest has no packs";
		return std::nullopt;
	}
	manifest m;
	for (std::size_t i = 0; i < packs->keys.size(); ++i)
	{
		const auto &key{packs->keys[i]};
		const auto &v{packs->items[i]};
		const auto bad = [&error, &key](const char *const what) {
			error = "manifest: pack \"" + key + "\": " + what;
			return std::nullopt;
		};
		if (!valid_mission_key(key))
			return bad("bad name");
		if (v.t != json_value::type::object)
			return bad("not an object");
		pack_info p;
		p.mission = key;
		std::uint64_t n{};
		if (!integer_member(v, "version", 1, 1000000, n))
			return bad("bad version");
		p.version = static_cast<unsigned>(n);
		if (!integer_member(v, "size", 1, max_pack_bytes, p.size))
			return bad("bad size");
		if (!integer_member(v, "files", 1, max_pack_files, n))
			return bad("bad file count");
		p.files = static_cast<unsigned>(n);
		const auto url{v.member("url")};
		if (!url || url->t != json_value::type::string || !usable_url(url->s))
			return bad("bad url");
		p.url = url->s;
		const auto sha{v.member("sha256")};
		if (!sha || sha->t != json_value::type::string || !sha256_from_hex(sha->s, p.sha256))
			return bad("bad sha256");
		m.packs.push_back(std::move(p));
	}
	return m;
}

std::string format_marker(const pack_info &p)
{
	return "d2xx-ai-textures " + p.mission + " " + std::to_string(p.version) + " " + sha256_hex(p.sha256) + "\n";
}

std::optional<unsigned> parse_marker(std::string_view text, const std::string_view mission)
{
	constexpr std::string_view head{"d2xx-ai-textures "};
	if (!text.starts_with(head))
		return std::nullopt;
	text.remove_prefix(head.size());
	if (!text.starts_with(mission) || text.size() <= mission.size() || text[mission.size()] != ' ')
		return std::nullopt;
	text.remove_prefix(mission.size() + 1);
	unsigned version{};
	const auto r{std::from_chars(text.data(), text.data() + text.size(), version)};
	if (r.ec != std::errc{} || !version || (r.ptr != text.data() + text.size() && *r.ptr != ' ' && *r.ptr != '\n'))
		return std::nullopt;
	return version;
}

pack_action decide(const pack_info &remote, const local_state state, const unsigned local_version)
{
	switch (state)
	{
		case local_state::missing:
			return pack_action::download;
		case local_state::manual:
			return pack_action::keep_manual;
		case local_state::downloaded:
		default:
			return local_version < remote.version ? pack_action::download : pack_action::none;
	}
}

std::uint32_t crc32_update(std::uint32_t crc, const std::span<const std::uint8_t> data)
{
	static const auto table{[] {
		std::array<std::uint32_t, 256> t{};
		for (std::uint32_t i = 0; i < 256; ++i)
		{
			std::uint32_t c{i};
			for (unsigned k = 0; k < 8; ++k)
				c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
			t[i] = c;
		}
		return t;
	}()};
	crc = ~crc;
	for (const auto b : data)
		crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
	return ~crc;
}

std::optional<std::vector<zip_entry>> read_zip_directory(const std::uint64_t file_size, const read_at_function &read_at, std::string &error)
{
	/* The end of central directory record: 22 bytes plus a comment of
	 * up to 65535 bytes at the end of the file.
	 */
	constexpr std::size_t eocd_size{22};
	if (file_size < eocd_size)
	{
		error = "zip: file too small";
		return std::nullopt;
	}
	const std::uint64_t tail_size{std::min<std::uint64_t>(file_size, eocd_size + 65535)};
	std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
	if (!read_at(file_size - tail_size, tail))
	{
		error = "zip: read error";
		return std::nullopt;
	}
	std::optional<std::size_t> eocd;
	for (std::size_t i = tail.size() - eocd_size + 1; i-- > 0;)
		if (le32(&tail[i]) == 0x06054b50 && i + eocd_size + le16(&tail[i + 20]) == tail.size())
		{
			eocd = i;
			break;
		}
	if (!eocd)
	{
		error = "zip: no end of central directory";
		return std::nullopt;
	}
	const auto *const e{&tail[*eocd]};
	const unsigned disk{le16(e + 4)}, cd_disk{le16(e + 6)}, entries_here{le16(e + 8)}, entries{le16(e + 10)};
	const std::uint32_t cd_size{le32(e + 12)}, cd_offset{le32(e + 16)};
	if (disk != 0 || cd_disk != 0 || entries_here != entries)
	{
		error = "zip: multi-part archives are not supported";
		return std::nullopt;
	}
	if (entries == 0xffff || cd_size == 0xffffffffu || cd_offset == 0xffffffffu)
	{
		error = "zip: zip64 is not supported";
		return std::nullopt;
	}
	const std::uint64_t eocd_offset{file_size - tail_size + *eocd};
	if (static_cast<std::uint64_t>(cd_offset) + cd_size > eocd_offset)
	{
		error = "zip: central directory out of range";
		return std::nullopt;
	}
	if (entries > max_pack_files + 64)
	{
		error = "zip: too many entries";
		return std::nullopt;
	}
	std::vector<std::uint8_t> cd(cd_size);
	if (cd_size && !read_at(cd_offset, cd))
	{
		error = "zip: read error";
		return std::nullopt;
	}
	std::vector<zip_entry> result;
	result.reserve(entries);
	std::size_t p{};
	for (unsigned i = 0; i < entries; ++i)
	{
		constexpr std::size_t header{46};
		if (p + header > cd.size() || le32(&cd[p]) != 0x02014b50)
		{
			error = "zip: bad central directory entry";
			return std::nullopt;
		}
		const auto *const h{&cd[p]};
		const std::size_t name_len{le16(h + 28)}, extra_len{le16(h + 30)}, comment_len{le16(h + 32)};
		if (p + header + name_len + extra_len + comment_len > cd.size())
		{
			error = "zip: bad central directory entry";
			return std::nullopt;
		}
		zip_entry z;
		z.flags = le16(h + 8);
		z.method = le16(h + 10);
		z.crc32 = le32(h + 16);
		z.compressed_size = le32(h + 20);
		z.size = le32(h + 24);
		z.local_header_offset = le32(h + 42);
		z.name.assign(reinterpret_cast<const char *>(h + header), name_len);
		if (z.compressed_size == 0xffffffffu || z.size == 0xffffffffu || z.local_header_offset == 0xffffffffu)
		{
			error = "zip: zip64 is not supported";
			return std::nullopt;
		}
		result.push_back(std::move(z));
		p += header + name_len + extra_len + comment_len;
	}
	return result;
}

std::optional<std::uint64_t> zip_data_offset(const zip_entry &z, const std::uint64_t file_size, const read_at_function &read_at, std::string &error)
{
	constexpr std::size_t header{30};
	std::array<std::uint8_t, header> h;
	if (z.local_header_offset + header > file_size || !read_at(z.local_header_offset, h) || le32(h.data()) != 0x04034b50)
	{
		error = "zip: bad local header of " + z.name;
		return std::nullopt;
	}
	const std::size_t name_len{le16(&h[26])}, extra_len{le16(&h[28])};
	std::string name(name_len, '\0');
	if (!read_at(z.local_header_offset + header, std::span{reinterpret_cast<std::uint8_t *>(name.data()), name.size()}) || name != z.name)
	{
		error = "zip: local name differs from directory: " + z.name;
		return std::nullopt;
	}
	const std::uint64_t data{z.local_header_offset + header + name_len + extra_len};
	if (data + z.compressed_size > file_size)
	{
		error = "zip: data out of range: " + z.name;
		return std::nullopt;
	}
	return data;
}

std::optional<std::vector<extract_item>> plan_extraction(const std::span<const zip_entry> entries, const std::string_view mission, const unsigned expected_files, std::string &error)
{
	if (!valid_mission_key(mission))
	{
		error = "bad mission name";
		return std::nullopt;
	}
	const std::string prefix{"textures/" + std::string{mission} + "/"};
	std::vector<extract_item> result;
	std::set<std::string> seen;
	std::uint64_t total{};
	for (std::size_t i = 0; i < entries.size(); ++i)
	{
		const auto &z{entries[i]};
		const std::string_view name{z.name};
		if (name == "textures/" || name == prefix)
			continue;	/* directory entries */
		if (!name.starts_with(prefix))
		{
			error = "unexpected file in pack: " + std::string{name.substr(0, 80)};
			return std::nullopt;
		}
		const auto file{name.substr(prefix.size())};
		if (!valid_texture_file_name(file))
		{
			error = "bad file name in pack: " + std::string{name.substr(0, 80)};
			return std::nullopt;
		}
		if (z.method != 0 || z.compressed_size != z.size || (z.flags & 1))
		{
			error = "compressed or encrypted file in pack: " + std::string{file};
			return std::nullopt;
		}
		if (z.size > max_pack_bytes || z.size > max_png_bytes || (total += z.size) > max_pack_bytes)
		{
			error = "file too large in pack: " + std::string{file};
			return std::nullopt;
		}
		std::string folded{file};
		std::transform(folded.begin(), folded.end(), folded.begin(), lower_ascii);
		if (!seen.insert(std::move(folded)).second)
		{
			error = "file twice in pack: " + std::string{file};
			return std::nullopt;
		}
		result.push_back({i, std::string{file}});
	}
	if (result.size() != expected_files)
	{
		error = "pack has " + std::to_string(result.size()) + " pictures, manifest says " + std::to_string(expected_files);
		return std::nullopt;
	}
	return result;
}

}
