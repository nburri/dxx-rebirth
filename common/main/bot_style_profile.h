/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Bot style profiles (Documentation/movement-recording.md section 8.5):
 * a small text file of `key = value` lines that says how a bot flies and
 * fights "in the style of" a player.  The analysis of the movement
 * recordings (movement_analysis.h, the tool movrec-analyse) writes one
 * per player; the game loads them (step 3, bot_style_library.h).
 *
 * A profile names the values of `style_params` (`style.<field>`), of the
 * movement fields of `skill_params` (`skill.<field>`) and of constants of
 * the bots' movement and combat code that are the same for every bot
 * today (`tune.<name>`), each with a confidence.  Keys a reader does not
 * know are kept and otherwise ignored, so the format can grow.
 *
 * Header-only, standard library and the bots' pure headers only.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bot_brain.h"
#include "bot_goals.h"
#include "bot_weapons.h"

namespace dcx::bot {

constexpr unsigned STYLE_PROFILE_FORMAT{1};
/* The file name of a profile is `<callsign>.botstyle`. */
inline constexpr std::string_view STYLE_PROFILE_EXTENSION{".botstyle"};

/* How much the recordings said about a value. */
enum class style_confidence : uint8_t
{
	/* Little or no evidence: the value is the base style's, or a guess. */
	low,
	/* Some evidence, or controls estimated from the motion. */
	medium,
	high,
};

inline constexpr std::array<const char *, 3> style_confidence_names{{"low", "medium", "high"}};

/* What a loader may do with the confidence: the share of the way from
 * the base value to the profile's value.
 */
[[nodiscard]]
constexpr double style_confidence_weight(const style_confidence c)
{
	switch (c)
	{
		case style_confidence::low:
			return 0.25;
		case style_confidence::medium:
			return 0.7;
		case style_confidence::high:
			break;
	}
	return 1.0;
}

/* The keys this version knows: the range a value is clamped to (wider
 * than the built-in styles, but inside what the bot code handles) and a
 * line for the file's comments and the documentation.
 */
struct style_profile_key
{
	std::string_view key;
	double lo, hi;
	std::string_view text;
};

inline constexpr std::array<style_profile_key, 38> style_profile_keys{{
	/* style_params */
	{"style.retreat_shields", 5, 90, "shields below which the bot retreats"},
	{"style.engage_weight", 0.5, 1.8, "weight of fighting against everything else"},
	{"style.collect_weight", 0.5, 2.0, "weight of collecting powerups"},
	{"style.range_scale", 0.5, 3.0, "scale of the fight distance (1 = 35 to 95 units)"},
	{"style.chase_memory", 0.4, 2.5, "scale of how long a lost target is hunted"},
	{"style.dodge_bonus", -0.3, 0.3, "added to the skill's dodge probability"},
	{"style.mine_interval", 0.5, 3.0, "scale of the time between two mines"},
	{"style.strafe_scale", 0.5, 1.3, "scale of the strafe speed"},
	{"style.close_scale", 0.5, 1.25, "scale of the speed backing off (closing in is a full key)"},
	{"style.behind_engage", 0.3, 1.0, "share of the engage weight left when behind in a fight"},
	{"style.outgunned_retreat", 0, 40, "shields added to the retreat threshold when outgunned"},
	{"style.burn_chase_distance", 40, 1000, "afterburner when chasing a target further than this"},
	/* The movement fields of skill_params */
	{"skill.strafe", 0, 1, "1: strafes while fighting"},
	{"skill.strafe_min_ms", 150, 3000, "shortest run of the strafe in one direction"},
	{"skill.strafe_max_ms", 250, 5000, "longest run of the strafe in one direction"},
	{"skill.strafe_vertical", 0, 1, "vertical share of the strafe (0 flat, 1 as much as sideways)"},
	{"skill.strafe_speed", 0, 1, "thrust of the strafe keys, share of full thrust"},
	{"skill.dodge_prob", 0, 0.95, "probability of dodging a shot that would hit"},
	/* Constants of the bot code, the same for every bot today. */
	{"tune.range_lo", 15, 400, "near edge of the fight band (BOT_RANGE_LO, 35)"},
	{"tune.range_hi", 30, 800, "far edge of the fight band (BOT_RANGE_HI, 95)"},
	{"tune.reverse_turn", 0, 1, "share of the large turns flown backwards (turn_habits::reverse, 0.12)"},
	/* The review of PR #64: no less than 0.3, a reverse turn at no speed
	 * is a bot that stands while its nose comes round.
	 */
	{"tune.reverse_turn_speed", 0.3, 1, "backward speed in such a turn, share of the top speed (REVERSE_TURN_SPEED, 0.8)"},
	{"tune.turn_boost", 0, 1, "share of the large turns followed by a push forward (turn_habits::boost, 0.8)"},
	{"tune.turn_boost_burn", 0, 1, "share of those pushes with the afterburner (turn_habits::boost_burn, 0.3)"},
	{"tune.burn_retreat", 0, 1, "share of the time fleeing with the afterburner"},
	{"tune.burn_roam", 0, 1, "share of the time with no enemy in sight with the afterburner"},
	{"tune.missile_interval_scale", 0.3, 4, "scale of the time between two missile volleys (missile_interval_scale)"},
	{"tune.volley_size", 1, 8, "missiles per volley"},
	{"tune.pursuit_seconds", 0, 30, "how long a target is followed after it left the sight (pursuit_seconds)"},
	{"tune.grab_detour", 0, 1, "share of the pickups taken by leaving the course (grab_is_detour)"},
	{"tune.power_pickup", 0, 1, "share of the power pickups in sight the bot goes for (power_pickup_weight)"},
	/* Section 9.17: the seconds from a heavy missile's pickup to its
	 * shot, and keeping out of exposed places when weak.
	 */
	{"tune.heavy_fire_delay", 0.5, 60, "seconds from a heavy missile's pickup to its shot (heavy_fire_delay, median)"},
	{"style.cover", 0, 2, "how strongly a weak or collecting bot keeps out of exposed places (cover_appetite)"},
	/* Section 9.18 of Documentation/multiplayer-bots.md: what the bot
	 * aims for by watching its own flight (habit_governor).
	 */
	{"tune.strafe_reversals", 0, 150, "strafe reversals per minute of fight (the bot's aim)"},
	{"tune.strafe_share", 0, 1, "share of the fight time with a strafe key (the bot's aim)"},
	{"tune.burn_chase", 0, 1, "share of the time chasing with the afterburner"},
	{"tune.burn_fight", 0, 1, "share of the other fight time with the afterburner"},
	{"tune.fire_distance", 10, 400, "median distance when firing (the bot's aim)"},
}};

[[nodiscard]]
constexpr const style_profile_key *find_style_profile_key(const std::string_view key)
{
	for (const auto &k : style_profile_keys)
		if (k.key == key)
			return &k;
	return nullptr;
}

struct style_profile_entry
{
	std::string key;
	double value{};
	style_confidence confidence{style_confidence::high};
	bool operator==(const style_profile_entry &) const = default;
};

struct style_profile
{
	unsigned format{STYLE_PROFILE_FORMAT};
	/* The name the bot setup shows ("<player> style"). */
	std::string name;
	/* The player it was made from. */
	std::string callsign;
	/* What it was made from, for people (not interpreted). */
	std::string source;
	/* What every value not in the profile is taken from. */
	bot_skill base_skill{BOT_DEFAULT_SKILL};
	bot_style base_style{bot_style::balanced};
	/* In file order.  `style.`, `skill.` and `tune.` keys are values of
	 * the bot; `measured.` keys are plain statistics for people.
	 */
	std::vector<style_profile_entry> entries;
	bool operator==(const style_profile &) const = default;

