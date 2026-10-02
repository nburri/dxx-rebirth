/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer bots on the host (Documentation/multiplayer-bots.md, stages
 * B1, B3 and B4): the slots bots take, their brain on a fixed tick
 * (perception, target choice, goals, navigation, aim), the controls it
 * produces, firing and the weapon choice, missiles and mines, pickups,
 * fuel centres, the afterburner, the converter, damage, death and
 * respawn.
 *
 * A bot is a player slot the host flies.  Its ship is an ordinary remote
 * ship (control_source remote), moved by do_physics_sim from the bot's
 * controls through apply_pilot_controls, with the bot's own `pilot`.
 * Everything it does reaches the clients as the messages of a human
 * player, with the bot as originator (section 7.1).
 *
 * The decisions are taken on the brain's 60 Hz tick (perception at
 * 20 Hz, strategy at 5 Hz, staggered per bot); only the steering
 * controller runs every frame, from the current state (section 3.4).
 * The logic that does not need the game is in bot_brain.h and bot_nav.h.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "bot.h"
#include "bot_arena.h"
#include "bot_goals.h"
#include "bot_movement.h"
#include "movement_record_format.h"
#include "bot_nav.h"
#include "bot_weapons.h"
#include "net_v2_objects.h"
#include "net_interp.h"
#include "net_v2_game.h"
#include "pilot.h"
#include "multibot.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "game.h"
#include "gameseq.h"
#include "gameseg.h"
#include "fvi.h"
#include "physics.h"
#include "laser.h"
#include "weapon.h"
#include "collide.h"
#include "fireball.h"
#include "wall.h"
#include "segment.h"
#include "textures.h"
#include "piggy.h"
#include "kconfig.h"
#include "controls.h"
#include "endlevel.h"
#include "d_enumerate.h"
#include "newdemo.h"
#include "timer.h"
#include "strutil.h"
#include "vclip.h"
#include "bm.h"
#include "console.h"
#include "d_levelstate.h"
#include "powerup.h"
#include "fuelcen.h"
#include "digi.h"
#include "sounds.h"
#include "args.h"

namespace dsx {

namespace {

namespace b = ::dcx::bot;
using b::vec3;

/* The death tumble lasts as long as the human's (dead_player_frame). */
constexpr fix BOT_DEATH_EXPLODE_TIME{F1_0 * 2};
/* Section 4.8: a bot waits a moment before it respawns, as a human
 * takes a moment to press fire.
 */
constexpr double BOT_RESPAWN_MIN_S{1.0};
constexpr double BOT_RESPAWN_MAX_S{2.5};
/* A* expands at most this many segments per plan (section 10, R5). */
constexpr unsigned BOT_NAV_NODE_LIMIT{4000};
/* Seconds after a hit during which the attacker counts for revenge and
 * is noticed outside the field of view.
 */
constexpr unsigned BOT_REVENGE_TICKS{3 * b::BOT_TICK_RATE};
/* A cloaked player is seen only this close (section 4.2). */
constexpr double BOT_CLOAK_SEE_DISTANCE{40};
/* Section 9.5: backing off to a standoff, no closer to a wall behind. */
constexpr double BOT_BACK_WALL_CLEARANCE{25};
/* A roaming bot flies at least this far (section 4.3). */
constexpr double BOT_ROAM_MIN_DISTANCE{120};
/* A hunt toward a target that moves to another segment is planned again
 * at most this often (the plan also expires after 2 s).
 */
constexpr unsigned BOT_HUNT_REPLAN_TICKS{b::BOT_TICK_RATE / 2};
/* Section 9.10: a target out of sight this long starts a new engagement
 * (its shields at the start: the damage seen, b::pursuit_view).
 */
constexpr unsigned BOT_ENGAGEMENT_GAP_TICKS{10 * b::BOT_TICK_RATE};
/* Section 9.10: the speed of a corner peek, a share of the top speed. */
constexpr double BOT_PEEK_SPEED{0.7};
/* Section 4.6, dodge: projectiles within this distance are looked at,
 * their closest pass within this time, for this long a dodge.
 */
constexpr double BOT_DODGE_SCAN{150};
constexpr double BOT_DODGE_HORIZON{0.7};
/* Section 9.8: the incoming damage (the death dump) is judged below
 * these shields.
 */
constexpr unsigned BOT_DUMP_SCAN_SHIELDS{45};
constexpr unsigned BOT_DODGE_TICKS{b::ticks_from_ms(350)};
/* Section 4.3: a wall beyond the steer point is a bend only while the
 * velocity points within this angle of the steer point.
 */
constexpr double BOT_BEND_MAX_ANGLE{b::radians(40)};
/* Stage B3 (section 4.7): the path costs a bot computes at each strategy
 * tick reach this far, over at most this many segments.
 */
constexpr double BOT_DISTANCE_MAX_COST{2500};
constexpr unsigned BOT_DISTANCE_NODE_LIMIT{3000};
/* Powerups whose line of sight a bot checks per strategy tick (nearest
 * first, section 9.5).
 */
constexpr unsigned BOT_POWERUP_LOS_BUDGET{8};
/* A powerup the bot came to without taking it, or could not reach, is no
 * goal for this long.
 */
constexpr unsigned BOT_COLLECT_IGNORE_TICKS{5 * b::BOT_TICK_RATE};
constexpr unsigned BOT_COLLECT_UNREACHABLE_TICKS{10 * b::BOT_TICK_RATE};
/* Section 9.14: a power pickup given up (b::power_gave_up). */
constexpr unsigned BOT_POWER_GIVE_UP_IGNORE_TICKS{30 * b::BOT_TICK_RATE};
/* Fuel and repair centres give this much per second (Fuelcen_give_amount). */
constexpr fix BOT_FUELCEN_RATE{i2f(25)};
constexpr fix BOT_FUELCEN_SOUND_DELAY{F1_0 / 4};
/* A retreat without a known shield source draws this many places. */
constexpr unsigned BOT_FLEE_TRIES{10};
/* Section 9.5: the evasion after a hit from an unseen attacker, and how
 * long the bot then turns to where the attacker was.
 */
constexpr unsigned BOT_EVADE_TICKS{b::ticks_from_ms(600)};
constexpr unsigned BOT_TURN_TO_ATTACKER_TICKS{b::ticks_from_ms(1500)};
/* Section 9.5, the log (-verbose): one summary line per bot this often;
 * a denied touch of the same powerup is logged again after this long.
 */
constexpr unsigned BOT_LOG_TICKS{b::BOT_TICK_RATE};
constexpr unsigned BOT_DENY_LOG_TICKS{b::BOT_TICK_RATE};
/* Powerups the summary line looks at (the nearest valuable one). */
constexpr double BOT_LOG_POWERUP_RADIUS{120};

/* The log is written only with -verbose (CON_VERBOSE): its lines are
 * not even formatted otherwise.
 */
[[nodiscard]]
bool bot_log_on()
{
	return CGameArg.DbgVerbose >= CON_VERBOSE;
}

inline constexpr std::array<const char *, 10> primary_names{{"laser", "vulcan", "spread", "plasma", "fusion", "super", "gauss", "helix", "phoenix", "omega"}};
inline constexpr std::array<const char *, 10> secondary_names{{"conc", "homing", "prox", "smart", "mega", "flash", "guided", "smine", "merc", "shaker"}};
inline constexpr std::array<const char *, b::BOT_GOAL_COUNT> goal_names{{"roam", "hunt", "engage", "collect", "retreat", "refuel"}};

[[nodiscard]]
const char *primary_name(const unsigned i)
{
	return i < primary_names.size() ? primary_names[i] : "?";
}

[[nodiscard]]
const char *goal_name(const b::goal_kind g)
{
	const auto i{static_cast<unsigned>(g)};
	return i < goal_names.size() ? goal_names[i] : "?";
}

/* Section 9.5, the log: why the nearest valuable powerup is not being
 * taken.
 */
enum class collect_why : uint8_t
{
	none,
	/* It is the bot's goal now. */
	collecting,
	/* The bot does not know it (not seen, heard, or on its map). */
	not_known,
	/* Not a goal for a while (reached without taking it, or stuck). */
	ignored,
	/* No path within reach. */
	unreachable,
	/* The rules would not let the bot take it (full, already has). */
	cannot_use,
	/* Worth little to the bot now. */
	no_value,
	/* Another goal scored higher. */
	goal_lower,
	/* Touched, and the host denied it. */
	denied,
};

inline constexpr std::array<const char *, 9> collect_why_names{{"-", "collecting", "not-known", "ignored", "unreachable", "cannot-use", "no-value", "goal-lower", "denied"}};

/* The names of the host's pickup denials (net_v2_objects.h deny_reason). */
inline constexpr std::array<const char *, 6> deny_names{{"gone", "dead", "range", "spat", "cannot-use", "not-arbitrated"}};

[[nodiscard]]
const char *deny_name(const uint8_t r)
{
	return r < deny_names.size() ? deny_names[r] : "no-netid";
}

[[nodiscard]]
vec3 to_vec(const vms_vector &v)
{
	return {v.x / 65536.0, v.y / 65536.0, v.z / 65536.0};
}

[[nodiscard]]
fix to_fix(const double d)
{
	return static_cast<fix>(std::clamp(std::lround(d * 65536), -0x7fffffffl, 0x7fffffffl));
}

[[nodiscard]]
vms_vector to_fixvec(const vec3 &v)
{
	return {to_fix(v.x), to_fix(v.y), to_fix(v.z)};
}

[[nodiscard]]
b::frame3 to_frame(const vms_matrix &m)
{
	return {to_vec(m.rvec), to_vec(m.uvec), to_vec(m.fvec)};
}

/* What the tactics layer reads, `reaction` late (section 4.2). */
struct percept
{
	uint8_t target{0xff};
	bool visible{};
	vec3 pos;
	vec3 vel;
};

enum class bot_life : uint8_t
{
	alive,
	/* Killed: its ship tumbles until it explodes. */
	dying,
	/* Exploded: a ghost until it respawns. */
	dead,
};

enum class bot_goal : uint8_t
{
	none,
	/* No target: fly to a random place. */
	roam,
	/* A target out of reach of a direct shot: fly to where it was seen. */
	hunt,
	/* Stage B3 (section 4.7): fly to a powerup. */
	collect,
	/* Weak and threatened: away, to shields if it knows any. */
	retreat,
	/* To a fuel or repair centre, and hover there until full. */
	refuel,
};

/* Section 9.15: the movement recorder writes the goal and the movement
 * mode as they are here (format minor 4).
 */
namespace mrb = ::dcx::movrec;
static_assert(static_cast<uint8_t>(bot_goal::none) == mrb::bot_goals::none);
static_assert(static_cast<uint8_t>(bot_goal::roam) == mrb::bot_goals::roam);
static_assert(static_cast<uint8_t>(bot_goal::hunt) == mrb::bot_goals::hunt);
static_assert(static_cast<uint8_t>(bot_goal::collect) == mrb::bot_goals::collect);
static_assert(static_cast<uint8_t>(bot_goal::retreat) == mrb::bot_goals::retreat);
static_assert(static_cast<uint8_t>(bot_goal::refuel) == mrb::bot_goals::refuel);
static_assert(static_cast<uint8_t>(b::move_mode::path) == mrb::bot_modes::path);
static_assert(static_cast<uint8_t>(b::move_mode::fight_keys) == mrb::bot_modes::fight);
static_assert(static_cast<uint8_t>(b::move_mode::path_keys) == mrb::bot_modes::path_keys);
static_assert(static_cast<uint8_t>(b::move_mode::turn_keys) == mrb::bot_modes::turn);
static_assert(static_cast<uint8_t>(b::move_mode::slide) == mrb::bot_modes::slide);
static_assert(static_cast<uint8_t>(b::move_mode::duck) == mrb::bot_modes::duck);
static_assert(static_cast<uint8_t>(b::move_mode::recover) == mrb::bot_modes::recover);
static_assert(b::MOVE_MODE_COUNT == mrb::bot_modes::count);

struct bot_controls
{
	double pitch{}, heading{}, forward{}, sideways{}, vertical{};
};

struct bot_state
{
	playernum_t pid;
	bot_config cfg;
	/* Section 9.7: the bot's own skill and style (its setup line). */
	b::bot_skill skill_level{b::BOT_DEFAULT_SKILL};
	b::skill_params skill{b::skill_of(b::BOT_DEFAULT_SKILL)};
	b::style_params style{b::style_of(b::bot_style::balanced)};
	/* Section 9.13: the constants a style profile tunes (the defaults
	 * for a built-in style), and the profile the bot flies, if any.
	 */
	b::tune_params tune;
	bool profile_applied{};
	/* The console said that the profile's file is missing. */
	bool profile_missing_said{};
	/* Section 9.13: long straight flights may light the afterburner
	 * (b::tune_params::roam_burn, drawn until roam_roll_at).
	 */
	bool roam_burn_ok{true};
	uint32_t roam_roll_at{};
	/* The retreat threshold of the last strategy tick: the style's,
	 * raised when outgunned (b::style_retreat_shields).
	 */
	double retreat_shields{35};
	/* Section 9.7: a beginner's trigger pauses (b::fire_burst). */
	b::fire_burst burst;
	/* Section 2.3: the order in which the bots were added; a joining
	 * human replaces the most recently added one.
	 */
	unsigned added{};
	pilot pl{};
	b::bot_rng rng;
	/* The stagger of the slower layers: the slot. */
	unsigned stagger{};
	/* Brain output, fixed at the last tick. */
	vec3 face_dir{0, 0, 1};
	/* The angular velocity of face_dir (the steering's feed-forward). */
	vec3 face_rate;
	vec3 move_cmd;
	bool fire{};
	int heading_pref{1};
	/* This frame's controls. */
	bot_controls ctl;
	/* Perception. */
	per_player_array<b::target_memory> memory{};
	per_player_array<bool> visible_now{};
	std::optional<uint8_t> target;
	b::delay_line<percept, 16> seen;
	bool shot_clear{};
	/* Along a clear line of fire, how far the first ship on it is (the
	 * target, or another enemy in front of it): where a missile bursts.
	 */
	double shot_first_hit{1e9};
	uint8_t last_attacker{0xff};
	uint32_t attacked_tick{};
	b::aim_error aim;
	/* Section 4.4: the lead factor, 1 on average (b::aim_lead). */
	b::aim_lead lead;
	/* Section 4.5: the range band of the weapon choice (hysteresis). */
	std::optional<b::range_band> band;
	b::juke_state juke;
	/* Section 4.6, dodge: the salt of this life's rolls (b::dodge_roll),
	 * and the dodge under way.
	 */
	uint32_t dodge_salt{};
	vec3 dodge_dir;
	uint32_t dodge_from{}, dodge_until{};
	/* Stage B3 (section 4.7): the powerups it knows, the path costs from
	 * where it is (each strategy tick), the powerup or centre it goes to.
	 */
	b::powerup_memory powerups;
	b::nav_distances dist;
	/* Section 9.5: the tick + 1 of the bot's last visit to each segment
	 * (0: never), for exploring.
	 */
	std::vector<uint32_t> visited;
	uint16_t collect_key{0xffff}, collect_sig{};
	uint32_t refuel_seg{};
	/* The afterburner is lit. */
	bool burning{};
	/* Stage B4 (section 9.4): the missile chosen and waiting for its
	 * release (until missile_until), the one to fire (and the rest of
	 * its volley), when the last missile, heavy missile and mine went,
	 * and the target of the last heavy one.
	 */
	std::optional<b::secondary> missile;
	uint32_t missile_until{};
	std::optional<b::secondary> missile_fire;
	unsigned missile_volley{};
	std::optional<uint32_t> last_missile, last_heavy, last_mine;
	/* Section 9.10 and the PR #47 review: the pending missile is the
	 * homing shot round the corner of a pursuit (b::release_aim::corner),
	 * and when the last such shot was chosen (b::HOMING_CORNER_INTERVAL).
	 */
	bool corner_shot{};
	std::optional<uint32_t> last_corner_homing;
	/* Section 9.8: the volley under way (b::volley_size): its missile
	 * and the rounds still to go.
	 */
	std::optional<b::secondary> volley_missile;
	unsigned volley_left{};
	/* Section 9.8, the death dump: this life's roll, whether it is under
	 * way, the damage of the enemy shots coming at the bot (perceive).
	 */
	double dump_roll{1};
	bool dumping{};
	double incoming_damage{};
	/* Section 9.8: turning round to a target behind. */
	b::turn_round_state turning;
	/* Section 9.12: the fight's keys: the slide while the nose comes
	 * round, the range key, the filter of the strafe keys.
	 */
	b::slide_state slide;
	b::approach_key approach;
	b::lateral_keys lateral;
	/* Section 9.15: the path flown with keys in a fight, the hold of the
	 * choice between the fight's keys and the path, the movement of the
	 * last tick (the movement recorder writes it).
	 */
	b::path_keys path_keys;
	b::mode_hold hold;
	b::fire_blocked blocked;
	b::move_mode mode{b::move_mode::none};
	/* Section 9.15: the free distance along the ship's axes (perceive,
	 * fighting with keys), for the juke.
	 */
	b::fight_room room;
	/* Section 9.12: retreating (at the last tick), flown turned away and
	 * with the afterburner (drawn until flee_roll_at).
	 */
	bool fleeing{}, flee_turned{}, flee_burn{};
	uint32_t flee_roll_at{};
	/* Where the bot aims (face_dir, unless it flees turned away from
	 * its target): what the missiles are fired along.
	 */
	vec3 aim_dir{0, 0, 1};
	/* Section 9.8: the power-up phase at the last strategy tick (the
	 * log).
	 */
	bool powerup_phase{};
	unsigned third_parties{};
	uint8_t heavy_target{0xff};
	/* Section 4.7: cloak and invulnerability (b::tactics_for), from the
	 * last strategy tick.
	 */
	b::powerup_tactics tactics;
	fix64 fuel_sound_at{};
	/* Section 9.5: the fusion cannon's charge (the ship's Fusion_charge
	 * holds it), what the last tick decided, the warm-up sound.
	 */
	bool fusion_charging{};
	b::fusion_action fusion_want{b::fusion_action::idle};
	fix64 fusion_sound_at{};
	/* Section 9.5: a fighting bot with a heavy missile ready keeps this
	 * distance (0: none); after it released one, it keeps blast_hold
	 * and does not close in until blast_hold_until (the missile's flight
	 * and blast, b::blast_danger_seconds).
	 */
	double standoff{};
	double blast_hold{};
	uint32_t blast_hold_until{};
	/* Section 9.6: the bot's appetite for risk (its skill and style),
	 * the best aim of each heavy missile at the last weighing (index:
	 * 0 earthshaker, 1 mega) and when that was, the aim of the heavy
	 * missile chosen, the log's tallies.
	 */
	b::risk_profile risk;
	std::array<b::aim_choice, 2> heavy_plan{};
	uint32_t heavy_plan_tick{};
	bool heavy_planned{};
	std::optional<b::aim_option> heavy_aim;
	/* Section 9.6, hugging: what the bot knows of each enemy's heavy
	 * missiles (b::heavy_holding: it saw it pick them up, fire them),
	 * the last shot of each counted, the heavy powerup next to each
	 * visible enemy (a pickup when it is gone), the hug under way, when
	 * it started or ended, the draw of this engagement.
	 */
	per_player_array<b::heavy_holding> heavy_holding{};
	per_player_array<uint16_t> heavy_fire_sig{};
	per_player_array<uint16_t> heavy_near_obj{};
	per_player_array<uint16_t> heavy_near_sig{};
	bool hugging{};
	uint32_t hug_changed{};
	/* Section 9.6: the rotation of the aim search (b::aim_search). */
	unsigned heavy_plan_phase{};
	double hug_keep{};
	double hug_roll{1};
	uint8_t hug_roll_target{0xff};
	/* Section 9.6: breaking the line of sight before a heavy shot. */
	std::optional<vec3> duck_point;
	uint32_t duck_until{};
	/* Section 9.5: after a hit from an unseen attacker, turn to it from
	 * turn_from until turn_until.
	 */
	uint8_t turn_to{0xff};
	uint32_t turn_from{}, turn_until{};
	/* Section 9.5: a valuable powerup close by is the collect goal. */
	bool grabbing{};
	/* Section 9.14: a power pickup is the collect goal; the turn of the
	 * further power pickups' line of sight checks.
	 */
	bool power_going{};
	uint8_t power_sight_turn{};
	/* The tick it set out for that power pickup (b::power_gave_up). */
	uint32_t power_since{};
	/* Section 9.9: with no target, the enemy whose last known place the
	 * bot flies to (b::seek_utility), and for each enemy the memory
	 * tick + 1 of the place it searched already (reached, nobody there).
	 */
	std::optional<uint8_t> seek_who;
	per_player_array<uint32_t> seek_done{};
	/* Section 9.10: the pursuit of a target round a corner. */
	struct pursuit_track
	{
		bool active{};
		uint8_t who{0xff};
		b::pursuit_reason reason{b::pursuit_reason::none};
		uint32_t started{};
		/* The engagement: the target last in sight while the bot
		 * engaged it, when, and its shields at the engagement's start
		 * (the most seen since).
		 */
		uint8_t engaged_who{0xff};
		uint32_t engaged_tick{};
		double engage_shields{};
		/* The bot's last hit on an enemy (bot_take_damage). */
		uint8_t hit_who{0xff};
		uint32_t hit_tick{};
		/* Hysteresis: after the bot gave up a pursuit, none again (nor
		 * a hunt) until it has seen that target again
		 * (b::pursuit_block).
		 */
		b::pursuit_block ended;
		/* Predicted places reached without finding the target. */
		unsigned advances{};
		/* The predicted place (b::predict_pursuit), when planned. */
		uint32_t seg{};
		vec3 point;
		bool dead_end{};
		/* Corner clearing (b::corner_approach_point): done for this
		 * pursuit, or the peek under way (until peek_until).
		 */
		bool corner_done{};
		bool peek{};
		vec3 peek_point, peek_aim;
		uint32_t peek_until{};
	} pursuit;
	/* Section 9.9: what it holds to fight with (the log). */
	b::armed_level armed{b::armed_level::none};
	/* Section 9.5, the log: the goal's choice at the last strategy
	 * tick (the chosen one and the best other), the heavy missiles'
	 * verdict, the last denied touch, the next summary line.
	 */
	b::goal_kind chosen{b::goal_kind::roam};
	double chosen_u{};
	b::goal_kind runner_up{b::goal_kind::roam};
	double runner_up_u{};
	double collect_u{};
	b::heavy_verdict heavy_why{b::heavy_verdict::none_owned};
	/* Section 9.9: the light missiles' verdict (the log). */
	b::light_verdict light_why{b::light_verdict::none_owned};
	double heavy_min{};
	uint16_t deny_key{0xffff};
	uint8_t deny_reason{};
	uint32_t deny_tick{};
	uint32_t next_log_tick{};
	/* Navigation. */
	bot_goal goal{bot_goal::none};
	uint32_t goal_seg{};
	std::vector<vec3> points;
	/* The edge (source segment, side) each path point lies on. */
	std::vector<std::pair<uint32_t, uint8_t>> point_edges;
	std::size_t point_index{};
	std::size_t steer_index{};
	uint32_t next_plan_tick{};
	uint32_t last_plan_tick{};
	b::stuck_detector stuck;
	b::edge_penalties penalties;
	vec3 recover_dir;
	vec3 avoid;
	uint32_t avoid_until{};
	/* Life. */
	bot_life life{bot_life::alive};
	bool death_pending{};
	fix64 died_at{};
	fix64 respawn_at{};
	explicit bot_state(const playernum_t p, const bot_config &c) :
		pid{p}, cfg{c}, stagger{p}, risk{b::risk_profile_of(c.skill, c.style)}
	{
		apply_config();
		heavy_near_obj.fill(0xffff);
	}
	/* Section 9.7: the presets of the bot's skill and style; section
	 * 9.13: or of its style profile at its skill (b::apply_style_profile),
	 * the base style's without the profile's file.
	 */
	void apply_config()
	{
		skill_level = cfg.skill;
		skill = b::skill_of(cfg.skill);
		style = b::style_of(cfg.style);
		tune = {};
		profile_applied = false;
		if (cfg.profile[0])
		{
			if (const auto ls{bots_style_library().find(cfg.profile.data())})
			{
				const auto p{b::apply_style_profile(ls->profile, cfg.skill)};
				skill = p.skill;
				style = p.style;
				tune = p.tune;
				profile_applied = true;
			}
			else if (!profile_missing_said)
			{
				profile_missing_said = true;
				con_printf(CON_NORMAL, "bots: '%s': no style \"%s\" in %s/ (a .botstyle file): it flies %s", static_cast<const char *>(cfg.name), cfg.profile.data(), b::BOT_STYLE_FOLDER, b::bot_style_names[static_cast<unsigned>(cfg.style) % b::BOT_STYLE_COUNT]);
			}
		}
		retreat_shields = style.retreat_shields;
		risk = b::risk_profile_of(cfg.skill, cfg.style);
	}
	void reset_for_life(const uint32_t tick)
	{
		pl = {};
		face_dir = {0, 0, 1};
		face_rate = {};
		move_cmd = {};
		fire = false;
		ctl = {};
		/* Section 4.8: memory is cleared, except who killed it. */
		for (auto &&[i, m] : enumerate(memory))
			if (i != last_attacker)
				m = {};
		visible_now = {};
		target.reset();
		seen.clear();
		shot_clear = false;
		shot_first_hit = 1e9;
		attacked_tick = tick;
		aim.reset();
		lead.reset();
		band.reset();
		juke.reset();
		dodge_salt = rng.next();
		dodge_dir = {};
		dodge_from = dodge_until = 0;
		collect_key = 0xffff;
		burning = false;
		fusion_charging = false;
		fusion_want = b::fusion_action::idle;
		standoff = 0;
		blast_hold = 0;
		blast_hold_until = 0;
		apply_config();
		burst.reset();
		heavy_plan = {};
		heavy_planned = false;
		heavy_aim.reset();
		heavy_near_obj.fill(0xffff);
		hugging = false;
		hug_changed = 0;
		hug_roll_target = 0xff;
		duck_point.reset();
		duck_until = 0;
		turn_to = 0xff;
		turn_from = turn_until = 0;
		grabbing = false;
		power_going = false;
		pursuit = {};
		heavy_why = b::heavy_verdict::none_owned;
		missile.reset();
		missile_fire.reset();
		missile_volley = 0;
		volley_missile.reset();
		volley_left = 0;
		dump_roll = rng.uniform();
		dumping = false;
		incoming_damage = 0;
		turning.reset();
		slide.reset();
		approach.reset();
		lateral.reset();
		path_keys.reset();
		hold.reset();
		blocked.reset();
		mode = b::move_mode::none;
		room = {};
		fleeing = flee_turned = flee_burn = false;
		flee_roll_at = 0;
		roam_burn_ok = true;
		roam_roll_at = 0;
		aim_dir = {0, 0, 1};
		powerup_phase = false;
		third_parties = 0;
		last_missile.reset();
		last_heavy.reset();
		last_mine.reset();
		corner_shot = false;
		last_corner_homing.reset();
		heavy_target = 0xff;
		tactics = {};
		clear_path();
		goal = bot_goal::none;
		stuck.reset();
		penalties.clear();
		avoid = {};
		avoid_until = 0;
		life = bot_life::alive;
		death_pending = false;
	}
	void clear_path()
	{
		points.clear();
		point_edges.clear();
		point_index = steer_index = 0;
		stuck.restart_window();
	}
};

/* The ship's limits, the same for every ship (section 3.4). */
struct ship_limits
{
	double max_speed{};
	b::turn_response turn;
};

struct bots_state
{
	per_player_array<std::optional<bot_state>> bots;
	::dcx::net_interp::tick_accumulator tick{b::BOT_TICK_RATE};
	bool tick_started{};
	fix64 last_time{};
	b::nav_graph graph;
	b::astar_search search;
	b::path_result path;
	/* Side centres, 6 per segment. */
	std::vector<vec3> side_centres;
	/* Stage B3: the fuel centres and repair centres of the level. */
	std::vector<uint32_t> fuel_centres;
	std::vector<uint32_t> repair_centres;
	/* Section 9.5: the spawn sites a ship can fly out of (b::spawn_site_open). */
	per_player_array<bool> site_open{};
	/* Section 9.8: the weapon powerups at the level's start, and whether
	 * that is too few for its players (b::weapon_poor_level).
	 */
	unsigned weapon_powerups{};
	bool weapon_poor{};
	ship_limits limits;
	/* The controls passed to apply_pilot_controls. */
	control_info controls{};
};

bots_state B;

[[nodiscard]]
bot_state *find_bot(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return nullptr;
	auto &o{B.bots[pnum]};
	if (!o)
		return nullptr;
	/* A human's connection for the slot: the bot is gone (the slot was
	 * released without the hooks, which must not happen).
	 */
	if (!b::slot_flown_by_bot(true, net_v2::host_slot_has_peer(pnum)))
	{
		o.reset();
		return nullptr;
	}
	return &*o;
}

[[nodiscard]]
bool bots_running()
{
	return +(Game_mode & GM_NETWORK) && multi_i_am_master() && Newdemo_state != ND_STATE_PLAYBACK;
}

[[nodiscard]]
object &ship_of(const playernum_t pnum)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	return *Objects.vmptr(vcplayerptr(pnum)->objnum);
}

/* Navigation graph (section 4.3): segment centres, the child adjacency,
 * the cost through the centre of the shared side.
 */
void build_nav_graph()
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &Vertices = LevelSharedVertexState.get_vertices();
	auto &vcvertptr = Vertices.vcptr;
	const auto count{static_cast<std::size_t>(Segments.get_count())};
	B.graph.begin(count);
	B.side_centres.assign(count * 6, vec3{});
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		B.graph.set_position(n, to_vec(compute_segment_center(vcvertptr, segp)));
		for (const auto side : MAX_SIDES_PER_SEGMENT)
			B.side_centres[n * 6 + underlying_value(side)] = to_vec(compute_center_point_on_side(vcvertptr, segp, side));
	}
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		const auto &from{B.graph.position(n)};
		for (const auto side : MAX_SIDES_PER_SEGMENT)
		{
			const auto child{segp->shared_segment::children[side]};
			if (!IS_CHILD(child) || child >= count)
				continue;
			const auto &mid{B.side_centres[n * 6 + underlying_value(side)]};
			const double cost{b::distance(from, mid) + b::distance(mid, B.graph.position(child))};
			B.graph.add_edge(n, {child, underlying_value(side), static_cast<float>(cost)});
		}
	}
	B.graph.finish();
	B.fuel_centres.clear();
	B.repair_centres.clear();
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		const shared_segment &ss{segp};
		if (ss.special == segment_special::fuelcen)
			B.fuel_centres.push_back(n);
		else if (ss.special == segment_special::repaircen)
			B.repair_centres.push_back(n);
	}
	con_printf(CON_VERBOSE, "bots: navigation graph of %zu segments, %zu edges", B.graph.size(), B.graph.edge_count());
}

/* An edge a bot may take now (section 4.3): open to flying, or a door
 * it can open by flying into it.
 */
