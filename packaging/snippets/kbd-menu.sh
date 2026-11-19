#!/bin/bash
# omen-kbd-menu: a rofi menu for the keyboard lighting.
#
# Install:  install -Dm755 packaging/snippets/kbd-menu.sh ~/.local/bin/omen-kbd-menu
# Bind it (modules/binds or wherever your launcher bind lives), with the full path because
# Hyprland's PATH lacks ~/.local/bin:
#     hl.bind("SUPER + K", hl.dsp.exec_cmd("~/.local/bin/omen-kbd-menu"))
# Uses your own rofi config and theme; icons come from rofi's icon theme through the row's icon
# field, so they show inside the icon box. Escape goes back one level; Escape at the top closes it.
# It only calls the omen-kbd CLI, so every change goes through the daemon like any other command.
# "Look" is one flow for `omen-kbd set`: a profile or colors, then an effect and its speed.
set -u

bin=~/.local/bin/omen-kbd
cfg=~/.config/omen-kbd/config.toml

# Icon names in the rofi icon theme (candy-icons here; other themes fall back to a generic icon).
ic_look=preferences-desktop-keyboard
ic_brightness=preferences-desktop-display
ic_power=system-shutdown
ic_effect=preferences-system-windows-effect-flipswitch
ic_doctor=utilities-system-monitor

menu() { # menu <prompt> [message]: rows on stdin, the picked text on stdout
	rofi -dmenu -i -p "$1" -mesg "${2:-}" -no-custom -format s
}
ask() { # ask <prompt> [message]: free text
	rofi -dmenu -p "$1" -mesg "${2:-}" -lines 0 </dev/null
}
row() { # row <text> <icon>: one rofi row with an icon
	printf '%s\0icon\x1f%s\n' "$1" "$2"
}
icons() { # icons <icon>: stdin lines become rows that all carry the same icon
	local l
	while IFS= read -r l; do row "$l" "$1"; done
}
run() { # run <omen-kbd args>: show the daemon's error, if any
	local out
	if ! out=$("$bin" "$@" 2>&1); then
		rofi -e "omen-kbd $*: ${out:-failed}"
	fi
}
state() { # one line for the menu header
	local j
	j=$("$bin" status --json 2>/dev/null) || {
		echo "omen-kbd daemon is not running"
		return
	}
	jq -r '"\(.profile) · \(if .animated then .effect else "static" end) · \(.brightness)% · \(if .on then "on" else "off" end)"' <<<"$j"
}
profiles() { # profile names from the config: only [[profiles]] tables, not [[rules]]
	awk '/^\[\[profiles\]\]/{p=1;next} /^\[/{p=0} p&&/^name *=/{gsub(/^name *= *"|".*$/,"");print}' "$cfg"
}

if ! command -v rofi >/dev/null || ! command -v jq >/dev/null; then
	echo "needs rofi and jq" >&2
	exit 1
fi
if ! "$bin" status >/dev/null 2>&1; then
	rofi -e "omen-kbd daemon is not running: systemctl --user start omen-kbd"
	exit 2
fi

while true; do
	# Piped straight into rofi: a $( ) capture would drop the NUL bytes that carry the icons.
	pick=$(
		{
			row Look "$ic_look"
			row Brightness "$ic_brightness"
			row "Power: toggle" "$ic_power"
			row Doctor "$ic_doctor"
		} | menu "Keyboard" "$(state)"
	) || exit 0
	case "$pick" in
	Look)
		sel=$( (
			profiles
			echo "Colors…"
			echo "Effect only…"
		) | icons "$ic_look" | menu "Look" "$(state)") || continue
		args=()
		case "$sel" in
		"Colors…")
			txt=$(ask "Colors" "1 color, or 4: left middle right wasd (e.g. #ff2d95 #7a3cff #00c2ff #ffb000)") || continue
			read -ra args <<<"$txt" # one to four words
			;;
		"Effect only…") ;;
		*) args=("$sel") ;;
		esac
		eff=$(printf '%s\n' keep static drift glow tide dusk |
			icons "$ic_effect" | menu "Effect" "keep leaves the effect as it is") || continue
		if [ "$eff" != keep ]; then
			args+=(--effect "$eff")
			if [ "$eff" != static ]; then
				spd=$(printf '%s\n' "slow (20)" "medium (50)" "fast (80)" | icons "$ic_effect" | menu "Speed" "$eff") || continue
				spd=${spd##*(}
				args+=(--speed "${spd%)}")
			fi
		fi
		[ "${#args[@]}" -gt 0 ] && run set "${args[@]}"
		;;
	Brightness)
		sel=$(printf '%s\n' 100 75 50 25 10 0 +10 -10 | icons "$ic_brightness" | menu "Brightness %" "$(state)") || continue
		run brightness "$sel"
		;;
	Power*) run toggle ;;
	Doctor)
		out=$("$bin" doctor 2>&1)
		printf '%s\n' "$out" | icons "$ic_doctor" | menu "doctor" "read-only checks" >/dev/null
		;;
	esac
done