	[[nodiscard]]
	const style_profile_entry *find(const std::string_view key) const
	{
		for (const auto &e : entries)
			if (e.key == key)
				return &e;
		return nullptr;
	}
	[[nodiscard]]
	std::optional<double> value(const std::string_view key) const
	{
		const auto e{find(key)};
		return e ? std::optional<double>{e->value} : std::nullopt;
	}
	/* Set a value, clamped to the key's range if the key is known. */
	void set(const std::string_view key, double value, const style_confidence c)
	{
		if (!std::isfinite(value))
			return;
		if (const auto k{find_style_profile_key(key)})
			value = std::clamp(value, k->lo, k->hi);
		for (auto &e : entries)
			if (e.key == key)
			{
				e.value = value;
				e.confidence = c;
				return;
			}
		entries.push_back({std::string{key}, value, c});
	}
};

namespace style_profile_detail {

[[nodiscard]]
inline std::string_view trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

[[nodiscard]]
inline bool equal_nocase(const std::string_view a, const std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i{}; i != a.size(); ++i)
	{
		const auto lower{[](const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }};
		if (lower(a[i]) != lower(b[i]))
			return false;
	}
	return true;
}

[[nodiscard]]
inline std::optional<double> parse_number(const std::string_view s)
{
	const std::string copy{s};
	if (copy.empty())
		return std::nullopt;
	char *end{};
	const double v{std::strtod(copy.c_str(), &end)};
	if (end == copy.c_str() || *end || !std::isfinite(v))
		return std::nullopt;
	return v;
}

/* A number as the file has it: short, and read back to the same value
 * within 1/10000 of it.
 */
[[nodiscard]]
inline std::string format_number(const double v)
{
	std::array<char, 40> buf;
	std::snprintf(buf.data(), buf.size(), "%.5g", v);
	return buf.data();
}

/* One line of text: no line breaks, and not so long that a menu breaks. */
[[nodiscard]]
inline std::string clean_text(const std::string_view s)
{
	std::string r;
	for (const char c : trim(s).substr(0, 200))
		r += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
	return r;
}

}

