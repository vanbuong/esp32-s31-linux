SHELL := /bin/bash

BUILD_DIR := build
LOG_DIR := logs
HOST_OS := $(shell uname -s)

IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_TOOLS_PATH ?= $(HOME)/.espressif
PYTHON ?= $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/python_env/*/bin/python)))
ifeq ($(PYTHON),)
PYTHON := $(shell command -v python3 2>/dev/null)
endif
ESPTOOL := $(PYTHON) -m esptool
# export.sh names its virtualenv after whatever python3 it finds, so a host
# Python upgrade points it at one that was never installed.  Pin what exists.
IDF_PYTHON_ENV := $(patsubst %/bin/python,%,$(PYTHON))
ESP_RISCV_BIN := $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin)))
CROSS_COMPILE ?= $(ESP_RISCV_BIN)/riscv32-esp-elf-

GMAKE ?= $(shell command -v gmake 2>/dev/null || command -v make)
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

OPENSBI_DIR := external/opensbi
OPENSBI_SRC := $(BUILD_DIR)/opensbi-src
OPENSBI_OUT := $(CURDIR)/$(BUILD_DIR)/opensbi
OPENSBI_PATCHES := $(sort $(wildcard opensbi/patches/*.patch))

# Every file, not the directories: editing a source in place leaves the
# directory mtime alone, and the patch would not be regenerated.
LINUX_SOURCES := $(shell find linux -type f -not -path 'linux/patches/*') \
	shared/esp32s31-wifi-ipc.h shared/esp32s31-eth-ipc.h
LINUX_GENERATED_PATCH := linux/patches/0000-esp32s31-add-source-files.patch

# Board selection.  korvo-1 is the historical default; function-coreboard-1
# enables on-board RGMII Gigabit Ethernet, an external SDIO microSD on the
# dedicated SDMMC pads, and an SPI ILI9341 panel on the J2 header pins
# documented in bootloader/main/board.h.
BOARD ?= korvo-1
ifeq ($(BOARD),function-coreboard-1)
BR_DEFCONFIG := esp32s31_fcb1_defconfig
BR_BOARD_DIR := br2-external/board/esp32s31-fcb1
BOOTLOADER_SDKCONFIG_DEFAULTS := sdkconfig.defaults;sdkconfig.defaults.function-coreboard-1
else ifeq ($(BOARD),korvo-1)
BR_DEFCONFIG := esp32s31_defconfig
BR_BOARD_DIR := br2-external/board/esp32s31
BOOTLOADER_SDKCONFIG_DEFAULTS := sdkconfig.defaults;sdkconfig.defaults.korvo-1
else
$(error Unknown BOARD=$(BOARD); use korvo-1 or function-coreboard-1)
endif

# Buildroot cannot build on macOS, so it runs in a container.  Its output/ and
# dl/ stay in a volume; several gigabytes have no business on virtiofs.  On
# Linux CI the same Containerfile is built with Docker.
BR_DIR := external/buildroot
BR_IMAGE ?= esp32s31-buildroot:bookworm
BR_VOLUME ?= esp32s31-br
BR_VOLUME_SIZE ?= 60G
# The release Buildroot builds, which is also what the series is checked on.
LINUX_VERSION := $(shell sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="\(.*\)"/\1/p' \
	br2-external/configs/$(BR_DEFCONFIG))
BR_MEMORY ?= 8G
BR_OUT := $(BUILD_DIR)/buildroot
BR_MAKE := make O=/br/output BR2_EXTERNAL=/work/br2-external BR2_DL_DIR=/br/dl

ifeq ($(HOST_OS),Darwin)
CONTAINER ?= $(shell command -v container 2>/dev/null)
BR_RUN = "$(CONTAINER)" run --rm --cpus $(JOBS) --memory $(BR_MEMORY) \
	--uid $(shell id -u) --gid $(shell id -g) -e HOME=/br/home \
	-v $(BR_VOLUME):/br -v "$(CURDIR)":/work "$(BR_IMAGE)"
BR_TOOLS = "$(CONTAINER)" run --rm --uid $(shell id -u) --gid $(shell id -g) \
	-v "$(CURDIR)":/work "$(BR_IMAGE)"
else
CONTAINER ?= $(shell command -v docker 2>/dev/null)
BR_RUN = "$(CONTAINER)" run --rm \
	--user $(shell id -u):$(shell id -g) -e HOME=/br/home -e BR2_JLEVEL=$(JOBS) \
	-v $(BR_VOLUME):/br -v "$(CURDIR)":/work -w /work "$(BR_IMAGE)"
BR_TOOLS = "$(CONTAINER)" run --rm \
	--user $(shell id -u):$(shell id -g) \
	-v "$(CURDIR)":/work -w /work "$(BR_IMAGE)"
endif

INITRAMFS_INIT := $(BR_BOARD_DIR)/init
SD_DISK ?=
SD_RAW = $(subst /dev/disk,/dev/rdisk,$(SD_DISK))
# FAT area; the rest of the card becomes the root partition.  Shrink this
# for a smaller card -- diskutil fails loudly if it does not fit.
SD_DATA_SIZE ?= 28G

FLASH_PORT ?=
SERIAL_PORT ?=
RESET_PORT ?=
BAUD ?= 115200
BOOT_TIMEOUT ?= 300
OPENOCD_CFG ?= openocd/esp32s31-linux.cfg

BOOTLOADER_OFFSET := 0x2000
PARTITION_TABLE_OFFSET := 0x8000
APP_OFFSET := 0x20000
OPENSBI_OFFSET := 0x220000
LINUX_OFFSET := 0x2a0000
LINUX_SIZE_OFFSET := 0xa1fff4
INITRAMFS_OFFSET := 0xa20000

.DEFAULT_GOAL := help

.PHONY: help check ports build bootloader opensbi kernel kernel-patches \
	kernel-check kernel-clean kernel-vmlinux kernel-menuconfig \
	kernel-saveconfig ci-board-check ci-kernel ci-rootfs \
	container-image \
	br-volume br-artifacts rootfs rootfs-menuconfig initramfs sdcard sdpart \
	sdwrite sdroot flash monitor openocd clean

help:
	@printf '%s\n' \
		'ESP32-S31 Linux (macOS host)' \
		'' \
		'  BOARD=$(BOARD)   (korvo-1 | function-coreboard-1)' \
		'' \
		'  make check                         verify host tools and submodules' \
		'  make ci-board-check                validate both board configs (CI)' \
		'  make ci-kernel                     CI: Linux Image only' \
		'  make ci-rootfs                     CI: Image, rootfs.ext2, sdcard.img, initramfs' \
		'  make ports                         list connected serial devices' \
		'  make build                         build loader, OpenSBI, Linux, rootfs' \
		'  make flash FLASH_PORT=/dev/cu.X    build and flash the complete image' \
		'  make monitor SERIAL_PORT=/dev/cu.X open the Linux UART terminal' \
		'  make openocd                       start JTAG via GPIO33/34 USB breakout' \
		'  make clean                         remove generated build output' \
		'' \
		'Component targets, each buildable on its own:' \
		'' \
		'  make bootloader                    ESP-IDF loader application' \
		'  make opensbi                       patched OpenSBI fw_jump' \
		'  make rootfs                        Buildroot: kernel, userspace, images' \
		'  make initramfs                     the flash slot'\''s early userspace' \
		'' \
		'Kernel, which Buildroot builds from a stock release:' \
		'' \
		'  make kernel                        rebuild just the kernel' \
		'  make kernel-patches                regenerate linux/patches from linux/' \
		'  make kernel-check                  dry-run the series on a pristine tree' \
		'  make kernel-clean                  re-extract the kernel after a patch change' \
		'  make kernel-vmlinux                fetch vmlinux from the build volume for GDB' \
		'  make kernel-menuconfig             configure the kernel' \
		'  make kernel-saveconfig             write the configuration back' \
		'' \
		'SD card, once a card is in the Mac (see: diskutil list external):' \
		'' \
		'  make sdcard                        card image for a fresh card' \
		'  make sdpart  SD_DISK=/dev/diskN   lay out a large card (ERASES it)' \
		'  make sdwrite SD_DISK=/dev/diskN   write the whole card image (ERASES it)' \
		'  make sdroot  SD_DISK=/dev/diskN   rewrite only the root partition' \
		'' \
		'Buildroot host container (it cannot build on macOS):' \
		'' \
		'  make container-image               build the Debian build image' \
		'  make rootfs-menuconfig             explore the Buildroot configuration' \
		'' \
		'FLASH_PORT is the esptool target: the native USB Serial/JTAG' \
		'(/dev/cu.usbmodem*) or the CP2102N UART bridge (/dev/cu.usbserial-*).' \
		'SERIAL_PORT is the external UART carrying the Linux console.' \
		'RESET_PORT optionally names a second port whose RTS pulse resets the' \
		'board before monitoring when SERIAL_PORT cannot (BAUD, BOOT_TIMEOUT' \
		'tune the monitor).' \
		'' \
		'The rootfs lives on the SD card; the flash slot carries only the' \
		'initramfs that switch_roots into it.'

check:
ifneq ($(HOST_OS),Darwin)
	@test -n "$${CI:-}" || { echo 'interactive builds are documented for macOS; set CI=1 to skip this check'; exit 1; }
endif
	@test -n "$(PYTHON)" -a -x "$(PYTHON)" || { echo 'missing Python (ESP-IDF env or python3)'; exit 1; }
	@test -n "$(ESP_RISCV_BIN)" -a -x "$(CROSS_COMPILE)gcc" || { echo 'missing Espressif RISC-V toolchain'; exit 1; }
	@test -n "$(GMAKE)" -a -x "$(GMAKE)" || { echo 'missing GNU make'; exit 1; }
	@test -f "$(IDF_PATH)/export.sh" || { echo 'missing ESP-IDF at $(IDF_PATH)'; exit 1; }
	@test -n "$(CONTAINER)" -a -x "$(CONTAINER)" || { \
		if test "$(HOST_OS)" = Darwin; then \
			echo 'missing the container CLI (Buildroot cannot build on macOS)'; \
		else \
			echo 'missing docker (Buildroot host container)'; \
		fi; exit 1; }
ifeq ($(HOST_OS),Darwin)
	@"$(CONTAINER)" system status >/dev/null 2>&1 || { echo 'container services are not running: container system start'; exit 1; }
	@"$(PYTHON)" -c 'import serial' || { echo 'missing pyserial in ESP-IDF Python environment'; exit 1; }
endif
	@git submodule status --recursive

ci-board-check:
	@bash scripts/ci-board-check.sh

ports:
	@"$(PYTHON)" -m serial.tools.list_ports -v

build: bootloader opensbi rootfs initramfs

# idf.py reads sdkconfig.defaults only when creating sdkconfig, then rewrites
# sdkconfig every build, so edits to the defaults would never take effect.
# The defaults win here, including over "idf.py menuconfig".
bootloader:
	@mkdir -p "$(BUILD_DIR)"
	@if test -f "$(BUILD_DIR)/bootloader.board" && \
		test "$$(cat "$(BUILD_DIR)/bootloader.board")" != "$(BOARD)"; then \
		echo "BOARD changed to $(BOARD); regenerating bootloader/sdkconfig"; \
		rm -f bootloader/sdkconfig; \
	fi
	@echo "$(BOARD)" > "$(BUILD_DIR)/bootloader.board"
	@if test -f bootloader/sdkconfig && \
		test bootloader/sdkconfig.defaults -nt bootloader/sdkconfig; then \
		echo 'sdkconfig.defaults changed; regenerating bootloader/sdkconfig'; \
		rm -f bootloader/sdkconfig; \
	fi
	@export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV)"; \
	export BOARD="$(BOARD)"; \
	if ! source "$(IDF_PATH)/export.sh" >"$(BUILD_DIR)/idf-export.log" 2>&1; then \
		cat "$(BUILD_DIR)/idf-export.log"; \
		echo 'ESP-IDF environment setup failed (run $(IDF_PATH)/install.sh)'; \
		exit 1; \
	fi; \
	cd bootloader && idf.py --preview -B ../$(BUILD_DIR)/bootloader \
		-D SDKCONFIG_DEFAULTS="$(BOOTLOADER_SDKCONFIG_DEFAULTS)" build

opensbi:
	@rm -rf "$(OPENSBI_SRC)"
	@mkdir -p "$(OPENSBI_SRC)"
	@cp -R "$(OPENSBI_DIR)"/* "$(OPENSBI_SRC)/"
	@rm -rf "$(OPENSBI_OUT)"
	@# A for loop exits with the status of its last iteration, so a reject
	@# anywhere but the end would build a silently unpatched OpenSBI.
	@for patch_file in $(OPENSBI_PATCHES); do \
		patch -d "$(OPENSBI_SRC)" -p1 -F0 -s < "$$patch_file" || \
			{ echo "$$patch_file does not apply"; exit 1; }; \
	done
	@$(GMAKE) -C "$(OPENSBI_SRC)" \
		PLATFORM_DIR="$(CURDIR)/opensbi/platform" PLATFORM=esp32s31 \
		O="$(OPENSBI_OUT)" CROSS_COMPILE="$(CROSS_COMPILE)"
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.elf" "$(BUILD_DIR)/opensbi.elf"
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.bin" "$(BUILD_DIR)/opensbi.bin"

# Every file under linux/ is new to the kernel, so it ships as a generated
# patch instead of a copy over the tree.  The rest modify existing files.
$(LINUX_GENERATED_PATCH): $(LINUX_SOURCES) scripts/mkkernelpatches.py
	@"$(PYTHON)" scripts/mkkernelpatches.py

kernel-patches: $(LINUX_GENERATED_PATCH)

# Against a pristine extract of the release Buildroot builds, in the
# container at zero fuzz, because that is what Buildroot does: the patch macOS
# ships accepts stale context that GNU patch later rejects.  Also the test a
# kernel version bump has to pass.
kernel-check: kernel-patches br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		if ! test -f /br/dl/linux/linux-$(LINUX_VERSION).tar.xz; then \
			$(BR_MAKE) $(BR_DEFCONFIG); \
			$(BR_MAKE) linux-source; \
		fi; \
		rm -rf /br/check && mkdir -p /br/check; \
		tar -xf /br/dl/linux/linux-$(LINUX_VERSION).tar.xz \
			--strip-components=1 -C /br/check; \
		cd /br/check; fail=0; \
		for p in /work/linux/patches/*.patch; do \
			printf "%-50s " "$$(basename $$p)"; \
			if patch -p1 -F0 --dry-run -s -f < "$$p" >/dev/null 2>&1; \
				then echo applies; else echo FAILS; fail=1; fi; \
		done; rm -rf /br/check; exit $$fail'

kernel: kernel-patches br-volume
	@$(BR_RUN) sh -c 'set -e; cd /work/$(BR_DIR); $(BR_MAKE) linux-rebuild all'
	@$(MAKE) --no-print-directory br-artifacts

# Buildroot records what it applied, so a changed series needs the extracted
# trees thrown away first.
kernel-clean: br-volume
	@$(BR_RUN) sh -c 'cd /work/$(BR_DIR); $(BR_MAKE) linux-dirclean linux-headers-dirclean'

# A hundred megabytes of DWARF, so GDB gets it on demand and no build pays
# for it.
kernel-vmlinux: br-volume
	@$(BR_RUN) sh -c 'cp /br/output/build/linux-*/vmlinux /work/$(BUILD_DIR)/'
	@ls -l "$(BUILD_DIR)/vmlinux"

