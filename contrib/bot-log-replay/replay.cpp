/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
// Replays the goal choice of a playtest log's per-second bot lines (parsed by
// extract.py) with the goal utilities of bot_goals.h (section 9.9 of
// Documentation/multiplayer-bots.md).  Prints each line with the log's goal
// and the replayed one, and the shares on stderr.
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "bot_goals.h"
using namespace dcx::bot;
int main(int argc, char **argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: %s inputs.csv (from extract.py)\n", argv[0]);
		return 2;
	}
	std::ifstream f(argv[1]);
	std::string line;
	std::map<std::string, goal_kind> current;
	std::map<std::string, unsigned> oldc, newc;
	unsigned n{0};
	const char *names[]{"roam", "hunt", "engage", "collect", "retreat", "refuel"};
	while (std::getline(f, line))
	{
		std::vector<std::string> c;
		std::stringstream ss(line);
		std::string x;
		while (std::getline(ss, x, ','))
			c.push_back(x);
		goal_inputs in;
		const auto &name{c[0]};
		const bool collector{c[15] == "1"};
		in.has_target = c[2] == "1";
		in.target_visible = c[3] == "1";
		const double E{std::stod(c[4])};
		in.target_score = E / 2;
		in.threatened = in.has_target;
		in.collect = std::stod(c[5]);
		in.collect_path = collector ? 100 : 0;
		in.phase_collect = std::stod(c[6]);
		in.grab = c[7] == "1";
		in.grab_value = std::stod(c[8]);
		in.grab_path = std::stod(c[9]);
		in.weak = c[10] == "1";
		in.armed = static_cast<armed_level>(std::stoi(c[11]));
		in.seek = std::stod(c[12]);
		in.shields = std::stod(c[13]);
		in.retreat_shields = std::stod(c[14]);
		in.collector = collector;
		if (const auto it{current.find(name)}; it != current.end())
			in.current = it->second;
		const auto g{choose_goal(in)};
		current[name] = g;
		++newc[std::string(names[static_cast<unsigned>(g)]) + (g == goal_kind::collect && goal_utility(in).collect_from == collect_source::grab ? "/grab" : "")];
		++oldc[c[16]];
		std::printf("%s,%s,%s,%s\n", name.c_str(), c[16].c_str(), names[static_cast<unsigned>(g)], line.c_str());
		++n;
	}
	std::fprintf(stderr, "lines %u\nbefore:", n);
	for (auto &[k, v] : oldc)
		std::fprintf(stderr, " %s %u (%.0f%%)", k.c_str(), v, 100.0 * v / n);
	std::fprintf(stderr, "\nafter: ");
	for (auto &[k, v] : newc)
		std::fprintf(stderr, " %s %u (%.0f%%)", k.c_str(), v, 100.0 * v / n);
	std::fprintf(stderr, "\n");
}
