#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Generate the USB support table for Settings > USB devices (the UsbMode app
# in device/gameconsole/common, assets/usb_support.txt) straight from the
# kernel's own MODULE_DEVICE_TABLE(usb, ...) entries, so the page can tell
# "no driver for this chip" from "there is a driver, but it did not start".
#
# Usage: gen-usb-support-table.sh <scratch objtree> <output file> [defconfig]
#
#   <scratch objtree>  empty or throw-away O= directory (a few hundred MB)
#   <output file>      the table, e.g. .../UsbMode/assets/usb_support.txt
#   [defconfig]        default lineageos_r36s_defconfig
#
# ARCH and CROSS_COMPILE come from the environment, as for a normal build.
#
# Built-in drivers have no .mod.c (file2alias only runs for modules), so the
# scratch build switches every tristate option that is =y in the defconfig
# to =m, checks that exactly the same options are still on, and builds only
# the modules in the folders that hold USB drivers. modpost then writes the
# same MODULE_ALIAS("usb:...") lines the kernel matches devices with.
#
# Output lines (see UsbTable.java):
#   D <module> <kind>          kind: wifi bluetooth network audio gamepad
#                              input storage serial video other
#   A <module> <pattern>       one device table entry, modalias glob
#                              without the "usb:" prefix
#   C <vid> <pid> <wifi|modem> starts as a driver CD, usb-storage switches it
#   U <vid> <pid> <chip>       well-known chip we have no driver for
#   N <module> <chip>          plain chip name for a Wi-Fi driver

set -euo pipefail