[[nodiscard]]
bool edge_passable(const uint32_t from, const b::nav_edge &e, const player_flags powerup_flags)
{
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &vcwallptr = Walls.vcptr;
	const auto &&segp{vcsegptr(static_cast<segnum_t>(from))};
	const auto side{static_cast<sidenum_t>(e.side)};
	if (WALL_IS_DOORWAY(GameBitmaps, Textures, vcwallptr, segp, side) & WALL_IS_DOORWAY_FLAG::fly)
		return true;
	const auto wall_num{segp->shared_segment::sides[side].wall_num};
	if (wall_num == wall_none)
		return false;
	const auto &w{*vcwallptr(wall_num)};
	if (w.type != WALL_DOOR || +(w.flags & wall_flag::door_locked))
		return false;
	return w.keys == wall_key::none || +(powerup_flags & static_cast<player_flag>(w.keys));
}

/* The segments a ship without keys flies to from `seg`, by the doors and
 * walls as they are now.
 */
[[nodiscard]]
std::size_t reachable_from(const uint32_t seg)
{
	if (seg >= B.graph.size())
		return 0;
	b::nav_distances d;
	d.compute(B.graph, seg, [](const uint32_t from, const b::nav_edge &e) {
		return edge_passable(from, e, player_flags{});
	}, [](uint32_t, const b::nav_edge &) {
		return 0.0;
	}, 1e12, static_cast<unsigned>(B.graph.size()));
	std::size_t n{0};
	for (uint32_t i = 0; i < B.graph.size(); ++i)
		n += d.reached(i);
	return n;
}

/* Section 9.5: which spawn sites a bot can fly out of, at level start. */
void judge_spawn_sites()
{
	B.site_open.fill(true);
	for (unsigned i = 0; i < NumNetPlayerPositions && i < MAX_PLAYERS; ++i)
	{
		const uint32_t seg{Player_init[i].segnum};
		const auto n{reachable_from(seg)};
		B.site_open[i] = b::spawn_site_open(n, B.graph.size());
		if (!B.site_open[i])
			con_printf(CON_VERBOSE, "bots: spawn site %u (segment %u) is sealed: %zu of %zu segments reachable; bots avoid it", i, seg, n, B.graph.size());
	}
}

/* A spawn for a bot: the draw of choose_spawn among the sites it can fly
 * out of (the sealed ones are left out before the draw), with the bot's
 * own random numbers: the game's d_rand is not reseeded by bot code.
 */
[[nodiscard]]
spawn_choice bot_spawn(bot_state &bs)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	return choose_bot_spawn(Objects.vmptr, bs.pid, B.site_open, bs.rng.next());
}

/* A clear line from `from` to `to`: `rad` > 0 for a flight path (the
 * ship's body), 0 for sight through grates (FQ_TRANSWALL).
 */
[[nodiscard]]
bool line_clear(const object &obj, const vms_vector &from, const segnum_t from_seg, const vms_vector &to, const fix rad, const bool see_through)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		from,
		to,
		fvi_query::unused_ignore_obj_list,
		fvi_query::unused_LevelUniqueObjectState,
		fvi_query::unused_Robot_info,
		see_through ? FQ_TRANSWALL : 0,
		Objects.vcptridx(&obj),
	}, from_seg, rad, hit)};
	return type == fvi_hit_type::None;
}

[[nodiscard]]
bool same_team(const playernum_t a, const playernum_t c)
{
	return +(Game_mode & GM_TEAM) && multi_get_team_from_player(Netgame, a) == multi_get_team_from_player(Netgame, c);
}

/* Section 4.4, trigger discipline: nothing but the target (or another
 * enemy) is first on the line of fire, never a teammate or the reactor
 * (decision 6 of section 11).  `first_hit` gets the distance of the ship
 * first on a clear line (a missile's blast is checked against it).
 */
[[nodiscard]]
bool shot_line_clear(const bot_state &bs, const object &obj, const object &target, double &first_hit)
{
	first_hit = 1e9;
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		obj.pos,
		target.pos,
		fvi_query::unused_ignore_obj_list,
		&LevelUniqueObjectState,
		&LevelSharedRobotInfoState.Robot_info,
		FQ_IGNORE_POWERUPS,
		Objects.vcptridx(&obj),
	}, obj.segnum, F1_0 / 2, hit)};
	if (type == fvi_hit_type::None)
		return true;
	if (type != fvi_hit_type::Object || hit.hit_object == object_none)
		return false;
	const auto &o{*Objects.vcptr(hit.hit_object)};
	switch (o.type)
	{
		case object_type::OBJ_WEAPON:
			/* Another shot in flight: it does not stop this one. */
			return true;
		case object_type::OBJ_PLAYER:
		{
			const auto who{get_player_id(o)};
			if (who == bs.pid || same_team(bs.pid, who))
				return false;
			first_hit = b::distance(to_vec(obj.pos), to_vec(hit.hit_pnt));
			return true;
		}
		default:
			/* The reactor, robots, clutter. */
			return false;
	}
}

/* The speed of the shots of the ship's primary weapon, in game units,
 * as the lead needs it (section 4.4): the weapon data's speed at the
 * game's difficulty, which is the speed Laser_create_new gives the shot
 * (GameUniqueState.Difficulty_level: in multiplayer the netgame's).
 */
[[nodiscard]]
double weapon_speed(const player_info &pi)
{
#if DXX_BUILD_DESCENT == 2
	/* Omega's lightning is there at once (do_omega_stuff): no lead. */
	if (pi.Primary_weapon == primary_weapon_index::omega)
		return 1e5;
#endif
	const auto &wi{Weapon_info[Primary_weapon_to_weapon_info[pi.Primary_weapon]]};
	return b::effective_shot_speed(wi.speed[GameUniqueState.Difficulty_level] / 65536.0, wi.thrust != 0);
}

/* Omega's reach (laser.cpp MAX_OMEGA_DIST: 16 blobs 5 units apart). */
constexpr double BOT_OMEGA_RANGE{16 * 5};

/* How far its shots fly before they expire: the bot does not fire
 * beyond that (four fifths of it, with a floor for the data's odd
 * weapons).
 */
[[nodiscard]]
double weapon_range(const player_info &pi)
{
#if DXX_BUILD_DESCENT == 2
	if (pi.Primary_weapon == primary_weapon_index::omega)
		return BOT_OMEGA_RANGE * 0.9;
#endif
	const auto &wi{Weapon_info[Primary_weapon_to_weapon_info[pi.Primary_weapon]]};
	const double r{weapon_speed(pi) * (wi.lifetime / 65536.0)};
	return std::clamp(r * 0.8, 20.0, 400.0);
}

/* The gun a primary's shots leave from, in the ship's frame (right, up,
 * forward): the average of the guns do_laser_firing uses, since every
 * shot flies parallel to the nose from its gun.  Section 4.4: the lead
 * is solved from there, not from the ship's centre.
 */
[[nodiscard]]
vec3 gun_local(const primary_weapon_index w, const bool quad)
{
	const auto gun{[](const player_gun_number g) {
		return to_vec(Player_ship->gun_points[g]);
	}};
	switch (w)
	{
		case primary_weapon_index::laser:
			if (quad)
				return (gun(player_gun_number::_0) + gun(player_gun_number::_1) + gun(player_gun_number::_2) + gun(player_gun_number::_3)) * 0.25;
			[[fallthrough]];
		case primary_weapon_index::plasma:
		case primary_weapon_index::fusion:
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::phoenix:
#endif
			return (gun(player_gun_number::_0) + gun(player_gun_number::_1)) * 0.5;
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::omega:
			return gun(player_gun_number::_1);
#endif
		default:
			return gun(player_gun_number::center);
	}
}

/* The half angle (radians) of a primary's shot pattern (do_laser_firing:
 * spreadfire's outer shots 1/16 off the nose, helix's 2/16).
 */
[[nodiscard]]
double spread_half_angle(const primary_weapon_index w)
{
	switch (w)
	{
		case primary_weapon_index::spreadfire:
			return std::atan(1.0 / 16);
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::helix:
			return std::atan(2.0 / 16);
#endif
		default:
			return 0;
	}
}

/* The energy a primary uses per second of continuous fire
 * (do_laser_firing_player: the difficulty's cost per volley, helix twice
 * in multiplayer, fire_wait between volleys); 0 for the ammunition
 * weapons and omega (charged, not fired from the energy).
 */
[[nodiscard]]
std::array<double, 10> energy_rates()
{
	std::array<double, 10> r{};
	for (unsigned i = 0; i < r.size() && i < MAX_PRIMARY_WEAPONS; ++i)
	{
		const auto w{static_cast<primary_weapon_index>(i)};
		const auto id{Primary_weapon_to_weapon_info[w]};
		if (id >= N_weapon_types)
			continue;
		const auto &wi{Weapon_info[id]};
		if (weapon_index_uses_vulcan_ammo(w))
			continue;
#if DXX_BUILD_DESCENT == 2
		if (w == primary_weapon_index::omega)
			continue;
#endif
		double e{wi.energy_usage / 65536.0};
		const auto d{GameUniqueState.Difficulty_level};
		if (d == Difficulty_level_type::_0 || d == Difficulty_level_type::_1)
			e *= (underlying_value(d) + 2) / 4.0;
#if DXX_BUILD_DESCENT == 2
		if (id == weapon_id_type::HELIX_ID && +(Game_mode & GM_MULTI))
			e *= 2;
#endif
		const double wait{std::max(wi.fire_wait / 65536.0, 0.01)};
		r[i] = e / wait;
	}
	return r;
}

/* Stage B4: a secondary's weapon data (b::missile_data). */
[[nodiscard]]
b::missile_data missile_data_of(const secondary_weapon_index w)
{
	const auto &wi{Weapon_info[Secondary_weapon_to_weapon_info[w]]};
	return {
		.speed = wi.speed[GameUniqueState.Difficulty_level] / 65536.0,
		.blast_radius = wi.damage_radius / 65536.0,
		.thrust = wi.thrust != 0,
		.homing = wi.homing_flag != 0,
		.child_blast_radius = wi.children != weapon_id_type::unspecified && wi.children < N_weapon_types ? Weapon_info[wi.children].damage_radius / 65536.0 : 0,
		/* Section 9.6: the blasts' damage (explode_badass_weapon: the
		 * strength at the game's difficulty) and the children
		 * (create_weapon_smart_children: NUM_SMART_CHILDREN).
		 */
		.damage = wi.strength[GameUniqueState.Difficulty_level] / 65536.0,
		.child_damage = wi.children != weapon_id_type::unspecified && wi.children < N_weapon_types ? Weapon_info[wi.children].strength[GameUniqueState.Difficulty_level] / 65536.0 : 0,
		.children = wi.children != weapon_id_type::unspecified && wi.children < N_weapon_types ? NUM_SMART_CHILDREN : 0u,
	};
}

[[nodiscard]]
secondary_weapon_index game_secondary(const b::secondary s)
{
	return static_cast<secondary_weapon_index>(static_cast<uint8_t>(s));
}

/* The gun a secondary leaves from (do_missile_firing: guns 4 and 5 in
 * turn for the missiles), in the ship's frame.
 */
[[nodiscard]]
vec3 missile_gun_local(const secondary_weapon_index w)
{
	const auto g{Secondary_weapon_to_gun_num[w]};
	if (g == player_gun_number::_4)
		return (to_vec(Player_ship->gun_points[player_gun_number::_4]) + to_vec(Player_ship->gun_points[static_cast<player_gun_number>(5)])) * 0.5;
	return to_vec(Player_ship->gun_points[g]);
}

[[nodiscard]]
bool has_flag(const player_info &pi, const player_flag f)
{
	return +(pi.powerup_flags & f) ? true : false;
}

[[nodiscard]]
b::weapon_view weapons_of(const player_info &pi)
{
	return {
		.owned = pi.primary_weapon_flags,
		.laser_level = underlying_value(pi.laser_level),
		.quad = has_flag(pi, player_flag::quad_lasers),
		.energy = pi.energy / 65536.0,
		.vulcan_ammo = pi.vulcan_ammo,
		.energy_rate = energy_rates(),
#if DXX_BUILD_DESCENT == 2
		.omega_charge = pi.Omega_charge / static_cast<double>(MAX_OMEGA_CHARGE),
#else
		.omega_charge = 0,
#endif
	};
}

/* The log: what the bot has, compactly ("laser2q,plasma,vulcan(1200)
 * sec=conc4,shaker1").
 */
void describe_armament(const player_info &pi, char *const buf, const std::size_t size)
{
	if (!size)
		return;
	buf[0] = 0;
	std::size_t n{0};
	/* snprintf's result, the text kept within the buffer. */
	const auto adv{[&](const int w) {
		if (w > 0)
			n = std::min(size - 1, n + static_cast<std::size_t>(w));
	}};
	adv(std::snprintf(buf + n, size - n, "laser%u%s", underlying_value(pi.laser_level) + 1u, has_flag(pi, player_flag::quad_lasers) ? "q" : ""));
	for (unsigned i = 1; i < MAX_PRIMARY_WEAPONS && i < primary_names.size(); ++i)
	{
		if (i == 5 || !(pi.primary_weapon_flags & (1u << i)))
			continue;
		if (weapon_index_uses_vulcan_ammo(static_cast<primary_weapon_index>(i)))
			adv(std::snprintf(buf + n, size - n, ",%s(%u)", primary_names[i], static_cast<unsigned>(pi.vulcan_ammo)));
		else
			adv(std::snprintf(buf + n, size - n, ",%s", primary_names[i]));
	}
	adv(std::snprintf(buf + n, size - n, " sec="));
	bool any{false};
	for (unsigned i = 0; i < MAX_SECONDARY_WEAPONS && i < secondary_names.size(); ++i)
		if (const unsigned a{pi.secondary_ammo[static_cast<secondary_weapon_index>(i)]})
		{
			adv(std::snprintf(buf + n, size - n, "%s%s%u", any ? "," : "", secondary_names[i], a));
			any = true;
		}
	if (!any)
		adv(std::snprintf(buf + n, size - n, "-"));
}

/* Section 4.5: the primary for the range to the target (none: no
 * target), from the weapon table (bot_goals.h); below weapon smarts 2
 * the fixed order of stage B1.  A switch costs the rearm time, as
 * select_primary_weapon's does.  No HUD, no sound.
 */
void choose_weapon(const bot_state &bs, object &obj, const std::optional<b::range_band> band)
{
	auto &pi{obj.ctype.player_info};
	/* Section 9.5: a charging fusion cannon is fired first. */
	if (bs.fusion_charging)
		return;
	const auto active{pi.Primary_weapon.get_active()};
	const auto current{static_cast<b::primary>(underlying_value(active))};
	const auto wanted{bs.skill.weapon_smarts < 2
		? b::choose_primary({
			.owned = pi.primary_weapon_flags,
			.energy = pi.energy / 65536.0,
			.vulcan_ammo = pi.vulcan_ammo,
		})
		: b::choose_primary_for(weapons_of(pi), band, current)};
	const auto w{static_cast<primary_weapon_index>(underlying_value(wanted))};
	if (active == w)
		return;
	if (bot_log_on())
	{
		const auto v{weapons_of(pi)};
		const auto rb{band.value_or(b::range_band::mid)};
		con_printf(CON_VERBOSE, "bots: '%s' switches %s -> %s (band %u, scores %.2f -> %.2f, energy %.0f)", static_cast<const char *>(bs.cfg.name), primary_name(underlying_value(active)), primary_name(underlying_value(w)), static_cast<unsigned>(rb), b::weapon_score(current, rb, v), b::weapon_score(wanted, rb, v), pi.energy / 65536.0);
	}
	pi.Primary_weapon = w;
	pi.Next_laser_fire_time = GameTime64 + REARM_TIME;
}

/* Section 4.7: what the bot has, for the collection values. */
[[nodiscard]]
b::resource_view resources_of(const object &obj)
{
	const auto &pi{obj.ctype.player_info};
	b::resource_view r;
	r.shields = obj.shields / 65536.0;
	r.energy = pi.energy / 65536.0;
	r.weapons = weapons_of(pi);
	r.ammo_rack = has_flag(pi, player_flag::ammo_rack);
	const double ammo_max{static_cast<double>(VULCAN_AMMO_MAX) * (r.ammo_rack ? 2 : 1)};
	r.vulcan_ammo_share = pi.vulcan_ammo / ammo_max;
	r.afterburner = has_flag(pi, player_flag::afterburner);
	r.converter = has_flag(pi, player_flag::converter);
	r.cloaked = has_flag(pi, player_flag::cloaked);
	r.invulnerable = has_flag(pi, player_flag::invulnerable) && !pi.FakingInvul;
	return r;
}

/* Section 4.7: what a powerup is to a bot. */
[[nodiscard]]
b::item_desc item_of(const powerup_type_t id)
{
	using b::item;
	const auto primary{[](const b::primary p) {
		return b::item_desc{item::primary, p, 0};
	}};
	const auto missile{[](const secondary_weapon_index w) {
		return b::item_desc{item::secondary, b::primary::laser, underlying_value(w)};
	}};
	switch (id)
	{
		case powerup_type_t::POW_ENERGY:
			return {item::energy};
		case powerup_type_t::POW_SHIELD_BOOST:
			return {item::shield};
		case powerup_type_t::POW_LASER:
			return {item::laser};
		case powerup_type_t::POW_QUAD_FIRE:
			return {item::quad};
		case powerup_type_t::POW_VULCAN_WEAPON:
			return primary(b::primary::vulcan);
		case powerup_type_t::POW_SPREADFIRE_WEAPON:
			return primary(b::primary::spreadfire);
		case powerup_type_t::POW_PLASMA_WEAPON:
			return primary(b::primary::plasma);
		case powerup_type_t::POW_FUSION_WEAPON:
			return primary(b::primary::fusion);
		case powerup_type_t::POW_VULCAN_AMMO:
			return {item::vulcan_ammo};
		case powerup_type_t::POW_MISSILE_1:
		case powerup_type_t::POW_MISSILE_4:
			return missile(secondary_weapon_index::concussion);
		case powerup_type_t::POW_HOMING_AMMO_1:
		case powerup_type_t::POW_HOMING_AMMO_4:
			return missile(secondary_weapon_index::homing);
		case powerup_type_t::POW_PROXIMITY_WEAPON:
			return missile(secondary_weapon_index::proximity);
		case powerup_type_t::POW_SMARTBOMB_WEAPON:
			return missile(secondary_weapon_index::smart);
		case powerup_type_t::POW_MEGA_WEAPON:
			return missile(secondary_weapon_index::mega);
		case powerup_type_t::POW_CLOAK:
			return {item::cloak};
		case powerup_type_t::POW_INVULNERABILITY:
			return {item::invulnerability};
#if DXX_BUILD_DESCENT == 2
		case powerup_type_t::POW_GAUSS_WEAPON:
			return primary(b::primary::gauss);
		case powerup_type_t::POW_HELIX_WEAPON:
			return primary(b::primary::helix);
		case powerup_type_t::POW_PHOENIX_WEAPON:
			return primary(b::primary::phoenix);
		case powerup_type_t::POW_OMEGA_WEAPON:
			return primary(b::primary::omega);
		case powerup_type_t::POW_SUPER_LASER:
			return {item::super_laser};
		case powerup_type_t::POW_FULL_MAP:
			return {item::full_map};
		case powerup_type_t::POW_CONVERTER:
			return {item::converter};
		case powerup_type_t::POW_AMMO_RACK:
			return {item::ammo_rack};
		case powerup_type_t::POW_AFTERBURNER:
			return {item::afterburner};
		case powerup_type_t::POW_HEADLIGHT:
			return {item::headlight};
		case powerup_type_t::POW_SMISSILE1_1:
		case powerup_type_t::POW_SMISSILE1_4:
			return missile(secondary_weapon_index::flash);
		case powerup_type_t::POW_GUIDED_MISSILE_1:
		case powerup_type_t::POW_GUIDED_MISSILE_4:
			return missile(secondary_weapon_index::guided);
		case powerup_type_t::POW_SMART_MINE:
			return missile(secondary_weapon_index::smart_mine);
		case powerup_type_t::POW_MERCURY_MISSILE_1:
		case powerup_type_t::POW_MERCURY_MISSILE_4:
			return missile(secondary_weapon_index::mercury);
		case powerup_type_t::POW_EARTHSHAKER_MISSILE:
			return missile(secondary_weapon_index::earthshaker);
#endif
		default:
			return {};
	}
}

/* Plan a path from the ship's segment to `goal_seg` (section 4.3). */
void plan_path(bot_state &bs, const object &obj, const uint32_t goal_seg, const std::optional<vec3> goal_pos, const uint32_t tick)
{
	bs.clear_path();
	bs.next_plan_tick = tick + 2 * b::BOT_TICK_RATE;
	bs.last_plan_tick = tick;
	const uint32_t start{obj.segnum};
	if (start >= B.graph.size() || goal_seg >= B.graph.size())
		return;
	const auto flags{obj.ctype.player_info.powerup_flags};
	const auto passable{[flags](const uint32_t from, const b::nav_edge &e) {
		return edge_passable(from, e, flags);
	}};
	const auto extra{[&bs](const uint32_t from, const b::nav_edge &e) {
		return bs.penalties.cost(from, e.side);
	}};
	if (!B.search.find(B.graph, start, goal_seg, passable, extra, BOT_NAV_NODE_LIMIT, B.path))
		return;
	const auto &steps{B.path.steps};
	/* Section 4.3: the centre of each side passed, then the centre of
	 * the segment entered, so a path never cuts a corner.
	 */
	for (std::size_t i = 1; i < steps.size(); ++i)
	{
		const auto from{steps[i - 1].node};
		const auto side{steps[i].side};
		bs.points.push_back(B.side_centres[from * 6 + side]);
		bs.point_edges.emplace_back(from, side);
		bs.points.push_back(B.graph.position(steps[i].node));
		bs.point_edges.emplace_back(from, side);
	}
	/* The PR #47 review: the goal's place replaces the centre of its
	 * segment only if a straight line reaches it from there (from the
	 * ship, already in that segment): a place inside a wall (a
	 * predicted one) left the bot pressing against the wall.
	 */
	if (B.path.complete && goal_pos && !line_clear(obj, bs.points.empty() ? obj.pos : to_fixvec(B.graph.position(goal_seg)), bs.points.empty() ? obj.segnum : static_cast<segnum_t>(goal_seg), to_fixvec(*goal_pos), 0, false))
	{
		if (bot_log_on())
			con_printf(CON_VERBOSE, "bots: '%s' goal place in segment %u out of reach: its centre instead", static_cast<const char *>(bs.cfg.name), goal_seg);
		if (bs.points.empty())
		{
			bs.points.push_back(B.graph.position(goal_seg));
			bs.point_edges.emplace_back(start, b::NAV_NO_SIDE);
		}
	}
	else if (B.path.complete && goal_pos)
	{
		if (bs.points.empty())
		{
			bs.points.push_back(*goal_pos);
			bs.point_edges.emplace_back(start, b::NAV_NO_SIDE);
		}
		else
			bs.points.back() = *goal_pos;
	}
	bs.stuck.restart_window();
	if (bot_arena_active() && !bs.points.empty())
		bot_arena_note_path(bs.pid, b::remaining_length(bs.points, 0, to_vec(obj.pos)));
}

void set_goal(bot_state &bs, const object &obj, const bot_goal g, const uint32_t seg, const std::optional<vec3> pos, const uint32_t tick)
{
	if (bs.goal != g || bs.goal_seg != seg)
	{
		bs.stuck.reset();
		bs.penalties.clear();
	}
	bs.goal = g;
	bs.goal_seg = seg;
	plan_path(bs, obj, seg, pos, tick);
}

void choose_roam_goal(bot_state &bs, const object &obj, const uint32_t tick)
{
	if (B.graph.size() < 2)
		return;
	const auto below{[&bs](const uint32_t n) {
		return bs.rng.below(n);
	}};
	/* Section 9.5: far and long unvisited places (bot_nav.h), within
	 * reach of the path costs of this strategy tick.
	 */
	const auto seconds_since{[&bs, tick](const uint32_t seg) {
		return seg < bs.visited.size() && bs.visited[seg] ? (tick - (bs.visited[seg] - 1)) / static_cast<double>(b::BOT_TICK_RATE) : 1e9;
	}};
	const auto explore{b::pick_explore_goal(B.graph, obj.segnum, below, [&bs](const uint32_t seg) {
		return bs.dist.cost(seg);
	}, seconds_since)};
	/* Else somewhere else, not next door (B1). */
	const uint32_t seg{explore ? *explore : b::pick_roam_goal(B.graph, obj.segnum, to_vec(obj.pos), BOT_ROAM_MIN_DISTANCE, below)};
	if (bot_log_on())
	{
		const auto cost{bs.dist.cost(seg)};
		const double since{seconds_since(seg)};
		con_printf(CON_VERBOSE, "bots: '%s' roams from segment %u to %u (%s, path %.0f, last there %s%.0f s ago)", static_cast<const char *>(bs.cfg.name), static_cast<unsigned>(obj.segnum), seg, explore ? "explore" : "random", cost ? *cost : -1.0, since > 1e8 ? "never, " : "", since > 1e8 ? 0.0 : since);
	}
	set_goal(bs, obj, bot_goal::roam, seg, std::nullopt, tick);
}

/* Section 4.2: what the bot sees, at 20 Hz. */
/* Section 9.6: the heavy missiles an enemy holds are known to a bot only
 * as a human knows them: it saw the enemy fire one (it may hold more),
 * or pick one up (a heavy powerup next to the enemy in sight is gone at
 * the next look).  b::heavy_holding counts them and forgets the enemy's
 * last known one fired; knowledge ends with the enemy's death (perceive
 * clears it with the memory).
 */
constexpr double BOT_PICKUP_NOTICE{14};

#if DXX_BUILD_DESCENT == 2
[[nodiscard]]
bool heavy_weapon_id(const weapon_id_type id)
{
	return id == weapon_id_type::MEGA_ID || id == weapon_id_type::EARTHSHAKER_ID;
}

[[nodiscard]]
bool heavy_powerup_id(const powerup_type_t id)
{
	return id == powerup_type_t::POW_MEGA_WEAPON || id == powerup_type_t::POW_EARTHSHAKER_MISSILE;
}
#endif

void notice_heavy_holders(bot_state &bs, const object &obj, const uint32_t tick)
{
#if DXX_BUILD_DESCENT == 2
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto pos{to_vec(obj.pos)};
	/* A pickup: the powerup seen next to the enemy is gone. */
	for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
	{
		const auto k{bs.heavy_near_obj[i]};
		if (k == 0xffff)
			continue;
		bs.heavy_near_obj[i] = 0xffff;
		if (!bs.memory[i].valid || k > Highest_object_index)
			continue;
		const auto &o{*Objects.vcptr(objnum_t{k})};
		const bool still{o.type == object_type::OBJ_POWERUP && underlying_value(o.signature) == bs.heavy_near_sig[i] && !(o.flags & OF_SHOULD_BE_DEAD)};
		if (!still && bs.visible_now[i])
		{
			bs.heavy_holding[i] = b::heavy_picked_up(bs.heavy_holding[i], tick);
			if (bot_log_on())
				con_printf(CON_VERBOSE, "bots: '%s' saw P#%u pick up a heavy missile (%u known)", static_cast<const char *>(bs.cfg.name), i, bs.heavy_holding[i].count);
		}
	}
	const double awareness{bs.skill.awareness};
	for (auto &&o : Objects.vcptridx)
	{
		if (o->type == object_type::OBJ_WEAPON && heavy_weapon_id(get_weapon_id(o)))
		{
			/* A heavy missile in flight from an enemy in sight range. */
			const auto &li{o->ctype.laser_info};
			if (li.parent_type != object_type::OBJ_PLAYER)
				continue;
			const auto &parent{*Objects.vcptr(li.parent_num)};
			if (parent.type != object_type::OBJ_PLAYER || !laser_parent_is_matching_signature(li, parent))
				continue;
			const auto who{get_player_id(parent)};
			/* Each shot counted once (its signature). */
			const uint16_t sig{underlying_value(o->signature)};
			if (who == bs.pid || who >= MAX_PLAYERS || same_team(bs.pid, who) || bs.heavy_fire_sig[who] == sig)
				continue;
			if (b::distance(pos, to_vec(o->pos)) > awareness || !line_clear(obj, obj.pos, obj.segnum, o->pos, 0, true))
				continue;
			bs.heavy_fire_sig[who] = sig;
			bs.heavy_holding[who] = b::heavy_fired(bs.heavy_holding[who], tick);
			if (bot_log_on())
				con_printf(CON_VERBOSE, "bots: '%s' saw P#%u fire a heavy missile (%u more known%s)", static_cast<const char *>(bs.cfg.name), who, bs.heavy_holding[who].count, bs.heavy_holding[who].held(tick) ? "" : ", forgotten");
		}
		else if (o->type == object_type::OBJ_POWERUP && heavy_powerup_id(get_powerup_id(o)))
		{
			const auto ppos{to_vec(o->pos)};
			for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
				if (bs.visible_now[i] && b::distance(bs.memory[i].pos, ppos) < BOT_PICKUP_NOTICE)
				{
					bs.heavy_near_obj[i] = o.get_unchecked_index();
					bs.heavy_near_sig[i] = underlying_value(o->signature);
				}
		}
	}
#else
	(void)bs;
	(void)obj;
	(void)tick;
#endif
}

[[nodiscard]]
double wall_distance(const object &obj, const vec3 &dir, double limit);

