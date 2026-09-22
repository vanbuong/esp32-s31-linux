/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Shared-memory ABI between Linux on hart 1 and the ESP-IDF Ethernet
 * firmware on hart 0 for the ESP32-S31 Function-CoreBoard-1 gigabit MAC.
 * Same rules as the Wi-Fi IPC: uncached SRAM, fixed slots, one producer and
 * one consumer per ring.
 */
#ifndef _ESP32S31_ETH_IPC_H
#define _ESP32S31_ETH_IPC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint8_t u8;
#endif

#define ESP32S31_ETH_IPC_MAGIC		0x45313353	/* "S31E" */
#define ESP32S31_ETH_IPC_VERSION	1

/*
 * Sits above the Wi-Fi IPC window and below the LCD DMA descriptor ring.
 * The ethernet@ node in the board DTS carries the same address.
 */
#define ESP32S31_ETH_IPC_SRAM_ADDR	0x2f060000
#define ESP32S31_ETH_IPC_SRAM_SIZE	0x00018000

#define ESP32S31_ETH_IPC_LINE		64
#define ESP32S31_ETH_IPC_SLOT_DATA	1536
#define ESP32S31_ETH_IPC_SLOTS		16

struct esp32s31_eth_ipc_slot {
	u32 len;
	u8 reserved[ESP32S31_ETH_IPC_LINE - 4];
	u8 data[ESP32S31_ETH_IPC_SLOT_DATA];
};

struct esp32s31_eth_ipc_ring {
	u32 head;
	u8 reserved_head[ESP32S31_ETH_IPC_LINE - 4];
	u32 tail;
	u8 reserved_tail[ESP32S31_ETH_IPC_LINE - 4];
	struct esp32s31_eth_ipc_slot slot[ESP32S31_ETH_IPC_SLOTS];
};

struct esp32s31_eth_ipc {
	u32 magic;
	u32 version;
	u32 link_up;
	u8 mac[6];
	u8 reserved[ESP32S31_ETH_IPC_LINE - 18];
	struct esp32s31_eth_ipc_ring to_linux;
	struct esp32s31_eth_ipc_ring to_firmware;
};

#endif /* _ESP32S31_ETH_IPC_H */
