# Every goal the firmware Makefile offers, forwarded. `lean` and `profile` were missing
# and are documented as `make lean` / `make profile` in this repository's build notes --
# so the two commands the production and profiling images are built with failed at the
# repository root with "No rule to make target", and only worked if you already knew to
# cd into z3660-firmware/Z-TURN/vitis_ide.
.PHONY: all build bootbin lean profile q2probe images fsbl z3660 z3660_emu z3660_emu_lean \
        z3660_emu_prof z3660_emu_q2probe bsp clean clean-all version

FIRMWARE_DIR := z3660-firmware/Z-TURN/vitis_ide

all build bootbin lean profile q2probe images fsbl z3660 z3660_emu z3660_emu_lean \
z3660_emu_prof z3660_emu_q2probe bsp clean clean-all version:
	$(MAKE) -C $(FIRMWARE_DIR) $@