void perceive(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{bs.skill};
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	/* Section 9.5: where the bot has been (exploring). */
	if (const uint32_t seg{obj.segnum}; seg < bs.visited.size())
		bs.visited[seg] = tick + 1;
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		bs.visible_now[i] = false;
		if (i == bs.pid)
			continue;
		auto &plr{*vcplayerptr(i)};
		if (plr.connected != player_connection_status::playing)
		{
			bs.memory[i] = {};
			continue;
		}
		const auto &t{*Objects.vcptr(plr.objnum)};
		const bool dying{i == Player_num ? Player_dead_state != player_dead_state::no : bot_ship_dying(i)};
		if (t.type != object_type::OBJ_PLAYER || dying)
		{
			bs.memory[i] = {};
			/* Section 9.6: its heavy missiles are dropped. */
			bs.heavy_holding[i] = {};
			continue;
		}
		if (same_team(bs.pid, i))
			continue;
		const auto tpos{to_vec(t.pos)};
		const auto to{tpos - pos};
		const double dist{b::length(to)};
		const bool revenge{bs.last_attacker == i && tick - bs.attacked_tick < BOT_REVENGE_TICKS};
		if (dist > sk.awareness && !revenge)
			continue;
		if (!revenge && !b::in_field_of_view(frame.f, to, sk.fov_half_deg))
			continue;
		if (+(t.ctype.player_info.powerup_flags & player_flag::cloaked) && dist > BOT_CLOAK_SEE_DISTANCE)
			continue;
		if (!line_clear(obj, obj.pos, obj.segnum, t.pos, 0, true))
			continue;
		bs.visible_now[i] = true;
		bs.memory[i] = {
			.valid = true,
			.pos = tpos,
			.vel = to_vec(t.mtype.phys_info.velocity),
			.segment = static_cast<uint16_t>(t.segnum),
			.tick = tick,
		};
	}
	percept p;
	bs.shot_clear = false;
	bs.shot_first_hit = 1e9;
	if (bs.target)
	{
		const auto t{*bs.target};
		p.target = t;
		p.visible = bs.visible_now[t];
		p.pos = bs.memory[t].pos;
		p.vel = bs.memory[t].vel;
		if (p.visible)
			bs.shot_clear = shot_line_clear(bs, obj, *Objects.vcptr(vcplayerptr(t)->objnum), bs.shot_first_hit);
	}
	bs.seen.push(p);
	/* Section 4.3: string pulling; section 9.15: far along the path, at
	 * most b::PULL_PROBES probes.
	 */
	if (!bs.points.empty())
	{
		const fix rad{obj.size * 2 / 3};
		bs.steer_index = b::pull_string_ahead(bs.point_index, bs.steer_index, bs.points.size(), [&](const std::size_t k) {
			return line_clear(obj, obj.pos, obj.segnum, to_fixvec(bs.points[k]), rad, false);
		});
	}
	/* Section 4.3: wall avoidance, a probe along the velocity.  It takes
	 * away the speed into the wall, the more the nearer the wall, and
	 * pushes off a little.  A wall beyond the path point the bot steers
	 * at is the bend it is about to take, not an obstacle: B1 braked for
	 * every bend of a corridor, which made the bots crawl.
	 */
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const double speed{b::length(vel)};
	if (speed > 5)
	{
		/* Section 9.15: fighting with keys, only a wall near enough to
		 * matter (it fired all through the fights of exp-31).
		 */
		const double probe_time{bs.mode == b::move_mode::fight_keys ? 0.25 : 0.4};
		const auto end{to_fixvec(pos + vel * probe_time)};
		fvi_info hit;
		const auto type{find_vector_intersection(fvi_query{
			obj.pos,
			end,
			fvi_query::unused_ignore_obj_list,
			fvi_query::unused_LevelUniqueObjectState,
			fvi_query::unused_Robot_info,
			0,
			Objects.vcptridx(&obj),
		}, obj.segnum, obj.size / 2, hit)};
		if (type == fvi_hit_type::Wall)
		{
			const double hit_distance{b::distance(pos, to_vec(hit.hit_pnt))};
			/* Fighting in the open, the bot does not follow its path.
			 * Section 9.15: what it flies is the movement of its last
			 * tick (the fight's keys, the slide, the turn round): the
			 * path flown with keys in a fight bends with its path.
			 */
			const bool following{b::follows_path(bs.mode)};
			const bool bend{following && bs.steer_index < bs.points.size() && b::wall_hit_is_bend(pos, vel, bs.points[bs.steer_index], hit_distance, BOT_BEND_MAX_ANGLE)};
			const auto normal{b::normalized(to_vec(hit.hit_wallnorm))};
			const double into{-b::dot(vel, normal)};
			if (!bend && into > 0)
			{
				const double nearness{1 - std::clamp(hit_distance / (speed * probe_time), 0.0, 1.0)};
				bs.avoid = normal * (into * (0.5 + nearness) + B.limits.max_speed * 0.1);
				bs.avoid_until = tick + b::PERCEPTION_DIVISOR * 2;
			}
		}
	}
	/* Section 9.15: the room along the ship's axes, for the juke of the
	 * fight's keys (b::juke_turn_from_walls, b::keys_off_walls).
	 */
	if (bs.mode == b::move_mode::fight_keys)
		bs.room = {
			.left = wall_distance(obj, -frame.r, b::JUKE_WALL_ROOM),
			.right = wall_distance(obj, frame.r, b::JUKE_WALL_ROOM),
			.down = wall_distance(obj, -frame.u, b::JUKE_WALL_ROOM),
			.up = wall_distance(obj, frame.u, b::JUKE_WALL_ROOM),
			.back = wall_distance(obj, -frame.f, b::JUKE_WALL_ROOM),
		};
	else
		bs.room = {};
	/* Section 4.6, dodge: each projectile coming at the bot gets one roll
	 * (b::dodge_roll); with the skill's probability the bot thrusts across
	 * its flight for a moment, a reaction time later.  The bot's own shots
	 * and, without friendly fire, its partners' are not dodged.
	 */
	const double dodge_prob{b::effective_dodge(bs.skill, bs.style)};
	if (dodge_prob > 0 && tick >= bs.dodge_until)
	{
		const double radius{obj.size / 65536.0 + 3};
		const auto frame{to_frame(obj.orient)};
		const auto own_objnum{vcplayerptr(bs.pid)->objnum};
		const bool coop = +(Game_mode & GM_MULTI_COOP);
		const bool friendly_fire{!Netgame.NoFriendlyFire};
		for (const object &o : Objects.vcptr)
		{
			if (o.type != object_type::OBJ_WEAPON)
				continue;
			const auto &li{o.ctype.laser_info};
			bool own{false}, from_partner{false};
			if (li.parent_type == object_type::OBJ_PLAYER)
			{
				own = li.parent_num == own_objnum;
				const auto &parent{*Objects.vcptr(li.parent_num)};
				if (!own && parent.type == object_type::OBJ_PLAYER && laser_parent_is_matching_signature(li, parent))
				{
					const auto shooter{get_player_id(parent)};
					from_partner = coop || same_team(bs.pid, shooter);
				}
			}
			if (!b::shot_worth_dodging(own, from_partner, friendly_fire))
				continue;
			const auto rel{to_vec(o.pos) - pos};
			if (b::length(rel) > BOT_DODGE_SCAN)
				continue;
			/* Stage B4: a homing missile after this bot turns with it;
			 * it is judged with a wider pass and dodged more often.
			 */
			const bool homing_at_me{Weapon_info[get_weapon_id(o)].homing_flag && li.track_goal == own_objnum};
			const auto away{b::dodge_direction(rel, to_vec(o.mtype.phys_info.velocity) - vel, BOT_DODGE_HORIZON, b::dodge_radius(radius, homing_at_me), frame.r)};
			if (!away)
				continue;
			if (b::dodge_roll(bs.dodge_salt, static_cast<uint16_t>(o.signature)) >= b::dodge_chance(dodge_prob, homing_at_me))
				continue;
			bs.dodge_dir = *away;
			bs.dodge_from = tick + b::ticks_from_ms(bs.skill.reaction_ms) / 2;
			bs.dodge_until = bs.dodge_from + BOT_DODGE_TICKS;
			break;
		}
	}
	/* Section 9.8, the death dump: the damage of the enemy shots on a
	 * course that meets the bot within the dodge's horizon (a hurt bot
	 * only: the scan is not free).
	 */
	bs.incoming_damage = 0;
	if (obj.shields <= i2f(BOT_DUMP_SCAN_SHIELDS))
	{
		const double radius{obj.size / 65536.0 + 2};
		const auto own_objnum{vcplayerptr(bs.pid)->objnum};
		const bool coop = +(Game_mode & GM_MULTI_COOP);
		const bool friendly_fire{!Netgame.NoFriendlyFire};
		for (const object &o : Objects.vcptr)
		{
			if (o.type != object_type::OBJ_WEAPON)
				continue;
			const auto &li{o.ctype.laser_info};
			if (li.parent_type != object_type::OBJ_PLAYER || li.parent_num == own_objnum)
				continue;
			const auto &parent{*Objects.vcptr(li.parent_num)};
			const bool from_partner{parent.type == object_type::OBJ_PLAYER && laser_parent_is_matching_signature(li, parent) && (coop || same_team(bs.pid, get_player_id(parent)))};
			if (!b::shot_worth_dodging(false, from_partner, friendly_fire))
				continue;
			const auto rel{to_vec(o.pos) - pos};
			if (b::length(rel) > BOT_DODGE_SCAN)
				continue;
			const auto &wi{Weapon_info[get_weapon_id(o)]};
			const bool homing_at_me{wi.homing_flag && li.track_goal == own_objnum};
			if (!b::dodge_direction(rel, to_vec(o.mtype.phys_info.velocity) - to_vec(obj.mtype.phys_info.velocity), BOT_DODGE_HORIZON, b::dodge_radius(radius, homing_at_me), frame.r))
				continue;
			bs.incoming_damage += wi.strength[GameUniqueState.Difficulty_level] / 65536.0;
		}
	}
	/* Section 9.6: who holds a heavy missile. */
	notice_heavy_holders(bs, obj, tick);
	/* The host's copy of the bot's inventory, and the clients' (the
	 * energy and ammunition it spent).
	 */
	net_objects_host_own_ship_inventory(bs.pid, false);
}

/* Section 4.7: the path costs from where the bot is to everything within
 * reach, with the passability and penalties of its paths.
 */
void compute_distances(bot_state &bs, const object &obj)
{
	const auto flags{obj.ctype.player_info.powerup_flags};
	bs.dist.compute(B.graph, obj.segnum, [flags](const uint32_t from, const b::nav_edge &e) {
		return edge_passable(from, e, flags);
	}, [&bs](const uint32_t from, const b::nav_edge &e) {
		return bs.penalties.cost(from, e.side);
	}, BOT_DISTANCE_MAX_COST, BOT_DISTANCE_NODE_LIMIT);
}

/* The live powerup a memory entry names, if it is still there. */
[[nodiscard]]
const object *live_powerup(const b::known_powerup &k)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	if (k.key > Highest_object_index)
		return nullptr;
	const auto &o{*Objects.vcptr(objnum_t{k.key})};
	if (o.type != object_type::OBJ_POWERUP || underlying_value(o.signature) != k.signature || (o.flags & OF_SHOULD_BE_DEAD))
		return nullptr;
	return &o;
}

/* Section 4.7 and decision 4 of section 11: what the bot learns of the
 * powerups at this strategy tick.  It knows the level's initial layout
 * within its skill's map knowledge (path segments from where it is),
 * sees powerups (awareness, field of view, line of sight; a few checks
 * per tick) and hears a new one appear within its hearing radius (a
 * respawn, a death's drop).  A remembered powerup whose place it sees
 * empty, or comes to, is forgotten; so is one remembered too long.
 */
void learn_powerups(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{bs.skill};
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	unsigned budget{BOT_POWERUP_LOS_BUDGET};
	/* Section 9.5: close by, also outside the field of view. */
	const auto sees{[&](const vms_vector &where, const vec3 &to) {
		if (!budget || !b::notices_powerup(b::length(to), b::in_field_of_view(frame.f, to, sk.fov_half_deg), sk.awareness))
			return false;
		--budget;
		return line_clear(obj, obj.pos, obj.segnum, where, 0, true);
	}};
	/* First the places it remembers: still there? */
	std::array<uint16_t, 128> gone{};
	std::size_t n_gone{0};
	for (const auto &k : bs.powerups.items())
	{
		if (const auto o{live_powerup(k)})
		{
			/* It rolled (a drop settling): the bot follows it. */
			if (o->pos != to_fixvec(k.pos))
			{
				auto u{k};
				u.pos = to_vec(o->pos);
				u.segment = o->segnum;
				u.count = static_cast<uint32_t>(std::max(o->ctype.powerup_info.count, 0));
				bs.powerups.learn(u, k.learned_tick);
			}
			continue;
		}
		const auto to{k.pos - pos};
		if ((b::length(to) < 15 || sees(to_fixvec(k.pos), to)) && n_gone < gone.size())
			gone[n_gone++] = k.key;
	}
	for (std::size_t i = 0; i < n_gone; ++i)
		bs.powerups.forget(gone[i]);
	bs.powerups.expire(tick, b::ticks_from_ms(b::powerup_memory_ms(sk)));
	/* Then the powerups it does not know yet.  Those it must see are
	 * checked nearest first (section 9.5): B3 checked them in the order
	 * of the object slots, and the level's powerups behind walls used
	 * up the checks at every tick.
	 */
	struct unseen
	{
		double dist;
		objnum_t objnum;
	};
	std::array<unseen, 64> to_see;
	std::size_t n_to_see{0};
	const auto learn_one{[&](const vcobjptridx_t o, const bool initial, const bool perceived) {
		const uint16_t key = o.get_unchecked_index();
		const uint16_t sig = underlying_value(o->signature);
		bs.powerups.learn({
			.key = key,
			.signature = sig,
			.type = static_cast<uint8_t>(get_powerup_id(o)),
			.count = static_cast<uint32_t>(std::max(o->ctype.powerup_info.count, 0)),
			.initial = initial,
			.pos = to_vec(o->pos),
			.segment = o->segnum,
		}, tick, perceived);
	}};
	for (const auto &&o : Objects.vcptridx)
	{
		if (o->type != object_type::OBJ_POWERUP || (o->flags & OF_SHOULD_BE_DEAD))
			continue;
		const uint16_t key = o.get_unchecked_index();
		const uint16_t sig = underlying_value(o->signature);
		if (bs.powerups.knows(key, sig))
			continue;
		if (item_of(get_powerup_id(o)).kind == b::item::none)
			continue;
		const auto netid{net_objects_netid_of(o)};
		const bool initial{netid != 0xffff && ::dcx::net_v2::is_level_netid(netid)};
		const auto ppos{to_vec(o->pos)};
		const auto to{ppos - pos};
		const double dist{b::length(to)};
		const bool from_map{b::knows_from_map(initial, bs.dist.hops(o->segnum), sk.map_knowledge, bs.dist.cost(o->segnum))};
		/* Only a perceived powerup may push another out of a full
		 * memory (powerup_memory::learn); the map alone need not be
		 * checked further while there is room.
		 */
		if (from_map && !bs.powerups.full())
		{
			learn_one(o, initial, false);
			continue;
		}
		if (!initial)
		{
			const double age{(GameTime64 - o->ctype.powerup_info.creation_time) / 65536.0};
			if (b::hears_appearance(age, dist, sk.hearing))
			{
				learn_one(o, initial, true);
				continue;
			}
		}
		if (!b::notices_powerup(dist, b::in_field_of_view(frame.f, to, sk.fov_half_deg), sk.awareness))
		{
			if (from_map)
				learn_one(o, initial, false);
			continue;
		}
		const objnum_t objnum{o.get_unchecked_index()};
		if (n_to_see < to_see.size())
			to_see[n_to_see++] = {dist, objnum};
		else
		{
			/* Keep the nearest. */
			auto furthest{std::ranges::max_element(to_see, {}, &unseen::dist)};
			if (furthest->dist > dist)
				*furthest = {dist, objnum};
		}
	}
	std::sort(to_see.begin(), to_see.begin() + n_to_see, [](const unseen &a, const unseen &c) {
		return a.dist < c.dist;
	});
	for (std::size_t i = 0; i < n_to_see; ++i)
	{
		const auto &&o{Objects.vcptridx(to_see[i].objnum)};
		const auto netid{net_objects_netid_of(o)};
		const bool initial{netid != 0xffff && ::dcx::net_v2::is_level_netid(netid)};
		const bool seen{sees(o->pos, to_vec(o->pos) - pos)};
		if (seen || b::knows_from_map(initial, bs.dist.hops(o->segnum), sk.map_knowledge, bs.dist.cost(o->segnum)))
			learn_one(o, initial, seen);
	}
}

/* A goal place: a powerup or a centre. */
struct goal_place
{
	double utility{};
	double path{};
	uint32_t segment{};
	vec3 pos;
	uint16_t key{0xffff}, sig{};
	bool shields{};
	/* A much stronger armament (b::BIG_UPGRADE_RATIO). */
	bool upgrade{};
	/* Section 9.8: its value (b::item_value), and whether it is
	 * invulnerability.
	 */
	double value{};
	bool invulnerability{};
};

/* Section 4.7: the best powerup to collect, by value (its need) over the
 * path cost.  Only what the rules let the bot take (as a human's "already
 * have") and what it can reach.
 */
[[nodiscard]]
goal_place best_collect(const bot_state &bs, const b::resource_view &res, const uint32_t tick, const bool shields_only = false)
{
	goal_place best;
	for (const auto &k : bs.powerups.items())
	{
		if (tick < k.ignore_until)
			continue;
		const auto type{static_cast<powerup_type_t>(k.type)};
		const auto desc{item_of(type)};
		if (shields_only && desc.kind != b::item::shield)
			continue;
		const auto cost{bs.dist.cost(k.segment)};
		if (!cost)
			continue;
		const double value{b::item_value(desc, res)};
		if (value <= 0)
			continue;
		if (!net_objects_bot_can_use(bs.pid, type, k.count))
			continue;
		const double path{*cost + b::distance(B.graph.position(k.segment), k.pos)};
		const double u{b::collect_utility(value, path)};
		if (u > best.utility)
			best = {u, path, k.segment, k.pos, k.key, k.signature, desc.kind == b::item::shield, b::collect_in_fight(desc, res)};
	}
	return best;
}

/* Section 9.5: the valuable powerup close by that the bot takes whatever
 * its goal (b::grab_worthwhile): the most valuable for the distance.
 */
[[nodiscard]]
goal_place best_grab(const bot_state &bs, const object &obj, const b::resource_view &res, const uint32_t tick)
{
	goal_place best;
	double best_score{0};
	const auto pos{to_vec(obj.pos)};
	const uint32_t own_seg{obj.segnum};
	for (const auto &k : bs.powerups.items())
	{
		if (tick < k.ignore_until)
			continue;
		const double straight{b::distance(pos, k.pos)};
		if (!b::grab_in_range(straight))
			continue;
		const auto cost{bs.dist.cost(k.segment)};
		if (!cost)
			continue;
		/* The path cost runs between segment centres: in the bot's own
		 * segment, the straight line.
		 */
		const double path{k.segment == own_seg ? straight : std::min(*cost + b::distance(B.graph.position(k.segment), k.pos), *cost + straight)};
		const auto type{static_cast<powerup_type_t>(k.type)};
		const auto desc{item_of(type)};
		const double value{b::item_value(desc, res)};
		if (!b::grab_candidate(value, straight, path))
			continue;
		if (!net_objects_bot_can_use(bs.pid, type, k.count))
			continue;
		/* Section 9.9: the nearest reasonable one first. */
		const double score{b::grab_rank(value, path)};
		if (score > best_score)
		{
			best_score = score;
			best = {b::collect_utility(value, path), path, k.segment, k.pos, k.key, k.signature, desc.kind == b::item::shield, true, value, desc.kind == b::item::invulnerability};
		}
	}
	return best;
}

/* Section 9.8: the best weapon upgrade the bot knows and can reach, for
 * the power-up phase (b::PHASE_MIN_UPGRADE within b::PHASE_MAX_PATH):
 * the laser levels, super laser, quad and the guns that make its
 * armament stronger.
 */
[[nodiscard]]
goal_place best_upgrade(const bot_state &bs, const b::resource_view &res, const uint32_t tick)
{
	goal_place best;
	for (const auto &k : bs.powerups.items())
	{
		if (tick < k.ignore_until)
			continue;
		const auto type{static_cast<powerup_type_t>(k.type)};
		const auto desc{item_of(type)};
		if (!b::is_weapon_item(desc.kind) || b::upgrade_ratio(desc, res.weapons) < b::PHASE_MIN_UPGRADE)
			continue;
		const auto cost{bs.dist.cost(k.segment)};
		if (!cost)
			continue;
		const double path{*cost + b::distance(B.graph.position(k.segment), k.pos)};
		if (path > b::PHASE_MAX_PATH)
			continue;
		if (!net_objects_bot_can_use(bs.pid, type, k.count))
			continue;
		const double value{b::item_value(desc, res)};
		const double u{b::collect_utility(value, path)};
		if (u > best.utility)
			best = {u, path, k.segment, k.pos, k.key, k.signature, false, true, value, false};
	}
	return best;
}

/* Section 9.14: the power pickups in sight (b::power_class_of): in the
 * field of view with a line of sight, at any distance (a human sees an
 * earthshaker anywhere on the screen).  One the bot does not know yet is
 * learned; one it remembers whose place it sees empty (someone took it)
 * is forgotten.  At most BOT_POWER_LOS_BUDGET lines of sight per
 * strategy tick: the nearest, then the others in turn.
 */
constexpr std::size_t BOT_POWER_LOS_BUDGET{4};
constexpr std::size_t BOT_POWER_LOS_NEAREST{2};

void sight_power_powerups(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	const double fov{bs.skill.fov_half_deg};
	struct candidate
	{
		double dist;
		objnum_t objnum;
		/* A remembered entry whose powerup is gone: its key and
		 * signature.
		 */
		uint16_t gone_key, gone_sig;
		vec3 place;
	};
	std::array<candidate, 16> cand;
	std::size_t n{0};
	const auto add{[&](const candidate &c) {
		if (n < cand.size())
			cand[n++] = c;
		else if (auto furthest{std::ranges::max_element(cand, {}, &candidate::dist)}; furthest->dist > c.dist)
			*furthest = c;
	}};
	for (const auto &k : bs.powerups.items())
	{
		if (b::power_class_of(item_of(static_cast<powerup_type_t>(k.type))) == b::power_class::none)
			continue;
		const auto to{k.pos - pos};
		if (!b::in_field_of_view(frame.f, to, fov))
			continue;
		if (live_powerup(k))
			add({b::length(to), objnum_t{k.key}, 0xffff, 0, k.pos});
		else
			add({b::length(to), objnum_t{}, k.key, k.signature, k.pos});
	}
	for (const auto &&o : Objects.vcptridx)
	{
		if (o->type != object_type::OBJ_POWERUP || (o->flags & OF_SHOULD_BE_DEAD))
			continue;
		if (b::power_class_of(item_of(get_powerup_id(o))) == b::power_class::none)
			continue;
		if (bs.powerups.knows(o.get_unchecked_index(), underlying_value(o->signature)))
			continue;
		const auto ppos{to_vec(o->pos)};
		const auto to{ppos - pos};
		if (!b::in_field_of_view(frame.f, to, fov))
			continue;
		add({b::length(to), o.get_unchecked_index(), 0xffff, 0, ppos});
	}
	std::sort(cand.begin(), cand.begin() + n, [](const candidate &a, const candidate &c) {
		return a.dist < c.dist;
	});
	/* The nearest BOT_POWER_LOS_NEAREST every time, the others in turn
	 * (the review of PR #71: a near one behind a wall, checked first at
	 * every tick, would starve a far one in plain sight, as B3's checks
	 * did, section 9.5).
	 */
	std::array<std::size_t, BOT_POWER_LOS_BUDGET> order;
	std::size_t checks{0};
	for (; checks < n && checks < BOT_POWER_LOS_NEAREST; ++checks)
		order[checks] = checks;
	if (n > BOT_POWER_LOS_NEAREST)
	{
		const std::size_t rest{n - BOT_POWER_LOS_NEAREST};
		const std::size_t turns{std::min<std::size_t>(rest, BOT_POWER_LOS_BUDGET - BOT_POWER_LOS_NEAREST)};
		for (std::size_t j = 0; j < turns; ++j)
			order[checks++] = BOT_POWER_LOS_NEAREST + (bs.power_sight_turn + j) % rest;
		bs.power_sight_turn = static_cast<uint8_t>((bs.power_sight_turn + turns) % rest);
	}
	for (std::size_t i = 0; i < checks; ++i)
	{
		const auto &c{cand[order[i]]};
		if (!line_clear(obj, obj.pos, obj.segnum, to_fixvec(c.place), 0, true))
			continue;
		if (c.gone_key != 0xffff)
		{
			/* Its place in sight, empty: someone took it. */
			bs.powerups.forget(c.gone_key, c.gone_sig);
			continue;
		}
		const auto &&o{Objects.vcptridx(c.objnum)};
		const uint16_t key = o.get_unchecked_index();
		const uint16_t sig = underlying_value(o->signature);
		if (!bs.powerups.knows(key, sig))
		{
			const auto netid{net_objects_netid_of(o)};
			bs.powerups.learn({
				.key = key,
				.signature = sig,
				.type = static_cast<uint8_t>(get_powerup_id(o)),
				.count = static_cast<uint32_t>(std::max(o->ctype.powerup_info.count, 0)),
				.initial = netid != 0xffff && ::dcx::net_v2::is_level_netid(netid),
				.pos = c.place,
				.segment = o->segnum,
			}, tick, true);
		}
		bs.powerups.sight(key, sig, tick);
	}
}

/* Section 9.14: the best power pickup the bot knows (b::power_pickup_value),
 * with the race against the enemies it knows of.
 */
struct power_place
{
	goal_place place;
	b::power_value value;
};

[[nodiscard]]
power_place best_power(const bot_state &bs, const object &obj, const b::resource_view &res, const uint32_t tick, const uint32_t memory_ticks)
{
	power_place best;
	double best_rank{0};
	const double weight{bs.tune.power_pickup >= 0 ? bs.tune.power_pickup : b::power_pickup_weight(bs.skill_level, bs.cfg.style)};
	if (!(weight > 0))
		return best;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto pos{to_vec(obj.pos)};
	for (const auto &k : bs.powerups.items())
	{
		if (tick < k.ignore_until)
			continue;
		const auto type{static_cast<powerup_type_t>(k.type)};
		const auto desc{item_of(type)};
		const auto cls{b::power_class_of(desc)};
		if (cls == b::power_class::none || !(b::item_value(desc, res) > 0))
			continue;
		const auto cost{bs.dist.cost(k.segment)};
		if (!cost)
			continue;
		const double straight{b::distance(pos, k.pos)};
		const double path{k.segment == obj.segnum ? straight : std::min(*cost + b::distance(B.graph.position(k.segment), k.pos), *cost + straight)};
		/* The enemies it knows of (seen or heard within its memory). */
		std::optional<double> enemy_way;
		for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
		{
			const auto &m{bs.memory[i]};
			if (i == bs.pid || !m.valid || same_team(bs.pid, i) || tick - m.tick > memory_ticks)
				continue;
			if (Objects.vcptr(vcplayerptr(i)->objnum)->type != object_type::OBJ_PLAYER)
				continue;
			const double way{b::distance(m.pos, k.pos) * b::POWER_ENEMY_PATH_FACTOR};
			if (!enemy_way || way < *enemy_way)
				enemy_way = way;
		}
		const bool going{bs.goal == bot_goal::collect && bs.power_going && bs.collect_key == k.key && bs.collect_sig == k.signature};
		const auto v{b::power_pickup_value({
			.cls = cls,
			.in_sight = b::power_in_sight(k.sighted, tick, going),
			.path = path,
			.enemy_way = enemy_way,
			.usable = net_objects_bot_can_use(bs.pid, type, k.count),
			.weight = weight,
			.going = going,
		})};
		/* The one it goes for keeps its place against another of about
		 * the same utility (the goal's hysteresis).
		 */
		const double rank{v.utility * (going ? b::GOAL_HYSTERESIS : 1)};
		if (v.utility > 0 && rank > best_rank)
		{
			best_rank = rank;
			best = {{v.utility, path, k.segment, k.pos, k.key, k.signature, false, true, b::item_value(desc, res), desc.kind == b::item::invulnerability}, v};
		}
	}
	return best;
}

/* Section 4.7: the best fuel centre (energy) or repair centre (shields). */
[[nodiscard]]
goal_place best_centre(const bot_state &bs, const b::resource_view &res, const bool repair_only = false)
{
	goal_place best;
	const auto consider{[&](const std::vector<uint32_t> &centres, const bool repair) {
		const double value{b::fuel_centre_value(repair, res)};
		if (value <= 0)
			return;
		for (const auto seg : centres)
		{
			const auto cost{bs.dist.cost(seg)};
			if (!cost)
				continue;
			const double u{b::collect_utility(value, *cost)};
			if (u > best.utility)
				best = {u, *cost, seg, B.graph.position(seg), 0xffff, 0, repair};
		}
	}};
	if (!repair_only)
		consider(B.fuel_centres, false);
	consider(B.repair_centres, true);
	return best;
}

[[nodiscard]]
bool in_centre(const object &obj, const bool repair)
{
	const shared_segment &seg{*vcsegptr(obj.segnum)};
	return seg.special == (repair ? segment_special::repaircen : segment_special::fuelcen);
}

/* Section 4.7, retreat: a shield source away from the threat (a shield
 * powerup it knows, a repair centre), else the place of a few drawn that
 * is furthest from the threat for the least flying.
 */
[[nodiscard]]
std::optional<goal_place> retreat_place(bot_state &bs, const object &obj, const b::resource_view &res, const vec3 &threat, const uint32_t tick)
{
	const auto pos{to_vec(obj.pos)};
	std::optional<goal_place> best;
	double best_u{0};
	for (const auto &p : {best_collect(bs, res, tick, true), best_centre(bs, res, true)})
	{
		if (p.utility <= 0)
			continue;
		const double u{p.utility * b::retreat_direction_factor(pos, p.pos, threat)};
		if (u > best_u)
		{
			best_u = u;
			best = p;
		}
	}
	if (best && best_u > 0.3)
		return best;
	best.reset();
	double best_flee{-1e9};
	const auto n{static_cast<unsigned>(B.graph.size())};
	for (unsigned i = 0; i < BOT_FLEE_TRIES; ++i)
	{
		const uint32_t seg{bs.rng.below(n)};
		const auto cost{bs.dist.cost(seg)};
		if (!cost || *cost < 80)
			continue;
		const auto &place{B.graph.position(seg)};
		const double f{b::flee_score(place, threat, *cost)};
		if (f > best_flee)
		{
			best_flee = f;
			best = goal_place{0, *cost, seg, place, 0xffff, 0, false};
		}
	}
	return best;
}

/* Section 9.10: pursuit.  Seconds from ticks. */
[[nodiscard]]
double tick_seconds(const uint32_t ticks)
{
	return ticks / static_cast<double>(b::BOT_TICK_RATE);
}

/* Section 9.10: what the pursuit rules (b::pursuit_start,
 * b::pursuit_stop) see of the bot and target `who`.
 */
[[nodiscard]]
b::pursuit_view pursuit_view_of(const bot_state &bs, const object &obj, const uint8_t who, const uint32_t tick, const unsigned memory_ticks)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &p{bs.pursuit};
	const auto res{resources_of(obj)};
	const double own_arms{b::armament_score(res.weapons)};
	double target_shields{100}, advantage{1};
	if (const auto &t{*Objects.vcptr(vcplayerptr(who)->objnum)}; t.type == object_type::OBJ_PLAYER)
	{
		target_shields = t.shields / 65536.0;
		advantage = b::fight_advantage(res.shields, own_arms, target_shields, b::armament_score(weapons_of(t.ctype.player_info)));
	}
	/* Stronger enemies known near where the target went. */
	unsigned stronger{0};
	const auto &m{bs.memory[who]};
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		if (i == bs.pid || i == who || !bs.memory[i].valid || same_team(bs.pid, i))
			continue;
		if (tick - bs.memory[i].tick > memory_ticks || b::distance(bs.memory[i].pos, m.pos) > b::THIRD_PARTY_DISTANCE)
			continue;
		const auto &o{*Objects.vcptr(vcplayerptr(i)->objnum)};
		if (o.type == object_type::OBJ_PLAYER && b::fight_advantage(o.shields / 65536.0, b::armament_score(weapons_of(o.ctype.player_info)), res.shields, own_arms) > b::THIRD_PARTY_STRONGER)
			++stronger;
	}
	return {
		.skill = bs.skill_level,
		.style = bs.cfg.style,
		.since_engaged = p.engaged_who == who ? tick_seconds(tick - p.engaged_tick) : 1e9,
		.since_hit = p.hit_who == who ? tick_seconds(tick - p.hit_tick) : 1e9,
		.target_shields = target_shields,
		.damage_seen = p.engaged_who == who ? std::max(0.0, p.engage_shields - target_shields) : 0,
		.advantage = advantage,
		.shields = res.shields,
		.retreat_shields = b::style_retreat_shields(bs.style, advantage),
		.invulnerable = res.invulnerable,
		.stronger_near = stronger,
		.target_heavy = bs.heavy_holding[who].held(tick),
		.seconds = bs.tune.pursuit_seconds,
	};
}