kernel-menuconfig: br-volume
	@$(BR_RUN) -i -t sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) linux-menuconfig'

kernel-saveconfig: br-volume
	@$(BR_RUN) sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) linux-update-defconfig'
	@git diff --stat -- $(BR_BOARD_DIR)/linux.config

container-image:
	@test -n "$(CONTAINER)" || { echo 'missing container/docker CLI'; exit 1; }
ifeq ($(HOST_OS),Darwin)
	@"$(CONTAINER)" build -t "$(BR_IMAGE)" container
else
	@"$(CONTAINER)" build -f container/Containerfile -t "$(BR_IMAGE)" container
endif

# Buildroot must not run as root, so the volume is handed to the caller once.
# Always (re)create /br/{output,dl,home} and chown: CI may have created an
# empty root-owned volume before this target runs, which leaves mkdir -p
# /br/output failing and Buildroot reporting output directory "".
br-volume:
	@test -n "$(CONTAINER)" || { echo 'missing container/docker CLI'; exit 1; }
ifeq ($(HOST_OS),Darwin)
	@"$(CONTAINER)" volume inspect "$(BR_VOLUME)" >/dev/null 2>&1 || { \
		echo "creating the $(BR_VOLUME) volume ($(BR_VOLUME_SIZE))"; \
		"$(CONTAINER)" volume create -s $(BR_VOLUME_SIZE) "$(BR_VOLUME)"; }
	@"$(CONTAINER)" run --rm --uid 0 --gid 0 -v $(BR_VOLUME):/br "$(BR_IMAGE)" \
		sh -c 'mkdir -p /br/output /br/dl /br/home && \
			chown -R $(shell id -u):$(shell id -g) /br'
