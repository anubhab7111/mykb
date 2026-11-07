#!/bin/bash
# omen-kbd-menu: a rofi menu for the keyboard lighting.
#
# Install:  install -Dm755 packaging/snippets/kbd-menu.sh ~/.local/bin/omen-kbd-menu
# Bind it (modules/binds or wherever your launcher bind lives), with the full path because
# Hyprland's PATH lacks ~/.local/bin:
#     hl.bind("SUPER + K", hl.dsp.exec_cmd("~/.local/bin/omen-kbd-menu"))
# Uses your own rofi config and theme. Escape goes back one level; Escape at the top closes it.
# It only calls the omen-kbd CLI, so every change goes through the daemon like any other command.
set -u

bin=~/.local/bin/omen-kbd
cfg=~/.config/omen-kbd/config.toml

menu() { # menu <prompt> [message]: choices on stdin, the pick on stdout
	rofi -dmenu -i -p "$1" -mesg "${2:-}" -no-custom -format s
}
ask() { # ask <prompt> [message]: free text
	rofi -dmenu -p "$1" -mesg "${2:-}" -lines 0 </dev/null
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

accents="rosewater flamingo pink mauve red maroon peach yellow green teal sky sapphire blue lavender"

while true; do
	pick=$(printf '%s\n' \
		"󰌌  Profile" "󰃟  Brightness" "󰐥  Power: toggle" "󰓅  Effect" "󰏘  Color" "󰋠  Doctor" |
		menu "Keyboard" "$(state)") || exit 0
	case "$pick" in
	*Profile)
		sel=$( (profiles; echo custom) | menu "Profile" "$(state)") || continue
		run profile "$sel"
		;;
	*Brightness)
		sel=$(printf '%s\n' 100 75 50 25 10 0 +10 -10 | menu "Brightness %" "$(state)") || continue
		run brightness "$sel"
		;;
	*Power*) run toggle ;;
	*Effect)
		sel=$(printf '%s\n' static breathe wave cycle gradient | menu "Effect" "$(state)") || continue
		spd=""
		if [ "$sel" != static ]; then
			spd=$(printf '%s\n' "slow (20)" "medium (50)" "fast (80)" | menu "Speed" "$sel") || continue
			spd=${spd##*(}
			spd=${spd%)}
		fi
		if [ -n "$spd" ]; then
			run effect "$sel" --speed "$spd"
		else
			run effect "$sel"
		fi
		;;
	*Color)
		sel=$( (
			for a in $accents; do echo "theme:$a"; done
			echo "Custom hex…"
		) | menu "Color (all zones)" "$(state)") || continue
		if [ "$sel" = "Custom hex…" ]; then
			sel=$(ask "Hex" "1 color, or 4: left middle right wasd (e.g. ff2d95 7a3cff 00c2ff ffb000)") || continue
		fi
		read -ra cols <<<"$sel" # one to four words
		[ "${#cols[@]}" -gt 0 ] && run set "${cols[@]}"
		;;
	*Doctor)
		out=$("$bin" doctor 2>&1)
		printf '%s\n' "$out" | menu "doctor" "read-only checks" >/dev/null
		;;
	esac
done