void end_pursuit(bot_state &bs, const b::pursuit_end why, const uint32_t tick)
{
	auto &p{bs.pursuit};
	if (!p.active)
		return;
	if (bot_log_on())
		con_printf(CON_VERBOSE, "bots: '%s' ends the pursuit of P#%u: %s (after %.1f s, %u predicted places reached)", static_cast<const char *>(bs.cfg.name), p.who, b::name_of(why), tick_seconds(tick - p.started), p.advances);
	p.ended.on_end(why, p.who, p.who < MAX_PLAYERS && bs.memory[p.who].valid ? bs.memory[p.who].tick : tick);
	p.active = false;
	p.who = 0xff;
	p.peek = false;
}

/* Section 9.10, at each strategy tick before the target choice: the
 * engagement (the target in sight while the bot engages or hunts it),
 * the end of a pursuit (seen again, gone, the rules of b::pursuit_stop),
 * and the start of one when the engaged target just broke the line of
 * sight (b::pursuit_start; not again for a target before it is seen
 * again).
 */
void update_pursuit(bot_state &bs, const object &obj, const uint32_t tick, const unsigned memory_ticks)
{
	auto &p{bs.pursuit};
	if (bs.target && bs.visible_now[*bs.target] && (bs.chosen == b::goal_kind::engage || bs.chosen == b::goal_kind::hunt))
	{
		const auto t{*bs.target};
		const double sh{ship_of(t).shields / 65536.0};
		/* A new engagement: another target, or none for a while. */
		if (p.engaged_who != t || tick - p.engaged_tick > BOT_ENGAGEMENT_GAP_TICKS)
			p.engage_shields = sh;
		else
			p.engage_shields = std::max(p.engage_shields, sh);
		p.engaged_who = t;
		p.engaged_tick = tick;
	}
	if (p.active)
	{
		const auto w{p.who};
		std::optional<b::pursuit_end> end;
		if (w >= MAX_PLAYERS || !bs.memory[w].valid)
			end = b::pursuit_end::lost;
		else if (bs.visible_now[w])
			end = b::pursuit_end::seen;
		else
			end = b::pursuit_stop(pursuit_view_of(bs, obj, w, tick, memory_ticks), tick_seconds(tick - p.started));
		if (end)
			end_pursuit(bs, *end, tick);
		return;
	}
	if (!bs.target)
		return;
	const auto w{*bs.target};
	const auto &m{bs.memory[w]};
	if (bs.visible_now[w] || !m.valid || p.engaged_who != w)
		return;
	if (p.ended.blocks(w, m.tick))
		return;
	const auto v{pursuit_view_of(bs, obj, w, tick, memory_ticks)};
	const auto reason{b::pursuit_start(v)};
	if (reason == b::pursuit_reason::none)
		return;
	p.active = true;
	p.who = w;
	p.reason = reason;
	p.started = tick;
	p.advances = 0;
	p.dead_end = false;
	p.corner_done = false;
	p.peek = false;
	if (bot_log_on())
		con_printf(CON_VERBOSE, "bots: '%s' pursues P#%u round a corner: %s (for %.1f s; last seen %.0f units away %.1f s ago at %.0f units/s; shields %.0f against %.0f, advantage %.2f)", static_cast<const char *>(bs.cfg.name), w, b::name_of(reason), b::pursuit_limit(v), b::distance(to_vec(obj.pos), m.pos), tick_seconds(tick - m.tick), b::length(m.vel), v.shields, v.target_shields, v.advantage);
}

/* Section 9.10: where the pursued target probably is (b::predict_pursuit
 * from its last known place and velocity), unless out of the bot's
 * reach (the path costs of this strategy tick): then where it was seen.
 */
void predict_pursued(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &p{bs.pursuit};
	const auto &m{bs.memory[p.who]};
	const auto flags{obj.ctype.player_info.powerup_flags};
	const double travel{b::pursuit_travel(b::length(m.vel), tick_seconds(tick - m.tick), p.advances)};
	/* The PR #47 review: the predicted point's leg checked against the
	 * walls (one fvi call), else a place inside (b::predict_pursuit).
	 */
	const auto r{b::predict_pursuit(B.graph, m.segment, m.pos, m.vel, travel, [flags](const uint32_t from, const b::nav_edge &e) {
		return edge_passable(from, e, flags);
	}, [&obj](const uint32_t seg, const vec3 &from, const vec3 &to) {
		return line_clear(obj, to_fixvec(from), static_cast<segnum_t>(seg), to_fixvec(to), 0, false);
	})};
	p.dead_end = r.dead_end;
	if (r.segment == m.segment || bs.dist.cost(r.segment))
	{
		p.seg = r.segment;
		p.point = r.point;
	}
	else
	{
		p.seg = m.segment;
		p.point = m.pos;
	}
}

/* Section 9.10, corner clearing: near the place the pursued target was
 * last seen (the corner), once per pursuit, a peek point short of it and
 * swung wide (b::corner_approach_point), if the bot can fly there in a
 * straight line (at most three probes: the full swing, half, none).
 */
void plan_corner(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &p{bs.pursuit};
	if (p.corner_done || p.peek)
		return;
	const auto &m{bs.memory[p.who]};
	const auto pos{to_vec(obj.pos)};
	const double keep{b::corner_keep(bs.skill_level, bs.cfg.style)};
	const double d{b::distance(pos, m.pos)};
	if (keep <= 0 || d <= keep)
	{
		p.corner_done = true;
		return;
	}
	if (d > b::CORNER_APPROACH_RANGE)
		return;
	const fix rad{obj.size * 2 / 3};
	for (const double share : {b::CORNER_SWING_SHARE, b::CORNER_SWING_SHARE / 2, 0.0})
	{
		const auto a{b::corner_approach_point(pos, m.pos, m.vel, keep, share)};
		if (!a.peek)
			break;
		if (!line_clear(obj, obj.pos, obj.segnum, to_fixvec(a.point), rad, false))
			continue;
		p.peek = true;
		p.peek_point = a.point;
		p.peek_aim = a.aim;
		p.peek_until = tick + static_cast<uint32_t>(b::CORNER_PEEK_SECONDS * b::BOT_TICK_RATE);
		if (bot_log_on())
			con_printf(CON_VERBOSE, "bots: '%s' clears the corner where P#%u went: peeks from %.0f units, swing %.0f", static_cast<const char *>(bs.cfg.name), p.who, keep, keep * share);
		return;
	}
	p.corner_done = true;
}

/* The goal of the last strategy tick, as the goal choice sees it. */
[[nodiscard]]
std::optional<b::goal_kind> current_goal(const bot_state &bs, const bool target_visible)
{
	switch (bs.goal)
	{
		case bot_goal::none:
			return std::nullopt;
		case bot_goal::roam:
			return b::goal_kind::roam;
		case bot_goal::hunt:
			return target_visible ? b::goal_kind::engage : b::goal_kind::hunt;
		case bot_goal::collect:
			return b::goal_kind::collect;
		case bot_goal::retreat:
			return b::goal_kind::retreat;
		case bot_goal::refuel:
			return b::goal_kind::refuel;
	}
	return std::nullopt;
}

/* Section 4.4: target choice; section 4.7: the goal, at 5 Hz. */
void think(bot_state &bs, object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{bs.skill};
	const auto &st{bs.style};
	const auto pos{to_vec(obj.pos)};
	std::array<b::target_candidate, MAX_PLAYERS> cand{};
	unsigned n{0};
	/* Section 9.7: an aggressive bot hunts a lost target longer. */
	const unsigned memory_ticks{b::ticks_from_ms(b::effective_memory_ms(sk, st))};
	/* Section 9.10: the pursuit starts or ends before the target choice
	 * (the pursued target keeps its score).
	 */
	update_pursuit(bs, obj, tick, memory_ticks);
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		if (i == bs.pid || !bs.memory[i].valid)
			continue;
		auto &c{cand[n++]};
		c.pursued = bs.pursuit.active && bs.pursuit.who == i;
		c.id = i;
		c.excluded = same_team(bs.pid, i);
		c.visible = bs.visible_now[i];
		c.confidence = b::memory_confidence(bs.memory[i], tick, memory_ticks);
		c.distance = b::distance(pos, bs.memory[i].pos);
		c.damaged_me_recently = bs.last_attacker == i && tick - bs.attacked_tick < BOT_REVENGE_TICKS;
		c.low_shields = Objects.vcptr(vcplayerptr(i)->objnum)->shields < i2f(30);
		c.bounty = +(Game_mode & GM_BOUNTY) && Bounty_target == i;
	}
	bs.target = b::choose_target(std::span(cand.data(), n), bs.target, sk.awareness);
	if (bs.pursuit.active && bs.target != bs.pursuit.who)
		end_pursuit(bs, b::pursuit_end::other_target, tick);
	double target_score{0};
	bool target_visible{false};
	std::optional<double> target_distance;
	if (bs.target)
	{
		for (unsigned k = 0; k < n; ++k)
			if (cand[k].id == *bs.target)
			{
				target_score = b::target_score(cand[k], sk.awareness);
				target_distance = cand[k].distance;
			}
		target_visible = bs.visible_now[*bs.target];
	}
	/* Section 4.5: the primary for the range to the target; the band
	 * changes only a little past its border.
	 */
	bs.band = target_distance ? std::optional<b::range_band>{b::band_of(*target_distance, bs.band)} : std::nullopt;
	choose_weapon(bs, obj, bs.band);
	/* Section 4.7: what it knows and what it needs. */
	compute_distances(bs, obj);
	learn_powerups(bs, obj, tick);
	/* Section 9.14: the power pickups in sight, at any distance. */
	sight_power_powerups(bs, obj, tick);
	const auto res{resources_of(obj)};
	auto collect{best_collect(bs, res, tick)};
	/* Section 9.5: a valuable powerup close by. */
	const auto grab{best_grab(bs, obj, res, tick)};
	/* Section 9.14: a power pickup the bot has gone for too long (it
	 * does not get there: a door it cannot open after all, a fight on
	 * the way again and again) is no goal for a while.
	 */
	if (bs.power_going && bs.goal == bot_goal::collect && b::power_gave_up(bs.power_since, tick))
	{
		bs.powerups.ignore_for(bs.collect_key, tick + BOT_POWER_GIVE_UP_IGNORE_TICKS);
		bs.power_going = false;
		con_printf(CON_VERBOSE, "bots: '%s' gives up power pickup %hu", static_cast<const char *>(bs.cfg.name), bs.collect_key);
	}
	/* Section 9.14: the best power pickup. */
	const auto power{best_power(bs, obj, res, tick, memory_ticks)};
	auto centre{best_centre(bs, res)};
	/* A bot hovering in a centre stays until it is full, unless an enemy
	 * comes into sight while it has enough to fight.
	 */
	const bool refuelling{bs.goal == bot_goal::refuel && bs.points.empty()};
	if (refuelling && (in_centre(obj, false) || in_centre(obj, true)))
	{
		const bool repair{in_centre(obj, true)};
		const double level{repair ? res.shields : res.energy};
		if (b::keep_refuelling(level, target_visible))
			centre = {std::max(centre.utility, 3.0), 0, static_cast<uint32_t>(obj.segnum), pos, 0xffff, 0, repair};
	}
	const bool attacked{bs.last_attacker < MAX_PLAYERS && tick - bs.attacked_tick < BOT_REVENGE_TICKS};
	/* Section 4.7: cloaked it sneaks, invulnerable it attacks. */
	bs.tactics = b::tactics_for(res.cloaked, res.invulnerable);
	/* Section 9.7: how the bot stands against its target (shields and
	 * armament, as a human judges an opponent by its ship and its
	 * shots): not ahead, a collector avoids the fight; outgunned, a
	 * cautious bot breaks off.
	 */
	double advantage{1};
	if (bs.target)
	{
		const auto &t{*Objects.vcptr(vcplayerptr(*bs.target)->objnum)};
		if (t.type == object_type::OBJ_PLAYER)
			advantage = b::fight_advantage(res.shields, b::armament_score(res.weapons), t.shields / 65536.0, b::armament_score(weapons_of(t.ctype.player_info)));
	}
	bs.retreat_shields = b::style_retreat_shields(st, advantage);
	/* Section 9.8: the early-life power-up phase (weak, a weapon upgrade
	 * known, the level not poor in weapons, not attacked at close
	 * range, the target not as weak and alone), and the stronger third
	 * parties nearby.
	 */
	double phase_engage{1}, phase_collect{0};
	std::optional<goal_place> phase_upgrade;
	{
		std::array<uint8_t, b::BOT_SECONDARY_COUNT> ammo{};
		for (unsigned i = 0; i < b::BOT_SECONDARY_COUNT && i < MAX_SECONDARY_WEAPONS; ++i)
			ammo[i] = obj.ctype.player_info.secondary_ammo[static_cast<secondary_weapon_index>(i)];
		const double own_arms{b::armament_score(res.weapons)};
		bool target_weak{false}, target_alone{true};
		unsigned stronger{0};
		for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
		{
			if (i == bs.pid || !bs.memory[i].valid || same_team(bs.pid, i))
				continue;
			const auto &t{*Objects.vcptr(vcplayerptr(i)->objnum)};
			if (t.type != object_type::OBJ_PLAYER)
				continue;
			const auto &tpi{t.ctype.player_info};
			const auto tw{weapons_of(tpi)};
			if (bs.target && i == *bs.target)
			{
				std::array<uint8_t, b::BOT_SECONDARY_COUNT> tammo{};
				for (unsigned k = 0; k < b::BOT_SECONDARY_COUNT && k < MAX_SECONDARY_WEAPONS; ++k)
					tammo[k] = tpi.secondary_ammo[static_cast<secondary_weapon_index>(k)];
				target_weak = b::weak_armament(tw, tammo);
				continue;
			}
			/* Seen or heard of lately, near the bot. */
			if (tick - bs.memory[i].tick > memory_ticks || b::distance(pos, bs.memory[i].pos) > b::THIRD_PARTY_DISTANCE)
				continue;
			target_alone = false;
			if (b::fight_advantage(t.shields / 65536.0, b::armament_score(tw), res.shields, own_arms) > b::THIRD_PARTY_STRONGER)
				++stronger;
		}
		bs.third_parties = stronger;
		const bool attacked_close{attacked && bs.memory[bs.last_attacker].valid && b::distance(pos, bs.memory[bs.last_attacker].pos) <= b::PHASE_CLOSE_ATTACK};
		const auto up{best_upgrade(bs, res, tick)};
		bs.powerup_phase = b::in_powerup_phase({
			.weak = b::weak_armament(res.weapons, ammo),
			.upgrade_known = up.key != 0xffff,
			.weapon_poor_level = B.weapon_poor,
			.attacked_close = attacked_close,
			.target_weak = target_weak,
			.target_alone = target_alone,
			.invulnerable = res.invulnerable,
		});
		if (bs.powerup_phase)
		{
			phase_engage = b::powerup_phase_engage(bs.cfg.style);
			phase_collect = up.utility * b::powerup_phase_collect(bs.cfg.style) * bs.tactics.collect_weight;
			phase_upgrade = up;
		}
	}
	/* Section 9.9: what it holds to fight with, whether its armament is
	 * weak, and with no target the enemy to seek (the freshest place it
	 * saw one within b::SEEK_MEMORY_SCALE of its memory time, not
	 * searched yet, not where it is now).
	 */
	std::array<uint8_t, b::BOT_SECONDARY_COUNT> own_ammo{};
	for (unsigned i = 0; i < b::BOT_SECONDARY_COUNT && i < MAX_SECONDARY_WEAPONS; ++i)
		own_ammo[i] = obj.ctype.player_info.secondary_ammo[static_cast<secondary_weapon_index>(i)];
	bs.armed = b::armed_of(own_ammo, sk.weapon_smarts);
	const bool weak{b::weak_armament(res.weapons, own_ammo)};
	bs.seek_who.reset();
	double seek{0};
	if (!bs.target && bs.armed != b::armed_level::none)
	{
		const auto seek_ticks{static_cast<uint32_t>(memory_ticks * b::SEEK_MEMORY_SCALE)};
		uint32_t freshest{0};
		for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
		{
			const auto &m{bs.memory[i]};
			if (i == bs.pid || !m.valid || same_team(bs.pid, i) || tick - m.tick > seek_ticks)
				continue;
			if (bs.seek_done[i] == m.tick + 1)
				continue;
			if (b::seek_place_done(b::distance(pos, m.pos), true))
			{
				/* Searched: nobody there. */
				bs.seek_done[i] = m.tick + 1;
				continue;
			}
			if (!bs.seek_who || m.tick > freshest)
			{
				bs.seek_who = i;
				freshest = m.tick;
			}
		}
		if (bs.seek_who)
			seek = b::seek_utility(bs.armed, st.engage_weight * bs.tactics.engage_weight);
	}
	/* Section 9.10: pursuing its unseen target; or done with it (a
	 * pursuit given up: persistence over, weak, an ambush, searched)
	 * until it is seen again: no hunt for it.
	 */
	const bool pursuing{bs.pursuit.active && bs.target == bs.pursuit.who && !target_visible};
	const bool given_up{bs.target && !target_visible && !bs.pursuit.active && bs.pursuit.ended.blocks(*bs.target, bs.memory[*bs.target].tick)};
	/* Section 9.14: the target is about to die (a power pickup does not
	 * break off this fight).
	 */
	bool kill_soon{false};
	if (bs.target && target_distance)
	{
		const auto &t{*Objects.vcptr(vcplayerptr(*bs.target)->objnum)};
		kill_soon = t.type == object_type::OBJ_PLAYER && b::kill_imminent(target_visible, t.shields / 65536.0, *target_distance);
	}
	const bool power_current{bs.power_going && bs.goal == bot_goal::collect && power.place.key == bs.collect_key && power.place.sig == bs.collect_sig};
	const b::goal_inputs gin{
		.has_target = bs.target.has_value() && !given_up,
		.target_visible = target_visible,
		.target_score = target_score,
		.threatened = bs.target.has_value() || attacked,
		.shields = res.shields,
		.invulnerable = res.invulnerable,
		.collect = collect.utility,
		.collect_path = collect.path,
		.collect_upgrade = collect.upgrade,
		.grab = grab.key != 0xffff,
		.grab_shields = grab.shields,
		.grab_value = grab.value,
		.grab_invulnerability = grab.invulnerability,
		.grab_path = grab.path,
		.armed = bs.armed,
		.weak = weak,
		.seek = seek,
		.phase_engage = phase_engage,
		.phase_collect = phase_collect,
		.third_party = b::third_party_factor(bs.cfg.style, bs.third_parties),
		.refuel = centre.utility,
		.retreat_shields = bs.retreat_shields,
		.engage_weight = st.engage_weight * b::style_engage_factor(st, advantage) * bs.tactics.engage_weight,
		.collect_weight = st.collect_weight * bs.tactics.collect_weight,
		.collector = bs.cfg.style == b::bot_style::collector,
		.detour_scale = bs.tune.grab_detour_scale,
		.pursuing = pursuing,
		.power = power.value.utility,
		.power_fight = power.value.fight,
		.power_invulnerability = power.place.invulnerability,
		.kill_imminent = kill_soon,
		.power_current = power_current,
		.current = current_goal(bs, target_visible),
	};
	const auto goal{b::choose_goal(gin)};
	const auto u{b::goal_utility(gin)};
	/* The grab is the collect goal while it is what made collecting win
	 * (section 9.9: b::collect_source), and so is the phase's upgrade
	 * (section 9.8).
	 */
	bs.grabbing = goal == b::goal_kind::collect && u.collect_from == b::collect_source::grab;
	bs.power_going = goal == b::goal_kind::collect && u.collect_from == b::collect_source::power;
	if (bs.power_going && !power_current)
		bs.power_since = tick;
	if (bs.grabbing)
		collect = grab;
	else if (bs.power_going)
		collect = power.place;
	else if (goal == b::goal_kind::collect && phase_upgrade && u.collect_from == b::collect_source::phase)
		collect = *phase_upgrade;
	/* Section 9.9: seeking is the hunt without a target. */
	if (goal != b::goal_kind::hunt || bs.target)
		bs.seek_who.reset();
	/* The log: the goal's utility against the best other. */
	{
		bs.chosen = goal;
		bs.chosen_u = u[goal];
		bs.collect_u = u[b::goal_kind::collect];
		bs.runner_up_u = -1;
		for (unsigned i = 0; i < b::BOT_GOAL_COUNT; ++i)
		{
			const auto g{static_cast<b::goal_kind>(i)};
			if (g != goal && u[g] > bs.runner_up_u)
			{
				bs.runner_up = g;
				bs.runner_up_u = u[g];
			}
		}
	}
	const bool replan_due{bs.points.empty() || tick >= bs.next_plan_tick};
	switch (goal)
	{
		case b::goal_kind::engage:
		case b::goal_kind::hunt:
		{
			/* Hunt: a path to where the target is (or was last seen),
			 * also while the bot fights it in the open.  B1 dropped the
			 * path for a direct engagement, so until the bot reacted to
			 * the target (a reaction time), whenever the line of fire
			 * closed and whenever the target left the field of view for
			 * a moment, the bot had nowhere to go and stopped dead: it
			 * fought on one spot.  Now it chases along the path in those
			 * moments.  A target that moves on is planned for again at
			 * most every half second.
			 */
			/* Section 9.9: or, without one, the enemy it seeks. */
			const auto &m{bs.memory[bs.target ? *bs.target : *bs.seek_who]};
			uint32_t hunt_seg{m.segment};
			vec3 hunt_pos{m.pos};
			/* Section 9.10: pursuing, where the target probably is now
			 * (a walk of at most b::PREDICT_MAX_STEPS segments each
			 * strategy tick; a new path at most every half second), and
			 * the corner it went round cleared first.
			 */
			if (pursuing)
			{
				predict_pursued(bs, obj, tick);
				plan_corner(bs, obj, tick);
				hunt_seg = bs.pursuit.seg;
				hunt_pos = bs.pursuit.point;
			}
			const bool moved{bs.goal != bot_goal::hunt || bs.goal_seg != hunt_seg};
			if (replan_due || (moved && tick - bs.last_plan_tick >= BOT_HUNT_REPLAN_TICKS))
			{
				set_goal(bs, obj, bot_goal::hunt, hunt_seg, hunt_pos, tick);
				/* Section 9.9: a place sought without a path to it is
				 * given up (b::seek_place_done), not planned for again
				 * every strategy tick.
				 */
				if (!bs.target && bs.seek_who && b::seek_place_done(b::distance(pos, m.pos), !bs.points.empty()))
					bs.seek_done[*bs.seek_who] = m.tick + 1;
			}
			return;
		}
		case b::goal_kind::collect:
		{
			const bool other{bs.goal != bot_goal::collect || bs.collect_key != collect.key || bs.collect_sig != collect.sig};
			bs.collect_key = collect.key;
			bs.collect_sig = collect.sig;
			if (other || replan_due)
				set_goal(bs, obj, bot_goal::collect, collect.segment, collect.pos, tick);
			return;
		}
		case b::goal_kind::refuel:
			if (obj.segnum == centre.segment)
			{
				/* In the centre: hover (follow_path holds the place). */
				if (bs.goal != bot_goal::refuel || !bs.points.empty())
				{
					bs.goal = bot_goal::refuel;
					bs.goal_seg = centre.segment;
					bs.clear_path();
				}
				bs.refuel_seg = centre.segment;
				return;
			}
			if (bs.goal != bot_goal::refuel || bs.refuel_seg != centre.segment || replan_due)
			{
				bs.refuel_seg = centre.segment;
				set_goal(bs, obj, bot_goal::refuel, centre.segment, centre.pos, tick);
			}
			return;
		case b::goal_kind::retreat:
		{
			/* On its way: keep the place (planned again every 2 s, as
			 * doors and penalties change); a new one only on arrival.
			 */
			if (bs.goal == bot_goal::retreat && !bs.points.empty())
			{
				if (tick >= bs.next_plan_tick)
					plan_path(bs, obj, bs.goal_seg, std::nullopt, tick);
				return;
			}
			vec3 threat{pos};
			if (bs.target)
				threat = bs.memory[*bs.target].pos;
			else if (attacked && bs.memory[bs.last_attacker].valid)
				threat = bs.memory[bs.last_attacker].pos;
			if (const auto place{retreat_place(bs, obj, res, threat, tick)})
			{
				set_goal(bs, obj, bot_goal::retreat, place->segment, place->pos, tick);
				return;
			}
			break;
		}
		case b::goal_kind::roam:
			break;
	}
	if (bs.goal != bot_goal::roam)
		bs.goal = bot_goal::none;
	if (bs.goal == bot_goal::none || bs.points.empty())
		choose_roam_goal(bs, obj, tick);
}

/* Section 4.3: fly along the path; returns the wanted velocity. */
vec3 follow_path(bot_state &bs, object &obj, const bool engaged)
{
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	const double max_speed{B.limits.max_speed};
	if (bs.points.empty())
	{
		if (!engaged)
		{
			bs.face_dir = frame.f;
			bs.face_rate = {};
		}
		/* Refuelling (section 4.7): hover in the centre. */
		if (bs.goal == bot_goal::refuel && bs.goal_seg < B.graph.size())
		{
			const auto hold{(B.graph.position(bs.goal_seg) - pos) * 0.5};
			const double l{b::length(hold)};
			const double cap{max_speed * 0.3};
			return l > cap ? hold * (cap / l) : hold;
		}
		return {};
	}
	const double reach{std::max(obj.size / 65536.0 * 1.5, 5.0)};
	/* A powerup is taken by touching it: the bot flies through its
	 * centre rather than stopping next to it.
	 */
	const bool collecting{bs.goal == bot_goal::collect};
	bs.point_index = b::advance_along(bs.points, bs.point_index, pos, reach);
	/* The review of PR #74: the points the string skipped are passed
	 * (b::advance_skipped).
	 */
	bs.point_index = b::advance_skipped(bs.points, bs.point_index, bs.steer_index, pos);
	if (bs.steer_index < bs.point_index)
		bs.steer_index = bs.point_index;
	const auto &target{bs.points[bs.steer_index]};
	const auto to{target - pos};
	const double dist{b::length(to)};
	const bool last{bs.steer_index + 1 == bs.points.size()};
	double speed{max_speed};
	if (last)
		speed = std::min(speed, collecting ? dist * 3 + 8 : dist * 1.5);
	else
	{
		/* Slow down before a sharp turn (section 9.15: the more the
		 * sharper).
		 */
		speed = b::corner_speed(b::angle_between(to, bs.points[bs.steer_index + 1] - target), dist, max_speed);
	}
	if (!engaged)
	{
		bs.face_dir = b::normalized(to);
		bs.face_rate = {};
	}
	/* Stuck recovery (section 4.3). */
	switch (bs.stuck.update(b::remaining_length(bs.points, bs.point_index, pos)))
	{
		case b::stuck_event::none:
			break;
		case b::stuck_event::stuck:
		{
			const auto &e{bs.point_edges[std::min(bs.point_index, bs.point_edges.size() - 1)]};
			if (e.second != b::NAV_NO_SIDE)
				bs.penalties.add(e.first, e.second, 200);
			const double side{bs.rng.uniform() < 0.5 ? -1.0 : 1.0};
			bs.recover_dir = b::normalized(frame.f * -0.7 + frame.r * (0.7 * side) + frame.u * bs.rng.uniform(-0.3, 0.3));
			con_printf(CON_VERBOSE, "bots: '%s' stuck in segment %hu", static_cast<const char *>(bs.cfg.name), static_cast<uint16_t>(obj.segnum));
			bot_arena_note_stuck(bs.pid);
			break;
		}
		case b::stuck_event::give_up:
			/* A powerup it cannot reach is no goal for a while. */
			if (collecting)
				bs.powerups.ignore_for(bs.collect_key, B.tick.tick() + BOT_COLLECT_UNREACHABLE_TICKS);
			bs.goal = bot_goal::none;
			bs.clear_path();
			return {};
	}
	if (last && dist < (collecting ? 2.0 : reach))
	{
		/* Arrived.  A hunt that finds nobody forgets the target's old
		 * position; a roam picks a new place at the next strategy tick.
		 * At a powerup that is still there (it could not be taken) the
		 * bot does not come back for a while; a refuelling bot hovers.
		 */
		if (bs.goal == bot_goal::hunt && bs.target)
		{
			/* Section 9.10: a pursuit goes on to the next predicted
			 * place (further along the target's way), until a dead end
			 * or b::PURSUIT_MAX_ADVANCES places: searched.
			 */
			auto &p{bs.pursuit};
			if (p.active && *bs.target == p.who && !p.dead_end && ++p.advances <= b::PURSUIT_MAX_ADVANCES)
				p.corner_done = true;
			else
			{
				end_pursuit(bs, b::pursuit_end::searched, B.tick.tick());
				bs.memory[*bs.target] = {};
			}
		}
		if (collecting)
			bs.powerups.ignore_for(bs.collect_key, B.tick.tick() + BOT_COLLECT_IGNORE_TICKS);
		if (bs.goal != bot_goal::refuel)
			bs.goal = bot_goal::none;
		bs.clear_path();
		return {};
	}
	return b::normalized(to) * speed;
}

/* Section 4.7: the afterburner, by the skill's rule (bot_goals.h): when
 * chasing a target far away, retreating, dodging, or on a long straight
 * flight, with enough charge, and only while the bot wants to go where
 * its nose points (the afterburner is full forward thrust).  The others
 * hear it as they hear a human's (MULTI_SOUND_FUNCTION as the bot).
 */