else
	@"$(CONTAINER)" volume inspect "$(BR_VOLUME)" >/dev/null 2>&1 || { \
		echo "creating the $(BR_VOLUME) docker volume"; \
		"$(CONTAINER)" volume create "$(BR_VOLUME)"; }
	@"$(CONTAINER)" run --rm --user 0:0 -v $(BR_VOLUME):/br "$(BR_IMAGE)" \
		sh -c 'mkdir -p /br/output /br/dl /br/home && \
			chown -R $(shell id -u):$(shell id -g) /br'
endif

# CI builds only the kernel Image for the selected BOARD (builtin DTB included).
ci-kernel: kernel-patches br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		$(BR_MAKE) $(BR_DEFCONFIG); \
		$(BR_MAKE) linux; \
		test -f /br/output/images/Image; \
		mkdir -p /work/$(BUILD_DIR); \
		cp /br/output/images/Image /work/$(BUILD_DIR)/'
	@"$(PYTHON)" -c 'import pathlib, struct, zlib; \
p = pathlib.Path("$(BUILD_DIR)/Image"); \
data = p.read_bytes(); \
pathlib.Path("$(BUILD_DIR)/linux.size").write_bytes( \
    struct.pack("<III", 0x455A4953, len(data), zlib.crc32(data))); \
print("linux.size: %d bytes" % len(data))'
	@ls -l "$(BUILD_DIR)/Image" "$(BUILD_DIR)/linux.size"

