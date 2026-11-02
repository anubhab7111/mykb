# SPDX-License-Identifier: GPL-2.0-only
KDIR ?= /usr/lib/modules/$(shell uname -r)/build

VM_GATE := gate-plain gate-board-no-wmi gate-board-substring gate-vendor-other
VM_EMU := emu-basic emu-kbd-type-3 emu-kbd-type-0 emu-kbd-query-fails emu-short-reply
VM_RW := rw-readonly rw-invalid rw-write-and-reload rw-header-drift rw-restore-on-unload \
	 rw-off-keeps-colors rw-budget rw-interval rw-interval-clamp rw-firmware-error \
	 rw-resume-reapply rw-resume-one-kind rw-resume-nothing-written rw-resume-unload
VM_ALL := $(VM_GATE) $(VM_EMU) $(VM_RW)

SSDT_VARIANTS := KBD_TYPE_3 KBD_TYPE_0 KBD_RET_3 SHORT_GET SET_RET_5 HEADER_DRIFT
ASL := tests/vm/hp-bios-wmi.asl
OUT := tests/vm/out
IASL := iasl -vs -vr -we

.PHONY: all check ssdt vmtest-gate vmtest clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# A clean W=1 build (so every file really is compiled), the style gate and the module checks.
check:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 KCFLAGS=-Werror modules
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree -f main.c core.c core.h
	tests/check-module.sh

# Every scenario runs even after a failure, so one run shows them all.
vmtest-gate: all
	@fail=0; for s in $(VM_GATE); do tests/vm/run.sh $$s || fail=1; done; exit $$fail

vmtest: all ssdt
	@fail=0; for s in $(VM_ALL); do tests/vm/run.sh $$s || fail=1; done; exit $$fail

# The firmware emulator, once as the default and once per variant.
ssdt:
	@command -v iasl >/dev/null || { echo "install acpica for the emulator scenarios" >&2; exit 1; }
	@mkdir -p $(OUT)
	@$(IASL) -p $(OUT)/hp-bios-wmi $(ASL) >$(OUT)/iasl.log
	@for v in $(SSDT_VARIANTS); do \
		$(IASL) -p $(OUT)/hp-bios-wmi-$$v -D $$v $(ASL) >>$(OUT)/iasl.log || exit 1; \
	done

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf tests/vm/out
