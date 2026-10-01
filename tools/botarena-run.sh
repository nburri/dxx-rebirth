#!/bin/sh
# Run a headless bot game (-botarena, Documentation/multiplayer-bots.md
# section 8.2) with movement recording, then analyse the recording with
# movrec-analyse (Documentation/movement-recording.md section 8).
#
#   tools/botarena-run.sh [options] DATA_DIR MISSION [LEVEL]
#
# DATA_DIR is a Descent 2 game folder (descent2.hog, .ham, .s11, .s22,
# the .pig files, a missions/ folder with the mission; pilot files and
# botstyles/ are used if there).  It is only read: the game writes into
# a fresh user folder under the output directory.  MISSION is a mission
# file name ("Corona", "ESHAKER.MN2") or title ("Earth Shaker").
#
# Options:
#   -n N        bots (1-7, default 5)
#   -b LIST     the bots, skill:style[:name] by commas (default with 5
#               bots: those of the group's games, Hotshot/Balanced,
#               Insane/Balanced, Insane/Aggressive, Insane/Cautious,
#               Insane/Collector; with another -n: the pilot's setup)
#   -s SECONDS  game time (default 600)
#   -f FPS      frames per game second (default 200)
#   -r SEED     random seed (default 1)
#   -p PILOT    the pilot whose .plr/.ngp (game options) to use
#   -o DIR      output directory (default: a new one under ${TMPDIR:-/tmp})
#   -B DIR      the build directory with d2x-rebirth/ and common/ (default:
#               build/ next to this script's folder)
#   -x ARG      one more argument for the game (repeatable, e.g. -x -verbose)
#
# Needs a build of the game and of the analysis tool:
#   scons sdl2=1 d1x=0 d2x=1
#   scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 movrec-analyse
# Without a display, SDL's "offscreen" video driver gives the game its
# OpenGL context (EGL, e.g. Mesa); nothing is drawn while the bots play.

set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
bots=5
list='hotshot:balanced,insane:balanced,insane:aggressive,insane:cautious,insane:collector'
list_given=
seconds=600
fps=200
seed=1
pilot=
out=
build="$here/build"
extra=

usage() {
	sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-2}"
}

while getopts n:b:s:f:r:p:o:B:x:h opt; do
	case $opt in
		n) bots=$OPTARG ;;
		b) list=$OPTARG; list_given=1 ;;
		s) seconds=$OPTARG ;;
		f) fps=$OPTARG ;;
		r) seed=$OPTARG ;;
		p) pilot=$OPTARG ;;
		o) out=$OPTARG ;;
		B) build=$OPTARG ;;
		x) extra="$extra $OPTARG" ;;
		h) usage 0 ;;
		*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 2 ] && [ $# -le 3 ] || usage
data=$(cd "$1" && pwd)
mission=$2
level=${3:-1}

game="$build/d2x-rebirth/d2x-rebirth"
analyse="$build/common/movrec-analyse"
for f in "$game" "$analyse"; do
	if [ ! -x "$f" ]; then
		echo "botarena-run: $f is missing; build it (see the top of $0)" >&2
		exit 1
	fi
done
missions=
for d in missions MISSIONS Missions; do
	[ -d "$data/$d" ] && missions="$data/$d" && break
done

[ -n "$out" ] || out=$(mktemp -d "${TMPDIR:-/tmp}/botarena.XXXXXX")
mkdir -p "$out"
out=$(cd "$out" && pwd)
home="$out/home"
mkdir -p "$home"
# The game's user folder (Linux: $HOME/.d2x-rebirth): an empty d2x.ini, so
# that the data folder's own d2x.ini (if any) does not apply.
userdir="$home/.d2x-rebirth"
mkdir -p "$userdir"
: > "$userdir/d2x.ini"

# With -n but no -b, the bots play the pilot's setup (or the default).
if [ -n "$list_given" ] || [ "$bots" = 5 ]; then
	set -- -botarena-bots "$list"
else
	set --
fi
[ -z "$pilot" ] || set -- "$@" -pilot "$pilot"

echo "botarena-run: $mission level $level, $bots bots, $seconds s at $fps fps, seed $seed; output in $out"
# shellcheck disable=SC2086
HOME="$home" SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}" \
	"$game" -hogdir "$data" -nosound -nomusic -notitles -nomovies -nojoystick -nomouse \
	-recordmoves -recordmoves-bots \
	-botarena "$mission" "$level" "$bots" "$seconds" -fixedfps "$fps" -botarena-seed "$seed" \
	"$@" $extra > "$out/game.log" 2>&1 || {
	status=$?
	grep -a 'botarena' "$out/game.log" >&2 || tail -20 "$out/game.log" >&2
	echo "botarena-run: the game failed (status $status); the log is $out/game.log" >&2
	exit 1
}
grep -a 'botarena: ' "$out/game.log" | sed 's/^.*botarena: /  /'

recording=$(ls -t "$userdir"/recordings/*.dmr 2>/dev/null | head -n 1)
if [ -z "$recording" ]; then
	echo "botarena-run: no recording in $userdir/recordings" >&2
	exit 1
fi
mkdir -p "$out/analysis"
if [ -n "$missions" ]; then
	"$analyse" --bots --missions "$missions" --out "$out/analysis" "$recording" > "$out/analysis.log" 2>&1
else
	"$analyse" --bots --out "$out/analysis" "$recording" > "$out/analysis.log" 2>&1
fi
echo "botarena-run: recording $recording"
echo "botarena-run: reports in $out/analysis (analysis log $out/analysis.log)"