/* The profile as the text of a `.botstyle` file:
 *
 *	# comment
 *	format = 1
 *	name = Nico style
 *	callsign = Nico
 *	source = 3 recordings, 2 games, 41 min alive
 *	base_skill = Hotshot
 *	base_style = Balanced
 *	style.retreat_shields = 42
 *	confidence.style.retreat_shields = medium
 *	...
 *
 * One `key = value` per line; spaces around the key and the value do not
 * count; a line that starts with `#` and an empty line are skipped.  A
 * value without a `confidence.<key>` line has confidence high.
 */
[[nodiscard]]
inline std::string write_style_profile(const style_profile &p)
{
	using style_profile_detail::clean_text;
	using style_profile_detail::format_number;
	std::string out;
	out += "# D2X-Rebirth bot style profile (Documentation/movement-recording.md, section 8.5)\n";
	out += "format = " + std::to_string(p.format) + "\n";
	out += "name = " + clean_text(p.name) + "\n";
	out += "callsign = " + clean_text(p.callsign) + "\n";
	if (!p.source.empty())
		out += "source = " + clean_text(p.source) + "\n";
	out += std::string{"base_skill = "} + bot_skill_names[std::min<std::size_t>(static_cast<std::size_t>(p.base_skill), BOT_SKILL_COUNT - 1)] + "\n";
	out += std::string{"base_style = "} + bot_style_names[std::min<std::size_t>(static_cast<std::size_t>(p.base_style), BOT_STYLE_COUNT - 1)] + "\n";
	std::string_view section;
	for (const auto &e : p.entries)
	{
		const std::string_view key{e.key};
		const auto prefix{key.substr(0, key.find('.'))};
		if (prefix != section)
		{
			section = prefix;
			out += "\n";
		}
		if (const auto k{find_style_profile_key(key)})
			out += "# " + std::string{k->text} + "\n";
		out += e.key + " = " + format_number(e.value) + "\n";
		if (e.confidence != style_confidence::high)
			out += "confidence." + e.key + " = " + style_confidence_names[static_cast<std::size_t>(e.confidence)] + "\n";
	}
	return out;
}