void decide_afterburner(bot_state &bs, object &obj, const vec3 &wanted, const uint32_t tick)
{
#if DXX_BUILD_DESCENT == 2
	const auto &pi{obj.ctype.player_info};
	const auto frame{to_frame(obj.orient)};
	const auto pos{to_vec(obj.pos)};
	const double max_speed{B.limits.max_speed};
	bool chasing_far{false};
	if (bs.target && (bs.goal == bot_goal::hunt) && bs.memory[*bs.target].valid)
		chasing_far = b::distance(pos, bs.memory[*bs.target].pos) > bs.style.burn_chase_distance;
	bool long_straight{false};
	if ((bs.goal == bot_goal::roam || bs.goal == bot_goal::collect) && bs.steer_index < bs.points.size())
		/* Section 9.15: straight on along the path, past the point it
		 * steers at.
		 */
		long_straight = b::straight_ahead(bs.points, bs.steer_index, pos) > b::BOT_LONG_STRAIGHT;
	/* Section 9.13: the style profile's share of long flights burnt. */
	if (long_straight)
	{
		if (tick >= bs.roam_roll_at)
		{
			bs.roam_burn_ok = bs.tune.roam_burn >= 1 || bs.rng.uniform() < bs.tune.roam_burn;
			bs.roam_roll_at = tick + b::FLEE_ROLL_TICKS;
		}
		long_straight = bs.roam_burn_ok;
	}
	const bool dodging{tick >= bs.dodge_from && tick < bs.dodge_until};
	const bool aligned{b::length(wanted) > max_speed * 0.5 && b::angle_between(frame.f, wanted) < b::radians(25)};
	const bool burn{!bs.stuck.recovering() && b::want_afterburner({
		.have = has_flag(pi, player_flag::afterburner),
		.charge = bs.pl.afterburner_charge / 65536.0,
		.use = b::afterburner_of(bs.skill_level),
		.chasing_far = chasing_far,
		.retreating = bs.goal == bot_goal::retreat && bs.flee_burn,
		.dodging = dodging,
		.long_straight = long_straight,
		.turn_boost = bs.turning.phase == b::turn_phase::boost && bs.turning.burn,
		.aligned = aligned,
		.burning = bs.burning,
	})};
	if (burn == bs.burning)
		return;
	bs.burning = burn;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &&objp{Objects.vcptridx(&obj)};
	if (burn)
	{
		digi_link_sound_to_object3(sound_effect::SOUND_AFTERBURNER_IGNITE, objp, 1, F1_0, sound_stack::allow_stacking, vm_distance{i2f(256)}, 20098, 25776);
		multi_send_sound_function(3, sound_effect::SOUND_AFTERBURNER_IGNITE, bs.pid);
	}
	else
	{
		digi_kill_sound_linked_to_object(objp);
		multi_send_sound_function(0, 0, bs.pid);
	}
#else
	(void)bs;
	(void)obj;
	(void)wanted;
	(void)tick;
#endif
}

/* The afterburner goes out (death, level end). */
void stop_afterburner(bot_state &bs, const object &obj)
{
	if (!bs.burning)
		return;
	bs.burning = false;
#if DXX_BUILD_DESCENT == 2
	auto &Objects = LevelUniqueObjectState.Objects;
	digi_kill_sound_linked_to_object(Objects.vcptridx(&obj));
	multi_send_sound_function(0, 0, bs.pid);
#else
	(void)obj;
#endif
}

/* The distance to the wall a shot fired from the ship along `dir` meets,
 * up to `limit`: where a missile that misses would burst.  (Objects are
 * left out: the bot's own shots in flight would stop the line; the
 * target is taken into account by the caller.)
 */
[[nodiscard]]
double wall_distance(const object &obj, const vec3 &dir, const double limit)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto pos{to_vec(obj.pos)};
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		obj.pos,
		to_fixvec(pos + dir * limit),
		fvi_query::unused_ignore_obj_list,
		fvi_query::unused_LevelUniqueObjectState,
		fvi_query::unused_Robot_info,
		0,
		Objects.vcptridx(&obj),
	}, obj.segnum, F1_0, hit)};
	if (type == fvi_hit_type::None)
		return limit;
	return b::distance(pos, to_vec(hit.hit_pnt));
}

/* Section 9.8: the velocity while the nose comes round: the reverse turn
 * or its boost (b::turn_round_velocity), else the slide of section 9.5
 * (b::keep_moving_in_turn); reversing only with room behind along the
 * velocity it wants (the PR #38 review, b::reverse_turn_has_room).
 * Outside a fight (turning to an attacker it did not see).
 */
[[nodiscard]]
vec3 turn_velocity(const object &obj, const b::turn_phase turn, const vec3 &wanted, const double err, const vec3 &to, const vec3 &vel, const vec3 &hint, const double max_speed, const double reverse_speed)
{
	if (turn == b::turn_phase::reversing || turn == b::turn_phase::boost)
	{
		const auto v{b::turn_round_velocity(turn, wanted, to, vel, hint, max_speed, reverse_speed)};
		if (turn != b::turn_phase::reversing || b::reverse_turn_has_room(wall_distance(obj, b::normalized(v), b::REVERSE_TURN_CLEARANCE)))
			return v;
	}
	return b::keep_moving_in_turn(wanted, err, to, vel, hint, max_speed);
}

/* Section 9.6: the fvi calls of the heavy missile tactics, all bots
 * together, per brain tick: a weighing starts only while fewer than
 * BOT_HEAVY_FVI_PER_TICK were made this tick (or when it has waited
 * BOT_HEAVY_PLAN_OVERDUE ticks), a release check only while the tick's
 * calls are below twice that.
 */
constexpr unsigned BOT_HEAVY_FVI_PER_TICK{300};
/* The indirect aims weighed per heavy missile and plan (b::aim_search). */
constexpr unsigned BOT_AIM_MAX_INDIRECT{8};
constexpr uint32_t BOT_HEAVY_PLAN_OVERDUE{b::STRATEGY_DIVISOR};

struct heavy_fvi_budget
{
	uint32_t tick{};
	unsigned used{};
	unsigned &at(const uint32_t t)
	{
		if (tick != t)
		{
			tick = t;
			used = 0;
		}
		return used;
	}
};

heavy_fvi_budget heavy_budget;

/* Section 9.6: the level as the heavy missile tactics ask it
 * (b::evaluate_burst): a missile's cast (fvi, walls only) and a blast's
 * line of sight (through grates).  Each point's segment is found once per
 * weighing (the cache), starting from the segment of the nearest point
 * already known (the bot's, the target's, where casts ended), so
 * find_point_seg traces a few segments instead of scanning the level.
 * `calls` counts the fvi calls.
 */
struct fvi_geometry
{
	const object &obj;
	/* The points whose segment is known (segment_none: outside), and
	 * near the ends of casts, segments to start a search from.
	 */
	mutable std::vector<std::pair<vec3, segnum_t>> known;
	mutable std::vector<std::pair<vec3, segnum_t>> hints;
	mutable unsigned calls{};
	fvi_geometry(const object &o, const std::initializer_list<std::pair<vec3, segnum_t>> k) :
		obj{o}, known(k)
	{
		known.reserve(64);
		hints.reserve(64);
	}
	[[nodiscard]]
	segnum_t segment_of(const vec3 &p) const
	{
		segnum_t start{obj.segnum};
		double nearest{1e18};
		for (const auto &[kp, ks] : known)
		{
			const double d{b::distance(kp, p)};
			if (d < 1e-6)
				return ks;
			if (ks != segment_none && d < nearest)
			{
				nearest = d;
				start = ks;
			}
		}
		for (const auto &[kp, ks] : hints)
		{
			const double d{b::distance(kp, p)};
			if (d < nearest)
			{
				nearest = d;
				start = ks;
			}
		}
		const auto seg{find_point_seg(LevelSharedSegmentState, to_fixvec(p), vcsegptridx(start) DXX_lighting_hack_pass_parameter)};
		segnum_t found{segment_none};
		if (seg != segment_none)
			found = seg;
		if (known.size() < 256)
			known.emplace_back(p, found);
		return found;
	}
	[[nodiscard]]
	double cast(const vec3 &from, const vec3 &dir, const double limit) const
	{
		const auto seg{segment_of(from)};
		if (seg == segment_none)
			return 0;
		auto &Objects = LevelUniqueObjectState.Objects;
		fvi_info hit;
		++calls;
		const auto type{find_vector_intersection(fvi_query{
			to_fixvec(from),
			to_fixvec(from + dir * limit),
			fvi_query::unused_ignore_obj_list,
			fvi_query::unused_LevelUniqueObjectState,
			fvi_query::unused_Robot_info,
			0,
			Objects.vcptridx(&obj),
		}, seg, 0, hit)};
		if (type == fvi_hit_type::None)
			return limit;
		/* Where it ended: a start for the next lookups near it. */
		const auto end{to_vec(hit.hit_pnt)};
		if (hit.hit_seg != segment_none && hints.size() < 256)
			hints.emplace_back(end, hit.hit_seg);
		return b::distance(from, end);
	}
	[[nodiscard]]
	bool sees(const vec3 &from, const vec3 &to) const
	{
		const auto seg{segment_of(from)};
		if (seg == segment_none)
			return false;
		++calls;
		return line_clear(obj, to_fixvec(from), seg, to_fixvec(to), 0, true);
	}
};

/* Section 9.6: the objects on the line to an indirect aim point (fvi
 * with the objects, as shot_line_clear): the aim is refused for a
 * teammate, the reactor, a robot or clutter first on it, and for another
 * enemy ship nearer than `nearest` (the missile would burst on it, near
 * the bot).  The target on it is the meeting b::merge_meet weighs.  A
 * wall before the point is the weighing's.
 */
[[nodiscard]]
bool aim_line_clear(const bot_state &bs, const object &obj, const vec3 &point, const double nearest, unsigned &calls)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	++calls;
	const auto type{find_vector_intersection(fvi_query{
		obj.pos,
		to_fixvec(point),
		fvi_query::unused_ignore_obj_list,
		&LevelUniqueObjectState,
		&LevelSharedRobotInfoState.Robot_info,
		FQ_IGNORE_POWERUPS,
		Objects.vcptridx(&obj),
	}, obj.segnum, F1_0 / 2, hit)};
	if (type != fvi_hit_type::Object)
		return true;
	if (hit.hit_object == object_none)
		return false;
	const auto &o{*Objects.vcptr(hit.hit_object)};
	switch (o.type)
	{
		case object_type::OBJ_WEAPON:
			/* Another shot in flight: it does not stop this one. */
			return true;
		case object_type::OBJ_PLAYER:
		{
			const auto who{get_player_id(o)};
			if (who == bs.pid || same_team(bs.pid, who))
				return false;
			if (bs.target && who == *bs.target)
				return true;
			return b::distance(to_vec(obj.pos), to_vec(hit.hit_pnt)) >= nearest;
		}
		default:
			/* The reactor, robots, clutter. */
			return false;
	}
}

/* Section 9.8, the death dump (the PR #38 review): the line along the
 * nose with the objects, as shot_line_clear casts it: a teammate, the
 * reactor, a robot or clutter first on it refuses every missile (false);
 * an enemy ship first on it is where the missile bursts (`ship`).
 */
[[nodiscard]]
bool dump_nose_clear(const bot_state &bs, const object &obj, const vec3 &point, std::optional<playernum_t> &ship, unsigned &calls)
{
	ship.reset();
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	++calls;
	const auto type{find_vector_intersection(fvi_query{
		obj.pos,
		to_fixvec(point),
		fvi_query::unused_ignore_obj_list,
		&LevelUniqueObjectState,
		&LevelSharedRobotInfoState.Robot_info,
		FQ_IGNORE_POWERUPS,
		Objects.vcptridx(&obj),
	}, obj.segnum, F1_0 / 2, hit)};
	if (type != fvi_hit_type::Object)
		return true;
	if (hit.hit_object == object_none)
		return false;
	const auto &o{*Objects.vcptr(hit.hit_object)};
	switch (o.type)
	{
		case object_type::OBJ_WEAPON:
			return true;
		case object_type::OBJ_PLAYER:
		{
			const auto who{get_player_id(o)};
			if (who == bs.pid || same_team(bs.pid, who))
				return false;
			ship = who;
			return true;
		}
		default:
			return false;
	}
}

/* Section 9.6: the scene of a heavy shot, as the bot knows it. */
[[nodiscard]]
b::blast_scene heavy_scene(const bot_state &bs, const object &obj, const vec3 &target_pos, const vec3 &target_vel, const bool target_visible, const double unseen_for, const double target_shields, const double invulnerable_left)
{
	return {
		.bot = to_vec(obj.pos),
		.bot_vel = to_vec(obj.mtype.phys_info.velocity),
		.target = target_pos,
		.target_vel = target_vel,
		.target_visible = target_visible,
		.unseen_for = unseen_for,
		.aim_sigma = b::radians(bs.skill.aim_sigma_deg),
		.shields = obj.shields / 65536.0,
		.target_shields = target_shields,
		.invulnerable_left = invulnerable_left,
	};
}

/* Section 9.6: a place nearby that the target cannot see and the bot
 * can fly to in a straight line: segment centres up to three segments
 * away, 15-90 units, the best by b::duck_score first, at most
 * BOT_DUCK_CHECKS of them checked.
 */
constexpr unsigned BOT_DUCK_CHECKS{6};
constexpr uint32_t BOT_DUCK_TICKS{2 * b::BOT_TICK_RATE};

[[nodiscard]]
std::optional<vec3> find_duck_point(const object &obj, const vec3 &target_pos, const segnum_t target_seg)
{
	const auto pos{to_vec(obj.pos)};
	std::array<uint32_t, 64> segs{};
	std::size_t count{0};
	segs[count++] = obj.segnum;
	for (std::size_t head = 0, hop = 0, level_end = 1; head < count && hop < 3; ++hop)
	{
		for (; head < level_end; ++head)
			for (const auto &e : B.graph.neighbours(segs[head]))
				if (count < segs.size() && std::find(segs.begin(), segs.begin() + count, e.to) == segs.begin() + count)
					segs[count++] = e.to;
		level_end = count;
	}
	std::array<vec3, 64> points{};
	for (std::size_t i = 0; i < count; ++i)
		points[i] = B.graph.position(segs[i]);
	const fix rad{obj.size * 2 / 3};
	return b::pick_duck_point(std::span(points.data(), count), pos, target_pos, BOT_DUCK_CHECKS,
		[&](const vec3 &p) {
			return line_clear(obj, obj.pos, obj.segnum, to_fixvec(p), rad, false);
		},
		[&](const vec3 &p) {
			return !line_clear(obj, to_fixvec(target_pos), target_seg, to_fixvec(p), 0, true);
		});
}

/* Section 9.4: a mine dropped now would be met by a teammate following
 * the bot (a team game with friendly fire on): one close behind, or
 * further behind but flying the bot's way, in sight.
 */
[[nodiscard]]
bool teammate_behind(const bot_state &bs, const object &obj)
{
	if (!(Game_mode & GM_TEAM) || Netgame.NoFriendlyFire)
		return false;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto pos{to_vec(obj.pos)};
	const auto dir{b::normalized(to_vec(obj.mtype.phys_info.velocity))};
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		if (i == bs.pid || !same_team(bs.pid, i))
			continue;
		const auto &plr{*vcplayerptr(i)};
		if (plr.connected != player_connection_status::playing)
			continue;
		const auto &t{*Objects.vcptr(plr.objnum)};
		if (t.type != object_type::OBJ_PLAYER)
			continue;
		const auto to{to_vec(t.pos) - pos};
		const double dist{b::length(to)};
		if (dist > b::MINE_TEAMMATE_DISTANCE || b::dot(dir, b::normalized(to)) > 0)
			continue;
		const bool coming{b::dot(to_vec(t.mtype.phys_info.velocity), -to) > 0};
		if ((dist <= b::MINE_TEAMMATE_CLOSE || coming) && line_clear(obj, obj.pos, obj.segnum, t.pos, 0, true))
			return true;
	}
	return false;
}

/* Section 9.6: the heavy missiles' aims weighed (b::choose_heavy_aim) at
 * the strategy rate, while the bot may fire one (the rules before the
 * blast hold: a target in sight or hidden for less than
 * b::CORNER_SEEN_WITHIN, no cooldown, not cloaked, not yet used on this
 * target); m gets each one's verdict.  Also the hug (b::want_hug) and
 * the duck (b::want_duck).
 */
void plan_heavy(bot_state &bs, const object &obj, const uint32_t tick, b::missile_situation &m, const bool target_visible, const std::optional<vec3> &target_pos, const double invulnerable_left)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	m.standoff_scale = bs.risk.standoff_scale;
	const bool due{!bs.heavy_planned || tick - bs.heavy_plan_tick >= b::STRATEGY_DIVISOR};
	/* The fvi budget of all bots this tick: a weighing waits for the
	 * next tick when it is spent (up to twice it for one overdue).
	 */
	unsigned &fvi_used{heavy_budget.at(tick)};
	const bool overdue{tick - bs.heavy_plan_tick >= b::STRATEGY_DIVISOR + BOT_HEAVY_PLAN_OVERDUE};
	const bool weigh{due && (fvi_used < BOT_HEAVY_FVI_PER_TICK || (overdue && fvi_used < 2 * BOT_HEAVY_FVI_PER_TICK))};
	const auto t{bs.target ? *bs.target : 0xffu};
	const bool may{bs.target && target_pos && (target_visible || m.target_seen_ago <= b::CORNER_SEEN_WITHIN) &&
		m.smarts >= b::min_smarts(b::secondary::mega) &&
		m.since_missile >= b::missile_interval(m.smarts) * m.missile_interval_scale && m.since_heavy >= b::heavy_interval(m.smarts) &&
		!(m.cloaked && (m.smarts < 3 || m.target_distance > b::CLOAKED_MISSILE_DISTANCE)) && !m.heavy_used_on_target};
	if (!may)
		bs.heavy_planned = false;
	else
	{
		const auto &mem{bs.memory[t]};
		const auto &ship{*Objects.vcptr(vcplayerptr(t)->objnum)};
		const auto scene{heavy_scene(bs, obj, *target_pos, mem.vel, target_visible, target_visible ? 0 : m.target_seen_ago, ship.shields / 65536.0, invulnerable_left)};
		const fvi_geometry geo{obj, {{to_vec(obj.pos), obj.segnum}, {mem.pos, static_cast<segnum_t>(mem.segment)}}};
		constexpr std::array<b::secondary, 2> heavies{{b::secondary::earthshaker, b::secondary::mega}};
		for (std::size_t k = 0; k < heavies.size(); ++k)
		{
			const auto s{heavies[k]};
			const auto i{static_cast<unsigned>(s)};
			if (!m.ammo[i])
			{
				bs.heavy_plan[k] = {};
				continue;
			}
			if (weigh)
			{
				/* Half the fan and probes per weighing, the most
				 * promising indirect aims, the last best one again.
				 */
				b::aim_search search{.subsets = 2, .phase = bs.heavy_plan_phase, .max_indirect = BOT_AIM_MAX_INDIRECT, .previous = std::nullopt};
				const auto &last{bs.heavy_plan[k].best};
				if (bs.heavy_planned && last && last->kind != b::aim_kind::direct && last->kind != b::aim_kind::wall_behind)
					search.previous = last->point;
				const double nearest{std::max(m.data[i].blast_radius, 0.0) + b::BLAST_MARGIN};
				bs.heavy_plan[k] = b::choose_heavy_aim(geo, scene, b::role_of(s), m.data[i], bs.risk, search, [&](const b::aim_option &a) {
					return aim_line_clear(bs, obj, a.point, nearest, geo.calls);
				});
			}
			else if (!bs.heavy_planned)
				continue;
			const auto &c{bs.heavy_plan[k]};
			m.heavy_risk[i] = c.best ? b::heavy_verdict::fire : b::heavy_verdict_of(c.why);
			m.heavy_aim[i] = c.best ? c.best->kind : b::aim_kind::direct;
		}
		if (weigh)
		{
			bs.heavy_planned = true;
			bs.heavy_plan_tick = tick;
			++bs.heavy_plan_phase;
			fvi_used += geo.calls;
		}
	}
	if (!due)
		return;
	const auto pos{to_vec(obj.pos)};
	/* Hugging an enemy known to hold a heavy missile (b::want_hug): not
	 * with its own heavy shot usable soon (the standoff then), each mode
	 * held a moment, re-weighed while the enemy is out of sight.
	 */
	{
		bool hug{false};
		if (bs.target && target_pos && (target_visible || bs.hugging))
		{
			if (bs.hug_roll_target != t)
			{
				bs.hug_roll_target = static_cast<uint8_t>(t);
				bs.hug_roll = bs.rng.uniform();
			}
			const auto pending{bs.missile ? b::role_of(*bs.missile) : b::missile_role::none};
			const bool own_soon{b::heavy_usable_soon(m) || pending == b::missile_role::heavy || pending == b::missile_role::shaker};
			hug = b::want_hug({
				.enemy_heavy = bs.heavy_holding[t].held(tick),
				.enemy_facing = target_visible && m.target_facing,
				.distance = m.target_distance,
				.hugging = bs.hugging,
				.since_change = (tick - bs.hug_changed) / static_cast<double>(b::BOT_TICK_RATE),
				.unseen_for = target_visible ? 0 : m.target_seen_ago,
				.own_heavy_soon = own_soon,
				.weak = obj.shields / 65536.0 < bs.retreat_shields,
				.roll = bs.hug_roll,
			}, bs.risk);
		}
		if (hug != bs.hugging)
		{
			if (bot_log_on())
				con_printf(CON_VERBOSE, "bots: '%s' %s P#%u (%.0f units%s)", static_cast<const char *>(bs.cfg.name), hug ? "hugs" : "stops hugging", t, m.target_distance, target_visible ? "" : ", out of sight");
			bs.hug_changed = tick;
			/* One mode: no ducking while hugging. */
			if (hug)
				bs.duck_point.reset();
		}
		bs.hugging = hug;
		if (hug)
		{
			double radius{0};
			for (const auto w : {game_secondary(b::secondary::mega), game_secondary(b::secondary::earthshaker)})
				if (static_cast<unsigned>(w) < MAX_SECONDARY_WEAPONS)
					radius = std::max(radius, missile_data_of(w).blast_radius);
			bs.hug_keep = b::hug_distance(radius);
		}
	}
	/* Breaking the line of sight before a heavy shot. */
	const bool own_heavy{m.ammo[static_cast<unsigned>(b::secondary::earthshaker)] || m.ammo[static_cast<unsigned>(b::secondary::mega)]};
	if (bs.heavy_planned && own_heavy && !bs.hugging && bs.target && target_pos && target_visible && tick >= bs.duck_until)
	{
		bool favourable{false};
		for (const auto &c : bs.heavy_plan)
			favourable = favourable || c.best.has_value();
		const auto away{b::normalized(pos - *target_pos)};
		const bool duck{b::want_duck({
			.heavy_ready = true,
			.favourable = favourable,
			.distance = m.target_distance,
			.standoff = bs.standoff,
			.target_closing = m.target_closing_speed,
			.back_blocked = wall_distance(obj, away, BOT_BACK_WALL_CLEARANCE) < BOT_BACK_WALL_CLEARANCE,
			.duck_share = bs.risk.duck_share,
			.duck_closing = bs.risk.duck_closing,
		})};
		bs.duck_point.reset();
		if (duck)
		{
			bs.duck_point = find_duck_point(obj, *target_pos, static_cast<segnum_t>(bs.memory[t].segment));
			bs.duck_until = tick + (bs.duck_point ? BOT_DUCK_TICKS : b::STRATEGY_DIVISOR * 2);
			if (bot_log_on())
			{
				if (bs.duck_point)
					con_printf(CON_VERBOSE, "bots: '%s' ducks out of P#%u's sight (%.0f units, needs %.0f): %.0f units away", static_cast<const char *>(bs.cfg.name), t, m.target_distance, bs.standoff, b::distance(pos, *bs.duck_point));
				else
					con_printf(CON_VERBOSE, "bots: '%s' finds no cover from P#%u (%.0f units, needs %.0f)", static_cast<const char *>(bs.cfg.name), t, m.target_distance, bs.standoff);
			}
		}
	}
}

/* Stage B4 (section 9.4): missiles and mines.  At each tick the bot
 * either waits for the release of the missile it chose (the aim within
 * its cone, the blast along the nose far enough away) or chooses one
 * (b::choose_secondary); a mine is dropped at once.  bots_fire fires it
 * through do_missile_firing as the bot, as a human's.  Section 9.6: a
 * heavy missile is aimed where its weighing found the best outcome (the
 * target, a wall near it, the corner it hides behind) and released when
 * the outcome along the nose is favourable.
 */
/* Section 9.10 and the PR #47 review: the homing shot round the corner
 * of a pursuit is pending (chosen as such, the pursuit of the target
 * still under way).
 */
[[nodiscard]]
bool corner_shot_pending(const bot_state &bs)
{
	return bs.corner_shot && bs.missile == b::secondary::homing && bs.pursuit.active && bs.pursuit.who < MAX_PLAYERS && bs.target == bs.pursuit.who && bs.memory[bs.pursuit.who].valid;
}

/* Its aim: the corner's exit (the peek's aim, b::corner_approach_point),
 * the place the target was last seen plus b::CORNER_AIM_AHEAD along its
 * way.
 */
[[nodiscard]]
vec3 corner_exit(const bot_state &bs, const vec3 &pos)
{
	const auto &m{bs.memory[bs.pursuit.who]};
	return b::corner_approach_point(pos, m.pos, m.vel, 0).aim;
}