# Full SD-card filesystem for CI: rootfs.ext2, provisioning sdcard.img, the
# flashed Image (+ linux.size manifest), and the initramfs that switch_roots.
ci-rootfs: kernel-patches br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		$(BR_MAKE) $(BR_DEFCONFIG); \
		$(BR_MAKE); \
		test -f /br/output/images/Image; \
		test -f /br/output/images/rootfs.ext2; \
		test -f /br/output/images/sdcard.img; \
		test -f /br/output/images/linux.size; \
		mkdir -p /work/$(BR_OUT) /work/$(BUILD_DIR); \
		cp /br/output/images/rootfs.ext2 /br/output/images/sdcard.img \
			/work/$(BR_OUT)/; \
		cp /br/output/images/Image /br/output/images/linux.size \
			/work/$(BUILD_DIR)/'
	@$(BR_RUN) sh -c 'cd /work && python3 scripts/mkinitramfs.py \
		--target /br/output/target --init "$(INITRAMFS_INIT)" \
		--output "$(BUILD_DIR)/initramfs.cpio" --size 0x200000'
	@ls -l "$(BUILD_DIR)/Image" "$(BUILD_DIR)/linux.size" \
		"$(BUILD_DIR)/initramfs.cpio" \
		"$(BR_OUT)/rootfs.ext2" "$(BR_OUT)/sdcard.img"