/* The profile in `text`, or nothing if it is not one (no `format` line,
 * or a format newer than this reader).  Known keys are clamped to their
 * range; lines that are not understood are skipped.  The review of PR
 * #64: a UTF-8 byte order mark (an editor's, on a hand-edited file) is
 * skipped; it made the first line (`format`) unknown and the file no
 * profile.
 */
[[nodiscard]]
inline std::optional<style_profile> parse_style_profile(std::string_view text)
{
	using namespace style_profile_detail;
	if (constexpr std::string_view bom{"\xef\xbb\xbf"}; text.starts_with(bom))
		text.remove_prefix(bom.size());
	style_profile p;
	p.format = 0;
	std::vector<std::pair<std::string, style_confidence>> confidences;
	for (std::size_t pos{}; pos < text.size();)
	{
		const auto eol{std::min(text.find('\n', pos), text.size())};
		const auto line{trim(text.substr(pos, eol - pos))};
		pos = eol + 1;
		if (line.empty() || line.front() == '#')
			continue;
		const auto eq{line.find('=')};
		if (eq == std::string_view::npos)
			continue;
		const auto key{trim(line.substr(0, eq))};
		const auto value{trim(line.substr(eq + 1))};
		if (key.empty())
			continue;
		if (key == "format")
		{
			if (const auto v{parse_number(value)}; v && *v >= 1 && *v <= 1000)
				p.format = static_cast<unsigned>(*v);
		}
		else if (key == "name")
			p.name = clean_text(value);
		else if (key == "callsign")
			p.callsign = clean_text(value);
		else if (key == "source")
			p.source = clean_text(value);
		else if (key == "base_skill")
		{
			for (std::size_t i{}; i != BOT_SKILL_COUNT; ++i)
				if (equal_nocase(value, bot_skill_names[i]))
					p.base_skill = static_cast<bot_skill>(i);
		}
		else if (key == "base_style")
		{
			for (std::size_t i{}; i != BOT_STYLE_COUNT; ++i)
				if (equal_nocase(value, bot_style_names[i]))
					p.base_style = static_cast<bot_style>(i);
		}
		else if (constexpr std::string_view cp{"confidence."}; key.substr(0, cp.size()) == cp)
		{
			for (std::size_t i{}; i != style_confidence_names.size(); ++i)
				if (equal_nocase(value, style_confidence_names[i]))
					confidences.emplace_back(std::string{key.substr(cp.size())}, static_cast<style_confidence>(i));
		}
		else if (key.find('.') != std::string_view::npos)
		{
			if (const auto v{parse_number(value)})
				p.set(key, *v, style_confidence::high);
		}
	}
	if (p.format == 0 || p.format > STYLE_PROFILE_FORMAT)
		return std::nullopt;
	for (const auto &[key, c] : confidences)
		for (auto &e : p.entries)
			if (e.key == key)
				e.confidence = c;
	return p;
}