void missile_tick(bot_state &bs, object &obj, const uint32_t tick, const percept *const p)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{bs.skill};
	auto &pi{obj.ctype.player_info};
	if (bs.missile_fire)
		return;
	if (bs.missile && tick >= bs.missile_until)
	{
		bs.missile.reset();
		bs.heavy_aim.reset();
	}
	const auto seconds_since{[tick](const std::optional<uint32_t> &t) {
		return t ? (tick - *t) / static_cast<double>(b::BOT_TICK_RATE) : 1e9;
	}};
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const auto res_cloaked{has_flag(pi, player_flag::cloaked)};
	/* Real invulnerability only (the faked respawn one ends at the first
	 * hit), in seconds left: the blast safety relaxes only while it
	 * outlasts the missile (b::blast_safe).
	 */
	const double invulnerable_left{has_flag(pi, player_flag::invulnerable) && !pi.FakingInvul
		? std::max(0.0, static_cast<double>(pi.invulnerable_time + INVULNERABLE_TIME_MAX - GameTime64) / 65536.0)
		: 0};
	/* The target, as the tactics layer sees it (a reaction time late),
	 * or as remembered.
	 */
	const bool target_visible{p && p->visible && bs.target && *bs.target == p->target && bs.visible_now[p->target]};
	std::optional<vec3> target_pos;
	if (target_visible)
		target_pos = p->pos;
	else if (bs.target && bs.memory[*bs.target].valid)
		target_pos = bs.memory[*bs.target].pos;
	/* Section 9.8, the death dump: about to die (shields very low under
	 * fire, or a lethal hit coming), the bot fires its missiles and
	 * drops its mines as fast as the game lets it, most valuable first
	 * (b::death_dump_order), each only with its blast clear of the bot.
	 */
	{
		std::array<uint8_t, b::BOT_SECONDARY_COUNT> ammo{};
		bool has_ammo{false};
		for (unsigned i = 0; i < b::BOT_SECONDARY_COUNT && i < MAX_SECONDARY_WEAPONS; ++i)
		{
			ammo[i] = pi.secondary_ammo[static_cast<secondary_weapon_index>(i)];
			const auto s{static_cast<b::secondary>(i)};
			if (ammo[i] && sk.weapon_smarts >= b::min_smarts(s))
				has_ammo = true;
		}
		const bool dump{b::death_dump_wanted({
			.shields = obj.shields / 65536.0,
			.since_hit = bs.last_attacker < MAX_PLAYERS ? (tick - bs.attacked_tick) / static_cast<double>(b::BOT_TICK_RATE) : 1e9,
			.incoming_damage = bs.incoming_damage,
			.invulnerable = has_flag(pi, player_flag::invulnerable) && !pi.FakingInvul,
			.has_ammo = has_ammo,
			.roll = bs.dump_roll,
		}, bs.skill_level, bs.cfg.style)};
		if (dump != bs.dumping && bot_log_on())
			con_printf(CON_VERBOSE, "bots: '%s' %s (shields %.0f, incoming %.0f)", static_cast<const char *>(bs.cfg.name), dump ? "dumps its missiles: about to die" : "stops the dump", obj.shields / 65536.0, bs.incoming_damage);
		bs.dumping = dump;
		if (dump)
		{
			/* Section 9.8 and the PR #38 review: each missile weighed as
			 * the heavy ones are (b::dump_outcome: the wall along the
			 * nose, the ship first on the line, any ship it may meet;
			 * the children), under the same fvi budget, with the rule
			 * of b::dump_blast_ok; a mine as the normal path drops one
			 * (no teammate behind).
			 */
			struct nose_view
			{
				bool clear{};
				double behind{};
				std::vector<b::dump_ship> ships;
			};
			std::optional<nose_view> nose;
			std::optional<bool> mine_ok;
			unsigned &fvi_used{heavy_budget.at(tick)};
			const fvi_geometry geo{obj, {{pos, obj.segnum}}};
			const auto blast_ok{[&](const b::secondary s) {
				const auto r{b::role_of(s)};
				if (r == b::missile_role::mine)
				{
					if (!mine_ok)
						mine_ok = !teammate_behind(bs, obj);
					return *mine_ok;
				}
				if (fvi_used + geo.calls >= 2 * BOT_HEAVY_FVI_PER_TICK)
					return false;
				if (!nose)
				{
					nose.emplace();
					const double wall{wall_distance(obj, frame.f, b::BURST_CAST_LIMIT)};
					nose->behind = wall_distance(obj, -frame.f, b::BURST_CAST_LIMIT);
					std::optional<playernum_t> first;
					nose->clear = dump_nose_clear(bs, obj, pos + frame.f * wall, first, geo.calls);
					geo.calls += 2;
					if (nose->clear)
						for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
						{
							if (i == bs.pid)
								continue;
							const auto &plr{*vcplayerptr(i)};
							if (plr.connected != player_connection_status::playing)
								continue;
							const auto &t{*Objects.vcptr(plr.objnum)};
							if (t.type != object_type::OBJ_PLAYER)
								continue;
							const auto tp{to_vec(t.pos)};
							/* Only ships ahead and within the cast. */
							if (b::dot(tp - pos, frame.f) <= 0 || b::distance(tp, pos) > b::BURST_CAST_LIMIT)
								continue;
							nose->ships.push_back({
								.pos = tp,
								.vel = to_vec(t.mtype.phys_info.velocity),
								.teammate = same_team(bs.pid, i) && !Netgame.NoFriendlyFire,
								.on_line = first == i,
							});
						}
				}
				if (!nose->clear)
					return false;
				const auto md{missile_data_of(game_secondary(s))};
				const auto o{b::dump_outcome(geo, heavy_scene(bs, obj, pos, {}, false, 0, 100, invulnerable_left), r, md, frame.f, nose->ships)};
				return o && b::dump_blast_ok(*o, r, md, invulnerable_left, nose->behind);
			}};
			const auto dumped{b::death_dump_choice(ammo, sk.weapon_smarts, blast_ok)};
			fvi_used += geo.calls;
			if (const auto s{dumped})
			{
				const auto w{game_secondary(*s)};
				bs.missile.reset();
				bs.heavy_aim.reset();
				bs.volley_left = 0;
				bs.volley_missile.reset();
				bs.missile_fire = *s;
				bs.missile_volley = std::max<unsigned>(1, Weapon_info[Secondary_weapon_to_weapon_info[w]].fire_count);
				if (b::role_of(*s) == b::missile_role::mine)
					bs.last_mine = tick;
				else
					bs.last_missile = tick;
				if (bot_log_on())
					con_printf(CON_VERBOSE, "bots: '%s' dumps %s (%u left)", static_cast<const char *>(bs.cfg.name), secondary_names[static_cast<unsigned>(*s)], ammo[static_cast<unsigned>(*s)] - 1u);
			}
			return;
		}
	}
	if (!bs.missile)
	{
		b::missile_situation m;
		for (unsigned i = 0; i < b::BOT_SECONDARY_COUNT && i < MAX_SECONDARY_WEAPONS; ++i)
		{
			const auto w{static_cast<secondary_weapon_index>(i)};
			m.ammo[i] = pi.secondary_ammo[w];
			if (m.ammo[i])
				m.data[i] = missile_data_of(w);
		}
		m.smarts = sk.weapon_smarts;
		m.mine_interval_scale = bs.style.mine_interval;
		/* Section 9.13: the style profile's, else the style's. */
		m.missile_interval_scale = bs.tune.missile_interval_scale >= 0 ? bs.tune.missile_interval_scale : b::missile_interval_scale(bs.cfg.style);
		m.accepted_damage = bs.risk.self_budget * std::clamp(obj.shields / 65536.0, 0.0, 200.0);
		m.has_target = target_pos.has_value();
		m.target_visible = target_visible;
		m.shot_clear = target_visible && bs.shot_clear;
		m.pursuing = bs.pursuit.active && bs.target == bs.pursuit.who;
		m.since_corner_homing = seconds_since(bs.last_corner_homing);
		m.invulnerable_left = invulnerable_left;
		m.cloaked = res_cloaked;
		m.since_missile = seconds_since(bs.last_missile);
		m.since_heavy = seconds_since(bs.last_heavy);
		m.since_mine = seconds_since(bs.last_mine);
		if (bs.target)
		{
			const auto t{*bs.target};
			const auto &mem{bs.memory[t]};
			m.target_seen_ago = mem.valid ? (tick - mem.tick) / static_cast<double>(b::BOT_TICK_RATE) : 1e9;
			m.heavy_used_on_target = bs.heavy_target == t && m.since_heavy < b::heavy_per_target(sk.weapon_smarts);
			if (target_pos)
			{
				const auto to{*target_pos - pos};
				m.target_distance = b::length(to);
				const auto los{b::normalized(to)};
				/* Section 9.5: the blast where the bot is when the
				 * missile meets the target, both closing in.
				 */
				m.closing_speed = b::dot(vel, los);
				m.target_closing_speed = b::dot(mem.vel, -los);
				const auto rel_vel{mem.vel - vel};
				m.target_lateral_speed = b::length(rel_vel - los * b::dot(rel_vel, los));
				const auto &ship{ship_of(t)};
				m.target_facing = b::dot(to_vec(ship.orient.fvec), -los) > std::cos(b::radians(30));
			}
			/* Section 4.7: flying away from it (retreat, collect,
			 * refuel) with it close behind.
			 */
			const bool flying_away{bs.goal == bot_goal::retreat || bs.goal == bot_goal::collect || bs.goal == bot_goal::refuel};
			if (flying_away && mem.valid && m.target_seen_ago <= 1)
			{
				const auto to{mem.pos - pos};
				const double speed{b::length(vel)};
				if (speed > 15 && b::dot(b::normalized(vel), b::normalized(to)) < -0.3)
				{
					m.chased = true;
					m.pursuer_distance = b::length(to);
					m.teammate_behind = teammate_behind(bs, obj);
				}
			}
		}
		/* A doorway: the next path point is the centre of a side. */
		m.at_doorway = bs.point_index < bs.points.size() && !(bs.point_index & 1) && b::distance(pos, bs.points[bs.point_index]) < 12;
		/* Section 9.6: the heavy missiles' aims and their outcomes. */
		plan_heavy(bs, obj, tick, m, target_visible, target_pos, invulnerable_left);
		/* Section 9.5: the heavy missiles' verdict (the log), and the
		 * distance to keep for them.
		 */
		const auto verdict{b::heavy_check(m)};
		bs.standoff = b::heavy_standoff(m);
		/* Its own missile in flight: still clear of the blast. */
		if (tick < bs.blast_hold_until)
			bs.standoff = std::max(bs.standoff, bs.blast_hold);
		bs.heavy_min = 0;
		const b::missile_data *heavy_md{nullptr};
		for (const auto s : {b::secondary::earthshaker, b::secondary::mega})
			if (m.ammo[static_cast<unsigned>(s)])
			{
				heavy_md = &m.data[static_cast<unsigned>(s)];
				bs.heavy_min = b::heavy_min_distance(s, *heavy_md, m.accepted_damage) * m.standoff_scale;
				break;
			}
		/* The log: each change of the verdict while the bot has one. */
		if (heavy_md && verdict != bs.heavy_why && bot_log_on())
		{
			const auto &c{bs.heavy_plan[m.ammo[static_cast<unsigned>(b::secondary::earthshaker)] ? 0 : 1]};
			con_printf(CON_VERBOSE, "bots: '%s' heavy missile: %s (target %.0f units, crossing %.0f, closing %.0f, needs %.0f, blast %.0f%s, keeps %.0f; aims %u, favourable %u, indirect %u%s%s)", static_cast<const char *>(bs.cfg.name), b::name_of(verdict), m.has_target ? m.target_distance : -1.0, m.target_lateral_speed, m.closing_speed, bs.heavy_min, heavy_md->blast_radius, heavy_md->homing ? ", homing" : "", bs.standoff,
				bs.heavy_planned ? c.candidates : 0u, bs.heavy_planned ? c.favourable : 0u, bs.heavy_planned ? c.indirect_favourable : 0u,
				bs.heavy_planned && c.best ? ", best " : "", bs.heavy_planned && c.best ? b::name_of(c.best->kind) : "");
		}
		bs.heavy_why = verdict;
		/* Section 9.8: the next round of a volley under way, else the
		 * choice (whose interval runs from the volley's last round).
		 */
		std::optional<b::secondary> chosen;
		bs.corner_shot = false;
		if (bs.volley_left && bs.volley_missile)
		{
			if (m.since_missile < b::VOLLEY_GAP)
				return;
			const auto vs{*bs.volley_missile};
			if (b::volley_continues({
				.s = vs,
				.smarts = m.smarts,
				.style = bs.cfg.style,
				.ammo = m.ammo[static_cast<unsigned>(vs)],
				.target_visible = m.target_visible,
				.shot_clear = m.shot_clear,
				.target_distance = m.target_distance,
				.target_lateral_speed = m.target_lateral_speed,
				.target_seen_ago = m.target_seen_ago,
				.cloaked = m.cloaked,
			}, bs.volley_left, m.since_missile))
				chosen = vs;
			else
			{
				bs.volley_left = 0;
				bs.volley_missile.reset();
			}
		}
		if (!chosen)
		{
			chosen = b::choose_secondary(m);
			bs.light_why = b::light_check(m, chosen);
			/* The PR #47 review: the corner shot, one try per
			 * b::HOMING_CORNER_INTERVAL.
			 */
			if (chosen == b::secondary::homing && b::homing_round_corner(m))
			{
				bs.corner_shot = true;
				bs.last_corner_homing = tick;
				if (bot_log_on())
					con_printf(CON_VERBOSE, "bots: '%s' aims a homing missile round the corner at P#%u (last seen %.0f units away %.1f s ago)", static_cast<const char *>(bs.cfg.name), bs.pursuit.who, m.target_distance, m.target_seen_ago);
			}
		}
		else
			bs.light_why = b::light_verdict::chosen;
		if (!chosen)
			return;
		bs.missile = chosen;
		bs.missile_until = tick + static_cast<uint32_t>(b::MISSILE_PENDING_SECONDS * b::BOT_TICK_RATE);
		bs.heavy_aim.reset();
		if (*chosen == b::secondary::earthshaker || *chosen == b::secondary::mega)
		{
			const auto &c{bs.heavy_plan[*chosen == b::secondary::earthshaker ? 0 : 1]};
			if (bs.heavy_planned && c.best)
				bs.heavy_aim = c.best;
		}
	}
	const auto s{*bs.missile};
	const auto w{game_secondary(s)};
	if (!pi.secondary_ammo[w])
	{
		bs.missile.reset();
		bs.heavy_aim.reset();
		return;
	}
	const auto role{b::role_of(s)};
	const bool heavy{role == b::missile_role::heavy || role == b::missile_role::shaker};
	/* Section 9.6: a heavy missile aimed at a wall or a corner. */
	const bool indirect{heavy && bs.heavy_aim && bs.heavy_aim->kind != b::aim_kind::direct};
	b::release_aim aim_at{b::release_aim::target};
	if (role != b::missile_role::mine)
	{
		/* The target: in sight, or a smart missile's seen a moment ago,
		 * or (the PR #47 review) the corner's exit for the homing shot
		 * round it (b::release_aim_of).  In sight, the line of fire
		 * must be clear for every missile, the smart one too (no
		 * teammate, reactor or robot first).
		 */
		aim_at = b::release_aim_of({
			.role = role,
			.has_target_pos = target_pos.has_value(),
			.target_visible = target_visible,
			.shot_clear = bs.shot_clear,
			.indirect = indirect,
			.corner_shot = bs.corner_shot,
			.pursuing = corner_shot_pending(bs),
		});
		if (aim_at == b::release_aim::hold)
			return;
	}
	double err{0}, impact{1e9}, target_closing{0};
	const auto md{missile_data_of(w)};
	const double cone{b::radians(sk.fire_cone_deg)};
	if (heavy)
	{
		/* Section 9.6: the aim, then the outcome along the nose. */
		const auto wanted{indirect ? bs.heavy_aim->point - pos : (target_visible ? bs.aim_dir : *target_pos - pos)};
		err = b::angle_between(frame.f, wanted);
		if (err > b::missile_cone(role, cone, md.homing))
		{
			bs.heavy_why = b::heavy_verdict::aiming;
			return;
		}
		/* The fvi budget of all bots this tick: spent, it waits. */
		unsigned &fvi_used{heavy_budget.at(tick)};
		if (fvi_used >= 2 * BOT_HEAVY_FVI_PER_TICK)
		{
			bs.heavy_why = b::heavy_verdict::aiming;
			return;
		}
		const auto t{*bs.target};
		const auto &mem{bs.memory[t]};
		const auto scene{heavy_scene(bs, obj, *target_pos, mem.vel, target_visible, target_visible ? 0 : (tick - mem.tick) / static_cast<double>(b::BOT_TICK_RATE), Objects.vcptr(vcplayerptr(t)->objnum)->shields / 65536.0, invulnerable_left)};
		const fvi_geometry geo{obj, {{pos, obj.segnum}, {mem.pos, static_cast<segnum_t>(mem.segment)}}};
		/* Aimed at the target: it meets the missile if it is on the
		 * line (within the cone of the aim).
		 */
		const bool direct{!indirect && target_visible};
		auto o{b::evaluate_burst(geo, scene, role, md, frame.f, direct)};
		/* Aimed at a wall, the missile may meet the target on the way
		 * (it homes, or the target is near the line): the direct shot
		 * weighed now too (the plan is up to a strategy period and the
		 * pending time old), the worse of both.
		 */
		if (!direct && b::may_meet_target(scene, frame.f, md))
			o = b::merge_meet(o, b::evaluate_burst(geo, scene, role, md, *target_pos - pos, true));
		/* The objects on the line to the aim point (the direct shot's:
		 * bs.shot_clear).
		 */
		const bool line_ok{!indirect || aim_line_clear(bs, obj, pos + frame.f * b::distance(pos, bs.heavy_aim->point), std::max(md.blast_radius, 0.0) + b::BLAST_MARGIN, geo.calls)};
		fvi_used += geo.calls;
		if (!line_ok)
		{
			bs.heavy_why = b::heavy_verdict::no_clear_shot;
			return;
		}
		const auto v{b::judge_blast(o, scene, md, bs.risk)};
		if (v != b::risk_verdict::fire)
		{
			bs.heavy_why = b::heavy_verdict::nose_blast;
			return;
		}
		impact = o.impact;
		if (bot_log_on())
			con_printf(CON_VERBOSE, "bots: '%s' fires %s at P#%u, %.0f units, aim %s (impact %.0f; expected damage to it %.0f, to itself %.0f, shields %.0f; blast %.0f damage %.0f%s; children %u blast %.0f damage %.0f)", static_cast<const char *>(bs.cfg.name), secondary_names[static_cast<unsigned>(s)], t, b::distance(pos, *target_pos), b::name_of(indirect ? bs.heavy_aim->kind : b::aim_kind::direct), impact, o.target_damage, o.self_damage, obj.shields / 65536.0, md.blast_radius, md.damage, md.homing ? ", homing" : "", md.children, md.child_blast_radius, md.child_damage);
	}
	else if (role != b::missile_role::mine)
	{
		/* Section 9.10: round a corner, at its exit (peeking, the bot
		 * faces it).
		 */
		err = b::angle_between(frame.f, aim_at == b::release_aim::corner ? corner_exit(bs, pos) - pos : target_visible || (bs.pursuit.active && bs.pursuit.peek) ? bs.aim_dir : *target_pos - pos);
		/* Where it bursts: the wall along the nose, or the target on
		 * the way.
		 */
		const double wall{wall_distance(obj, frame.f, 400)};
		impact = wall;
		if (target_visible)
		{
			impact = std::min({impact, b::distance(pos, *target_pos), bs.shot_first_hit});
			/* Section 9.5: a ship is the impact, not the wall: it flies
			 * at the missile (the target's closing speed).
			 */
			if (impact < wall && bs.target)
				target_closing = b::dot(bs.memory[*bs.target].vel, -b::normalized(*target_pos - pos));
		}
	}
	/* Section 9.5: the bot's own flight toward the burst along the nose. */
	const double closing{b::dot(vel, frame.f)};
	if (!heavy && role != b::missile_role::mine)
	{
		/* Section 9.9: the rounds after the first of a straight volley
		 * within the wider cone of the stream.
		 */
		const bool stream{bs.volley_left && bs.volley_missile == s && role == b::missile_role::straight};
		const double release_cone{stream ? b::volley_cone(cone) : cone};
		if (!b::missile_release(s, err, release_cone, impact, md, invulnerable_left, closing, target_closing))
		{
			bs.light_why = err > b::missile_cone(role, release_cone, md.homing) ? b::light_verdict::aiming : b::light_verdict::nose_blast;
			return;
		}
		bs.light_why = b::light_verdict::fired;
	}
	bs.missile.reset();
	bs.heavy_aim.reset();
	bs.missile_fire = s;
	bs.missile_volley = std::max<unsigned>(1, Weapon_info[Secondary_weapon_to_weapon_info[w]].fire_count);
	if (role == b::missile_role::mine)
		bs.last_mine = tick;
	else
	{
		bs.last_missile = tick;
		/* Section 9.8: a round of the volley under way, or the first of
		 * a new one (b::volley_size: 1 is a single shot).
		 */
		if (!heavy)
		{
			if (bs.volley_left && bs.volley_missile == s)
				--bs.volley_left;
			else
			{
				double lateral{0};
				const double distance{target_pos ? b::distance(pos, *target_pos) : 0};
				if (target_pos && bs.target)
				{
					const auto los{b::normalized(*target_pos - pos)};
					const auto rel_vel{bs.memory[*bs.target].vel - vel};
					lateral = b::length(rel_vel - los * b::dot(rel_vel, los));
				}
				const unsigned n{b::volley_size({
					.s = s,
					.smarts = sk.weapon_smarts,
					.style = bs.cfg.style,
					.ammo = pi.secondary_ammo[w],
					.target_visible = target_visible,
					.shot_clear = bs.shot_clear,
					.target_distance = distance,
					.target_lateral_speed = lateral,
					.target_seen_ago = bs.target && bs.memory[*bs.target].valid ? (tick - bs.memory[*bs.target].tick) / static_cast<double>(b::BOT_TICK_RATE) : 1e9,
					.cloaked = res_cloaked,
					.mean = bs.tune.volley_size,
				})};
				bs.volley_missile = s;
				bs.volley_left = n > 1 ? n - 1 : 0;
				if (n > 1 && bot_log_on())
					con_printf(CON_VERBOSE, "bots: '%s' fires a volley of %u %s at P#%u, %.0f units", static_cast<const char *>(bs.cfg.name), n, secondary_names[static_cast<unsigned>(s)], bs.target ? *bs.target : 0xffu, distance);
			}
			if (!bs.volley_left)
				bs.volley_missile.reset();
		}
		if (heavy)
		{
			bs.last_heavy = tick;
			bs.heavy_target = bs.target ? *bs.target : 0xff;
			bs.heavy_planned = false;
			bs.duck_point.reset();
			/* Section 9.5: clear of its own blast until it is over (the
			 * cooldown now drops the standoff).
			 */
			bs.blast_hold = b::heavy_min_distance(s, md, bs.risk.self_budget * std::clamp(obj.shields / 65536.0, 0.0, 200.0)) * bs.risk.standoff_scale + 8;
			bs.blast_hold_until = tick + static_cast<uint32_t>(std::ceil(b::blast_danger_seconds(role, impact, md) * b::BOT_TICK_RATE));
			bs.standoff = std::max(bs.standoff, bs.blast_hold);
		}
	}
}

/* The log: a powerup's short name. */
[[nodiscard]]
const char *powerup_label(const powerup_type_t id)
{
	const auto d{item_of(id)};
	switch (d.kind)
	{
		case b::item::primary:
			return primary_name(static_cast<unsigned>(d.weapon));
		case b::item::secondary:
			return d.secondary < secondary_names.size() ? secondary_names[d.secondary] : "missile";
		case b::item::energy:
			return "energy";
		case b::item::shield:
			return "shield";
		case b::item::laser:
			return "laser";
		case b::item::super_laser:
			return "superlaser";
		case b::item::quad:
			return "quad";
		case b::item::vulcan_ammo:
			return "vulcanammo";
		case b::item::afterburner:
			return "afterburner";
		case b::item::converter:
			return "converter";
		case b::item::ammo_rack:
			return "ammorack";
		case b::item::cloak:
			return "cloak";
		case b::item::invulnerability:
			return "invuln";
		case b::item::headlight:
			return "headlight";
		case b::item::full_map:
			return "fullmap";
		case b::item::none:
			break;
	}
	return "other";
}

/* Section 9.5, the log (-verbose), once a second per bot: the goal and
 * its utility against the best other, the target, the weapons, shields
 * and energy, the nearest valuable powerup and why it is not being
 * taken, the heavy missiles' last verdict.
 */
void log_summary(const bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &pi{obj.ctype.player_info};
	const auto pos{to_vec(obj.pos)};
	const auto res{resources_of(obj)};
	/* The target. */
	char target[64];
	if (bs.target && bs.memory[*bs.target].valid)
		std::snprintf(target, sizeof(target), "P#%u %.0fu%s%s", *bs.target, b::distance(pos, bs.memory[*bs.target].pos), bs.visible_now[*bs.target] ? " vis" : "", bs.visible_now[*bs.target] && bs.shot_clear ? " clr" : "");
	else
		std::snprintf(target, sizeof(target), "-");
	/* The nearest valuable powerup. */
	const object *near_pu{nullptr};
	double near_d{BOT_LOG_POWERUP_RADIUS};
	for (const object &o : Objects.vcptr)
	{
		if (o.type != object_type::OBJ_POWERUP || (o.flags & OF_SHOULD_BE_DEAD))
			continue;
		const double d{b::distance(pos, to_vec(o.pos))};
		if (d >= near_d || b::item_value(item_of(get_powerup_id(o)), res) < b::GRAB_MIN_VALUE)
			continue;
		near_pu = &o;
		near_d = d;
	}
	char powerup[96];
	if (near_pu)
	{
		const auto &&o{Objects.vcptridx(near_pu)};
		const uint16_t key = o.get_unchecked_index();
		const uint16_t sig = underlying_value(o->signature);
		const auto id{get_powerup_id(o)};
		auto why{collect_why::goal_lower};
		const auto *const k{[&]() -> const b::known_powerup * {
			for (const auto &e : bs.powerups.items())
				if (e.key == key && e.signature == sig)
					return &e;
			return nullptr;
		}()};
		if (bs.goal == bot_goal::collect && bs.collect_key == key && bs.collect_sig == sig)
			why = collect_why::collecting;
		else if (bs.deny_key == key && tick - bs.deny_tick < 2 * b::BOT_TICK_RATE)
			why = collect_why::denied;
		else if (!k)
			why = collect_why::not_known;
		else if (tick < k->ignore_until)
			why = collect_why::ignored;
		else if (!bs.dist.cost(k->segment))
			why = collect_why::unreachable;
		else if (!net_objects_bot_can_use(bs.pid, id, static_cast<uint32_t>(std::max(o->ctype.powerup_info.count, 0))))
			why = collect_why::cannot_use;
		std::snprintf(powerup, sizeof(powerup), "%s %.0fu v%.1f %s%s%s", powerup_label(id), near_d, b::item_value(item_of(id), res), collect_why_names[static_cast<unsigned>(why)], why == collect_why::denied ? ":" : "", why == collect_why::denied ? deny_name(bs.deny_reason) : "");
	}
	else
		std::snprintf(powerup, sizeof(powerup), "-");
	char arm[160];
	describe_armament(pi, arm, sizeof(arm));
	con_printf(CON_VERBOSE, "bots: '%s' goal=%s %.2f (next %s %.2f, collect %.2f%s%s%s%s%s%s) tgt=%s | %s [%s] | sh=%.0f en=%.0f | pu=%s | heavy=%s min=%.0f keep=%.0f risk=%.2f/%.1f%s%s%s%s | light=%s armed=%s",
		static_cast<const char *>(bs.cfg.name),
		goal_name(bs.chosen), bs.chosen_u, goal_name(bs.runner_up), bs.runner_up_u, bs.collect_u, bs.grabbing ? " grab" : "", bs.power_going ? " power" : "", bs.powerup_phase ? " phase" : "", bs.third_parties ? " 3rd-party" : "", bs.seek_who ? " seek" : "", bs.pursuit.active ? " pursuit" : "",
		target,
		primary_name(underlying_value(pi.Primary_weapon.get_active())), arm,
		obj.shields / 65536.0, pi.energy / 65536.0,
		powerup,
		b::name_of(bs.heavy_why), bs.heavy_min, bs.standoff,
		bs.risk.self_budget, bs.risk.trade, bs.hugging ? " hug" : "", bs.duck_point && tick < bs.duck_until ? " duck" : "",
		bs.dumping ? " dump" : "", bs.turning.phase == b::turn_phase::reversing ? " reverse-turn" : bs.turning.phase == b::turn_phase::sliding ? " slide-turn" : bs.turning.phase == b::turn_phase::boost ? (bs.turning.burn ? " boost-burn" : " boost") : "",
		b::name_of(bs.light_why), b::name_of(bs.armed));
}

/* Section 9.6: toward the duck point, slowing to a hold on it. */
[[nodiscard]]
vec3 duck_velocity(const vec3 &pos, const vec3 &point, const double max_speed)
{
	const auto to{point - pos};
	const double d{b::length(to)};
	if (d < 3)
		return {};
	return to * (std::min(max_speed, 3 * d + 8) / d);
}

