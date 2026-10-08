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
#   -b LIST     the bots, skill:style[:name[:ship]] by commas (default with 5
#               bots: those of the group's games, Hotshot/Balanced,
#               Insane/Balanced, Insane/Aggressive, Insane/Cautious,
#               Insane/Collector; with another -n: the pilot's setup);
#               the style may be a style profile's /bot word or name
#   -S FILE     a style profile (.botstyle) for the game's botstyles/
#               folder (repeatable)
#   -F FILE[@BOT]  compare the bot BOT (else the bots named like the
#               profile's callsign) with this profile's measured values
#               (movrec-analyse --fidelity, repeatable)
#   -s SECONDS  game time (default 600)
#   -f FPS      frames per game second (default 200)
#   -r SEED     random seed (default 1)
#   -t SECONDS  wall-clock limit of the game (-botarena-timeout; default
#               max(120, game time / 5 + 60)); the script also kills the
#               game 30 s past it (timeout(1), if installed)
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
limit=
pilot=
out=
build="$here/build"
extra=
styles=
fidelity=
nl='
'

usage() {
	sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-2}"
}

while getopts n:b:s:f:r:t:p:o:B:x:S:F:h opt; do
	case $opt in
		n) bots=$OPTARG ;;
		b) list=$OPTARG; list_given=1 ;;
		s) seconds=$OPTARG ;;
		f) fps=$OPTARG ;;
		r) seed=$OPTARG ;;
		t) limit=$OPTARG ;;
		p) pilot=$OPTARG ;;
		o) out=$OPTARG ;;
		B) build=$OPTARG ;;
		x) extra="$extra $OPTARG" ;;
		S) styles="$styles$nl$OPTARG" ;;
		F) fidelity="$fidelity$nl$OPTARG" ;;
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
# Section 9.18 of multiplayer-bots.md: the style profiles the bots fly.
if [ -n "$styles" ]; then
	mkdir -p "$userdir/botstyles"
	oldifs=$IFS; IFS=$nl; set -f
	for f in $styles; do
		[ -z "$f" ] || cp "$f" "$userdir/botstyles/"
	done
	IFS=$oldifs; set +f
fi

# With -n but no -b, the bots play the pilot's setup (or the default).
if [ -n "$list_given" ] || [ "$bots" = 5 ]; then
	set -- -botarena-bots "$list"
else
	set --
fi
[ -z "$pilot" ] || set -- "$@" -pilot "$pilot"

# The game's own watchdog (-botarena-timeout) ends a run that hangs; as
# a backstop, timeout(1) kills it 30 s past that limit (SIGTERM, then
# SIGKILL 10 s later), and the trap kills it when the script ends
# (Ctrl-C, SIGTERM, an error) while the game still runs.
if [ -z "$limit" ]; then
	limit=$((seconds / 5 + 60))
	[ "$limit" -ge 120 ] || limit=120
fi
set -- "$@" -botarena-timeout "$limit"
timeout_cmd=
for t in timeout gtimeout; do
	if command -v "$t" > /dev/null 2>&1; then
		timeout_cmd="$t --kill-after=10 $((limit + 30))"
		break
	fi
done
[ -n "$timeout_cmd" ] || echo "botarena-run: no timeout(1) found; only the game's own watchdog limits the run" >&2

# A hung game does not answer SIGTERM (SDL turns it into a quit event
# the game never reads): SIGKILL for the game (the child of timeout(1))
# and the process the script started.
children=
cleanup() {
	for pid in $children; do
		pkill -KILL -P "$pid" 2> /dev/null || :
		kill -KILL "$pid" 2> /dev/null || :
	done
}
trap cleanup EXIT
trap 'cleanup; trap - EXIT; exit 130' INT
trap 'cleanup; trap - EXIT; exit 143' TERM HUP

echo "botarena-run: $mission level $level, $bots bots, $seconds s at $fps fps, seed $seed, wall-clock limit $limit s; output in $out"
# shellcheck disable=SC2086
HOME="$home" SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}" \
	$timeout_cmd "$game" -hogdir "$data" -nosound -nomusic -notitles -nomovies -nojoystick -nomouse \
	-recordmoves -recordmoves-bots \
	-botarena "$mission" "$level" "$bots" "$seconds" -fixedfps "$fps" -botarena-seed "$seed" \
	"$@" $extra > "$out/game.log" 2>&1 &
children=$!
status=0
wait "$children" || status=$?
children=
if [ "$status" -ne 0 ]; then
	grep -a 'botarena' "$out/game.log" >&2 || tail -20 "$out/game.log" >&2
	case $status in
		3) why='the wall-clock limit (watchdog)' ;;
		4) why='no game time progress (watchdog)' ;;
		124|137) why="timeout(1) after $((limit + 30)) s" ;;
		*) why="status $status" ;;
	esac
	echo "botarena-run: the game failed ($why); the log is $out/game.log" >&2
	exit 1
fi
grep -a 'botarena: ' "$out/game.log" | sed 's/^.*botarena: /  /'

recording=$(ls -t "$userdir"/recordings/*.dmr 2>/dev/null | head -n 1)
if [ -z "$recording" ]; then
	echo "botarena-run: no recording in $userdir/recordings" >&2
	exit 1
fi
mkdir -p "$out/analysis"
if [ -n "$missions" ]; then
	set -- --missions "$missions"
else
	set --
fi
oldifs=$IFS; IFS=$nl; set -f
for f in $fidelity; do
	[ -z "$f" ] || set -- "$@" --fidelity "$f"
done
IFS=$oldifs; set +f
"$analyse" --bots "$@" --out "$out/analysis" "$recording" > "$out/analysis.log" 2>&1 || {
	status=$?
	tail -20 "$out/analysis.log" >&2
	echo "botarena-run: movrec-analyse failed (status $status); its log is $out/analysis.log" >&2
	exit 1
}
echo "botarena-run: recording $recording"
echo "botarena-run: reports in $out/analysis (analysis log $out/analysis.log)"
[ -z "$fidelity" ] || sed -n '/^== fidelity/,/^$/p' "$out/analysis.log"
