#!/bin/bash
# Checks on the runbook and the recovery document: the mistakes in them that would hurt on hardware.
set -u

dir=$(cd "$(dirname "$0")/.." && pwd)
runbook=$dir/../../docs/hardware-validation.md
recovery=$dir/../../docs/recovery.md
status=0

problem() {
	echo "check-docs: $1" >&2
	status=1
}

# Steps get approved one at a time, often in a new terminal, and step 6 reboots. With D unset,
# "sudo tee $D/zones" reports success and writes a file called /zones on the root filesystem. So
# every block that uses D sets it first, and the colors to keep live in a file, not a variable.
for doc in "$runbook" "$recovery"; do
	while IFS= read -r finding; do
		problem "$finding"
	done < <(awk '
		/^```bash/ { block = 1; set = 0; kset = 0; next }
		/^```/ { block = 0; next }
		block && /^D=/ { set = 1 }
		block && /^K=/ { kset = 1 }
		block && /\$D/ && !set { print FILENAME ":" FNR ": uses $D before setting it in this block" }
		block && /\$K/ && !kset { print FILENAME ":" FNR ": uses $K before setting it in this block" }
		block && /\$V/ { print FILENAME ":" FNR ": a shell variable does not survive to the next step" }
	' "$doc")
done

# In P4 step 2, a pasted insmod line arrived split in two: restore_on_unload=0 ran as a shell
# variable and the module kept its default, so the unload wrote. So insmod lines stay short, and
# every load that relies on restore_on_unload=0 prints what the module really got.
while IFS= read -r finding; do
	problem "$finding"
done < <(awk '
	/^```bash/ { block = 1; pending = 0; next }
	/^```/ {
		if (pending) print FILENAME ":" pending ": nothing shows the module got restore_on_unload=0"
		block = 0; next
	}
	block && /insmod kernel\// { print FILENAME ":" FNR ": insmod the long path; use $K" }
	block && /insmod/ && /restore_on_unload=0/ { pending = FNR; next }
	block && pending && /parameters\/restore_on_unload/ { pending = 0 }
	block && pending && /insmod|rmmod|tee|poweroff/ {
		print FILENAME ":" pending ": nothing shows the module got restore_on_unload=0"
		pending = 0
	}
' "$runbook")

# This machine logs dozens of ACPI errors at every boot, and a grep for the module hides new ones.
grep -q 'sudo dmesg -W' "$runbook" || problem "the runbook must watch new kernel messages with dmesg -W"
if grep -q "grep -iE 'hp_omen_kbd|acpi'" "$runbook"; then
	problem "the runbook still greps old ACPI lines out of the whole log"
fi

# zones is a cache and info shows the boot copy: only a reload shows what a SET did to the table.
grep -q 'diff ~/omen-kbd-p4/table-step1 -' "$runbook" ||
	problem "the runbook must compare the table after a write with the one from step 1"

# HP's defaults can turn Secure Boot on, which this boot chain does not pass.
grep -q 'Secure Boot' "$recovery" || problem "the BIOS defaults step must warn about Secure Boot"

[ "$status" -eq 0 ] && echo "check-docs: ok"
exit "$status"
