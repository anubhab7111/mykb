#!/bin/bash
# Checks on the built module and its source that no VM scenario can see.
set -u

dir=$(cd "$(dirname "$0")/.." && pwd)
ko=$dir/hp_omen_kbd.ko
src=$dir/main.c
status=0

problem() {
	echo "check-module: $1" >&2
	status=1
}

if [ ! -f "$ko" ]; then
	echo "check-module: $ko is not built" >&2
	exit 1
fi
info=$(modinfo "$ko") || exit 1

field() {
	sed -n "s/^$1: *//p" <<<"$info"
}

[ "$(field license)" = GPL ] || problem "license is '$(field license)', want GPL"
[ "$(field depends)" = wmi ] || problem "depends is '$(field depends)', want wmi"

release=$(uname -r)
case "$(field vermagic)" in
"$release "*) ;;
*) problem "vermagic '$(field vermagic)' does not start with $release" ;;
esac

for param in readonly min_interval_ms max_writes_per_hour restore_on_unload; do
	grep -q "^parm: *$param:" <<<"$info" || problem "no parm: line for $param"
done

# An alias would make udev load the module at every boot.
if grep -q '^alias:' <<<"$info"; then
	problem "the module has an alias: line, so udev would autoload it"
fi

# omen-space bug K6: attributes added after the device miss the add uevent, and with it the udev
# rule that opens them to the omen-kbd group. They must be in dev.groups before the device is added.
groups_line=$(grep -n 'dev\.groups = ' "$src" | head -n 1 | cut -d: -f1)
add_line=$(grep -n 'platform_device_add(' "$src" | head -n 1 | cut -d: -f1)
if [ -z "$groups_line" ] || [ -z "$add_line" ]; then
	problem "main.c must set dev.groups and call platform_device_add"
elif [ "$groups_line" -ge "$add_line" ]; then
	problem "main.c sets dev.groups (line $groups_line) after platform_device_add (line $add_line)"
fi
if grep -Eq 'sysfs_create_groups?\(|\.dev_groups' "$src"; then
	problem "main.c must not create attributes with sysfs_create_group(s) or .dev_groups"
fi

[ "$status" -eq 0 ] && echo "check-module: ok"
exit "$status"
