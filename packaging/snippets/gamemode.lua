-- omen-kbd: in ~/dotfiles/config-hypr/.config/hypr/modules/modes.lua, inside
-- rice.gamemode.toggle(), right after `rice.set_mode("game", on)`, add the line
-- below. The default config's "gamemode" rule then switches the keyboard to the
-- "game" profile while game mode is on.
-- The daemon does not save this flag: if it restarts during game mode, toggle
-- game mode off and on again (SUPER+F1 twice).

    hl.exec_cmd("~/.local/bin/omen-kbd flag gamemode " .. (on and "on" or "off"))
