# syntax=docker/dockerfile:1.6
#
# Z3660 build environment.
#
# Two target stages:
#   --target drivers-only   ~3 GB image, no Vitis. Amiga toolchain only.
#   --target full           ~25-30 GB image. Drivers + Xilinx Vitis 2023.2.2.
#
# The "full" target needs a Xilinx installer staged on the host and passed
# in via BuildKit named context `xilinx`. See docker/build.sh.

ARG UID=1000
ARG GID=1000

# ---------------------------------------------------------------------------
# base — common deps + non-root builder user
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS base

ARG UID
ARG GID

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        wget \
        git \
        make \
        sudo \
        locales \
        xz-utils \
        unzip \
        dos2unix && \
    locale-gen en_US.UTF-8

RUN groupadd -g ${GID} builder || groupmod -n builder $(getent group ${GID} | cut -d: -f1) && \
    useradd -u ${UID} -g ${GID} -ms /bin/bash builder && \
    echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder && \
    mkdir -p /work && chown builder:builder /work

# ---------------------------------------------------------------------------
# drivers-only — Amiga m68k toolchain, vbcc, NDK 3.9, amitools, SAS/C
# ---------------------------------------------------------------------------
FROM base AS drivers-only

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        autogen \
        libtool \
        bison \
        flex \
        gawk \
        gettext \
        gperf \
        texinfo \
        rsync \
        lhasa \
        ncurses-dev \
        libgmp-dev \
        libmpfr-dev \
        libmpc-dev \
        zlib1g-dev \
        python3 \
        python3-pip \
        python3-dev

# amiga-gcc → /opt/amiga (provides m68k-amigaos-gcc, vasmm68k_mot, NDK includes).
# Upstream Bebbo deleted his GitHub repo; using AmigaPorts community fork of
# the same toolchain.
RUN git clone --depth=1 https://github.com/AmigaPorts/m68k-amigaos-gcc.git /tmp/amiga-gcc && \
    cd /tmp/amiga-gcc && \
    make -j"$(nproc)" PREFIX=/opt/amiga update && \
    make -j"$(nproc)" PREFIX=/opt/amiga all && \
    cd / && rm -rf /tmp/amiga-gcc

# Many files installed by amiga-gcc come from .lha archives that preserve mode
# 0600. Open them up so the non-root `builder` user (which actually runs the
# Z3660 build) can read headers, libraries and the assembler include tree.
RUN chmod -R a+rX /opt/amiga

# The Z3660 driver Makefile references $(VBCC)/NDK_3.9/Include/include_i for
# vasm assembler includes. amiga-gcc ships these at /opt/amiga/m68k-amigaos/
# ndk-include; symlink so the existing Makefile finds them unmodified.
RUN mkdir -p /opt/vbcc/NDK_3.9/Include && \
    ln -sf /opt/amiga/m68k-amigaos/ndk-include /opt/vbcc/NDK_3.9/Include/include_i

# amitools (xdftool) — used by `make adf` to build 0_Z3660.adf.
# Ubuntu 22.04 ships pip 22.0.2 which can't parse amitools' modern pyproject.toml
# (falls back to an older sdist that triggers a Cython build of machine/emu.pyx);
# upgrade pip first.
RUN pip3 install --no-cache-dir --upgrade pip && \
    pip3 install --no-cache-dir amitools

ENV PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin \
    VBCC=/opt/vbcc

# The driver Makefile does `$(CP) ... $(UAEPATH)` to ~/Documents/FS-UAE/...
# Create a dummy target directory so those copies don't fail.
RUN mkdir -p /home/builder/Documents/FS-UAE/HardDrives/Almacen && \
    chown -R builder:builder /home/builder/Documents

USER builder
WORKDIR /work
CMD ["/bin/bash"]

# ---------------------------------------------------------------------------
# full — drivers-only + Xilinx Vitis Unified 2023.2 + Update 2 (2023.2.2)
# ---------------------------------------------------------------------------
FROM drivers-only AS full