OBJ=${1:?usage: $0 <scratch objtree> <output file> [defconfig]}
OUTFILE=${2:?usage: $0 <scratch objtree> <output file> [defconfig]}
DEFCONFIG=${3:-lineageos_r36s_defconfig}
KSRC=$(cd "$(dirname "$0")/../.." && pwd)
: "${ARCH:=arm64}"
export ARCH
JOBS=${JOBS:-$(nproc)}
# the kernel commit for the header (git may not work inside the build chroot)
REV=${KERNEL_REV:-$(git -C "$KSRC" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}

mk() {
	make -C "$KSRC" O="$OBJ" "$@"
}

mkdir -p "$OBJ"
OBJ=$(cd "$OBJ" && pwd)

# 1. the real configuration, and which options are on in it
mk -s "$DEFCONFIG" >/dev/null
cp "$OBJ/.config" "$OBJ/.config.real"
on_symbols() {
	sed -n 's/^\(CONFIG_[A-Za-z0-9_]*\)=[ym]$/\1/p' "$1" | sort
}

# 2. the folders with USB device tables (not the USB core, host and gadget
# side, nor USB displays: their options change the rest of the config),
# built as whole folders three levels deep (drivers/net/usb, sound/usb)
grep -rlE --include='*.c' 'MODULE_DEVICE_TABLE[[:space:]]*\([[:space:]]*usb[[:space:]]*,' \
	"$KSRC/drivers" "$KSRC/sound" | sed "s|^$KSRC/||" | xargs -n1 dirname | sort -u |
	grep -vE '^drivers/usb$|^drivers/usb/(core|gadget|host|dwc2|dwc3|phy|common|typec)(/|$)|^drivers/(gpu|video)/' \
	>"$OBJ/srcdirs.txt"
cut -d/ -f1-3 "$OBJ/srcdirs.txt" | sort -u |
	awk '{ for (p in seen) if (index($0, p "/") == 1) next; seen[$0] = 1; print }' >"$OBJ/dirs.txt"

# 3. their tristate options (Kconfig files from each source folder up to its
# build folder): =y -> =m, then make sure no option went on or off
while read -r d; do
	top=$(awk -v d="$d" 'index(d "/", $0 "/") == 1 { print; exit }' "$OBJ/dirs.txt")
	while :; do
		for k in "$KSRC/$d"/Kconfig*; do
			[ -f "$k" ] && echo "$k"
		done
		[ "$d" = "$top" ] || [ "$d" = . ] && break
		d=$(dirname "$d")
	done
done <"$OBJ/srcdirs.txt" | sort -u >"$OBJ/kconfigs.txt"
# shellcheck disable=SC2016 # awk fields, not shell
xargs awk '
	/^[ \t]*(menu)?config[ \t]+[A-Za-z0-9_]+/ { sym = $2; next }
	/^[ \t]*(tristate|def_tristate)([ \t]|$)/ { if (sym != "") print "CONFIG_" sym; sym = ""; next }
	/^[ \t]*(bool|def_bool|int|hex|string)([ \t]|$)/ { sym = "" }
' <"$OBJ/kconfigs.txt" | sort -u >"$OBJ/tristate.txt"
awk 'NR == FNR { t[$1] = 1; next }
     /^CONFIG_[A-Za-z0-9_]*=y$/ { split($0, a, "="); if (a[1] in t) { print a[1] "=m"; next } }
     { print }' "$OBJ/tristate.txt" "$OBJ/.config.real" >"$OBJ/.config"
mk -s olddefconfig >/dev/null
if ! diff <(on_symbols "$OBJ/.config.real") <(on_symbols "$OBJ/.config") >"$OBJ/config.diff"; then
	echo "the =m build turns other options on or off:" >&2
	cat "$OBJ/config.diff" >&2
	exit 1
fi

# 4. build the modules of those folders; modpost writes the .mod.c files
mk -s -j"$JOBS" modules_prepare
while read -r d; do
	log=$OBJ/build-${d//\//_}.log
	echo "building $d" >&2
	mk -k -j"$JOBS" KBUILD_MODPOST_WARN=1 M="$d" modules >"$log" 2>&1 ||
		echo "  some objects in $d failed, see $log" >&2
done <"$OBJ/dirs.txt"

# 5. the table
kind_of() {
	case $1 in
	drivers/net/wireless/*|drivers/staging/rtl8*|drivers/staging/vt6656*) echo wifi ;;
	drivers/bluetooth/*) echo bluetooth ;;
	drivers/net/*) echo network ;;
	sound/*) echo audio ;;
	drivers/input/joystick/*) echo gamepad ;;
	drivers/hid/*|drivers/input/*) echo input ;;
	drivers/usb/storage/*) echo storage ;;
	drivers/usb/serial/*|drivers/usb/class/cdc-acm*) echo serial ;;
	drivers/media/*) echo video ;;
	*) echo other ;;
	esac
}

TMP=$OUTFILE.tmp
{
	echo "# USB support table for Settings > USB devices. Generated by"
	echo "# kernel scripts/android/gen-usb-support-table.sh from $DEFCONFIG"
	echo "# at kernel $REV. Do not edit."
	echo "# D module kind / A module modalias-glob / C vid pid wifi|modem /"
	echo "# U vid pid chip-without-driver / N module chip"

	while IFS= read -r modc; do
		rel=${modc#"$OBJ"/}
		name=$(basename "$modc" .mod.c)
		aliases=$(sed -n 's/^MODULE_ALIAS("usb:\(.*\)");$/\1/p' "$modc")
		[ -n "$aliases" ] || continue
		echo "D $name $(kind_of "$rel")"
		printf '%s\n' "$aliases" | sed "s/^/A $name /"
	done < <(find "$OBJ/drivers" "$OBJ/sound" -name '*.mod.c' 2>/dev/null | sort)

	# Devices usb-storage switches out of their driver-CD mode (only when
	# usb-storage is built)
	if grep -q '^CONFIG_USB_STORAGE=[ym]' "$OBJ/.config.real"; then
		awk '
			/^[ \t]*UNUSUAL_DEV[ \t]*\(/ { buf = ""; inb = 1 }
			inb { buf = buf " " $0 }
			inb && /\)[ \t]*,?[ \t]*$/ {
				inb = 0
				if (match(buf, /UNUSUAL_DEV[ \t]*\([ \t]*0x[0-9a-fA-F]+[ \t]*,[ \t]*0x[0-9a-fA-F]+/)) {
					s = substr(buf, RSTART, RLENGTH)
					gsub(/UNUSUAL_DEV[ \t]*\([ \t]*0x|[ \t]/, "", s)
					split(s, id, ",0x")
					kind = ""
					if (buf ~ /usb_stor_wifi_eject_init/) kind = "wifi"
					else if (buf ~ /usb_stor_huawei_e220_init|option_ms_init|sierra_ms_init|usb_stor_modeswitch_init/) kind = "modem"
					if (kind != "") print "C " tolower(id[1]) " " tolower(id[2]) " " kind
				}
			}' "$KSRC/drivers/usb/storage/unusual_devs.h"
		if [ -f "$KSRC/drivers/usb/storage/unusual_modeswitch.h" ]; then
			sed -n 's/^MODESWITCH_[A-Z]*([ \t]*0x\([0-9a-fA-F]*\),[ \t]*0x\([0-9a-fA-F]*\).*/C \1 \2 modem/p' \
				"$KSRC/drivers/usb/storage/unusual_modeswitch.h" |
				awk '{ print $1, tolower($2), tolower($3), $4 }'
		fi
	fi

	# Plain chip names for the Wi-Fi drivers (shown next to the maker's name)
	cat <<-'EOF'
	N 8188eu Realtek RTL8188EUS
	N 8188fu Realtek RTL8188FU
	N 8188gu Realtek RTL8188GU
	N 8192eu Realtek RTL8192EU
	N 8192fu Realtek RTL8192FU
	N 8812au Realtek RTL8812AU
	N 8821au Realtek RTL8811AU/8821AU
	N 8814au Realtek RTL8814AU
	N 8821cu Realtek RTL8811CU/8821CU
	N 88x2bu Realtek RTL8812BU/8822BU
	N 8852bu Realtek RTL8852BU/8832BU
	N rtl8192cu Realtek RTL8188CUS/8192CU
	N rtl8xxxu Realtek RTL8723BU/8188/8192
	N rtl8187 Realtek RTL8187
	N rt2800usb Ralink/MediaTek RT2870-RT5572
	N rt73usb Ralink RT2571W/RT2671
	N rt2500usb Ralink RT2570
	N mt7601u MediaTek MT7601U
	N mt76x0u MediaTek MT7610U
	N mt76x2u MediaTek MT7612U
	N ath9k_htc Atheros AR9271/AR7010
	N carl9170 Atheros AR9170
	N ar5523 Atheros AR5523
	N ath6kl_usb Atheros AR6004
	N brcmfmac Broadcom USB
	N mwifiex_usb Marvell USB
	N usb8xxx Marvell 88W8388/8682
	N vt6656_stage VIA VT6656
	N zd1211rw ZyDAS ZD1211
	N zd1201 ZyDAS ZD1201
	N p54usb Prism54 USB
	N at76c50x-usb Atmel AT76C50x
	N rsi_usb Redpine RSI 9113
	N aic8800_fdrv AICSemi AIC8800
	N aic_load_fw AICSemi AIC8800
	N rndis_wlan Broadcom 4320 RNDIS
	EOF
} >"$TMP"

# Well-known chips people plug in that have no driver here (Wi-Fi parts; the
# Bluetooth part of the same dongle may still work). Left out when the build
# gains a driver for the ID.
while read -r vid pid chip; do
	if grep -qiE "^A [^ ]+ v${vid}p${pid}d" "$TMP"; then
		echo "note: $vid:$pid ($chip) now has a driver, left out of the U list" >&2
		continue
	fi
	echo "U $vid $pid $chip"
done >"$TMP.u" <<'EOF'
0e8d 7961 MediaTek MT7921AU
0e8d 7925 MediaTek MT7925U
0bda 885a Realtek RTL8852AU
0bda 885c Realtek RTL8852AU
0bda 8832 Realtek RTL8832AU
0bda c82c Realtek RTL8822CU
0bda c82e Realtek RTL8822CU
0bda c812 Realtek RTL8812CU
0bda d723 Realtek RTL8723DU
0bda c832 Realtek RTL8852CU
0bda b851 Realtek RTL8851BU
EOF

cat "$TMP.u" >>"$TMP"
rm -f "$TMP.u"
mv "$TMP" "$OUTFILE"
echo "$(grep -c '^D ' "$OUTFILE") drivers, $(grep -c '^A ' "$OUTFILE") IDs," \
	"$(grep -c '^C ' "$OUTFILE") driver-CD IDs, $(grep -c '^U ' "$OUTFILE") known chips without a driver" >&2
