/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Custom player ships in the game (Documentation/custom-ships.md): the
 * ships found in `ships/` and `ships/cache/`, which ship each player
 * flies, and the hooks that draw it instead of the Pyro-GX.
 *
 * Purely cosmetic: a player object keeps the Pyro's model number, size,
 * guns and physics; only what is drawn changes.  Anything missing or
 * invalid is drawn as the Pyro.
 */

#pragma once

#include "dxxsconf.h"
#include "dsx-ns.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sha256.h"
#include "fwd-object.h"
#include "objnum.h"
#include "fwd-gr.h"
#include "fwd-player.h"
#include "3d.h"
#include "vecmat.h"

namespace dcx {

namespace custom_ship {

/* A valid ship file found in the search path. */
struct entry
{
	/* PhysFS path, e.g. "ships/striker.dxship". */
	std::string path;
	std::string name, title, author, licence, source;
	sha256_digest hash{};
	std::uint32_t size{};
	/* Received from a host (ships/cache/). */
	bool cached{};
};

constexpr const char *SHIPS_DIR{"ships"};
constexpr const char *CACHE_DIR{"ships/cache"};

/* The valid ships, sorted by title; scanned on first use. */
const std::vector<entry> &list();
/* Scan `ships/` and `ships/cache/` again. */
void rescan();
[[nodiscard]]
const entry *find_name(std::string_view name);
[[nodiscard]]
const entry *find_hash(const sha256_digest &hash);
/* The whole file of an entry (for a transfer), if it still reads and
 * still has its hash.
 */
[[nodiscard]]
std::optional<std::vector<std::uint8_t>> read_file(const entry &e);
/* A ship received over the network: checked (size, SHA-256 against
 * `expected`, the reader's rules) and stored as ships/cache/<hash>.dxship.
 * Returns the new entry or nullptr and the reason.
 */
const entry *store_received(std::span<const std::uint8_t> bytes, const sha256_digest &expected, std::string &error);

}

}

#ifdef DXX_BUILD_DESCENT
namespace dsx {

/* Which ship a player flies (nullptr hash: the Pyro).  The ship is drawn
 * as soon as it is available locally.
 */
void custom_ship_set_player(playernum_t pnum, const sha256_digest *hash);
[[nodiscard]]
std::optional<sha256_digest> custom_ship_of_player(playernum_t pnum);
/* Forget every player's ship (a new game). */
void custom_ship_clear_players();
/* The local pilot's choice (PlayerCfg.ShipName) for Player_num, and the
 * debug assignments of -shipfor; at the start of a game or level.
 */
void custom_ship_apply_local_choice();
/* Decode the ships of every player (at a level start). */
void custom_ship_preload();
/* A level starts: debris pieces of the last one are gone. */
void custom_ship_level_start();

/* object.cpp: draw the player object `obj` as its custom ship.  False if
 * it has none (the caller draws the Pyro).  `alpha` < 1 and
 * `flat_black` are the cloak's.
 */
bool custom_ship_draw_player(grs_canvas &canvas, const object_base &obj, const g3s_lrgb &light, float alpha, bool flat_black);
/* object.cpp: a debris object of a custom ship's explosion (the Pyro's
 * pieces, which still fly, are not drawn; the ship's own pieces are).
 */
[[nodiscard]]
bool custom_ship_hides_debris(objnum_t objnum, object_signature_t signature);
/* fireball.cpp: the Pyro debris object `debris` came from player `obj`. */
void custom_ship_debris_created(const object_base &obj, objnum_t debris, object_signature_t signature);
/* fireball.cpp: a player exploded; its ship's debris parts fly apart. */
void custom_ship_player_exploded(const object_base &obj);
/* render.cpp: draw the flying pieces (after the mine, before the 3D frame
 * ends).
 */
void custom_ship_draw_pieces(grs_canvas &canvas);

/* The ship menu's preview: `e` (nullptr: the Pyro) turned by `angles`,
 * in player colour `colour`, filling `canvas`.
 */
void custom_ship_draw_preview(grs_canvas &canvas, const custom_ship::entry *e, const vms_angvec &angles, unsigned colour);
/* The ship menu (Options -> Ship...). */
void custom_ship_menu();
/* The ship picker of the bot screens (Documentation/multiplayer-bots.md
 * section 9.20): the ship menu's list and preview with the rows Random
 * (`random_label`), the Pyro-GX, then custom_ship::list() (read again).
 * `selected`: the row to start on.  Returns the row chosen (0 Random,
 * 1 the Pyro-GX, 2 + i ship i), or nothing; `forced`: the game closed
 * it (game_leave_menus), not the pilot.
 */
[[nodiscard]]
std::optional<unsigned> custom_ship_pick_for_bot(const char *title, const char *random_label, unsigned selected, bool &forced);
/* The pilot's option "Show custom ships" (PlayerCfg.ShowCustomShips):
 * when off, every player is drawn as the Pyro-GX, whatever ship it
 * chose, and no ship is fetched from the host for drawing.
 */
[[nodiscard]]
bool custom_ships_shown();

}
#endif