/* The parameters of a bot that flies this profile: the base skill's and
 * the base style's values, with the profile's in place of them.  A value
 * of lower confidence goes only part of the way from the base to the
 * profile (style_confidence_weight).  `skill` decides the aim, the
 * reaction and the senses as before; only its movement fields change.
 *
 * Section 9.13 of Documentation/multiplayer-bots.md: every `style.`,
 * `skill.` and `tune.` key drives the bot.  The `tune.` keys become the
 * bot's tune_params:
 *
 * - `tune.range_lo`, `tune.range_hi`: the fight band; with either, the
 *   band is the profile's own and `style.range_scale` (the same
 *   measurement) is not applied on top (1);
 * - `tune.reverse_turn`, `reverse_turn_speed`, `turn_boost`,
 *   `turn_boost_burn`: the turn habits;
 * - `tune.burn_retreat` (a share of the fleeing time) and
 *   `tune.burn_roam` (of the time with no enemy in sight): the share of
 *   the draws that light the afterburner, the measured share over what
 *   a bot that burns at every draw reaches (FLEE_BURN_FULL_TIME,
 *   ROAM_BURN_FULL_TIME);
 * - `tune.missile_interval_scale`, `tune.volley_size`,
 *   `tune.pursuit_seconds`: in place of the style's and the skill's
 *   tables (0 seconds: it lets a lost enemy go);
 * - `tune.grab_detour` (the share of pickups off course): a scale of the
 *   detour a grab may take in a fight, over the share the bots took
 *   with the scale 1 (GRAB_DETOUR_BASE_SHARE);
 * - `tune.power_pickup` (the share of the power pickups in sight the
 *   pilot went for): the bot's power weight, in place of the skill's
 *   and the style's (power_pickup_weight, section 9.14);
 * - `tune.heavy_fire_delay` (seconds from a heavy missile's pickup to
 *   its shot, median): in place of the skill's (heavy_fire_delay,
 *   section 9.17);
 * - section 9.18: `tune.strafe_reversals`, `tune.strafe_share`,
 *   `tune.burn_chase`, `tune.burn_retreat`, `tune.burn_roam`,
 *   `tune.burn_fight` and `tune.fire_distance` are the aims of the bot's
 *   habit_governor (a value of low confidence is no aim; the
 *   afterburner's aims only for a skill that uses it).
 */
struct style_profile_params
{
	skill_params skill;
	style_params style;
	tune_params tune;
};

/* A bot that burns at every draw spends about this share of the time
 * fleeing (the charge allows no more: 3 s of burning, then 8 s to
 * recharge, relit at 30 %) with the afterburner (test-bot-fight-sim).
 */
constexpr double FLEE_BURN_FULL_TIME{0.4};
/* ... and about this share of the time with no enemy in sight (only on
 * long straight flights, above the charge reserve).
 */
constexpr double ROAM_BURN_FULL_TIME{0.05};
/* The share of pickups taken off course by the bots of the recordings
 * of 2026-09-30 (detour scale 1).
 */
constexpr double GRAB_DETOUR_BASE_SHARE{0.45};