USER root

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get install -y --no-install-recommends \
        libtinfo5 \
        libncurses5 \
        libstdc++6 \
        libxext6 \
        libxrender1 \
        libxtst6 \
        libxi6 \
        libgtk2.0-0 \
        libnss3 \
        net-tools \
        openssh-client

COPY install_config.txt /tmp/install_config.txt

# The xilinx context points at the host directory where the installer has been
# extracted. We locate xsetup inside the base installer dir (older naming:
# Xilinx_Unified_*; newer: FPGAs_AdaptiveSoCs_Unified_*) and run it in batch
# mode. If the user has only staged tarballs, the build.sh wrapper unpacks
# them first so we never deal with raw archives in here.
RUN --mount=type=bind,from=xilinx,target=/xilinx-src \
    set -eux; \
    XSETUP=$(find /xilinx-src -maxdepth 3 \
        \( -path '*FPGAs_AdaptiveSoCs_Unified_2023.2*/xsetup' \
        -o -path '*Xilinx_Unified_2023.2*/xsetup' \
        -o -path '*Unified_2023.2_1013*/xsetup' \) \
        -type f 2>/dev/null | head -n1); \
    if [ -z "$XSETUP" ]; then \
      echo "ERROR: base installer xsetup not found under /xilinx-src" >&2; \
      ls /xilinx-src/ >&2; exit 1; \
    fi; \
    echo "Running base installer: $XSETUP"; \
    "$XSETUP" \
        --agree XilinxEULA,3rdPartyEULA \
        --batch Install \
        --config /tmp/install_config.txt

# Update 2 (2023.2.2). Applied on top of the base install. Skipped if the
# user hasn't staged it.
RUN --mount=type=bind,from=xilinx,target=/xilinx-src \
    set -eux; \
    XSETUP=$(find /xilinx-src -maxdepth 3 \
        -path '*Vivado_Vitis_Update_2023.2.2*/xsetup' \
        -type f 2>/dev/null | head -n1); \
    if [ -z "$XSETUP" ]; then \
      echo "WARNING: Update 2023.2.2 not staged, skipping"; \
      exit 0; \
    fi; \
    echo "Applying update: $XSETUP"; \
    "$XSETUP" \
        --agree XilinxEULA,3rdPartyEULA \
        --batch Update \
        --config /tmp/install_config.txt

ENV XILINX_ROOT=/opt/Xilinx \
    XILINX_VITIS=/opt/Xilinx/Vitis/2023.2 \
    XILINX_VIVADO=/opt/Xilinx/Vivado/2023.2 \
    LM_LICENSE_FILE=""

RUN printf 'source /opt/Xilinx/Vitis/2023.2/settings64.sh\n' > /etc/profile.d/xilinx.sh && \
    printf 'source /opt/Xilinx/Vitis/2023.2/settings64.sh\n' >> /home/builder/.bashrc && \
    chown builder:builder /home/builder/.bashrc

# mkbootimage — used by the firmware Makefile (BOOTGEN_EXE_HOST) to assemble
# BOOT.BIN from FSBL + bitstream + ELFs. The Makefile defaults to mkbootimage
# rather than Xilinx's native bootgen because the project was authored on macOS;
# we keep that default and install mkbootimage to /usr/local/bin.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get install -y --no-install-recommends pkg-config libssl-dev uuid-dev libelf-dev && \
    git clone --depth=1 https://github.com/antmicro/zynq-mkbootimage.git /tmp/mkbootimage && \
    cd /tmp/mkbootimage && \
    make -j"$(nproc)" && \
    install -m 755 mkbootimage /usr/local/bin/mkbootimage && \
    cd / && rm -rf /tmp/mkbootimage

ENV BOOTGEN_EXE_HOST=/usr/local/bin/mkbootimage

USER builder
WORKDIR /work
CMD ["/bin/bash", "-l"]
