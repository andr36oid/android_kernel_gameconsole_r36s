#!/bin/sh
# Compile the board changes and link SV6160 using the prepared kernel exports.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-/tmp/ultra-x/kernel}
symvers=${SYMVERS:-/andr36oid/src/out/target/product/r36s/obj/KERNEL_OBJ/Module.symvers}
compiler=${CROSS_COMPILE:-aarch64-linux-gnu-}
mkdir -p "$out"
make -C "$root" O="$out" ARCH=arm64 CROSS_COMPILE="$compiler" LOCALVERSION= lineageos_r36s_defconfig
make -C "$root" O="$out" ARCH=arm64 CROSS_COMPILE="$compiler" LOCALVERSION= modules_prepare
make -C "$root" O="$out" ARCH=arm64 CROSS_COMPILE="$compiler" LOCALVERSION= -j"${JOBS:-8}" \
    drivers/input/joystick/adc-joystick.o sound/soc/rockchip/rockchip_multicodecs.o \
    drivers/mmc/host/dw_mmc.o drivers/mmc/core/host.o drivers/mmc/core/sdio_cis.o
if [ "$symvers" != "$out/Module.symvers" ]; then
    cp "$symvers" "$out/Module.symvers"
fi
make -C "$root" O="$out" ARCH=arm64 CROSS_COMPILE="$compiler" LOCALVERSION= -j"${JOBS:-8}" \
    M=drivers/net/wireless/seekwave/sv6160 modules