# The checked-in defconfig is the source of truth and is reapplied every build.
rootfs: kernel-patches br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		$(BR_MAKE) $(BR_DEFCONFIG); \
		$(BR_MAKE)'
	@$(MAKE) --no-print-directory br-artifacts

# The kernel and its manifest are flashed, so they come out of the volume
# next to the loader and OpenSBI binaries.
br-artifacts:
	@$(BR_RUN) sh -c 'set -e; \
		mkdir -p /work/$(BR_OUT) /work/$(BUILD_DIR); \
		cp /br/output/images/rootfs.ext2 /work/$(BR_OUT)/; \
		cp /br/output/images/Image /br/output/images/linux.size /work/$(BUILD_DIR)/; \
		if test -f /br/output/images/sdcard.img; then \
			cp /br/output/images/sdcard.img /work/$(BR_OUT)/; fi'

rootfs-menuconfig: br-volume
	@$(BR_RUN) -i -t sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) $(BR_DEFCONFIG) && $(BR_MAKE) menuconfig'

# Runs in the container because the target tree lives in the volume.
initramfs: rootfs
	@$(BR_RUN) sh -c 'cd /work && python3 scripts/mkinitramfs.py \
		--target /br/output/target --init "$(INITRAMFS_INIT)" \
		--output "$(BUILD_DIR)/initramfs.cpio" --size 0x200000'

