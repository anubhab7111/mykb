# SPDX-License-Identifier: GPL-2.0-only
KDIR ?= /usr/lib/modules/$(shell uname -r)/build

VM_GATE := gate-plain gate-board-no-wmi gate-board-substring gate-vendor-other
VM_ALL := $(VM_GATE)

.PHONY: all check vmtest-gate vmtest clean

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

vmtest: all
	@fail=0; for s in $(VM_ALL); do tests/vm/run.sh $$s || fail=1; done; exit $$fail

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf tests/vm/out
