#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Sanity-check both board profiles without building firmware.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail=0

check_file() {
	if [[ ! -e "$1" ]]; then
		echo "MISSING: $1"
		fail=1
	else
		echo "ok      $1"
	fi
}

check_board() {
	local board="$1"
	local defconfig="$2"
	local board_dir="$3"
	local dtb="$4"
	local defaults="$5"

	echo "== $board =="
	check_file "br2-external/configs/$defconfig"
	check_file "$board_dir/linux.config"
	check_file "$board_dir/init"
	check_file "$board_dir/genimage.cfg"
	check_file "$board_dir/post-build.sh"
	check_file "$board_dir/post-image.sh"
	check_file "$board_dir/rootfs-overlay/etc/init.d/S10sdcard"
	check_file "$board_dir/rootfs-overlay/etc/init.d/S99banner"
	check_file "bootloader/$defaults"
	check_file "linux/arch/riscv/boot/dts/espressif/${dtb}.dts"

	if ! grep -q "CONFIG_BUILTIN_DTB_NAME=\"espressif/${dtb}\"" \
		"$board_dir/linux.config"; then
		echo "FAIL: $board_dir/linux.config BUILTIN_DTB_NAME != espressif/${dtb}"
		fail=1
	else
		echo "ok      builtin DTB espressif/${dtb}"
	fi

	if ! grep -q "BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=.*${board_dir#br2-external/}/linux.config" \
		"br2-external/configs/$defconfig"; then
		echo "FAIL: $defconfig does not point at $board_dir/linux.config"
		fail=1
	else
		echo "ok      defconfig -> linux.config"
	fi
}

check_board korvo-1 \
	esp32s31_defconfig \
	br2-external/board/esp32s31 \
	esp32s31_generic \
	sdkconfig.defaults.korvo-1

check_board function-coreboard-1 \
	esp32s31_fcb1_defconfig \
	br2-external/board/esp32s31-fcb1 \
	esp32s31_function_coreboard_1 \
	sdkconfig.defaults.function-coreboard-1

# FCB1 must enable the Ethernet IPC netdev; Korvo must leave it off.
if ! grep -q '^CONFIG_ESP32S31_ETH=y$' \
	br2-external/board/esp32s31-fcb1/linux.config; then
	echo "FAIL: FCB1 linux.config missing CONFIG_ESP32S31_ETH=y"
	fail=1
else
	echo "ok      FCB1 enables ESP32S31_ETH"
fi
if ! grep -q '^# CONFIG_ETHERNET is not set$' \
	br2-external/board/esp32s31/linux.config; then
	echo "FAIL: Korvo linux.config should leave CONFIG_ETHERNET unset"
	fail=1
else
	echo "ok      Korvo leaves Ethernet disabled"
fi

# The generated source patch must match linux/ + shared IPC headers.
python3 scripts/mkkernelpatches.py >/tmp/mkkernelpatches.out
if ! git diff --quiet -- linux/patches/0000-esp32s31-add-source-files.patch; then
	echo "FAIL: linux/patches/0000-esp32s31-add-source-files.patch is stale"
	git --no-pager diff --stat -- linux/patches/0000-esp32s31-add-source-files.patch
	fail=1
else
	echo "ok      generated kernel source patch is current"
fi

# DTB Makefile must list both boards.
for dtb in esp32s31_generic esp32s31_korvo1 esp32s31_function_coreboard_1; do
	if ! grep -q "$dtb" linux/arch/riscv/boot/dts/espressif/Makefile; then
		echo "FAIL: dts Makefile missing $dtb"
		fail=1
	fi
done
echo "ok      dts Makefile lists board DTBs"

exit "$fail"
