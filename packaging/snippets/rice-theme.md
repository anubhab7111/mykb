# omen-kbd: rice-theme hook

In `~/dotfiles/config-rice/.config/rice/bin/rice-theme`, inside `reload_apps()`, next to the other
reloads (for example after the `swaync-client -rs -sw` line), add:

```bash
    ~/.local/bin/omen-kbd reload >/dev/null 2>&1 || true # keyboard colors follow the theme
```

`reload` re-reads `config.toml` and the new theme's palette in one step, so `theme:` colors switch
together with the rest of the desktop. The daemon also watches `~/.config/theming/current` and
reloads the palette by itself about 200 ms after a switch; this line makes the switch immediate and
still works if that watch ever misses one. `|| true` keeps a theme switch working while the daemon
is not running.
