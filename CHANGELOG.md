# Changelog

## 2026-07-07

- **piscsi: read-only 2048-byte CD-ROM units.** Expose a SCSI target as a
  read-only CD-ROM backed by a raw ISO/UDF image, alongside the existing RDB
  hardfiles. `struct piscsi_dev_` gains a `pdt` (peripheral device type) field;
  a new read-only mailbox register `PISCSI_CMD_PDT` (0xA0) returns the current
  drive's `pdt` and is the single source of truth for device type. A CD-flagged
  slot in `piscsi_map_drive()` bypasses the RDB scan (a bare optical image has no
  RDB), opens its backing image read-only, reports a 2048-byte block size
  (`BLOCKS = file_size / 2048`), sets `pdt = 0x05`, and uses benign non-zero CHS
  geometry; the normal disk path is unchanged. New config key
  `cdrom_units <id[,id...]>` flags SCSI target IDs as CD-ROM units, threaded
  through `piscsi_init()` into `piscsi_map_drive()`. Removability (start/stop,
  prevent/allow, media-change, unit attention) is deferred — the medium is
  assumed always present in this phase.
- **a3000_scsi (emulated A3000 WD33C93): present a read-only CD-ROM to the stock
  Amix kernel.** A backend unit flagged as a CD-ROM (via `PISCSI_CMD_PDT`, `pdt =
  0x05`) is shown as a read-only optical drive — INQUIRY device-type 0x05 + RMB +
  `"AMIX CD-ROM"`, READ CAPACITY at 2048-byte blocks, MODE SENSE(6)/(10)
  write-protect bit with the rigid-disk geometry pages dropped, WRITE(6)/(10)
  rejected with CHECK CONDITION / DATA PROTECT, and PREVENT/ALLOW MEDIUM REMOVAL as
  a GOOD no-op. The disk path (`pdt = 0x00`) is unchanged. New host unit test
  `test/host/scsi_cd_test.cpp` asserts the exact CDB response bytes (34/34) — the
  byte-level reference the native z3660scsi driver (Path A) must match. READ TOC
  and removability/media-change are deferred to a later increment.
