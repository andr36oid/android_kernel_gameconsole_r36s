#!/bin/sh
# Build without rebuilding old kernel host tools.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-/tmp/ultra-x}
stock=${2:-/andr36oid/work/reports/clone-issue-3/stock.dts}
dtc=${DTC:-/andr36oid/src/out/target/product/r36s/obj/KERNEL_OBJ/scripts/dtc/dtc}
mkdir -p "$out"
cpp -nostdinc -I "$root/arch/arm64/boot/dts" \
    -I "$root/arch/arm64/boot/dts/rockchip" -I "$root/include" \
    -I "$root/scripts/dtc/include-prefixes" -undef -D__DTS__ \
    -x assembler-with-cpp \
    "$root/arch/arm64/boot/dts/rockchip/rk3326-r36s-clone-grc802.dts" \
    -o "$out/grc802.pp.dts"
"$dtc" -I dts -O dtb -@ -o "$out/rk3326-r36s-clone-grc802.dtb" "$out/grc802.pp.dts"
"$dtc" -I dtb -O dts -o "$out/grc802.decompiled.dts" "$out/rk3326-r36s-clone-grc802.dtb"
"$dtc" -I dts -O dtb -o "$out/stock.dtb" "$stock"
python3 "$root/scripts/grc802/verify-dtb.py" "$out/stock.dtb" "$out/rk3326-r36s-clone-grc802.dtb"
