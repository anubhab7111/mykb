#!/bin/bash
# Builds the DKMS module and the Arch package as the user, in a temporary directory, and checks the
# udev rule and the sysusers file. Nothing is installed and nothing is loaded.
set -u

module=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$module/../.." && pwd)
packaging=$repo/packaging
release=$(uname -r)
status=0

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

problem() {
	echo "check-packaging: $1" >&2
	status=1
}

# The versions come first: the DKMS check below needs PACKAGE_VERSION.
version=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' "$module/dkms.conf" 2>/dev/null)
pkgver=$(sed -n 's/^pkgver=//p' "$packaging/arch/PKGBUILD" 2>/dev/null)
[ -n "$version" ] || problem "no PACKAGE_VERSION in dkms.conf"
[ "$version" = "$pkgver" ] || problem "dkms.conf has version '$version', PKGBUILD has '$pkgver'"

# What the package ships and nothing else, so this also proves that set builds on its own.
mkdir -p "$T/src/hp-omen-kbd-$version" "$T/tree"
if cp "$module"/{Kbuild,Makefile,dkms.conf,core.c,core.h,main.c} "$T/src/hp-omen-kbd-$version/"; then
	dkms_args=(-m hp-omen-kbd -v "$version" --sourcetree "$T/src" --dkmstree "$T/tree")
	dkms add "${dkms_args[@]}" >"$T/dkms-add.log" 2>&1 ||
		problem "dkms add failed: $(cat "$T/dkms-add.log")"
	dkms build "${dkms_args[@]}" -k "$release" >"$T/dkms-build.log" 2>&1 ||
		problem "dkms build failed: $(cat "$T/dkms-build.log")"
	grep -q 'Building module(s)\.\.\. done\.' "$T/dkms-build.log" ||
		problem "dkms build did not report a finished build: $(cat "$T/dkms-build.log")"

	ko=$T/tree/hp-omen-kbd/$version/$release/x86_64/module/hp_omen_kbd.ko.zst
	if [ -f "$ko" ]; then
		info=$(modinfo "$ko")
		grep -q '^depends: *wmi$' <<<"$info" || problem "the DKMS module does not depend on wmi"
		if grep -q '^alias:' <<<"$info"; then
			problem "the DKMS module has an alias: line, so udev would autoload it"
		fi
	else
		problem "dkms did not produce $ko"
	fi
else
	problem "a file the package ships is missing"
fi

rules=$packaging/udev/99-omen-kbd.rules
verify=$(udevadm verify "$rules" 2>&1) || problem "udevadm verify failed: $verify"
grep -Eq '^ *Success: +1$' <<<"$verify" || problem "udevadm verify: no 'Success: 1' in: $verify"
grep -Eq '^ *Fail: +0$' <<<"$verify" || problem "udevadm verify: no 'Fail: 0' in: $verify"

sysusers=$packaging/sysusers/omen-kbd.conf
dry_run=$(systemd-sysusers --dry-run "$sysusers" 2>&1) ||
	problem "systemd-sysusers failed: $dry_run"
# Once the package is installed the group exists and there is nothing left to create.
if ! getent group omen-kbd >/dev/null; then
	grep -q "^Creating group 'omen-kbd'" <<<"$dry_run" ||
		problem "systemd-sysusers would not create the group: $dry_run"
	grep -q '^Would write /etc/group' <<<"$dry_run" ||
		problem "systemd-sysusers would not write /etc/group: $dry_run"
fi

(cd "$packaging/arch" && BUILDDIR=$T/b PKGDEST=$T/p makepkg -f --nodeps) >"$T/makepkg.log" 2>&1 ||
	problem "makepkg failed: $(tail -n 20 "$T/makepkg.log")"
pkg=$(find "$T/p" -name 'hp-omen-kbd-dkms-*.pkg.tar*' 2>/dev/null | head -n 1)
if [ -n "$pkg" ]; then
	expected=$(sort <<EOF
usr/lib/modules-load.d/hp-omen-kbd.conf
usr/lib/sysusers.d/omen-kbd.conf
usr/lib/udev/rules.d/99-omen-kbd.rules
usr/src/hp-omen-kbd-$version/Kbuild
usr/src/hp-omen-kbd-$version/Makefile
usr/src/hp-omen-kbd-$version/core.c
usr/src/hp-omen-kbd-$version/core.h
usr/src/hp-omen-kbd-$version/dkms.conf
usr/src/hp-omen-kbd-$version/main.c
EOF
	)
	# Everything but the directories and pacman's own dot files.
	shipped=$(bsdtar -tf "$pkg" | grep -v '/$' | grep -v '^\.' | sort)
	[ "$shipped" = "$expected" ] ||
		problem "the package ships the wrong files: $(diff <(echo "$expected") <(echo "$shipped"))"
	bsdtar -tf "$pkg" | grep -qx '.INSTALL' || problem "the package has no install file"

	namcap_output=$(namcap "$pkg" 2>&1)
	[ -n "$namcap_output" ] && echo "$namcap_output"
	grep -q ' E: ' <<<"$namcap_output" && problem "namcap reports errors"
else
	problem "makepkg produced no package"
fi

[ "$status" -eq 0 ] && echo "check-packaging: ok"
exit "$status"
