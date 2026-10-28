#!/usr/bin/env bash
# Record the user guide's clips on this Mac: `just guide-record [name…]` (all
# of docs/clips/*.tape by default) → docs/src/media/<name>.mp4. `--dry-run`
# first: only parse the tapes and print their steps.
#
# Each clip runs in its own Ghostty window (docs/clips/ghostty.conf, no user
# config) on the tutorial's images, with fresh rega state. QuickRecord
# records the window (--record-rect / --stop / --export, 0.0.6+) and burns in
# the keys the tape pressed, each its own chip (this script writes the log),
# and ffmpeg squeezes the result under the size budget.
#
# With AeroSpace (a tiling window manager) each clip's window goes to its own
# workspace, `rega-guide`, filling the screen; the script switches there for
# the take and back after. Otherwise the window is the tape's Size.
#
# Needs, once: System Settings › Privacy & Security › Accessibility for the
# terminal running this (keystrokes and clicks), and QuickRecord running with
# Screen Recording and Input Monitoring. Hands off the keyboard and mouse while
# it runs: a take stops if the clip's window loses focus. The Mac stays awake
# meanwhile (caffeinate).
#
# Tape files, one command a line (# starts a comment line):
#   Output  name.mp4            docs/src/media/name.mp4 (default: <tape>.mp4);
#                               name.png: a still, the take's last frame
#   Size    100x28              window size in cells (default; fits a 13" MacBook)
#   Setup   <shell>             before the window opens ($TUT, $WORK set)
#   Run     <command>           what the window runs ($TUT, $WORK set)
#   Wait    <seconds>           before recording starts (default 2)
#   Pre     <step>              a step before recording starts (Pre Key -)
#   Sleep   <seconds>
#   Key     <key> [xN] [delay]  a key, N times, delay seconds apart (0.5)
#   Type    <text>              typed as text
#   Click   <fx> <fy> [ctrl]    at a fraction of the window (0..1)
#   Move    <fx> <fy>
#   Drag    <fx0> <fy0> <fx1> <fy1> [right]
#   Spawn   <shell>             in the background while recording
# Keys: a single character (G, [, :) or Enter Esc Tab Space Backspace Up Down
# Left Right PageUp PageDown F1-F12, with Ctrl+ / Shift+ / Alt+ in front.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
rega=$(cd "$root" && realpath "${REGA_BIN:-build/macos-arm64-Release/rega}")
qr=${QUICKRECORD:-/Applications/QuickRecord.app/Contents/MacOS/QuickRecord}
ghostty=${GHOSTTY_APP:-/Applications/Ghostty.app}
clips=$root/docs/clips
media=$root/docs/src/media
budget=${GUIDE_CLIP_BUDGET:-3145728} # bytes per clip
maxWidth=${GUIDE_CLIP_WIDTH:-1600}   # px, the guide's content column is narrower
# What the clips show lives here: rega prints full paths, and one under
# $HOME would put the user name in every take.
stage=${GUIDE_STAGE:-/Users/Shared/rega-guide}
cache=$stage/cache       # XDG_CACHE_HOME for every rega here
tut=$cache/rega/tutorial
render=$stage/render     # $WORK: a tape's own files (a render coming in)

die() { echo "guide-record: $*" >&2; exit 1; }
dry=0
if [ "${1:-}" = --dry-run ]; then
	dry=1
	shift
