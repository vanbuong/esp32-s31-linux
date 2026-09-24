#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Sanity-check board profiles without building firmware.
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

check_board function-coreboard-1-spi-sd \
	esp32s31_fcb1_spi_sd_defconfig \
	br2-external/board/esp32s31-fcb1-spi-sd \
	esp32s31_function_coreboard_1_spi_sd \
	sdkconfig.defaults.function-coreboard-1

# FCB1 variants must enable the Ethernet IPC netdev; Korvo must leave it off.
if ! grep -q '^CONFIG_ESP32S31_ETH=y$' \
	br2-external/board/esp32s31-fcb1/linux.config; then
	echo "FAIL: FCB1 linux.config missing CONFIG_ESP32S31_ETH=y"
	fail=1
else
	echo "ok      FCB1 enables ESP32S31_ETH"
fi
if ! grep -q '^CONFIG_ESP32S31_ETH=y$' \
	br2-external/board/esp32s31-fcb1-spi-sd/linux.config; then
	echo "FAIL: FCB1-SPI-SD linux.config missing CONFIG_ESP32S31_ETH=y"
	fail=1
else
	echo "ok      FCB1-SPI-SD enables ESP32S31_ETH"
fi
if ! grep -q '^# CONFIG_ETHERNET is not set$' \
	br2-external/board/esp32s31/linux.config; then
	echo "FAIL: Korvo linux.config should leave CONFIG_ETHERNET unset"
	fail=1
else
	echo "ok      Korvo leaves Ethernet disabled"
fi

# SDIO profile keeps DW MMC; SPI-SD profile uses mmc_spi + GPSPI3.
if ! grep -q '^CONFIG_MMC_DW=y$' \
	br2-external/board/esp32s31-fcb1/linux.config; then
	echo "FAIL: FCB1 linux.config missing CONFIG_MMC_DW=y"
	fail=1
else
	echo "ok      FCB1 enables MMC_DW"
fi
if ! grep -q '^CONFIG_MMC_SPI=y$' \
	br2-external/board/esp32s31-fcb1-spi-sd/linux.config; then
	echo "FAIL: FCB1-SPI-SD linux.config missing CONFIG_MMC_SPI=y"
	fail=1
else
	echo "ok      FCB1-SPI-SD enables MMC_SPI"
fi
if ! grep -q '^CONFIG_SPI_ESP32S31=y$' \
	br2-external/board/esp32s31-fcb1-spi-sd/linux.config; then
	echo "FAIL: FCB1-SPI-SD linux.config missing CONFIG_SPI_ESP32S31=y"
	fail=1
else
	echo "ok      FCB1-SPI-SD enables SPI_ESP32S31"
fi
if grep -q '^CONFIG_SPI_GPIO=y$' \
	br2-external/board/esp32s31-fcb1-spi-sd/linux.config; then
	echo "FAIL: FCB1-SPI-SD linux.config still enables CONFIG_SPI_GPIO"
	fail=1
else
	echo "ok      FCB1-SPI-SD leaves SPI_GPIO off"
fi
if ! grep -q 'compatible = "mmc-spi-slot"' \
	linux/arch/riscv/boot/dts/espressif/esp32s31_function_coreboard_1_spi_sd.dts; then
	echo "FAIL: SPI-SD DTS missing mmc-spi-slot"
	fail=1
else
	echo "ok      SPI-SD DTS has mmc-spi-slot"
fi
if ! grep -q 'compatible = "esp,esp32s31-spi"' \
	linux/arch/riscv/boot/dts/espressif/esp32s31_function_coreboard_1_spi_sd.dts \
	linux/arch/riscv/boot/dts/espressif/esp32s31.dtsi; then
	echo "FAIL: DTS missing esp,esp32s31-spi GPSPI host"
	fail=1
else
	echo "ok      DTS has esp,esp32s31-spi"
fi
if ! grep -q 'esp,gpio-ctrl = <&gpio>' \
	linux/arch/riscv/boot/dts/espressif/esp32s31.dtsi; then
	echo "FAIL: GPSPI must share GPIO matrix via esp,gpio-ctrl (avoids EBUSY)"
	fail=1
elif grep -A6 'spi3: spi@20390000' \
	linux/arch/riscv/boot/dts/espressif/esp32s31.dtsi |
	grep -q '0x20583000'; then
	echo "FAIL: spi3 still claims GPIO matrix regs (conflicts with gpio driver)"
	fail=1
else
	echo "ok      GPSPI uses shared esp,gpio-ctrl (no GPIO reg claim)"
fi
if ! grep -q 'of_iomap' linux/drivers/spi/spi-esp32s31.c; then
	echo "FAIL: GPSPI host must of_iomap GPIO matrix (not request_mem_region)"
	fail=1
else
	echo "ok      GPSPI host maps GPIO matrix without exclusive claim"
fi
if ! grep -q 'spi-max-frequency = <40000000>' \
	linux/arch/riscv/boot/dts/espressif/esp32s31_function_coreboard_1_spi_sd.dts; then
	echo "FAIL: SPI-SD DTS not requesting 40 MHz GPSPI clock"
	fail=1
else
	echo "ok      SPI-SD DTS requests 40 MHz"
fi
if ! test -f linux/drivers/spi/spi-esp32s31.c; then
	echo "FAIL: missing linux/drivers/spi/spi-esp32s31.c"
	fail=1
else
	echo "ok      GPSPI host driver present"
fi
if ! test -f linux/patches/0012-spi-esp32s31-gpspi-host.patch; then
	echo "FAIL: missing 0012-spi-esp32s31-gpspi-host.patch"
	fail=1
else
	echo "ok      GPSPI Kconfig patch present"
fi

if ! grep -q 'CONFIG_USB_VIDEO_CLASS=y' \
	br2-external/board/esp32s31-fcb1-spi-sd/linux.config \
	br2-external/board/esp32s31-fcb1/linux.config; then
	echo "FAIL: FCB1 linux.config missing USB Video Class"
	fail=1
else
	echo "ok      FCB1 enables USB_VIDEO_CLASS"
fi
if ! grep -q 'BR2_PACKAGE_CAM2FB=y' \
	br2-external/configs/esp32s31_fcb1_spi_sd_defconfig \
	br2-external/configs/esp32s31_fcb1_defconfig; then
	echo "FAIL: FCB1 defconfig missing cam2fb"
	fail=1
else
	echo "ok      FCB1 enables cam2fb"
fi
if ! test -f br2-external/package/cam2fb/cam2fb.c; then
	echo "FAIL: missing br2-external/package/cam2fb/cam2fb.c"
	fail=1
else
	echo "ok      cam2fb package present"
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

# DTB Makefile must list every board DTB.
for dtb in esp32s31_generic esp32s31_korvo1 \
	esp32s31_function_coreboard_1 esp32s31_function_coreboard_1_spi_sd; do
	if ! grep -q "$dtb" linux/arch/riscv/boot/dts/espressif/Makefile; then
		echo "FAIL: dts Makefile missing $dtb"
		fail=1
	fi
done
echo "ok      dts Makefile lists board DTBs"

exit "$fail"