/* One tick of the brain (section 4.1). */
void bot_tick(bot_state &bs, const uint32_t tick)
{
	auto &obj{ship_of(bs.pid)};
	if (bs.life != bot_life::alive || obj.type != object_type::OBJ_PLAYER)
		return;
	const auto &sk{bs.skill};
	if (b::layer_due(tick, b::PERCEPTION_DIVISOR, bs.stagger))
		perceive(bs, obj, tick);
	if (b::layer_due(tick, b::STRATEGY_DIVISOR, bs.stagger))
		think(bs, obj, tick);
	bs.aim.update(bs.rng, b::radians(sk.aim_sigma_deg), b::ticks_from_ms(sk.aim_drift_ms));
	bs.lead.update(bs.rng, sk.lead, b::ticks_from_ms(sk.aim_drift_ms));
	const double range_scale{bs.style.range_scale * bs.tactics.range_scale};
	/* Section 9.13: the band of the bot's style profile, else
	 * BOT_RANGE_LO/HI (b::tune_params).
	 */
	double range_lo{bs.tune.range_lo * range_scale}, range_hi{bs.tune.range_hi * range_scale};
	/* Section 9.5: with a heavy missile ready, far enough for its blast. */
	if (bs.standoff > range_lo)
	{
		range_lo = bs.standoff;
		range_hi = std::max(range_hi, range_lo + 25);
	}
	/* Section 9.6: hugging an enemy that holds a heavy missile, within
	 * its own blast.
	 */
	if (bs.hugging && !(tick < bs.blast_hold_until))
	{
		range_lo = b::HUG_NEAREST;
		range_hi = bs.hug_keep;
	}
	bs.juke.update(bs.rng, b::ticks_from_ms(sk.strafe_min_ms), b::ticks_from_ms(sk.strafe_max_ms), range_lo, range_hi, sk.strafe_vertical);
	/* Section 9.12: a retreat is flown turned away or facing the enemy,
	 * drawn when it starts and every b::FLEE_ROLL_TICKS.
	 */
	if (bs.goal == bot_goal::retreat)
	{
		if (!bs.fleeing || tick >= bs.flee_roll_at)
		{
			bs.flee_turned = bs.rng.uniform() < b::FLEE_TURNED_SHARE;
			bs.flee_burn = bs.rng.uniform() < bs.tune.flee_burn;
			bs.flee_roll_at = tick + b::FLEE_ROLL_TICKS;
		}
		bs.fleeing = true;
	}
	else
		bs.fleeing = bs.flee_turned = bs.flee_burn = false;
	const auto pos{to_vec(obj.pos)};
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const auto frame{to_frame(obj.orient)};
	const double max_speed{B.limits.max_speed};
	const unsigned reaction_ticks{b::ticks_from_ms(sk.reaction_ms)};
	const auto *const p{bs.seen.delayed(reaction_ticks / b::PERCEPTION_DIVISOR)};
	bs.fire = false;
	/* Section 4.3: the stuck recovery runs out on time, whatever moves
	 * the bot meanwhile; then the path is planned again.
	 */
	if (bs.stuck.tick_recovery() && bs.goal != bot_goal::none)
		plan_path(bs, obj, bs.goal_seg, std::nullopt, tick);
	vec3 wanted;
	/* Section 9.12: the fight is flown with keys (the strafe, the range,
	 * the slide), not a wanted velocity.
	 */
	bool use_keys{false};
	b::thrust_keys keys;
	bool aim_set{false};
	auto &pi{obj.ctype.player_info};
	const auto primary_now{pi.Primary_weapon.get_active()};
	/* The target in sight, as the tactics layer sees it: its distance,
	 * for the fusion cannon's charge.
	 */
	bool engaged{false};
	double engaged_dist{1e9};
	const bool engaged_now{p && p->visible && bs.target && *bs.target == p->target && bs.visible_now[p->target]};
	/* Section 9.15: in a fight the bot flies with keys: engaged, or a
	 * moment (b::FIGHT_KEYS_MS) after it last saw its target or was hit.
	 * The movement of this tick (bs.mode at its end); engaged, the path
	 * flown instead of the fight's keys.
	 */
	const uint32_t fight_ticks{b::ticks_from_ms(b::FIGHT_KEYS_MS)};
	const bool in_fight{engaged_now ||
		(bs.target && bs.memory[*bs.target].valid && tick - bs.memory[*bs.target].tick < fight_ticks) ||
		(bs.last_attacker < MAX_PLAYERS && tick - bs.attacked_tick < fight_ticks)};
	if (!in_fight)
		bs.hold.release();
	auto mode{b::move_mode::path};
	if (engaged_now)
	{
		engaged = true;
		/* Where the target is now, reckoned from what the bot saw a
		 * reaction time ago: a straight flight is followed, a turn is
		 * noticed late (section 4.2).
		 */
		const double delay{reaction_ticks / static_cast<double>(b::BOT_TICK_RATE)};
		const auto est{p->pos + p->vel * delay};
		/* Section 4.4: the lead from the gun the shots leave from, with
		 * the current weapon's speed and the bot's lead factor.
		 */
		/* Stage B4: a missile that flies straight is aimed with its
		 * own speed from its own gun while it waits for its release.
		 */
		const bool aim_missile{bs.missile && b::missile_aimed(*bs.missile)};
		const auto gun{aim_missile ? missile_gun_local(game_secondary(*bs.missile)) : gun_local(primary_now, has_flag(pi, player_flag::quad_lasers))};
		double shot_speed{weapon_speed(pi)};
		if (aim_missile)
		{
			const auto md{missile_data_of(game_secondary(*bs.missile))};
			shot_speed = b::effective_shot_speed(md.speed, md.thrust);
		}
		const auto shooter{pos + frame.to_world(gun)};
		const auto aim{b::aim_point(shooter, est, p->vel, shot_speed, bs.lead.factor())};
		/* Section 9.6: a heavy missile aimed at a wall or corner. */
		const bool aim_wall{aim_missile && bs.heavy_aim && bs.heavy_aim->kind != b::aim_kind::direct};
		if (aim_wall)
		{
			bs.face_dir = b::apply_aim_offset(bs.heavy_aim->point - shooter, frame.u, bs.aim.yaw(), bs.aim.pitch());
			bs.face_rate = {};
		}
		else
		{
			bs.face_dir = b::apply_aim_offset(aim - shooter, frame.u, bs.aim.yaw(), bs.aim.pitch());
			/* The steering's feed-forward: how fast the line to the
			 * target turns, as the bot reckons it.
			 */
			bs.face_rate = b::line_of_sight_rate(aim - pos, p->vel - vel);
		}
		bs.aim_dir = bs.face_dir;
		aim_set = true;
		const auto to{est - pos};
		const double dist{b::length(to)};
		engaged_dist = dist;
		/* Collecting, retreating or refuelling (section 4.7), the bot
		 * shoots at what it sees but flies its path: backward when it
		 * retreats facing its pursuer.
		 */
		const bool path_goal{bs.goal == bot_goal::collect || bs.goal == bot_goal::retreat || bs.goal == bot_goal::refuel};
		/* One movement per tick (b::engaged_movement): the fight, the
		 * path, or (section 9.6) out of the target's sight before a
		 * heavy shot, facing it (it comes round the corner, or the
		 * corner is hit).
		 */
		/* Section 9.15: a line of fire blocked a moment is no path. */
		auto move_mode{b::engaged_movement(bs.blocked.fight_on(bs.shot_clear, tick), path_goal, bs.duck_point && tick < bs.duck_until)};
		/* Section 9.15: the fight's keys or the path, held at least
		 * b::MODE_HOLD_MS (a line of fire that comes and goes, a pickup
		 * goal of a moment).
		 */
		if (move_mode != b::engaged_move::duck)
			move_mode = bs.hold.update(move_mode == b::engaged_move::combat, tick) ? b::engaged_move::combat : b::engaged_move::path;
		switch (move_mode)
		{
			case b::engaged_move::combat:
			{
				/* Section 4.6 and 9.12: the strafe keys (the skill's
				 * strafe, the style's thrust), the range key.
				 */
				/* Combat movement takes over: no recovery manoeuvre. */
				bs.stuck.cancel_recovery();
				/* Section 9.5: its own heavy missile in flight, the bot
				 * does not close in on the burst; keeping a standoff, it
				 * does not back into a wall (the earthshaker's children
				 * burst there).
				 */
				const double band{range_hi - range_lo};
				const double close{b::effective_close_speed(bs.style)};
				const double forward{band < b::FIGHT_KEY_MIN_BAND
					? b::approach_thrust(dist, bs.juke.range(), b::dot(vel, b::normalized(to)), close, max_speed)
					: bs.approach.update(dist, bs.juke.range(), band) * close};
				/* Section 9.15: nor (its line of fire blocked a moment,
				 * b::fire_blocked) into what blocks it: the strafe clears the
				 * line.
				 */
				const bool no_closer{tick < bs.blast_hold_until || !bs.shot_clear};
				const bool no_back{forward < 0 && bs.standoff > 0 && !bs.hugging && wall_distance(obj, -b::normalized(to), BOT_BACK_WALL_CLEARANCE) < BOT_BACK_WALL_CLEARANCE};
				/* Section 9.15: no run into a wall. */
				b::juke_turn_from_walls(bs.juke, bs.room);
				keys = b::keys_off_walls(b::fight_keys(bs.juke, forward, b::effective_strafe_speed(sk, bs.style), no_closer, no_back), bs.room);
				use_keys = true;
				mode = b::move_mode::fight_keys;
				break;
			}
			case b::engaged_move::path:
				/* Section 9.12: fleeing turned away, the nose follows the
				 * path (and the shots stay off: bs.aim_dir is the aim).
				 */
				wanted = follow_path(bs, obj, !(bs.fleeing && bs.flee_turned && bs.goal == bot_goal::retreat));
				break;
			case b::engaged_move::duck:
				wanted = duck_velocity(pos, *bs.duck_point, max_speed);
				mode = b::move_mode::duck;
				break;
		}
		const double err{b::angle_between(frame.f, bs.face_dir)};
		/* Section 9.8: a target behind: momentum away from it while
		 * the nose comes round (forward thrust becoming reverse), or
		 * (section 9.12) a slide, then a push toward it.  Only while the
		 * bot moves freely (not on its path, ducking, hugging or clear of
		 * its own blast).
		 */
		const bool free_move{move_mode != b::engaged_move::path && move_mode != b::engaged_move::duck && !bs.hugging && !(tick < bs.blast_hold_until)};
		const auto turn{free_move ? bs.turning.update(err, tick, dist, range_lo, bs.rng, bs.tune.turns) : b::turn_phase::none};
		if (!free_move)
		{
			bs.turning.reset();
			bs.slide.reset();
		}
		else
		{
			bool reversing{false};
			if (turn == b::turn_phase::reversing || turn == b::turn_phase::boost)
			{
				const auto v{b::turn_round_velocity(turn, {}, to, vel, frame.r * static_cast<double>(bs.heading_pref), max_speed, bs.tune.turns.reverse_speed)};
				if (turn != b::turn_phase::reversing || b::reverse_turn_has_room(wall_distance(obj, b::normalized(v), b::REVERSE_TURN_CLEARANCE)))
				{
					/* Section 9.15: flown with keys (below). */
					wanted = v;
					use_keys = false;
					reversing = true;
					mode = b::move_mode::turn_keys;
				}
			}
			/* Section 9.5 and 9.12: turning far round (or a reverse
			 * turn with a wall behind), the bot slides on.
			 */
			const double slide_err{turn == b::turn_phase::sliding || (turn == b::turn_phase::reversing && !reversing) ? std::max(err, b::SLIDE_START + 0.01) : err};
			if (const int side{bs.slide.update(reversing ? 0 : slide_err, b::dot(vel, frame.r), bs.heading_pref)}; side && !reversing)
			{
				keys = b::slide_keys(side);
				use_keys = true;
				mode = b::move_mode::slide;
			}
		}
		const double aim_err{b::angle_between(frame.f, bs.aim_dir)};
		/* Section 4.5: beyond the mid band, no shot that hits less than
		 * one time in ten.
		 */
		const auto rel_vel{p->vel - vel};
		const auto los{b::normalized(to)};
		const double lateral{b::length(rel_vel - los * b::dot(rel_vel, los))};
		const auto &target_obj{ship_of(p->target)};
		double cone{b::fire_cone_with_spread(b::radians(sk.fire_cone_deg), spread_half_angle(primary_now))};
#if DXX_BUILD_DESCENT == 2
		/* Section 9.5: omega locks on what is near the nose. */
		if (primary_now == primary_weapon_index::omega)
			cone = b::omega_fire_cone(cone);
#endif
		bs.fire = b::should_fire(aim_err, cone, bs.shot_clear, dist, std::min(weapon_range(pi), bs.tactics.max_fire_distance)) &&
			b::long_shot_worthwhile(dist, weapon_speed(pi), lateral, b::radians(sk.aim_sigma_deg), target_obj.size / 65536.0);
		/* Section 9.7: a beginner pauses between bursts. */
		bs.burst.update(bs.rng, sk.fire_duty);
		bs.fire = bs.fire && bs.burst.on();
	}
	else
	{
		/* The review of PR #63: a slide of a fight that ended out of
		 * sight does not carry its key into the next fight.
		 */
		bs.slide.reset();
		bs.blocked.reset();
		wanted = follow_path(bs, obj, false);
		/* Section 9.15: the target out of sight a moment, the fight's
		 * keys go on (b::MODE_HOLD_MS since they began) toward where it
		 * went, facing there.
		 */
		const auto *const m{bs.target && bs.memory[*bs.target].valid ? &bs.memory[*bs.target] : nullptr};
		if (in_fight && m && bs.hold.update(false, tick) && !bs.stuck.recovering())
		{
			const auto est{m->pos + m->vel * tick_seconds(tick - m->tick)};
			const auto to{est - pos};
			const double dist{b::length(to)};
			bs.face_dir = b::normalized(to);
			bs.face_rate = {};
			if (bs.turning.phase == b::turn_phase::boost)
				bs.turning.reset();
			const double forward{bs.approach.update(dist, bs.juke.range(), range_hi - range_lo) * b::effective_close_speed(bs.style)};
			b::juke_turn_from_walls(bs.juke, bs.room);
			keys = b::keys_off_walls(b::fight_keys(bs.juke, forward, b::effective_strafe_speed(sk, bs.style), tick < bs.blast_hold_until, false), bs.room);
			use_keys = true;
			mode = b::move_mode::fight_keys;
		}
		/* Section 9.5: hit by an attacker it did not see, the bot turns
		 * to where it was, a reaction time after the hit, and keeps
		 * moving while it turns.
		 */
		else if (bs.turn_to < MAX_PLAYERS && tick >= bs.turn_from && tick < bs.turn_until && bs.memory[bs.turn_to].valid)
		{
			const auto to{bs.memory[bs.turn_to].pos - pos};
			bs.face_dir = b::normalized(to);
			bs.face_rate = {};
			const double err{b::angle_between(frame.f, bs.face_dir)};
			/* Section 9.8: turning round to the attacker, momentum away
			 * from it (not on a path it must follow).
			 */
			const bool path_goal{bs.goal == bot_goal::collect || bs.goal == bot_goal::retreat || bs.goal == bot_goal::refuel};
			auto turn{path_goal ? b::turn_phase::none : bs.turning.update(err, tick, b::length(to), range_lo, bs.rng, bs.tune.turns)};
			/* Out of sight, no boost toward a remembered place: the
			 * turn ends there, so the afterburner (turn_boost) does not
			 * fire for a boost that is not flown (the PR #38 review).
			 */
			if (turn == b::turn_phase::boost)
			{
				bs.turning.reset();
				turn = b::turn_phase::none;
			}
			wanted = turn_velocity(obj, turn, wanted, err, to, vel, frame.r * static_cast<double>(bs.heading_pref), max_speed, bs.tune.turns.reverse_speed);
		}
		/* Section 9.10, corner clearing: to the peek point short of the
		 * corner and wide of it, facing the corner's exit (strafing
		 * across it), until there or the peek's time is up; then on
		 * along the path.
		 */
		else if (bs.pursuit.active && bs.pursuit.peek)
		{
			if (tick >= bs.pursuit.peek_until || b::distance(pos, bs.pursuit.peek_point) < b::CORNER_PEEK_REACHED)
			{
				bs.pursuit.peek = false;
				bs.pursuit.corner_done = true;
			}
			else
			{
				if (bs.turning.phase == b::turn_phase::boost)
					bs.turning.reset();
				bs.face_dir = b::normalized(bs.pursuit.peek_aim - pos);
				bs.face_rate = {};
				wanted = duck_velocity(pos, bs.pursuit.peek_point, max_speed * BOT_PEEK_SPEED);
				mode = b::move_mode::duck;
			}
		}
		/* The PR #47 review: the homing shot round the corner pending,
		 * the bot turns to the corner's exit (flying its path) for the
		 * release.
		 */
		else if (corner_shot_pending(bs))
		{
			if (bs.turning.phase == b::turn_phase::boost)
				bs.turning.reset();
			bs.face_dir = b::normalized(corner_exit(bs, pos) - pos);
			bs.face_rate = {};
		}
		/* Out of sight, no boost toward a remembered place. */
		else if (bs.turning.phase == b::turn_phase::boost)
			bs.turning.reset();
		/* Section 9.6: a heavy missile for the corner the target hides
		 * behind: the bot holds (or keeps ducking) and turns to it.
		 */
		if (bs.missile && bs.heavy_aim && bs.heavy_aim->kind == b::aim_kind::corner)
		{
			const auto shooter{pos + frame.to_world(missile_gun_local(game_secondary(*bs.missile)))};
			bs.face_dir = b::apply_aim_offset(bs.heavy_aim->point - shooter, frame.u, bs.aim.yaw(), bs.aim.pitch());
			bs.face_rate = {};
			wanted = bs.duck_point && tick < bs.duck_until ? duck_velocity(pos, *bs.duck_point, max_speed) : vec3{};
			use_keys = false;
			mode = b::move_mode::duck;
		}
		else if (bs.duck_point && tick < bs.duck_until && bs.target && bs.memory[*bs.target].valid)
		{
			/* Ducked: facing where the target will come from. */
			bs.face_dir = b::normalized(bs.memory[*bs.target].pos - pos);
			bs.face_rate = {};
			wanted = duck_velocity(pos, *bs.duck_point, max_speed);
			use_keys = false;
			mode = b::move_mode::duck;
		}
	}
	/* Section 9.5: the fusion cannon is charged, then released on the
	 * target (bots_fire does it frame by frame).
	 */
#if DXX_BUILD_DESCENT == 2
	{
		const bool fusion{primary_now == primary_weapon_index::fusion};
		bs.fusion_want = b::fusion_step({
			.selected = fusion,
			.charging = bs.fusion_charging,
			.charge = pi.Fusion_charge / 65536.0,
			.energy = pi.energy / 65536.0,
			.target_visible = engaged,
			.shot_clear = bs.shot_clear,
			.aimed = bs.fire,
			.distance = engaged_dist,
			.range = weapon_range(pi),
		}, b::fusion_release_charge(sk.weapon_smarts));
		if (fusion)
			bs.fire = false;
	}
#else
	(void)engaged;
	(void)engaged_dist;
#endif
	const bool dodging{tick >= bs.dodge_from && tick < bs.dodge_until};
	const bool avoiding{tick < bs.avoid_until};
	if (bs.stuck.recovering())
	{
		wanted = bs.recover_dir * max_speed;
		use_keys = false;
		mode = b::move_mode::recover;
	}
	/* Section 9.15: in a fight, the path (and the turn round, the turn to
	 * an attacker) is flown with keys too: the wanted velocity, a key per
	 * axis (b::path_keys); not aiming (the nose along the path), fewer
	 * strafe keys.  (A juke across the path as in the fight was tried:
	 * the -botarena games of exp-31's levels reversed the strafe 90 to
	 * 93 times a minute with it, 72 to 77 without, the human 36 to 52.)
	 */
	else if (!use_keys && in_fight && mode != b::move_mode::duck)
	{
		keys = bs.path_keys.update(frame.to_local(b::path_key_command(wanted, vel, max_speed)), tick, !aim_set || (bs.fleeing && bs.flee_turned));
		use_keys = true;
		if (mode != b::move_mode::turn_keys)
			mode = b::move_mode::path_keys;
	}
	/* The path's keys start afresh after another movement. */
	if (mode != b::move_mode::path_keys && mode != b::move_mode::turn_keys)
		bs.path_keys.reset();
	std::array<bool, 2> immediate{};
	if (use_keys)
	{
		/* Section 9.15: the dodge is one key at full thrust; the push off
		 * a wall turns a key held into it (only that one skips the
		 * strafe's filter) or presses a free one.
		 */
		if (dodging)
			keys = b::dodge_key(keys, frame.to_local(bs.dodge_dir));
		if (avoiding)
		{
			const auto a{b::avoid_keys(keys, frame.to_local(bs.avoid), max_speed)};
			keys = a.keys;
			immediate = a.immediate;
		}
		bs.move_cmd = frame.to_world(keys.local());
	}
	else
	{
		if (dodging && mode != b::move_mode::recover)
			wanted += bs.dodge_dir * max_speed;
		if (avoiding)
			wanted += bs.avoid;
		bs.move_cmd = b::velocity_command(wanted, vel, max_speed);
		/* The review of PR #63: the push off a wall does not wait for
		 * the filter (a strafe key held toward the wall, the push the
		 * other way was released for up to b::KEY_FLIP_TICKS: the bot
		 * slid on into it).
		 */
		immediate.fill(dodging || bs.stuck.recovering() || avoiding);
	}
	/* Section 9.12: the strafe keys do not flip at the tick rate. */
	bs.move_cmd = frame.to_world(bs.lateral.apply(frame.to_local(bs.move_cmd), tick, immediate));
	bs.mode = mode;
	if (!aim_set)
		bs.aim_dir = bs.face_dir;
	/* Section 9.15: the afterburner's check of the way the bot wants to
	 * go: the fight's keys and the slide by their thrust (full forward
	 * alone is the nose's way), a path or a turn flown with keys by the
	 * velocity wanted (the keys follow it; the afterburner pushes along
	 * the nose while the strafe keys correct).  PR #63 gave it nothing
	 * for keys, and a bot never burnt in a fight.
	 */
	decide_afterburner(bs, obj, use_keys && mode != b::move_mode::path_keys && mode != b::move_mode::turn_keys ? frame.to_world(keys.local()) * max_speed : wanted, tick);
	missile_tick(bs, obj, tick, p);
	if (bot_log_on() && tick >= bs.next_log_tick)
	{
		bs.next_log_tick = tick + BOT_LOG_TICKS;
		log_summary(bs, obj, tick);
	}
}

/* Section 3.4: the controls of this frame, from the direction and
 * thrust fixed at the last tick and the ship's current state.
 */
void steer(bot_state &bs, const object &obj)
{
	auto &c{bs.ctl};
	if (bs.life != bot_life::alive || obj.type != object_type::OBJ_PLAYER)
	{
		c = {};
		return;
	}
	/* The rotation applies to the orientation without the turn roll
	 * (do_physics_sim_rot); the thrust to the orientation with it
	 * (apply_pilot_controls).
	 */
	vms_matrix unrolled{obj.orient};
	if (const fixang roll{obj.mtype.phys_info.turnroll})
		unrolled = vm_matrix_x_matrix(obj.orient, vm_angles_2_matrix(vms_angvec{.p = 0, .b = static_cast<fixang>(-roll), .h = 0}));
	constexpr double rev_to_rad{2 * std::numbers::pi / 65536.0};
	const auto &rotvel{obj.mtype.phys_info.rotvel};
	const auto out{b::steer_controls({
		.unrolled = to_frame(unrolled),
		.thrust_frame = to_frame(obj.orient),
		.face_dir = bs.face_dir,
		.face_rate = bs.face_rate,
		.move_cmd = bs.move_cmd,
		.pitch_rate = rotvel.x * rev_to_rad,
		.heading_rate = rotvel.y * rev_to_rad,
	}, B.limits.turn, bs.skill.turn_cap, bs.heading_pref)};
	c = {out.pitch, out.heading, out.forward, out.sideways, out.vertical};
}

void compute_limits()
{
	const auto lin{compute_thrust_response(Player_ship->mass, Player_ship->drag, Player_ship->max_thrust)};
	const auto rot{compute_rotation_response(Player_ship->mass, Player_ship->drag, Player_ship->max_rotthrust)};
	B.limits.max_speed = lin.steady_velocity / 65536.0;
	B.limits.turn = {
		.max_rate = rot.steady_velocity * 2 * std::numbers::pi / 65536.0,
		.time_constant = rot.time_constant,
	};
	if (!(B.limits.max_speed > 1))
		B.limits.max_speed = 50;
	if (!(B.limits.turn.max_rate > 0.1))
		B.limits.turn = {.max_rate = std::numbers::pi, .time_constant = 0.1};
	con_printf(CON_VERBOSE, "bots: ship top speed %.1f, turn rate %.0f deg/s, time constant %.3f s", B.limits.max_speed, B.limits.turn.max_rate * 180 / std::numbers::pi, B.limits.turn.time_constant);
}

/* Spawn invulnerability, as init_player_stats_new_ship gives the human. */
void give_spawn_invulnerability(object &obj)
{
	if (!Netgame.InvulAppear)
		return;
	auto &pi{obj.ctype.player_info};
	pi.powerup_flags |= player_flag::invulnerable;
	pi.invulnerable_time = GameTime64 - (i2f(58 - Netgame.InvulAppear) >> 1);
	pi.FakingInvul = 1;
}

/* A new ship for the bot: spawn grants, invulnerability, weapon, and
 * the announcement (PLAYER_SPAWN as the bot, then its inventory).
 */
void new_ship(bot_state &bs, object &obj)
{
	give_spawn_invulnerability(obj);
	obj.ctype.player_info.Fusion_charge = 0;
	bs.fusion_charging = false;
	choose_weapon(bs, obj, std::nullopt);
	bs.reset_for_life(B.tick.tick());
	create_player_appearance_effect(Vclip, obj);
	net_combat_send_spawn(bs.pid);
	net_objects_host_own_ship_inventory(bs.pid, true);
}

/* Section 4.8, step 1: the tumble (the kill was announced by the host's
 * PLAYER_KILLED when the damage was applied).
 */
void start_death(bot_state &bs, object &obj)
{
	bs.death_pending = false;
	stop_afterburner(bs, obj);
	end_pursuit(bs, b::pursuit_end::died, B.tick.tick());
	bs.life = bot_life::dying;
	bs.pl.dead_state = player_dead_state::yes;
	bs.died_at = GameTime64;
	bs.ctl = {};
	bs.fire = false;
	obj.mtype.phys_info.thrust = {};
	obj.mtype.phys_info.rotthrust = {};
	/* The kill itself was the host's combat decision (PLAYER_KILLED,
	 * net_combat.cpp); the bot's robots are released, as a human's are.
	 */
	multi_strip_robots(bs.pid);
}

/* Section 4.8, step 2: the explosion.  The deres goes out as the bot's,
 * after its inventory (multi_send_player_deres brings the host's copy up
 * to date); the host drops the eggs from that copy, as for any player.
 */
void explode(bot_state &bs, object &obj, const d_robot_info_array &Robot_info)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &&objp{Objects.vmptridx(&obj)};
	multi_send_player_deres(deres_explode, bs.pid);
	drop_player_armed_bombs(objp);
	net_objects_host_drop_player_eggs(bs.pid);
	explode_badass_player(Robot_info, objp);
	objp->flags &= ~OF_SHOULD_BE_DEAD;
	multi_make_player_ghost(bs.pid);
	auto &pi{objp->ctype.player_info};
	pi.powerup_flags &= ~(player_flag::cloaked | player_flag::invulnerable);
#if DXX_BUILD_DESCENT == 2
	pi.powerup_flags &= ~player_flag::has_team_flag;
#endif
	bs.life = bot_life::dead;
	bs.respawn_at = GameTime64 + to_fix(bs.rng.uniform(BOT_RESPAWN_MIN_S, BOT_RESPAWN_MAX_S));
}

/* Section 4.8, step 3: respawn at a spawn site. */
void respawn(bot_state &bs, object &obj)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto spawn{bot_spawn(bs)};
	if (spawn.what == spawn_choice::kind::none)
	{
		bs.respawn_at = GameTime64 + F1_0;
		return;
	}
	place_player(vmsegptridx, Objects.vmptridx(&obj), spawn);
	multi_make_ghost_player(bs.pid);
	new_ship(bs, obj);
}

/* Section 4.7: a fuel centre gives energy, a repair centre shields, to
 * a ship in it, as object_move_one gives the local player's (up to 100,
 * Fuelcen_give_amount per second); the sound is heard on the host.
 */
void refuel_frame(bot_state &bs, object &obj)
{
	const shared_segment &seg{*vcsegptr(obj.segnum)};
	fix *level{nullptr};
	if (seg.special == segment_special::fuelcen)
		level = &obj.ctype.player_info.energy;
	else if (seg.special == segment_special::repaircen)
		level = &obj.shields;
	else
		return;
	const fix full{seg.special == segment_special::fuelcen ? INITIAL_ENERGY : INITIAL_SHIELDS};
	if (*level >= full)
		return;
	*level += std::min(fixmul(FrameTime, BOT_FUELCEN_RATE), full - *level);
	if (GameTime64 >= bs.fuel_sound_at || bs.fuel_sound_at > GameTime64 + F1_0)
	{
		bs.fuel_sound_at = GameTime64 + BOT_FUELCEN_SOUND_DELAY;
		auto &Objects = LevelUniqueObjectState.Objects;
		digi_link_sound_to_object(sound_effect::SOUND_REFUEL_STATION_GIVING_FUEL, Objects.vcptridx(&obj), 0, F1_0 / 2, sound_stack::allow_stacking);
	}
}

/* Section 4.7: the energy to shield converter, as the human's key
 * works it (transfer_energy_to_shield: only the energy above 100, 20 a
 * second, two for one), without its HUD text and local sound.
 */
void convert_frame(const bot_state &bs, object &obj)
{
#if DXX_BUILD_DESCENT == 2
	auto &pi{obj.ctype.player_info};
	if (!has_flag(pi, player_flag::converter) || !b::want_convert(obj.shields / 65536.0, pi.energy / 65536.0, bs.skill.weapon_smarts))
		return;
	constexpr fix converter_rate{i2f(20)};
	constexpr fix converter_scale{2};
	const fix e{std::min({fixmul(FrameTime, converter_rate), pi.energy - INITIAL_ENERGY, (MAX_SHIELDS - obj.shields) * converter_scale})};
	if (e <= 0)
		return;
	pi.energy -= e;
	obj.shields += e / converter_scale;
#else
	(void)bs;
	(void)obj;
#endif
}

void life_frame(bot_state &bs, object &obj, const d_robot_info_array &Robot_info)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	switch (bs.life)
	{
		case bot_life::alive:
		{
			if (bs.death_pending)
			{
				start_death(bs, obj);
				break;
			}
			/* do_invulnerable_stuff, for the bot: an invulnerability
			 * powerup that runs out is replaced in the level, as the
			 * human's is.
			 */
			auto &pi{obj.ctype.player_info};
			if (+(pi.powerup_flags & player_flag::invulnerable) && GameTime64 > pi.invulnerable_time + INVULNERABLE_TIME_MAX)
			{
				pi.powerup_flags &= ~player_flag::invulnerable;
				if (pi.FakingInvul)
					pi.FakingInvul = 0;
				else
					maybe_drop_net_powerup(powerup_type_t::POW_INVULNERABILITY, 1, 0);
			}
			refuel_frame(bs, obj);
			convert_frame(bs, obj);
#if DXX_BUILD_DESCENT == 2
			/* Section 9.5: the omega cannon recharges from the bot's
			 * energy, as the human's does (game.cpp), with the bot's
			 * pilot; do_omega_stuff takes the shots' charge.
			 */
			omega_charge_frame(bs.pl, pi);
#endif
			break;
		}
		case bot_life::dying:
		{
			const fix64 t{GameTime64 - bs.died_at};
			const fix left{static_cast<fix>(std::max<fix64>(0, BOT_DEATH_EXPLODE_TIME - t))};
			auto &rotvel{obj.mtype.phys_info.rotvel};
			rotvel.x = left / 4;
			rotvel.y = left / 2;
			rotvel.z = left / 3;
			if (t > BOT_DEATH_EXPLODE_TIME)
			{
				explode(bs, obj, Robot_info);
				break;
			}
			if (bs.rng.uniform() * 32768 < FrameTime * 4)
			{
				multi_send_create_explosion(bs.pid);
				create_small_fireball_on_object(Objects.vmptridx(&obj), F1_0, 1);
			}
			break;
		}
		case bot_life::dead:
			/* No respawn during the countdown: dead in the mine. */
			if (GameTime64 >= bs.respawn_at && !LevelUniqueObjectState.ControlCenterState.Control_center_destroyed && vcplayerptr(bs.pid)->connected == player_connection_status::playing)
				respawn(bs, obj);
			break;
	}
}

/* `departed`: a player who left counts too (section 6.4: a human may
 * come back by callsign, so a bot added or renamed during the game
 * must not take the name).
 */
[[nodiscard]]
bool callsign_taken(const callsign_t &name, const playernum_t except, const bool departed = false)
{
	for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
	{
		if (i == except)
			continue;
		const bool in_game{i == Player_num || Netgame.players[i].connected != player_connection_status::disconnected || B.bots[i] || (departed && Netgame.players[i].callsign[0u])};
		if (in_game && !d_stricmp(Netgame.players[i].callsign, name))
			return true;
	}
	return false;
}

/* Section 2.2: unique against every callsign in the game
 * ("havoc" -> "havoc2").
 */
callsign_t unique_callsign(const callsign_t &wanted, const playernum_t slot, const bool departed = false)
{
	if (!callsign_taken(wanted, slot, departed) && wanted[0u])
		return wanted;
	const char *const base{wanted[0u] ? static_cast<const char *>(wanted) : "bot"};
	for (unsigned k = 2; k < 100; ++k)
	{
		char suffix[4];
		const auto sl{static_cast<std::size_t>(std::snprintf(suffix, sizeof(suffix), "%u", k))};
		char buf[CALLSIGN_LEN + 1]{};
		const auto keep{std::min(std::strlen(base), CALLSIGN_LEN - sl)};
		std::memcpy(buf, base, keep);
		std::memcpy(buf + keep, suffix, sl);
		callsign_t c{};
		c.copy_lower(std::span<const char>(buf, keep + sl));
		if (!callsign_taken(c, slot, departed))
			return c;
	}
	return wanted;
}

/* A bot leaving (kicked, or its slot released) puts its afterburner out
 * first: the loop is linked to its ship object, which stays behind as a
 * ghost, so on the host and the clients it would otherwise play on
 * until the level ends, into the tenure of whoever takes the slot next.
 */
void put_out_afterburner(bot_state &bs)
{
	if (bs.burning && vcplayerptr(bs.pid)->objnum != object_none)
		stop_afterburner(bs, ship_of(bs.pid));
	bs.burning = false;
}

}

bool bot_is_local(const playernum_t pnum)
{
	return find_bot(pnum) != nullptr;
}

bool bot_movement_state(const playernum_t pnum, uint8_t &mode, uint8_t &goal)
{
	const auto bs{find_bot(pnum)};
	if (!bs)
		return false;
	mode = static_cast<uint8_t>(bs->life == bot_life::alive ? bs->mode : b::move_mode::none);
	goal = static_cast<uint8_t>(bs->goal);
	return true;
}

void bot_slot_released(const playernum_t pnum)
{
	if (pnum < MAX_PLAYERS && B.bots[pnum])
	{
		con_printf(CON_VERBOSE, "bots: P#%u is no longer a bot", pnum);
		put_out_afterburner(*B.bots[pnum]);
		B.bots[pnum].reset();
	}
}

namespace {

/* A bot leaves the game (kicked, or replaced by a human): a bot still
 * tumbling explodes first, otherwise the host's copy of its inventory is
 * brought up to date; multi_disconnect_player then drops its eggs, makes
 * the ship a ghost and tells the others (PLAYER_LEFT with `why`).
 */
bool remove_bot(const playernum_t pnum, const kick_player_reason why)
{
	const auto bs{find_bot(pnum)};
	if (!bs || !multi_i_am_master())
		return false;
	put_out_afterburner(*bs);
	auto &obj{ship_of(pnum)};
	if (obj.type == object_type::OBJ_PLAYER)
	{
		/* Killed and still tumbling: it explodes now (the deres, its
		 * eggs), as it would have.  Otherwise the host's copy of its
		 * inventory is brought up to date, and multi_disconnect_player
		 * drops the eggs from it.
		 */
		if (bs->life == bot_life::dying && Network_status == network_state::playing)
			explode(*bs, obj, LevelSharedRobotInfoState.Robot_info);
		else
			net_objects_host_own_ship_inventory(pnum, true);
	}
	net_v2::host_remove_player(pnum, why);
	B.bots[pnum].reset();
	return true;
}

}

bool bots_kick(const playernum_t pnum)
{
	return remove_bot(pnum, kick_player_reason::kicked);
}

bool bots_remove_for_human(const playernum_t pnum)
{
	const auto bs{find_bot(pnum)};
	if (!bs)
		return false;
	con_printf(CON_NORMAL, "bots: '%s' (P#%u) leaves to make room for a human", static_cast<const char *>(bs->cfg.name), pnum);
	return remove_bot(pnum, kick_player_reason::quit);
}

unsigned bot_added_order(const playernum_t pnum)
{
	const auto bs{find_bot(pnum)};
	return bs ? bs->added : 0;
}

bool bots_replaceable()
{
	return Bot_game.replace;
}

bool bot_ship_dying(const playernum_t pnum)
{
	const auto bs{find_bot(pnum)};
	return bs && bs->life == bot_life::dying;
}

void bots_session_reset()
{
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		/* The slot is free again (a game that did not start keeps the
		 * lobby's player list).
		 */
		const auto pid{o->pid};
		vmplayerptr(pid)->connected = player_connection_status::disconnected;
		Netgame.players[pid].connected = player_connection_status::disconnected;
		Netgame.players[pid].callsign = {};
		o.reset();
	}
	clear_player_bot_flags();
	B.graph.clear();
	B.side_centres.clear();
	B.tick_started = false;
}

