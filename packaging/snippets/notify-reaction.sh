#!/usr/bin/env bash
# omen-kbd: replace the subshell at the end of
# ~/dotfiles/config-rice/.config/rice/bin/rice-notify-sound (the script swaync runs for every
# notification) with the one below. It keeps the do-not-disturb check and the ping, and adds one
# line that tells omen-kbd, so the keyboard flashes. Keep the file's own header comment.
#
# It is not a second swaync script entry: swaync runs only one script per notification.
# The flash line sits in the not-in-DND branch, so do-not-disturb silences it like the ping. It runs
# detached (`&`) and before paplay, so neither waits for the other, and swaync never waits for either.
# The daemon decides the rest: it skips low urgency and the focused app's own notifications. The
# line uses the full path because ~/.local/bin is not on the PATH of what Hyprland starts.

( [ "$(swaync-client -D -sw 2>/dev/null)" = true ] || {
    ~/.local/bin/omen-kbd notify --app "$SWAYNC_APP_NAME" \
        --desktop-entry "$SWAYNC_DESKTOP_ENTRY" --urgency "$SWAYNC_URGENCY" &
    paplay /usr/share/sounds/freedesktop/stereo/message.oga
} ) >/dev/null 2>&1 &
