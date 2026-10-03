Firmware
  This card runs a modified Z3660 firmware (fork 1.0, based on Z3660 1.03 BETA 21):
    fat/BOOT.BIN and fat/Z3660.bin   fork 1.0              crc32 C96EAD03
    fat/FAILSAFE.bin                 stock 1.03 BETA 21    crc32 C0ADE49A
  At power-on the Z3660 starts Z3660.bin. If that file is missing or broken, it falls back
  to FAILSAFE.bin. Check a file in the boot menu's SD file manager (key E, then
  "CRC 0:/Z3660.bin"), or with any CRC32 tool on a PC.

  Going back to stock:
  - Quick: delete Z3660.bin from the FAT partition. The card then starts the stock 1.03
    BETA 21 firmware (FAILSAFE.bin) through this card's start-up loader.
  - Full stock: copy FAILSAFE.bin over BOOT.BIN, then delete Z3660.bin. The card then
    holds exactly upstream's files.

  What differs on stock firmware:
  - kickstart 0 (the Kickstart in the Amiga's own ROM sockets) leaves a real
    68060/68040 at a black screen. Map a Kickstart file instead (kickstartN + kickstart N).
  - A rare timing race can apply a disk request to the previous request's block. The
    fork re-reads the request registers before using them.
  Presets, z3660cfg.txt and the CPLD are the same as stock.
