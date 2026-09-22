// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#pragma once

/*
 * Board selection is driven by the top-level Makefile's BOARD= value, which
 * CMake turns into one of the BOARD_* macros below.  Default to Korvo-1 so a
 * bare idf.py build keeps the original pin map.
 */
#if defined(BOARD_FUNCTION_COREBOARD_1)
#define BOARD_NAME			"ESP32-S31-Function-CoreBoard-1"
#define BOARD_HAS_SD_POWER_EN		0
#define BOARD_HAS_RGB_LCD		0
#define BOARD_HAS_SPI_ILI9341		1
#define BOARD_HAS_GMAC_ETH		1

/*
 * External microSD on the dedicated SDMMC slot-0 pads (J2 D0..CMD =
 * GPIO20..25).  There is no board-level FET, so the socket is assumed to be
 * powered from the header's 3V3 rail.
 */
#define BOARD_SD_POWER_EN_GPIO		(-1)

/*
 * SPI ILI9341 wiring on free J2 pins (Ethernet owns GPIO5-19, SDMMC owns
 * GPIO20-25, USB Serial/JTAG owns GPIO33/34).  Rewire by editing these
 * and rebuilding the loader.
 */
#define BOARD_LCD_SPI_HOST		SPI2_HOST
#define BOARD_LCD_PIN_SCLK		36
#define BOARD_LCD_PIN_MOSI		37
#define BOARD_LCD_PIN_MISO		(-1)
#define BOARD_LCD_PIN_CS		39
#define BOARD_LCD_PIN_DC		40
#define BOARD_LCD_PIN_RST		42
#define BOARD_LCD_PIN_BL		43
#define BOARD_LCD_H_RES			320
#define BOARD_LCD_V_RES			240
#define BOARD_LCD_SPI_CLOCK_HZ		40000000

/* On-board Motorcomm YT8531 RGMII PHY (IDF default EMAC pin map). */
#define BOARD_ETH_MDC_GPIO		5
#define BOARD_ETH_MDIO_GPIO		6
#define BOARD_ETH_PHY_RST_GPIO		7
#define BOARD_ETH_PHY_ADDR		0

#elif defined(BOARD_KORVO_1) || !defined(BOARD_FUNCTION_COREBOARD_1)
#define BOARD_NAME			"ESP32-S31 Korvo-1"
#define BOARD_HAS_SD_POWER_EN		1
#define BOARD_HAS_RGB_LCD		1
#define BOARD_HAS_SPI_ILI9341		0
#define BOARD_HAS_GMAC_ETH		0
#define BOARD_SD_POWER_EN_GPIO		39
#define BOARD_LCD_H_RES			800
#define BOARD_LCD_V_RES			480

#else
#error "Unknown BOARD; set BOARD=korvo-1 or BOARD=function-coreboard-1"
#endif

/* Shared simple-framebuffer carve-out at the top of PSRAM. */
#define BOARD_FB_ADDR			0x50F40000U
