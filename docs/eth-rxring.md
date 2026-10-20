# eth-rxring: a GEM RX descriptor ring (branch notes)

Branch `eth-rxring`, on top of `eth-profile`. Everything is behind build flags; with both flags off
the image is code-identical to the release build (only `__DATE__` and the build-id differ).

## What the flags do

- `ETH_RXRING=N` gives the GEM `N` RX descriptors instead of one (`RXBD_CNT`). The receive ISR
  loops `XEmacPs_BdRingFromHwRx` → copy into the backlog → `BdRingFree` → re-arm until no BD is
  left. The loop is needed because the BSP returns one BD per call while the ring is full
  (`HwTail == HwHead`). A BD goes back to the GEM only at re-arm; clearing `RxNew` before
  `BdRingFree`, as the single-BD code does, would let a new frame be wiped.
- Ring builds also cap `FRAME_MAX_BACKLOG` at 32, with an `#error` if the backlog would overrun
  `RX_BACKLOG_ADDRESS`'s region (it ends at `TX_FRAME_ADDRESS`). A full backlog resets, as before.
- `ETH_PROF=1` (from `eth-profile`) adds `ESTAT`'s `ring N isr isr0 isr_frames_max` line.

## The release keeps a latent backlog overrun

`FRAME_MAX_BACKLOG` is 64 slots of `FRAME_SIZE` (2048 B), but the backlog region is 64 KiB:
- slots 32–47 overwrite the TX frame buffer;
- slots 48–63 overwrite the RX DMA buffers. With one RxBD, slot 48 *is* RX buffer 0, so the copy
  overlaps itself.

With a single descriptor the MAC drops frames before the backlog grows that far; the ring removes
that brake. The ring builds carry the cap. Giving it to the release is a one-line move: take
`#undef`/`#define FRAME_MAX_BACKLOG 32` and its `#error` out of the `#if ETH_RXRING` block. That
changes the release image, so it is a review decision.

## Measured on an A4000D (real 68060, 100 Mbit)

**Amix (z3660eth):**
- The ring removes MAC loss: BUFFNA, resource errors and overruns are 0.
- UDP to 1600/s and 24-frame line-rate bursts reach the driver without loss; the remaining loss
  is the driver's slot bound.

**AmigaOS (AmiTCP_NG, Z3660Net.device):**
- The ring is not usable yet; TCP pushes stall.
- The RX ISR is held off for up to ~14 frame times (about 1.7 ms), so 16 descriptors overflow.
- The backlog also fills, because the driver drains it slowly.
- The source of the hold-off is open.