fi
tapes=()
if [ $# -eq 0 ]; then
	tapes=("$clips"/*.tape)
else
	for n in "$@"; do tapes+=("$clips/${n%.tape}.tape"); done
fi
for t in "${tapes[@]}"; do [ -f "$t" ] || die "no tape $t"; done

[ "$(uname)" = Darwin ] || die "records on macOS only"
work=""
homeWorkspace="" # AeroSpace: where to come back to
clipWid=""       # AeroSpace: the clip window's id
ghosttyPid=""
spawned=()
if [ "$dry" = 0 ]; then # the Mac side: not for --dry-run
	command -v ffmpeg >/dev/null || die "ffmpeg missing (it's in the dev shell)"
	[ -x "$rega" ] || die "no rega at $rega: just build"
	[ -d "$ghostty" ] || die "no Ghostty at $ghostty"
	"$qr" --help 2>/dev/null | grep -q -- --record-rect || die "$qr has no --record-rect: QuickRecord 0.0.6+ needed"

	# No display or system sleep (nor the screen lock that comes with it)
	# while recording; caffeinate ends with this script.
	caffeinate -dimsu -w $$ &

	work=$(mktemp -d "${TMPDIR:-/tmp}/rega-guide.XXXXXX")
	cleanup()
	{
		if [ -n "$ghosttyPid" ]; then kill "$ghosttyPid" 2>/dev/null || true; fi
		for p in "${spawned[@]+"${spawned[@]}"}"; do kill "$p" 2>/dev/null || true; done
		"$qr" --stop >/dev/null 2>&1 || true # a take that failed half way
		if [ -n "$homeWorkspace" ]; then aerospace workspace "$homeWorkspace" >/dev/null 2>&1 || true; fi
		rm -rf "$work"
	}
	trap cleanup EXIT

	swiftc -O "$root/scripts/guide-mouse.swift" -o "$work/guide-mouse" 2>/dev/null || die "swiftc failed (Xcode Command Line Tools?)"

	# The tutorial's images, rendered when missing or out of date.
	mkdir -p "$stage"
	XDG_CACHE_HOME=$cache "$rega" --tutorial-render >/dev/null ||
		die "the tutorial didn't render into $tut"
fi

# --- the window --------------------------------------------------------------

as() { osascript -e "$1"; }

# GUIDE_DEBUG=1: which process is in front, after each step.
dbg()
{
	if [ "${GUIDE_DEBUG:-0}" = 1 ]; then
		echo "   [debug] $1: front $(osascript -e 'tell application "System Events" to get unix id of (first process whose frontmost is true)'), clip $ghosttyPid" >&2
	fi
}

# Run System Events commands on the clip's window only while it's in front.
inFront()
{
	as "tell application \"System Events\"
		-- The id at once: a process held in a variable is looked up again by
		-- name, and two Ghostty processes share one.
		set frontPid to unix id of (first process whose frontmost is true)
		if frontPid is not $ghosttyPid then error \"the clip's window lost focus to \" & (name of (first process whose unix id is frontPid)) & \" (\" & frontPid & \")\"
		$1
	end tell" >/dev/null
}

# x y w h of the window, in points.
windowRect()
{
	as "tell application \"System Events\" to tell (first process whose unix id is $ghosttyPid)
		set {x, y} to position of window 1
		set {w, h} to size of window 1
		return (x as text) & \" \" & y & \" \" & w & \" \" & h
	end tell"
}

openWindow() # cols rows command
{
	local cols=$1 rows=$2 launcher=$work/launch.sh
	{
		echo '#!/usr/bin/env bash'
		echo "export TUT='$tut' WORK='$render' XDG_CACHE_HOME='$cache' XDG_STATE_HOME='$work/state' XDG_CONFIG_HOME='$work/config'"
		echo "export PATH='$(dirname "$rega")':\"\$PATH\""
		echo 'cd "$TUT"'
		echo "$3"
		echo 'exec sleep 1000' # keep the window when rega quits early
	} >"$launcher"
	chmod +x "$launcher"
	open -na "$ghostty" --args --config-default-files=false \
		--config-file="$clips/ghostty.conf" --window-width="$cols" --window-height="$rows" \
		--title=rega-guide --command="$launcher"
	ghosttyPid=""
	for _ in $(seq 40); do
		ghosttyPid=$(pgrep -n -f "$ghostty/Contents/MacOS/ghostty.*rega-guide" || true)
		if [ -n "$ghosttyPid" ] && windowRect >/dev/null 2>&1; then break; fi
		sleep 0.25
	done
	[ -n "$ghosttyPid" ] || die "the Ghostty window didn't open"
	if command -v aerospace >/dev/null; then
		# A tiling window manager would squeeze the window in beside the
		# others: give it a workspace of its own, and go there.
		[ -n "$homeWorkspace" ] || homeWorkspace=$(aerospace list-workspaces --focused)
		clipWid=""
		for _ in $(seq 20); do
			clipWid=$(aerospace list-windows --monitor all --pid "$ghosttyPid" --format '%{window-id}' | head -1)
			if [ -n "$clipWid" ]; then break; fi
			sleep 0.25
		done
		[ -n "$clipWid" ] || die "AeroSpace doesn't see the clip's window"
		# An on-window-detected rule may move it too, maybe after us: move it
		# until it stays, with its workspace in front.
		local placed=0
		for _ in $(seq 10); do
			aerospace move-node-to-workspace --focus-follows-window --window-id "$clipWid" rega-guide >/dev/null 2>&1 || true
			sleep 1
			if aerospace list-windows --workspace rega-guide --format '%{window-id}' | grep -qx "$clipWid" &&
				[ "$(aerospace list-workspaces --focused)" = rega-guide ]; then
				placed=1
				break
			fi
		done
		[ "$placed" = 1 ] || die "AeroSpace keeps the clip's window off the rega-guide workspace"
		# The layout settles, and a notification the rule posts (macOS shows
		# them ~5 s) leaves before the take starts.
		dbg "placed"
		sleep "${GUIDE_SETTLE:-6}"
		dbg "settled"
		aerospace focus --window-id "$clipWid"
		dbg "aerospace focus"
	fi
	as "tell application \"System Events\" to set frontmost of (first process whose unix id is $ghosttyPid) to true"
	dbg "set frontmost"
	sleep 0.5
}

closeWindow()
{
	if [ -n "$ghosttyPid" ]; then kill "$ghosttyPid" 2>/dev/null || true; fi
	ghosttyPid=""
	if [ -n "$homeWorkspace" ]; then aerospace workspace "$homeWorkspace" >/dev/null 2>&1 || true; fi
	for p in "${spawned[@]+"${spawned[@]}"}"; do kill "$p" 2>/dev/null || true; done
	spawned=()
}

# --- input -------------------------------------------------------------------

# The chips' log, written from the tape (not the event tap: it merges a run
# of letters and spaces like typing, and keys another program posts come
# without Shift): one line per key or click, seconds into the take.
keylog=""
t0=""
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
sinceStart() { awk -v n="$(now)" -v t="$t0" 'BEGIN { printf "%.3f", n - t }'; }
logKey() # label typed(0/1)
{
	if [ -n "$keylog" ]; then printf '%s\tkey\t%s\t%s\n' "$(sinceStart)" "$2" "$1" >>"$keylog"; fi
}
logClick() # global x y
{
	if [ -n "$keylog" ]; then printf '%s\tclick\t%s\t%s\n' "$(sinceStart)" "$1" "$2" >>"$keylog"; fi
}

key() # Ctrl+Shift+x
{
	local spec=$1 mods=() k label="" prefix=""
	while [[ $spec == *+?* ]]; do
		case ${spec%%+*} in
			Ctrl) mods+=("control down") && prefix+="⌃" ;;
			Shift) mods+=("shift down") && prefix+="⇧" ;;
			Alt) mods+=("option down") && prefix+="⌥" ;;
			Cmd) mods+=("command down") && prefix+="⌘" ;;
			*) die "unknown modifier in $1" ;;
		esac
		spec=${spec#*+}
	done
	local using=""
	if [ ${#mods[@]} -gt 0 ]; then
		using=" using {$(IFS=,; echo "${mods[*]}")}"
	fi
	case $spec in
		Enter) k="key code 36" label="↩" ;;
		Esc) k="key code 53" label="⎋" ;;
		Tab) k="key code 48" label="⇥" ;;
		Space) k="key code 49" label="␣" ;;
		Backspace) k="key code 51" label="⌫" ;;
		Left) k="key code 123" label="←" ;;
		Right) k="key code 124" label="→" ;;
		Down) k="key code 125" label="↓" ;;
		Up) k="key code 126" label="↑" ;;
		PageUp) k="key code 116" label="⇞" ;;
		PageDown) k="key code 121" label="⇟" ;;
		F1) k="key code 122" ;; F2) k="key code 120" ;; F3) k="key code 99" ;;
		F4) k="key code 118" ;; F5) k="key code 96" ;; F6) k="key code 97" ;;
		F7) k="key code 98" ;; F8) k="key code 100" ;; F9) k="key code 101" ;;
		F10) k="key code 109" ;; F11) k="key code 103" ;; F12) k="key code 111" ;;
		\") k='keystroke quote' ;;
		\\) k='keystroke "\\"' ;;
		?) k="keystroke \"$spec\"" ;;
		*) die "unknown key $1" ;;
	esac
	logKey "$prefix${label:-$spec}" 0
	inFront "$k$using"
}

typeText()
{
	logKey "$1" 1
	local text=${1//\\/\\\\}
	inFront "keystroke \"${text//\"/\\\"}\""
}

# A fraction of the window → global points.
at() # fx fy
{
	read -r x y w h <<<"$(windowRect)"
	awk -v x="$x" -v y="$y" -v w="$w" -v h="$h" -v fx="$1" -v fy="$2" \
		'BEGIN { printf "%d %d", x + w * fx, y + h * fy }'
}

mouse()
{
	inFront "" # in front, or stop
	if [ "$1" = click ]; then
		logClick "$2" "$3"
		if [ "${4:-}" = ctrl ]; then logKey "⌃ click" 0; fi
	fi
	"$work/guide-mouse" "$@"
}

# One tape step: Sleep, Key, Type, Click, Move, Drag or Spawn.
runStep() # step tape
{
	local step=$1 cmd args
	cmd=${step%% *}
	args=$(sed -E 's/^[A-Za-z]+[[:space:]]*//' <<<"$step")
	# shellcheck disable=SC2046 # at() prints "x y": split on purpose
	case $cmd in
		Sleep) sleep "$args" ;;
		Key)
			local k rest n=1 d=0.5
			read -r k rest <<<"$args"
			for word in $rest; do
				case $word in x*) n=${word#x} ;; *) d=$word ;; esac
			done
			for ((i = 0; i < n; i++)); do
				key "$k"
				if ((i + 1 < n)); then sleep "$d"; fi
			done
			sleep 0.15
			;;
		Type) typeText "$args" ;;
		Click) mouse click $(at $(awk '{print $1, $2}' <<<"$args")) $(awk '{print $3}' <<<"$args") ;;
		Move) mouse move $(at $args) ;;
		Drag)
			local a b c d e
			read -r a b c d e <<<"$args"
			mouse drag $(at "$a" "$b") $(at "$c" "$d") $e
			;;
		Spawn)
			(cd "$tut" && TUT=$tut WORK=$render bash -c "$args") &
			spawned+=($!)
			;;
		*) die "$2: unknown command $cmd" ;;
	esac
}

