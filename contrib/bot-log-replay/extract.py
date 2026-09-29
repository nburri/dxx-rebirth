#!/usr/bin/env python3
# Replay of the bots' goal choice from a playtest log
# (Documentation/multiplayer-bots.md section 9.9).
#
# Step 1: this script parses the per-second summary lines of a -verbose
# gamelog ("bots: '<name>' goal=...") into the goal choice's inputs, as
# far as the log shows them, one CSV row per line.  Step 2: replay.cpp
# feeds them to bot_goals.h's goal_utility / choose_goal and prints the
# goal shares before (the log's) and after (the current code's).
#
#	python3 contrib/bot-log-replay/extract.py gamelog.txt > inputs.csv
#	g++ -std=gnu++23 -O2 -Icommon/main contrib/bot-log-replay/replay.cpp -o bot-log-replay
#	./bot-log-replay inputs.csv > replayed.csv
#
# What the log does not show is estimated: the engage or hunt utility of
# a line whose goal is neither is its runner-up's (or 0.9 of the lesser
# utility shown); a grab's plain collection is the median of the plain
# collections in the same situation (enemy in sight, known, none); a
# grab's path is 1.2 times the straight distance of the nearest valuable
# powerup when the grab started (the log shows no path), for the whole
# run of grab lines; weakness and what the bot holds come from its
# armament; seeking is possible when the bot knew a target within three
# times its memory time (by skill and style).  Each bot's previous
# replayed goal is its current goal (the hysteresis), a second apart.
import collections
import re
import statistics
import sys

L = open(sys.argv[1], errors='replace').read().splitlines()
styles = {}
for l in L:
    m = re.search(r"bots: '(\w+)' takes P#\d+ \((\w+), (\w+)\)", l)
    if m:
        styles[m.group(1)] = (m.group(2), m.group(3))
rx = re.compile(r"(\d+):(\d+):(\d+)\.\d+ bots: '(\w+)' goal=(\w+) ([\d.]+) \(next (\w+) ([-\d.]+), collect ([\d.]+)( grab)?( phase)?( 3rd-party)?( seek)?\) tgt=(\S+)(?: (\d+)u)?( vis)?( clr)? \| (\S+) \[([^\]]*)\] \| sh=(\d+) en=(\d+) \| pu=(.*?) \| heavy=(\S+)")
rows = []
for l in L:
    m = rx.search(l)
    if not m:
        continue
    hh, mm, ss, name, goal, u, nx, nu, cu, grab, phase, third, seek, tgt, td, vis, clr, prim, arm, sh, en, pu, hv = m.groups()
    rows.append(dict(t=int(hh) * 3600 + int(mm) * 60 + int(ss), name=name, goal=goal, u=float(u), nx=nx, nu=float(nu), cu=float(cu),
                     grab=bool(grab), phase=bool(phase), tgt=tgt, td=int(td) if td else -1, vis=bool(vis), clr=bool(clr), arm=arm,
                     sh=int(sh), pu=pu, hv=hv))


def ammo(arm):
    d = collections.Counter()
    if 'sec=' in arm:
        for x in arm.split('sec=')[1].split(','):
            m = re.match(r'([a-z]+)(\d+)', x)
            if m:
                d[m.group(1)] += int(m.group(2))
    return d


def armed(arm):
    a = ammo(arm)
    if a['smart'] or a['mega'] or a['shaker']:
        return 2
    if a['conc'] + a['homing'] + a['merc'] >= 3:
        return 1
    return 0


def weak(arm):
    guns = arm.split(' sec=')[0].split(',')
    a = ammo(arm)
    if a['smart'] or a['mega'] or a['shaker'] or a['homing'] + a['merc'] >= 4 or a['homing'] + a['merc'] + a['conc'] >= 8:
        return 0
    for g in guns:
        m = re.match(r'laser(\d)(q?)$', g)
        if not m:
            return 0
        lvl = int(m.group(1))
        if (m.group(2) and lvl > 1) or (not m.group(2) and lvl > 3):
            return 0
    return 1


def situation(r):
    return 'vis' if r['vis'] else 'known' if r['tgt'] != '-' else 'none'


plain = collections.defaultdict(list)
for r in rows:
    if not r['grab'] and not r['phase']:
        plain[situation(r)].append(r['cu'])
med = {k: statistics.median(v) for k, v in plain.items()}
sys.stderr.write('plain collection medians %s\n' % med)
style_engage = {'Balanced': 1.0, 'Aggressive': 1.5, 'Cautious': 0.8, 'Collector': 0.7}
style_retreat = {'Balanced': 35, 'Aggressive': 20, 'Cautious': 55, 'Collector': 40}
chase = {'Balanced': 1, 'Aggressive': 2, 'Cautious': 0.8, 'Collector': 1}
memory_s = {'Trainee': 2, 'Rookie': 3, 'Hotshot': 5, 'Ace': 7, 'Insane': 10}
last_seen = {}
episode = {}
for r in rows:
    skill, style = styles.get(r['name'], ('Insane', 'Balanced'))
    has = r['tgt'] != '-'
    if has:
        last_seen[r['name']] = r['t']
    if r['goal'] in ('engage', 'hunt'):
        E = r['u']
    elif r['nx'] in ('engage', 'hunt'):
        E = r['nu']
    elif has:
        E = min(r['nu'], r['u']) * 0.9
    else:
        E = 0
    k = situation(r)
    grab_value = grab_path = 0
    if r['grab']:
        grab_value = 2.5 if r['cu'] >= 6.5 - 1e-6 else 1.0
        pm = re.match(r'(\S+) (\d+)u v([\d.]+) (\S+)', r['pu'])
        grab_path = int(pm.group(2)) * 1.2 if pm else 30
        grab_path = episode.setdefault(r['name'], grab_path)
        collect = med[k]
    else:
        episode.pop(r['name'], None)
        collect = r['cu']
    phase_collect = r['cu'] if (r['phase'] and not r['grab']) else 0
    if phase_collect:
        collect = min(collect, med[k])
    a = armed(r['arm'])
    seek = 0
    if not has and a and r['name'] in last_seen and r['t'] - last_seen[r['name']] <= memory_s[skill] * chase[style] * 3:
        seek = (1.0 if a == 2 else 0.8) * style_engage[style]
    print(','.join(str(x) for x in [r['name'], style, int(has), int(r['vis']), E, collect, phase_collect, int(r['grab']), grab_value, grab_path,
                                    weak(r['arm']), a, seek, r['sh'], style_retreat[style], int(style == 'Collector'), r['goal'],
                                    int(r['hv'] != 'none-owned'), r['td']]))