unsigned bots_allocate_slots()
{
	bots_session_reset();
	if (!(Game_mode & GM_NETWORK) || !multi_i_am_master())
		return 0;
	bots_setup_init();
	/* Section 9.13: the style profiles as the folder has them now. */
	bots_load_styles(true);
	/* Section 6.4: the options of this game start as the setup's. */
	Bot_game = {Bot_setup.default_skill, Bot_setup.default_style, Bot_setup.default_profile, Bot_setup.replace};
	if (!bots_allowed_in_mode(Netgame.gamemode))
		return 0;
	unsigned placed{0};
	const unsigned limit{std::min<unsigned>(Netgame.max_numplayers, MAX_PLAYERS)};
	for (unsigned k = 0; k < Bot_setup.count; ++k)
	{
		/* Section 2.3: the lowest free slot below the player limit.  A
		 * slot whose connection still exists (a lobby player left out of
		 * the game, whose kick lingers) is not free: the end of that
		 * connection would disconnect the slot.
		 */
		per_player_array<b::slot_view> views{};
		for (auto &&[s, v] : enumerate(views))
		{
			const auto pn{static_cast<playernum_t>(s)};
			v.occupied = B.bots[pn].has_value() || vcplayerptr(pn)->connected != player_connection_status::disconnected;
			v.has_peer = net_v2::host_slot_has_peer(pn);
			v.reserved = pn < N_players && Netgame.players[pn].callsign[0u];
		}
		const auto chosen{b::choose_bot_slot(views, limit)};
		if (!chosen)
			break;
		const playernum_t slot{static_cast<playernum_t>(*chosen)};
		const auto &cfg{Bot_setup.bots[k]};
		const auto name{unique_callsign(cfg.name, slot)};
		auto &np{Netgame.players[slot]};
		np.callsign = name;
		np.rank = netplayer_info::player_rank::None;
		np.connected = player_connection_status::playing;
		np.protocol.udp.addr = {};
		np.LastPacketTime = timer_query();
		np.ping = 0;
		auto &plr{*vmplayerptr(slot)};
		plr.callsign = name;
		plr.connected = player_connection_status::playing;
		ship_of(slot).ctype.player_info.KillGoalCount = 0;
		auto &bs{B.bots[slot].emplace(slot, cfg)};
		bs.cfg.name = name;
		/* Section 2.3: the order of addition (a joining human replaces
		 * the last one); section 2.2: the PLAYER_LIST flag.
		 */
		bs.added = k + 1;
		set_player_is_bot(slot, true);
		if (slot >= N_players)
			N_players = slot + 1;
		++placed;
		con_printf(CON_NORMAL, "bots: '%s' takes P#%u (%s, %s)", static_cast<const char *>(name), slot, b::bot_skill_names[static_cast<unsigned>(bs.skill_level) % b::BOT_SKILL_COUNT], bs.cfg.profile[0] ? bs.cfg.profile.data() : b::bot_style_names[static_cast<unsigned>(bs.cfg.style) % b::BOT_STYLE_COUNT]);
	}
	Netgame.numplayers = N_players;
	if (placed < Bot_setup.count)
		con_printf(CON_URGENT, "bots: only %u of %u bots fit below the player limit of %u", placed, Bot_setup.count, limit);
	return placed;
}

void bots_apply_team_preferences(unsigned &team_vector, const unsigned num_players)
{
	for (playernum_t i = 0; i < num_players && i < MAX_PLAYERS; ++i)
		if (const auto bs{find_bot(i)})
		{
			if (bs->cfg.team == b::bot_team::blue)
				team_vector &= ~(1u << i);
			else if (bs->cfg.team == b::bot_team::red)
				team_vector |= 1u << i;
		}
}

namespace {

/* The level's data for the bots: the navigation graph, the ship limits,
 * the spawn sites, the weapon supply; the tick starts.  At the level
 * start, or when the first bot of the level is added during it.
 */
void prepare_level()
{
	build_nav_graph();
	compute_limits();
	judge_spawn_sites();
	/* Section 9.8: the level's supply of weapons against its players
	 * (a weapon-poor level: the bots fight with what they have).
	 */
	{
		auto &Objects = LevelUniqueObjectState.Objects;
		unsigned weapons{0};
		for (const object &o : Objects.vcptr)
			if (o.type == object_type::OBJ_POWERUP && b::is_weapon_item(item_of(get_powerup_id(o)).kind))
				++weapons;
		unsigned players{0};
		for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
			if (vcplayerptr(i)->connected == player_connection_status::playing)
				++players;
		B.weapon_powerups = weapons;
		B.weapon_poor = b::weapon_poor_level(weapons, players);
		con_printf(CON_VERBOSE, "bots: level has %u weapon powerups for %u players%s", weapons, players, B.weapon_poor ? ": weapon-poor, the bots fight with what they have" : "");
	}
	B.tick = ::dcx::net_interp::tick_accumulator{b::BOT_TICK_RATE};
	B.tick.reset(GameTime64);
	B.last_time = GameTime64;
	B.tick_started = true;
}

/* A bot's state for the level (at its start, or as it joins it). */
void enter_level(bot_state &bs)
{
	bs.rng.seed(b::bot_seed(Netgame.protocol.udp.session_id, bs.pid, Current_level_num));
	bs.last_attacker = 0xff;
	bs.memory = {};
	bs.powerups.clear();
	bs.visited.assign(B.graph.size(), 0);
}

}

void bots_level_start()
{
	if (!bots_running() || std::ranges::none_of(B.bots, [](const auto &o) { return o.has_value(); }))
	{
		/* No bot in this level (yet): one added during it prepares the
		 * level then, not with the previous level's graph.
		 */
		B.tick_started = false;
		return;
	}
	prepare_level();
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		auto &bs{*o};
		enter_level(bs);
		if (vcplayerptr(bs.pid)->connected != player_connection_status::playing)
			continue;
		auto &obj{ship_of(bs.pid)};
		obj.type = object_type::OBJ_PLAYER;
		obj.control_source = object::control_type::remote;
		obj.movement_source = object::movement_type::physics;
		init_player_stats_new_ship(bs.pid);
		auto &Objects = LevelUniqueObjectState.Objects;
		/* Section 9.5: a bot placed in a sealed cell moves to a site it
		 * can fly out of (a human shoots the switch; a bot does not).
		 */
		if (const uint32_t seg{obj.segnum}; +(Game_mode & GM_MULTI) && !(Game_mode & GM_MULTI_COOP) && !b::spawn_site_open(reachable_from(seg), B.graph.size()))
		{
			const auto spawn{bot_spawn(bs)};
			if (spawn.what == spawn_choice::kind::site && spawn.site < MAX_PLAYERS && B.site_open[spawn.site])
			{
				con_printf(CON_VERBOSE, "bots: '%s' starts in a sealed cell (segment %u): moved to spawn site %u", static_cast<const char *>(bs.cfg.name), seg, spawn.site);
				place_player(vmsegptridx, Objects.vmptridx(&obj), spawn);
			}
		}
		new_ship(bs, obj);
	}
}

void bots_level_end()
{
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		auto &plr{*vmplayerptr(o->pid)};
		/* The kill list waits for every player still in the level. */
		if (plr.connected == player_connection_status::playing)
			plr.connected = player_connection_status::end_menu;
		o->death_pending = false;
		o->ctl = {};
		o->fire = false;
		if (plr.objnum != object_none)
			stop_afterburner(*o, ship_of(o->pid));
	}
}

void bots_frame(const d_robot_info_array &Robot_info)
{
	if (!B.tick_started || !bots_running())
		return;
	if (Network_status != network_state::playing)
		return;
	/* The game clock went back (it restarts with each level): the tick
	 * restarts with it.
	 */
	if (GameTime64 < B.last_time)
		B.tick.reset(GameTime64);
	B.last_time = GameTime64;
	const unsigned ticks{B.tick.advance(GameTime64)};
	const uint32_t first{B.tick.tick() - ticks};
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		auto &bs{*o};
		/* Section 9.8: the host's own displays read the slot's flag too;
		 * a bot the host flies is marked whatever cleared it.
		 */
		set_player_is_bot(bs.pid, true);
		const auto connected{vcplayerptr(bs.pid)->connected};
		if (connected != player_connection_status::playing)
		{
			bs.ctl = {};
			bs.fire = false;
			/* Killed during the countdown (D2 marks it died in the mine
			 * with the kill): the death sequence still runs to the
			 * explosion, as the human's does; it does not respawn.
			 */
			if (connected == player_connection_status::died_in_mine && bs.life != bot_life::alive)
				life_frame(bs, ship_of(bs.pid), Robot_info);
			continue;
		}
		auto &obj{ship_of(bs.pid)};
		life_frame(bs, obj, Robot_info);
		for (unsigned k = 0; k < ticks; ++k)
			bot_tick(bs, first + k);
		steer(bs, obj);
	}
}

void bots_fire()
{
	if (!B.tick_started || !bots_running() || Network_status != network_state::playing)
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	for (auto &o : B.bots)
	{
		if (!o || o->life != bot_life::alive)
			continue;
		auto &bs{*o};
		const auto &&objp{Objects.vmptridx(vcplayerptr(bs.pid)->objnum)};
		if (objp->type != object_type::OBJ_PLAYER)
			continue;
#if DXX_BUILD_DESCENT == 2
		/* The afterburner's trail, after the move, as the human's
		 * (object_move_one).
		 */
		if (bs.pl.drop_afterburner_blob_flag)
		{
			bs.pl.drop_afterburner_blob_flag = 0;
			drop_afterburner_blobs(*objp, 2, i2f(5) / 2, -1);
			multi_send_drop_blobs(bs.pid);
		}
#endif
		/* Stage B4: the missile or mine the brain released, as the
		 * human fires one (do_missile_firing: FIRE as the bot);
		 * a volley's further rounds follow frame by frame, as the
		 * human's Global_missile_firing_count does.
		 */
		if (bs.missile_fire)
		{
			const auto w{game_secondary(*bs.missile_fire)};
			auto &pi{objp->ctype.player_info};
			const bool volley_on{bs.missile_volley < std::max<unsigned>(1, Weapon_info[Secondary_weapon_to_weapon_info[w]].fire_count)};
			if (!pi.secondary_ammo[w])
				bs.missile_fire.reset();
			else if (volley_on || allowed_to_fire_missile(pi))
			{
				do_missile_firing(bs.pl, w, objp);
				if (!bs.missile_volley || !--bs.missile_volley)
					bs.missile_fire.reset();
			}
		}
#if DXX_BUILD_DESCENT == 2
		/* Section 9.5: the fusion cannon, as FireLaser charges the
		 * human's: 2 energy to start, then 1 a second, the charge in the
		 * ship's Fusion_charge (the shot's damage grows with it); the
		 * shot goes through do_laser_firing_player (FIRE with the
		 * charge, as the human's).  The warm-up is heard on the host.
		 */
		{
			auto &pi{objp->ctype.player_info};
			const bool fusion{pi.Primary_weapon == primary_weapon_index::fusion};
			if (bs.fusion_charging && !fusion)
			{
				/* Switched away (the rearm): what was charged is lost,
				 * as it is not the weapon that would fire.
				 */
				bs.fusion_charging = false;
				pi.Fusion_charge = 0;
			}
			if (fusion && (bs.fusion_want == b::fusion_action::charge || (bs.fusion_charging && bs.fusion_want != b::fusion_action::release)))
			{
				if (!bs.fusion_charging)
				{
					if (!allowed_to_fire_laser(bs.pl, pi) || pi.energy < F1_0 * 2)
						continue;
					bs.fusion_charging = true;
					pi.Fusion_charge = 0;
					pi.energy -= F1_0 * 2;
				}
				pi.Fusion_charge += FrameTime;
				pi.energy -= FrameTime;
				if (pi.energy <= 0)
				{
					pi.energy = 0;
					bs.fusion_want = b::fusion_action::release;
				}
				if (GameTime64 >= bs.fusion_sound_at || bs.fusion_sound_at > GameTime64 + F1_0)
				{
					bs.fusion_sound_at = GameTime64 + F1_0 / 4;
					digi_link_sound_to_object(sound_effect::SOUND_FUSION_WARMUP, objp, 0, F1_0, sound_stack::allow_stacking);
				}
			}
			if (fusion && bs.fusion_charging && bs.fusion_want == b::fusion_action::release)
			{
				if (!allowed_to_fire_laser(bs.pl, pi))
					continue;
				const double charged{pi.Fusion_charge / 65536.0};
				do_laser_firing_player(bs.pl, objp);
				bs.fusion_charging = false;
				pi.Fusion_charge = 0;
				bs.fusion_want = b::fusion_action::idle;
				if (bot_log_on())
					con_printf(CON_VERBOSE, "bots: '%s' fires fusion, charge %.2f s", static_cast<const char *>(bs.cfg.name), charged);
				continue;
			}
			if (fusion)
				continue;
		}
#endif
		if (!bs.fire)
			continue;
		if (!allowed_to_fire_laser(bs.pl, objp->ctype.player_info))
			continue;
#if DXX_BUILD_DESCENT == 2
		/* Section 9.5: no omega shot without the charge for it (the host
		 * would delete it, the clients would draw it).
		 */
		if (const auto &pi{objp->ctype.player_info}; pi.Primary_weapon == primary_weapon_index::omega && !b::omega_can_fire(pi.Omega_charge / static_cast<double>(MAX_OMEGA_CHARGE), pi.energy / 65536.0))
			continue;
#endif
		do_laser_firing_player(bs.pl, objp);
	}
}

void bot_apply_controls(object &obj)
{
	if (obj.type != object_type::OBJ_PLAYER)
		return;
	const auto bs{find_bot(get_player_id(obj))};
	if (!bs || !bots_running())
		return;
	auto &c{B.controls};
	/* The host's exit sequence moves the objects without the game frame:
	 * the bots coast.
	 */
	const bot_controls bc{Endlevel_sequence ? bot_controls{} : bs->ctl};
	/* An axis held for the whole frame is FrameTime (the human's keys);
	 * rounded, not truncated toward minus infinity as fixmul would, so
	 * that at 500 fps (FrameTime 131) a small axis is not biased.
	 */
	const auto held{[](const double axis) {
		return b::held_axis_time(axis, FrameTime);
	}};
	c.pitch_time = held(bc.pitch);
	c.heading_time = held(bc.heading);
	c.bank_time = 0;
	c.forward_thrust_time = held(bc.forward);
	c.sideways_thrust_time = held(bc.sideways);
	c.vertical_thrust_time = held(bc.vertical);
#if DXX_BUILD_DESCENT == 2
	c.state.afterburner = bc.forward > 0 && bs->burning && !Endlevel_sequence;
#endif
	apply_pilot_controls(obj, bs->pl, c);
}

bool bot_take_damage(object &ship, const icobjptridx_t killer, const fix damage, const bool check_friendly_fire)
{
	if (ship.type != object_type::OBJ_PLAYER)
		return false;
	const auto pid{get_player_id(ship)};
	const auto bs{find_bot(pid)};
	if (!bs)
		return false;
	if (bs->life != bot_life::alive || bs->death_pending)
		return true;
	auto &pi{ship.ctype.player_info};
	if (+(pi.powerup_flags & player_flag::invulnerable))
		return true;
	if (check_friendly_fire && multi_maybe_disable_friendly_fire(static_cast<const object *>(killer), pid))
		return true;
	if (Endlevel_sequence)
		return true;
	/* Section 4.2: a hit tells the bot roughly where the attacker is.  A
	 * teammate's hit (friendly fire on) is no attack: no memory of it, no
	 * evasion, no turn, no target.
	 */
	if (killer != object_none && killer->type == object_type::OBJ_PLAYER)
	{
		const auto who{get_player_id(*killer)};
		if (who != pid && who < MAX_PLAYERS && !same_team(pid, who))
		{
			const auto tick{B.tick.tick()};
			bs->last_attacker = who;
			bs->attacked_tick = tick;
			/* Section 9.10: the attacker, if a bot, landed a hit (its
			 * hits on a human show only as the human's shields: the
			 * damage seen, b::pursuit_view).
			 */
			if (const auto attacker{find_bot(who)})
			{
				attacker->pursuit.hit_who = static_cast<uint8_t>(pid);
				attacker->pursuit.hit_tick = tick;
			}
			auto &m{bs->memory[who]};
			if (!m.valid || tick - m.tick > b::PERCEPTION_DIVISOR * 4)
			{
				const vec3 err{bs->rng.uniform(-20, 20), bs->rng.uniform(-20, 20), bs->rng.uniform(-20, 20)};
				m = {
					.valid = true,
					.pos = to_vec(killer->pos) + err,
					.vel = {},
					.segment = static_cast<uint16_t>(killer->segnum),
					.tick = tick,
				};
			}
			/* Section 9.5: an attacker it does not see (behind it): evade
			 * at once, turn to it a reaction time later.
			 */
			const auto pos{to_vec(ship.pos)};
			const auto frame{to_frame(ship.orient)};
			const auto to_attacker{m.pos - pos};
			const bool seen{bs->visible_now[who] && b::in_field_of_view(frame.f, to_attacker, bs->skill.fov_half_deg)};
			const auto reaction{b::react_to_hit({
				.attacker_seen = seen,
				.shields = (ship.shields - damage) / 65536.0,
				.retreat_shields = bs->retreat_shields,
				.invulnerable = false,
				.evading = tick < bs->dodge_until && bs->turn_to == who,
			})};
			if (reaction != b::hit_reaction::none)
			{
				const auto side{frame.r * (bs->rng.uniform() < 0.5 ? -1.0 : 1.0)};
				bs->dodge_dir = b::evade_direction(-to_attacker, to_vec(ship.mtype.phys_info.velocity), side);
				bs->dodge_from = tick;
				bs->dodge_until = tick + BOT_EVADE_TICKS;
				bs->turn_to = who;
				bs->turn_from = tick + b::ticks_from_ms(bs->skill.reaction_ms);
				bs->turn_until = bs->turn_from + BOT_TURN_TO_ATTACKER_TICKS;
				if (!bs->target || !bs->visible_now[*bs->target])
					bs->target = who;
				if (bot_log_on())
					con_printf(CON_VERBOSE, "bots: '%s' hit by unseen P#%u (%.0f units, %s): evades, turns in %u ms", static_cast<const char *>(bs->cfg.name), who, b::length(to_attacker), reaction == b::hit_reaction::evade_flee ? "weak, flees" : "fights", bs->skill.reaction_ms);
			}
		}
	}
	ship.shields -= damage;
	if (ship.shields < 0)
	{
		pi.killer_objnum = killer;
		/* The death starts with the next frame, as the human's does
		 * (obj_delete_all_that_should_be_dead), outside the collision
		 * handling.
		 */
		bs->death_pending = true;
	}
	return true;
}

bool bot_touch_powerup(object &ship, const vmobjptridx_t powerup)
{
	if (ship.type != object_type::OBJ_PLAYER)
		return false;
	const auto pid{get_player_id(ship)};
	const auto bs{find_bot(pid)};
	if (!bs || !bots_running())
		return false;
	if (bs->life != bot_life::alive || bs->death_pending || Endlevel_sequence || Network_status != network_state::playing)
		return true;
	auto &pi{ship.ctype.player_info};
	const auto id{get_powerup_id(powerup)};
	switch (id)
	{
		/* Keys stay in a multiplayer level, and every player may take
		 * one (do_powerup): the bot can then open those doors.
		 */
		case powerup_type_t::POW_KEY_BLUE:
			pi.powerup_flags |= player_flag::blue_key;
			return true;
		case powerup_type_t::POW_KEY_RED:
			pi.powerup_flags |= player_flag::red_key;
			return true;
		case powerup_type_t::POW_KEY_GOLD:
			pi.powerup_flags |= player_flag::gold_key;
			return true;
		default:
			break;
	}
	/* Stage B3 (section 4.7): the host's decision, as for every player's
	 * pickup (net_objects.cpp), granted at once: the host is the
	 * authority and the bot is its own ship.
	 */
	uint8_t deny{};
	if (!net_objects_bot_touch(pid, powerup, deny))
	{
		/* Section 9.5, the log: a denied touch (once per powerup and
		 * reason, again after a second: the touch repeats every frame
		 * while the ship overlaps it).
		 */
		const uint16_t key = powerup.get_unchecked_index();
		const auto tick{B.tick.tick()};
		if (bs->deny_key != key || bs->deny_reason != deny || tick - bs->deny_tick >= BOT_DENY_LOG_TICKS)
		{
			bs->deny_key = key;
			bs->deny_reason = deny;
			bs->deny_tick = tick;
			if (bot_log_on())
				con_printf(CON_VERBOSE, "bots: '%s' touches powerup %u: denied (%s)", static_cast<const char *>(bs->cfg.name), underlying_value(id), deny_name(deny));
		}
		return true;
	}
	/* What do_powerup does besides the inventory. */
	switch (id)
	{
		case powerup_type_t::POW_CLOAK:
			pi.cloak_time = GameTime64;
			multi_send_cloak(pid);
			break;
		case powerup_type_t::POW_INVULNERABILITY:
			pi.FakingInvul = 0;
			pi.invulnerable_time = GameTime64;
			break;
#if DXX_BUILD_DESCENT == 2
		case powerup_type_t::POW_AFTERBURNER:
			bs->pl.afterburner_charge = F1_0;
			break;
#endif
		default:
			break;
	}
	/* Only now do the clients see the new inventory (the grant only
	 * names the powerup): after MULTI_CLOAK, with the real
	 * invulnerability, and the host's copy taken from the ship as it is.
	 */
	net_objects_host_own_ship_inventory(pid, true);
	/* The powerup is no goal any more; the weapon choice sees what it got
	 * at the next strategy tick.
	 */
	bs->powerups.forget(powerup.get_unchecked_index());
	if (bot_log_on())
	{
		char arm[160];
		describe_armament(pi, arm, sizeof(arm));
		con_printf(CON_VERBOSE, "bots: '%s' takes powerup %u (granted%s): %s", static_cast<const char *>(bs->cfg.name), underlying_value(id), bs->grabbing ? ", grab" : bs->power_going ? ", power" : "", arm);
	}
	return true;
}

void bot_cloak_expired(const playernum_t pnum)
{
	if (!find_bot(pnum) || !bots_running())
		return;
	/* As the human's game does for its own cloak: the others see it, and
	 * the cloak comes back into the level.
	 */
	maybe_drop_net_powerup(powerup_type_t::POW_CLOAK, 1, 0);
	multi_send_decloak(pnum);
}

bool bot_hit_wall(const object &ship, const vmsegptridx_t seg, const sidenum_t side)
{
	if (ship.type != object_type::OBJ_PLAYER || !bot_is_local(get_player_id(ship)))
		return false;
	/* Section 4.3: a bot opens a door by flying into it, as a human does
	 * (wall_hit_process, without its HUD messages).
	 */
	const auto wall_num{seg->shared_segment::sides[side].wall_num};
	if (wall_num == wall_none)
		return true;
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &w{*Walls.vmptr(wall_num)};
	if (w.type != WALL_DOOR || +(w.flags & wall_flag::door_locked))
		return true;
	if (w.keys != wall_key::none && !(ship.ctype.player_info.powerup_flags & static_cast<player_flag>(w.keys)))
		return true;
	if (w.state != wall_state::opening)
	{
		wall_open_door(seg, side);
		multi_send_door_open(seg, side, w.flags);
	}
	return true;
}


/* Section 6.4: managing the bots during the game. */

namespace {

[[nodiscard]]
b::add_verdict judge_add_now(std::optional<playernum_t> &slot)
{
	const bool host{bots_running()};
	slot = host ? net_v2::host_free_slot_for_bot() : std::nullopt;
	return b::judge_add({
		.host = host,
		.mode_allowed = bots_allowed_in_mode(Netgame.gamemode),
		.playing = Network_status == network_state::playing,
		.countdown = LevelUniqueObjectState.ControlCenterState.Control_center_destroyed != 0,
		.join_in_progress = host && net_v2::host_join_in_progress(),
		.free_slot = slot.has_value(),
	});
}

/* The next built-in name (section 6.2) nobody in this game has. */
[[nodiscard]]
callsign_t default_bot_name(const playernum_t slot)
{
	for (const auto n : b::bot_default_names)
	{
		callsign_t c{};
		c.copy_lower(std::span<const char>(n, std::min<std::size_t>(std::strlen(n), CALLSIGN_LEN)));
		if (!callsign_taken(c, slot, true))
			return c;
	}
	return {};
}

/* Section 6.4: a bot's name in the game (b::usable_name): empty if
 * nothing of it may be a name, or it is a word `/bot` reads as
 * something else (`all`, a skill, a style, a command).
 */
[[nodiscard]]
callsign_t clean_name(const char *const name)
{
	const auto clean{b::usable_name(name)};
	callsign_t c{};
	c.copy_lower(std::span<const char>(clean.data(), std::strlen(clean.data())));
	return c;
}

[[nodiscard]]
const char *skill_label(const b::bot_skill k)
{
	return b::bot_skill_names[static_cast<unsigned>(k) % b::BOT_SKILL_COUNT];
}

[[nodiscard]]
const char *style_label(const b::bot_style st)
{
	return b::bot_style_names[static_cast<unsigned>(st) % b::BOT_STYLE_COUNT];
}

/* Section 9.13: the style profile's name, else the built-in style's. */
[[nodiscard]]
const char *style_label(const bot_config &c)
{
	return c.profile[0] ? c.profile.data() : style_label(c.style);
}

}

bool bots_manageable()
{
	return bots_running() && bots_allowed_in_mode(Netgame.gamemode) && Network_status != network_state::menu && Network_status != network_state::browsing;
}

b::add_verdict bots_add_verdict()
{
	std::optional<playernum_t> slot;
	return judge_add_now(slot);
}

unsigned bots_in_game(const std::span<bot_in_game, MAX_BOTS> out)
{
	unsigned n{0};
	for (playernum_t i = 0; i < MAX_PLAYERS && n < out.size(); ++i)
	{
		const auto bs{find_bot(i)};
		if (!bs || vcplayerptr(i)->connected == player_connection_status::disconnected)
			continue;
		out[n++] = {i, bs->added, bs->cfg};
	}
	std::ranges::sort(out.first(n), {}, &bot_in_game::added);
	return n;
}

unsigned bots_players_in_game()
{
	unsigned n{0};
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
		if (i == Player_num || vcplayerptr(i)->connected != player_connection_status::disconnected)
			++n;
	return n;
}

std::optional<playernum_t> bots_add(const bot_config &wanted, b::add_verdict &why)
{
	std::optional<playernum_t> free;
	why = judge_add_now(free);
	if (why != b::add_verdict::ok)
		return std::nullopt;
	const playernum_t slot{*free};
	/* The first bot of this level: the level's data for the bots. */
	if (!B.tick_started)
		prepare_level();
	bot_config cfg{wanted};
	cfg.name = clean_name(wanted.name);
	cfg.name = unique_callsign(cfg.name[0u] ? cfg.name : default_bot_name(slot), slot, true);
	std::array<unsigned, MAX_PLAYERS> orders{};
	for (auto &&[i, o] : enumerate(B.bots))
		if (o)
			orders[i] = o->added;
	auto &np{Netgame.players[slot]};
	np.callsign = cfg.name;
	np.rank = netplayer_info::player_rank::None;
	np.connected = player_connection_status::playing;
	np.protocol.udp.addr = {};
	np.LastPacketTime = timer_query();
	np.ping = 0;
	vmplayerptr(slot)->callsign = cfg.name;
	auto &bs{B.bots[slot].emplace(slot, cfg)};
	/* Section 2.3: added last, so a human who finds the game full
	 * replaces this bot first.
	 */
	bs.added = b::next_added_order(orders);
	set_player_is_bot(slot, true);
	bool teams_changed{};
	if (+(Game_mode & GM_TEAM))
	{
		/* Its team: the preferred one, else the smaller (blue on a tie). */
		unsigned red{0}, blue{0};
		for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
			if (i != slot && (i == Player_num || vcplayerptr(i)->connected != player_connection_status::disconnected))
				++((Netgame.team_vector >> i) & 1 ? red : blue);
		const bool to_red{cfg.team == b::bot_team::red || (cfg.team == b::bot_team::automatic && red < blue)};
		const auto bit{static_cast<uint8_t>(1u << slot)};
		if (to_red)
			Netgame.team_vector |= bit;
		else
			Netgame.team_vector &= static_cast<uint8_t>(~bit);
		teams_changed = true;
	}
	enter_level(bs);
	/* It joins as a player: new_player on the host, PLAYER_JOINED and
	 * the player list with its bot flag to everyone.
	 */
	net_v2::host_add_player(slot);
	if (teams_changed)
		multi_host_teams_changed();
	auto &obj{ship_of(slot)};
	obj.control_source = object::control_type::remote;
	/* Its first spawn, at a site the host assigns (as for a joiner). */
	bs.life = bot_life::dead;
	bs.respawn_at = GameTime64;
	respawn(bs, obj);
	if (bs.life == bot_life::dead)
		multi_make_player_ghost(slot);
	con_printf(CON_NORMAL, "bots: '%s' joins the game as P#%u (%s, %s)", static_cast<const char *>(bs.cfg.name), slot, skill_label(bs.cfg.skill), style_label(bs.cfg));
	return slot;
}

bool bots_remove(const playernum_t pnum)
{
	const auto bs{find_bot(pnum)};
	if (!bs || !bots_running() || vcplayerptr(pnum)->connected == player_connection_status::disconnected)
		return false;
	con_printf(CON_NORMAL, "bots: the host removes '%s' (P#%u)", static_cast<const char *>(bs->cfg.name), pnum);
	return remove_bot(pnum, kick_player_reason::quit);
}

bool bots_set_skill_style(const playernum_t pnum, const b::bot_skill skill, const b::bot_style style, const b::style_name &profile)
{
	const auto bs{find_bot(pnum)};
	if (!bs || !bots_running())
		return false;
	if (bs->cfg.skill == skill && bs->cfg.style == style && bs->cfg.profile == profile)
		return true;
	bs->cfg.skill = skill;
	bs->cfg.style = style;
	bs->cfg.profile = profile;
	bs->profile_missing_said = false;
	/* From its next tick on, not only from its next life. */
	bs->apply_config();
	con_printf(CON_NORMAL, "bots: '%s' (P#%u) now plays %s, %s", static_cast<const char *>(bs->cfg.name), pnum, skill_label(skill), style_label(bs->cfg));
	char msg[48];
	std::snprintf(msg, sizeof(msg), "%s now %s %s", static_cast<const char *>(bs->cfg.name), skill_label(skill), style_label(bs->cfg));
	multi_send_host_notice(msg);
	return true;
}

bool bots_set_team(const playernum_t pnum, const b::bot_team team)
{
	const auto bs{find_bot(pnum)};
	if (!bs || !bots_running())
		return false;
	bs->cfg.team = team;
	if (!(Game_mode & GM_TEAM) || team == b::bot_team::automatic)
		return true;
	const bool red{team == b::bot_team::red};
	const auto bit{static_cast<uint8_t>(1u << pnum)};
	if (((Netgame.team_vector & bit) != 0) == red)
		return true;
	if (red)
		Netgame.team_vector |= bit;
	else
		Netgame.team_vector &= static_cast<uint8_t>(~bit);
	multi_host_teams_changed();
	char msg[40];
	std::snprintf(msg, sizeof(msg), "%s joins team %s", static_cast<const char *>(bs->cfg.name), static_cast<const char *>(Netgame.team_name[red ? team_number::red : team_number::blue]));
	multi_send_host_notice(msg);
	return true;
}

bool bots_rename(const playernum_t pnum, const char *const name)
{
	const auto bs{find_bot(pnum)};
	if (!bs || !bots_running() || !name || !name[0])
		return false;
	const auto wanted{clean_name(name)};
	if (!wanted[0u])
		return false;
	if (!d_stricmp(wanted, bs->cfg.name))
		return true;
	const auto old{bs->cfg.name};
	const auto c{unique_callsign(wanted, pnum, true)};
	bs->cfg.name = c;
	Netgame.players[pnum].callsign = c;
	vmplayerptr(pnum)->callsign = c;
	/* The clients take a bot's new name from the player list. */
	net_v2::host_send_player_list();
	con_printf(CON_NORMAL, "bots: '%s' (P#%u) is now '%s'", static_cast<const char *>(old), pnum, static_cast<const char *>(c));
	char msg[40];
	std::snprintf(msg, sizeof(msg), "%s is now called %s", static_cast<const char *>(old), static_cast<const char *>(c));
	multi_send_host_notice(msg);
	return true;
}

}

#endif