# --- one clip ----------------------------------------------------------------

record() # tape
{
	local tape=$1 name output size=100x28 wait=2 run="" setup=() pre=() steps=()
	name=$(basename "$tape" .tape)
	output=$name.mp4
	while IFS= read -r line || [ -n "$line" ]; do
		line=$(sed -e 's/^[[:space:]]*//; s/[[:space:]]*$//' <<<"$line")
		if [ -z "$line" ] || [[ $line == \#* ]]; then continue; fi
		case ${line%% *} in
			Output) output=${line#Output } && output=${output## } ;;
			Size) size=$(awk '{print $2}' <<<"$line") ;;
			Wait) wait=$(awk '{print $2}' <<<"$line") ;;
			Run) run=$(sed -E 's/^Run[[:space:]]+//' <<<"$line") ;;
			Setup) setup+=("$(sed -E 's/^Setup[[:space:]]+//' <<<"$line")") ;;
			Pre) pre+=("$(sed -E 's/^Pre[[:space:]]+//' <<<"$line")") ;;
			*) steps+=("$line") ;;
		esac
	done <"$tape"
	[ -n "$run" ] || die "$tape: no Run line"
	for step in "${pre[@]+"${pre[@]}"}" "${steps[@]}"; do
		case ${step%% *} in
			Sleep | Key | Type | Click | Move | Drag | Spawn) ;;
			*) die "$tape: unknown command ${step%% *}" ;;
		esac
	done
	if [ "$dry" = 1 ]; then
		echo "── $name → $output  ${size}  run: $run  (${#steps[@]} steps)"
		for s in "${setup[@]+"${setup[@]}"}"; do echo "   setup: $s"; done
		for s in "${pre[@]+"${pre[@]}"}"; do echo "   pre: $s"; done
		for s in "${steps[@]}"; do echo "   $s"; done
		return
	fi

	echo "── $name"
	rm -rf "$work/state" "$work/config" "$render"
	mkdir -p "$work/state" "$work/config" "$render"
	for s in "${setup[@]+"${setup[@]}"}"; do
		(cd "$tut" && TUT=$tut WORK=$render bash -c "$s")
	done
	openWindow "${size%x*}" "${size#*x}" "$run"
	sleep "$wait"
	# Before the take: not recorded, no chips (keylog is off until it starts).
	for step in "${pre[@]+"${pre[@]}"}"; do runStep "$step" "$tape"; done
	dbg "waited"

	# The pointer is recorded, and rega reads it hovering: park it in the
	# menu bar above the window (outside the take; a window edge would show a
	# resize pointer), else mid status line, till a step moves it.
	if [ -n "$clipWid" ]; then aerospace focus --window-id "$clipWid"; fi
	dbg "focus again"
	read -r x y w h <<<"$(windowRect)"
	if [ "$y" -ge 8 ]; then
		mouse move "$((x + w * 7 / 10))" "$((y / 2))"
	else
		# shellcheck disable=SC2046 # at() prints "x y"
		mouse move $(at 0.6 0.985)
	fi
	sleep 0.3
	local rx ry rw rh
	read -r rx ry rw rh <<<"$(windowRect)"
	"$qr" --record-rect "$rx,$ry,$rw,$rh" >/dev/null || die "$name: quickrecord didn't start"
	t0=$(now)
	keylog=$work/keys.tsv
	: >"$keylog"
	sleep 0.5 # ffmpeg's first frames
	for step in "${steps[@]}"; do runStep "$step" "$tape"; done
	sleep 0.5

	local answer id master scale
	answer=$("$qr" --stop) || die "$name: quickrecord --stop failed"
	id=$(awk '{print $2}' <<<"$answer")
	master=${answer#*$'\t'}
	closeWindow
	# The chips' log next to the master (x.mp4 → x.input.json), clicks in its
	# pixels from the window's points.
	scale=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$master" |
		awk -v w="$rw" '{ printf "%.4f", $1 / w }')
	awk -F'\t' -v x="$rx" -v y="$ry" -v s="$scale" '
		BEGIN { printf "{\"version\":1,\"events\":[" }
		{
			if (NR > 1) printf ","
			if ($2 == "click")
				printf "{\"t\":%s,\"k\":\"click\",\"x\":%d,\"y\":%d}", $1, ($3 - x) * s, ($4 - y) * s
			else {
				l = $4; gsub(/\\/, "\\\\", l); gsub(/"/, "\\\"", l)
				printf "{\"t\":%s,\"k\":\"key\",\"label\":\"%s\"%s}", $1, l, ($3 == "1" ? ",\"typed\":true" : "")
			}
		}
		END { print "]}" }' "$keylog" >"${master%.mp4}.input.json"
	keylog=""
	"$qr" --export "$id" --out "$work/raw.mp4" --keys-corner bottom-right >/dev/null ||
		die "$name: quickrecord --export failed"

	mkdir -p "$media"
	local crf=24 dest=$media/$output
	if [[ $output == *.png ]]; then
		# A still: the take's last frame, at full size (no chips: the keys
		# aren't part of it).
		ffmpeg -loglevel error -y -sseof -0.3 -i "$master" -frames:v 1 \
			-vf "scale='min($maxWidth,iw)':-2:flags=lanczos" "$dest"
		echo "   $dest  $(($(wc -c <"$dest") / 1024)) KB"
		return
	fi
	# H.264 every browser plays, sized for the page, no audio, playable while
	# loading; the CRF goes up until it fits.
	while :; do
		ffmpeg -loglevel error -y -i "$work/raw.mp4" -an \
			-vf "scale='min($maxWidth,iw)':-2:flags=lanczos,format=yuv420p" \
			-c:v libx264 -preset slow -crf "$crf" -movflags +faststart "$dest"
		local bytes
		bytes=$(wc -c <"$dest" | tr -d " ")
		[ "$bytes" -le "$budget" ] && break
		crf=$((crf + 3))
		if [ "$crf" -gt 36 ]; then die "$name: $bytes bytes even at CRF 36 (budget $budget): shorten it"; fi
	done
	echo "   $dest  $((bytes / 1024)) KB  crf $crf"
}

for t in "${tapes[@]}"; do
	record "$t"
done

# Terminal setup's comparison: the same close-up of the two stills, full
# resolution left, half-blocks right; rebuilt when either was just recorded.
if [ "$dry" = 0 ] && printf '%s\n' "${tapes[@]}" | grep -q '/still-' &&
	[ -f "$media/still-fullres.png" ] && [ -f "$media/still-halfblock.png" ]; then
	crop="crop=iw*0.5:ih*0.48:iw*0.28:ih*0.28"
	ffmpeg -loglevel error -y -i "$media/still-fullres.png" -i "$media/still-halfblock.png" \
		-filter_complex "[0]${crop}[a];[1]${crop}[b];[a][b]hstack=inputs=2" \
		"$media/graphics-compare.png"
	echo "   $media/graphics-compare.png"
fi