sdcard: rootfs
	@test -f "$(BR_OUT)/sdcard.img" || { \
		echo 'Buildroot produced no sdcard.img; check post-image.sh'; exit 1; }
	@ls -l "$(BR_OUT)/sdcard.img"

# Lay out a large card: most of it FAT for the Mac, a small ext4 root.  Use
# this instead of sdwrite when the card is bigger than the image.
sdpart:
	@test -n "$(SD_DISK)" || { echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
	@diskutil info "$(SD_DISK)" | grep -E 'Device Node|Media Name|Disk Size|Removable Media'
	@echo
	@echo "This ERASES $(SD_DISK) and everything on it."
	@read -r -p 'Type ERASE to continue: ' reply; test "$$reply" = ERASE || { echo aborted; exit 1; }
	@diskutil unmountDisk "$(SD_DISK)"
	@diskutil partitionDisk "$(SD_DISK)" MBR \
		"MS-DOS FAT32" KORVO_SD $(SD_DATA_SIZE) "MS-DOS FAT32" ROOTFS R
	@diskutil unmountDisk "$(SD_DISK)"
# Linux creates the node whatever the type byte says, but 0x83 stops macOS
# trying to mount an ext4 partition as FAT on every insert.
	@sudo sh -c 'dd if="$(SD_RAW)" of="$(BUILD_DIR)/mbr.bin" bs=512 count=1 && \
		printf "\\x83" | dd of="$(BUILD_DIR)/mbr.bin" bs=1 seek=466 conv=notrunc && \
		dd if="$(BUILD_DIR)/mbr.bin" of="$(SD_RAW)" bs=512 count=1' \
		|| echo 'warning: p2 type byte unchanged; the board boots either way'
	@rm -f "$(BUILD_DIR)/mbr.bin"
	@diskutil list "$(SD_DISK)"
	@echo 'now: make sdroot SD_DISK=$(SD_DISK)'

# Erases the card, and drops everything past the last partition on a big one.
sdwrite:
	@test -n "$(SD_DISK)" || { echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
	@test -f "$(BR_OUT)/sdcard.img" || { echo 'no $(BR_OUT)/sdcard.img; run make sdcard'; exit 1; }
	@diskutil info "$(SD_DISK)" | grep -E 'Device Node|Media Name|Disk Size|Removable Media'
	@echo
	@echo "This ERASES $(SD_DISK) and everything on it."
	@read -r -p 'Type ERASE to continue: ' reply; test "$$reply" = ERASE || { echo aborted; exit 1; }
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo dd if="$(BR_OUT)/sdcard.img" of="$(SD_RAW)" bs=4m
	@sync
	@diskutil eject "$(SD_DISK)"

# The day-to-day loop: leaves the data partition and the card's size alone.
sdroot:
	@test -n "$(SD_DISK)" || { echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
	@test -f "$(BR_OUT)/rootfs.ext2" || { echo 'no $(BR_OUT)/rootfs.ext2; run make rootfs'; exit 1; }
	@test -e "$(SD_DISK)s2" || { \
		echo '$(SD_DISK)s2 does not exist; run make sdpart first'; exit 1; }
	@echo "This overwrites the root partition $(SD_DISK)s2."
	@read -r -p 'Type WRITE to continue: ' reply; test "$$reply" = WRITE || { echo aborted; exit 1; }
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo dd if="$(BR_OUT)/rootfs.ext2" of="$(SD_RAW)s2" bs=4m
	@sync
	@diskutil eject "$(SD_DISK)"

flash: build
	@test -n "$(FLASH_PORT)" || { echo 'set FLASH_PORT=/dev/cu.<flash-port>'; exit 1; }
	@$(ESPTOOL) --chip esp32s31 -p "$(FLASH_PORT)" -b 921600 \
		--before default-reset --after hard-reset write-flash \
		$(BOOTLOADER_OFFSET) "$(BUILD_DIR)/bootloader/bootloader/bootloader.bin" \
		$(PARTITION_TABLE_OFFSET) "$(BUILD_DIR)/bootloader/partition_table/partition-table.bin" \
		$(APP_OFFSET) "$(BUILD_DIR)/bootloader/s31-linux-loader.bin" \
		$(OPENSBI_OFFSET) "$(BUILD_DIR)/opensbi.bin" \
		$(LINUX_OFFSET) "$(BUILD_DIR)/Image" \
		$(LINUX_SIZE_OFFSET) "$(BUILD_DIR)/linux.size" \
		$(INITRAMFS_OFFSET) "$(BUILD_DIR)/initramfs.cpio"

monitor:
	@test -n "$(SERIAL_PORT)" || { echo 'set SERIAL_PORT=/dev/cu.<external-uart>'; exit 1; }
	@"$(PYTHON)" scripts/reset-monitor.py --port "$(SERIAL_PORT)" --baud "$(BAUD)" \
		$(if $(RESET_PORT),--reset-port "$(RESET_PORT)",) --log-dir "$(LOG_DIR)" \
		--timeout "$(BOOT_TIMEOUT)" --success-pattern 'ESP32-S31 Linux / Buildroot' --interactive

openocd:
	@mkdir -p "$(LOG_DIR)"
	@openocd_bin="$$(command -v openocd 2>/dev/null || true)"; \
	if test -z "$$openocd_bin"; then \
		openocd_bin="$$(find "$(IDF_TOOLS_PATH)/tools/openocd-esp32" \
			\( -path '*/bin/openocd' -o -path '*/bin/openocd.exe' \) \
			-type f 2>/dev/null | sort | tail -n 1)"; \
	fi; \
	test -n "$$openocd_bin" || { echo 'missing OpenOCD'; exit 1; }; \
	exec "$$openocd_bin" -c 'set ESP_RTOS none' -f "$(OPENOCD_CFG)" \
		-l "$(LOG_DIR)/$$(date +%Y%m%d-%H%M%S)-openocd.log"

# Buildroot's output is in the volume: "container volume rm $(BR_VOLUME)".
clean:
	@rm -rf "$(BUILD_DIR)"