[[nodiscard]]
inline style_profile_params apply_style_profile(const style_profile &p, const bot_skill skill)
{
	style_profile_params r{skill_of(skill), style_of(p.base_style), {}};
	const auto blend{[&p](const std::string_view key, const double base, const auto map) {
		const auto e{p.find(key)};
		if (!e)
			return base;
		const auto k{find_style_profile_key(key)};
		const double v{map(k ? std::clamp(e->value, k->lo, k->hi) : e->value)};
		return base + (v - base) * style_confidence_weight(e->confidence);
	}};
	const auto same{[](const double v) { return v; }};
	auto &s{r.style};
	s.retreat_shields = blend("style.retreat_shields", s.retreat_shields, same);
	s.engage_weight = blend("style.engage_weight", s.engage_weight, same);
	s.collect_weight = blend("style.collect_weight", s.collect_weight, same);
	s.range_scale = blend("style.range_scale", s.range_scale, same);
	s.chase_memory = blend("style.chase_memory", s.chase_memory, same);
	s.dodge_bonus = blend("style.dodge_bonus", s.dodge_bonus, same);
	s.mine_interval = blend("style.mine_interval", s.mine_interval, same);
	s.strafe_scale = blend("style.strafe_scale", s.strafe_scale, same);
	s.close_scale = blend("style.close_scale", s.close_scale, same);
	s.behind_engage = blend("style.behind_engage", s.behind_engage, same);
	s.outgunned_retreat = blend("style.outgunned_retreat", s.outgunned_retreat, same);
	s.burn_chase_distance = blend("style.burn_chase_distance", s.burn_chase_distance, same);
	s.cover = blend("style.cover", s.cover, same);
	auto &k{r.skill};
	/* A skill that does not strafe or dodge at all (Trainee) keeps that:
	 * the profile is a style, not a skill.
	 */
	if (k.strafe)
	{
		if (const auto e{p.find("skill.strafe")}; e && e->confidence != style_confidence::low)
			k.strafe = e->value >= 0.5;
		const auto ms{[](const double v) { return static_cast<unsigned>(std::max(v, 0.0) + 0.5); }};
		k.strafe_min_ms = ms(blend("skill.strafe_min_ms", k.strafe_min_ms, same));
		k.strafe_max_ms = std::max(k.strafe_min_ms, ms(blend("skill.strafe_max_ms", k.strafe_max_ms, same)));
		k.strafe_vertical = blend("skill.strafe_vertical", k.strafe_vertical, same);
		k.strafe_speed = blend("skill.strafe_speed", k.strafe_speed, same);
	}
	if (k.dodge_prob > 0)
		k.dodge_prob = std::clamp(blend("skill.dodge_prob", k.dodge_prob, same), 0.0, 0.95);
	/* Section 9.13: the tuned constants. */
	auto &t{r.tune};
	t.range_lo = blend("tune.range_lo", t.range_lo, same);
	t.range_hi = std::max(t.range_lo + 10, blend("tune.range_hi", t.range_hi, same));
	/* The review of PR #64: with either edge (a hand-written file may
	 * give one), the band is the profile's: the range scale on top would
	 * take a band of 400 units to 1200.
	 */
	if (p.find("tune.range_lo") || p.find("tune.range_hi"))
		s.range_scale = 1;
	auto &h{t.turns};
	h.reverse = blend("tune.reverse_turn", h.reverse, same);
	h.reverse_speed = blend("tune.reverse_turn_speed", h.reverse_speed, same);
	h.boost = blend("tune.turn_boost", h.boost, same);
	h.boost_burn = blend("tune.turn_boost_burn", h.boost_burn, same);
	const auto share{[](const double full) {
		return [full](const double v) { return std::clamp(v / full, 0.0, 1.0); };
	}};
	t.flee_burn = blend("tune.burn_retreat", t.flee_burn, share(FLEE_BURN_FULL_TIME));
	t.roam_burn = blend("tune.burn_roam", t.roam_burn, share(ROAM_BURN_FULL_TIME));
	if (p.find("tune.missile_interval_scale"))
		t.missile_interval_scale = blend("tune.missile_interval_scale", missile_interval_scale(p.base_style), same);
	if (p.find("tune.volley_size"))
		t.volley_size = blend("tune.volley_size", std::max(1, volley_table_size(k.weapon_smarts, p.base_style)), same);
	if (p.find("tune.pursuit_seconds"))
		t.pursuit_seconds = blend("tune.pursuit_seconds", pursuit_seconds(skill, p.base_style), same);
	t.grab_detour_scale = blend("tune.grab_detour", t.grab_detour_scale, [](const double v) { return std::clamp(v / GRAB_DETOUR_BASE_SHARE, 0.25, 3.0); });
	if (p.find("tune.power_pickup"))
		t.power_pickup = blend("tune.power_pickup", power_pickup_weight(skill, p.base_style), same);
	if (p.find("tune.heavy_fire_delay"))
		t.heavy_fire_delay = blend("tune.heavy_fire_delay", heavy_fire_delay(skill), same);
	/* Section 9.18: the aims of the habits. */
	const auto aim{[&p](const std::string_view key) {
		const auto e{p.find(key)};
		if (!e || e->confidence == style_confidence::low)
			return -1.0;
		const auto k{find_style_profile_key(key)};
		return k ? std::clamp(e->value, k->lo, k->hi) : e->value;
	}};
	auto &a{t.habits};
	if (k.strafe)
	{
		a.strafe_reversals = aim("tune.strafe_reversals");
		a.strafe_share = aim("tune.strafe_share");
		/* The measured vertical share is the key's own value. */
		if (a.strafe_reversals >= 0 || a.strafe_share >= 0)
			a.strafe_vertical = aim("skill.strafe_vertical");
	}
	if (afterburner_of(skill) != afterburner_use::never)
		a.burn = {{aim("tune.burn_chase"), aim("tune.burn_retreat"), aim("tune.burn_roam"), aim("tune.burn_fight")}};
	a.fire_distance = aim("tune.fire_distance");
	return r;
}

}
