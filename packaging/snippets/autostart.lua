-- omen-kbd: add this entry to the `commands` list in
-- ~/dotfiles/config-hypr/.config/hypr/modules/autostart.lua, after the
-- dbus-update-activation-environment line (that import gives the daemon
-- HYPRLAND_INSTANCE_SIGNATURE; it still finds Hyprland's socket without it).
-- Install the unit first (packaging/omen-kbd.service).

    -- Keyboard colors (omen-kbd daemon; profiles, mode colors, theme)
    "systemctl --user start omen-kbd.service",
