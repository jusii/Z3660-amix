#!/bin/bash
# build.sh -- assemble ztest.rom (512 KB) with vasm from the amigadev/crosstools docker image.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/w" -w /w amigadev/crosstools:m68k-amigaos \
  /opt/m68k-amigaos/bin/vasmm68k_mot -quiet -Fbin -m68060 -o ztest.raw -L ztest.lst ztest.s
python3 fixsum.py ztest.raw ztest.rom
rm -f ztest.raw
sha256sum ztest.rom
