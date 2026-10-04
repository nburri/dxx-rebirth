/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Texture pack download (Documentation/texture-packs.md): the game
 * fetches the AI texture packs (texture_pack.h) of the repository
 * nburri/d2xx-ai-textures from its GitHub releases.
 *
 * A rolling release "manifest" holds manifest.json:
 *
 *	{ "format": 1, "packs": { "corona": { "version": 1,
 *	  "url": "https://github.com/nburri/d2xx-ai-textures/releases/download/textures-corona-v1/corona-v1.zip",
 *	  "size": 44894890, "sha256": "<64 hex digits>", "files": 163 } } }
 *
 * The zip of a pack holds textures/<mission>/<name>.png, stored
 * without compression.  It is unpacked into the user directory's
 * textures/<mission>/, with a marker file (marker_file_name) that
 * records the installed version.
 *
 * This header has two parts: the checks that need neither the network
 * nor the game (manifest, zip directory, names, versions; unit tested
 * in common/unittest/texture_download.cpp, code in
 * texture_download_format.cpp), and the background downloader the game
 * uses (texture_download.cpp, libcurl; DXX_USE_CURL).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "sha256.h"

namespace dcx::texture_download {

/* Every download comes from this base (HTTPS only). */
inline constexpr std::string_view release_base_url{"https://github.com/nburri/d2xx-ai-textures/releases/download/"};
inline constexpr std::string_view manifest_url{"https://github.com/nburri/d2xx-ai-textures/releases/download/manifest/manifest.json"};
/* In textures/<mission>/ of a downloaded pack. */
inline constexpr std::string_view marker_file_name{"pack-version.txt"};

/* Limits (tools/build_pack.py of the texture repository checks the same). */
inline constexpr std::size_t max_manifest_bytes{1u << 20};
inline constexpr std::uint64_t max_png_bytes{8u << 20};
inline constexpr unsigned max_pack_files{4096};
inline constexpr std::uint64_t max_pack_bytes{512u << 20};

struct pack_info
{
	std::string mission;
	unsigned version{};
	std::string url;
	std::uint64_t size{};
	sha256_digest sha256{};
	unsigned files{};
};

struct manifest
{
	std::vector<pack_info> packs;
	[[nodiscard]]
	const pack_info *find(std::string_view mission) const;
};

/* nullopt and a reason if the text is not a usable manifest.  Packs
 * with a bad entry make the whole manifest unusable.
 */
[[nodiscard]]
std::optional<manifest> parse_manifest(std::string_view text, std::string &error);

/* A pack's directory name: [a-z0-9_-], 1-40 characters, not "shared". */
[[nodiscard]]
bool valid_mission_key(std::string_view);
/* A picture in a pack: <name>.png or <name>#<frame>.png, <name> as a
 * mission key (lower case), frame 0-999.
 */
[[nodiscard]]
bool valid_texture_file_name(std::string_view);

/* The marker of an installed pack, and its version (nullopt if the
 * text is not a marker of this mission).
 */
[[nodiscard]]
std::string format_marker(const pack_info &);
[[nodiscard]]
std::optional<unsigned> parse_marker(std::string_view text, std::string_view mission);

/* What is in textures/<mission>/ of the user directory. */
enum class local_state : std::uint8_t
{
	missing,	/* no directory */
	downloaded,	/* with a marker (version) */
	manual,	/* a directory without a marker: installed by hand, left alone */
};

enum class pack_action : std::uint8_t
{
	none,	/* up to date */
	download,
	keep_manual,
};

[[nodiscard]]
pack_action decide(const pack_info &remote, local_state, unsigned local_version);

/* Zip archives: the central directory, stored (method 0) entries only. */
struct zip_entry
{
	std::string name;
	std::uint16_t method{};
	std::uint16_t flags{};
	std::uint32_t crc32{};
	std::uint64_t compressed_size{}, size{};
	std::uint64_t local_header_offset{};
};

/* Reads size bytes at offset into the span; false at a read error or
 * beyond the end.
 */
using read_at_function = std::function<bool(std::uint64_t offset, std::span<std::uint8_t> out)>;

[[nodiscard]]
std::optional<std::vector<zip_entry>> read_zip_directory(std::uint64_t file_size, const read_at_function &, std::string &error);
/* Where the data of an entry starts (after its local header). */
[[nodiscard]]
std::optional<std::uint64_t> zip_data_offset(const zip_entry &, std::uint64_t file_size, const read_at_function &, std::string &error);

[[nodiscard]]
std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::uint8_t>);

struct extract_item
{
	std::size_t entry;	/* index into the zip directory */
	std::string file_name;	/* plain file name, valid_texture_file_name */
};

/* Which entries to unpack, or nullopt and a reason when the archive is
 * not a pack of this mission: every entry must be a directory entry of
 * textures/ or textures/<mission>/, or a stored picture
 * textures/<mission>/<valid file name> within the size limits, with no
 * name twice (also in another letter case), and expected_files
 * pictures in total.
 */
[[nodiscard]]
std::optional<std::vector<extract_item>> plan_extraction(std::span<const zip_entry>, std::string_view mission, unsigned expected_files, std::string &error);

/* The downloader (texture_download.cpp).  All functions are called
 * from the main thread; the network and file work runs on one
 * background thread.
 */

/* Whether this build can download (libcurl). */
[[nodiscard]]
bool available();
/* At startup, after the configuration is read: enabled = the texture
 * pack toggle is on (and -notexturepack is not given); offline =
 * -notexturedownload.  Fetches the manifest in the background.
 */
void start(bool enabled, bool offline);
/* The toggle changed (Visual Quality menu). */
void set_enabled(bool enabled);
/* A level of this mission (texture_pack::mission_directory) loads:
 * download its pack if the manifest has a newer one.  It is used from
 * the next level load on.
 */
void level_loaded(std::string_view mission);
/* Menu buttons. */
void download_all();
void delete_downloaded();
/* Wait until the background thread has nothing to do (at most
 * timeout_ms); for -visshot, which takes pictures right away.
 */
void wait_idle(unsigned timeout_ms);
/* Increases whenever a pack was installed or deleted: the renderer
 * then loads its textures again at the next level load.
 */
[[nodiscard]]
unsigned installed_generation();
/* A short status for the menu ("" when there is nothing to say), and
 * the progress line for the HUD ("" when no download runs).
 */
[[nodiscard]]
std::string status_text();
[[nodiscard]]
std::string hud_text();
/* Main thread, often (event loop): writes the messages of the
 * background thread to the console and gamelog.
 */
void poll();
void shutdown();

}
