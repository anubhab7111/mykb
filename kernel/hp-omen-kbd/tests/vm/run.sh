#!/bin/bash
# Runs one scenario: boots this machine's own kernel in QEMU with the module and tests/vm/init.c,
# then compares the VM lines with <scenario>.expected. This is the only place the module is loaded.
set -euo pipefail

name=${1:?usage: run.sh <scenario>}
vm_dir=$(cd "$(dirname "$0")" && pwd)
module_dir=$(cd "$vm_dir/../.." && pwd)
scenario=$vm_dir/$name.scenario
expected=$vm_dir/$name.expected
out=$vm_dir/out
work=$out/$name

kernel=/boot/vmlinuz-linux
release=$(uname -r)
ko=$module_dir/hp_omen_kbd.ko
wmi_ko=/usr/lib/modules/$release/kernel/drivers/platform/wmi/wmi.ko.zst

refuse() {
	echo "run.sh: $name: refusing to run: $1" >&2
	exit 2
}

[ -f "$scenario" ] || refuse "$scenario does not exist"
[ -f "$expected" ] || refuse "$expected does not exist"
[ -f "$ko" ] || refuse "$ko is not built"
[ -f "$wmi_ko" ] || refuse "$wmi_ko does not exist"

# After a kernel upgrade without a reboot the image on disk is not the kernel the module was built
# for, and the VM would test another kernel than the one this machine runs.
image_version=$(file -bL "$kernel" | sed -n 's/.*version \([^ ]*\) .*/\1/p')
[ "$image_version" = "$release" ] || refuse "$kernel is '$image_version', this machine runs $release"
ko_version=$(modinfo -F vermagic "$ko" | cut -d' ' -f1)
[ "$ko_version" = "$release" ] || refuse "the module was built for '$ko_version', not $release"

mkdir -p "$out"
if [ ! "$out/init" -nt "$vm_dir/init.c" ]; then
	gcc -static -O2 -Wall -Wextra -Werror -o "$out/init" "$vm_dir/init.c"
fi

rm -rf "$work"
mkdir -p "$work/root/dev" "$work/root/proc" "$work/root/sys"
cp "$out/init" "$work/root/init"
cp "$wmi_ko" "$ko" "$work/root/"
cp "$scenario" "$work/root/scenario"
bsdtar -c --format=newc -f "$work/initramfs.cpio" -C "$work/root" .

read -r -a extra <<<"$(sed -n 's/^# qemu: //p' "$scenario")"
extra=("${extra[@]//@SSDT@/$out/hp-bios-wmi.aml}")

qemu_status=0
timeout 120 qemu-system-x86_64 -enable-kvm -cpu host -m 256 -nographic -no-reboot \
	-kernel "$kernel" -initrd "$work/initramfs.cpio" \
	-append 'console=ttyS0 panic=-1 loglevel=4' "${extra[@]}" \
	</dev/null >"$work/console.raw" 2>&1 || qemu_status=$?
tr -d '\r' <"$work/console.raw" >"$work/console.log"
grep '^VM ' "$work/console.log" >"$work/got" || true

if [ "$qemu_status" -ne 0 ]; then
	echo "run.sh: $name: FAIL: QEMU exited with $qemu_status (124 is the timeout); see $work/console.log" >&2
	exit 1
fi
# A crash after the last command would still leave matching VM lines behind.
if ! grep -q '^HARNESS done$' "$work/console.log"; then
	echo "run.sh: $name: FAIL: the scenario did not finish; see $work/console.log" >&2
	exit 1
fi
if ! diff -u "$expected" "$work/got"; then
	echo "run.sh: $name: FAIL; see $work/console.log" >&2
	exit 1
fi
echo "run.sh: $name: ok"
