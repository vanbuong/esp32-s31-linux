// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include "board.h"

#if BOARD_HAS_SPI_ILI9341

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"

#define LCD_FB_ADDR		BOARD_FB_ADDR
#define LCD_H_RES		BOARD_LCD_H_RES
#define LCD_V_RES		BOARD_LCD_V_RES
#define LCD_FB_SIZE		(LCD_H_RES * LCD_V_RES * 2U)
#define LCD_REFRESH_MS		100
/*
 * SPI DMA cannot pull the PSRAM framebuffer directly, and an internal bounce
 * of the whole 150 KiB frame will not fit next to Wi-Fi/Ethernet.  Stream a
 * few lines at a time through a small DMA-capable strip instead.
 */
#define LCD_STRIP_LINES		8U
#define LCD_STRIP_BYTES		(LCD_STRIP_LINES * LCD_H_RES * 2U)

static const char *TAG = "s31-linux-ili9341";
static esp_lcd_panel_handle_t panel;
static uint16_t *dma_strip;

static void fill_color_bars(void)
{
    static const uint16_t colors[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    volatile uint16_t *fb = (volatile uint16_t *)LCD_FB_ADDR;

    for (uint32_t y = 0; y < LCD_V_RES; y++) {
        for (uint32_t x = 0; x < LCD_H_RES; x++) {
            fb[y * LCD_H_RES + x] = colors[x * 8U / LCD_H_RES];
        }
    }
}

static void lcd_flush_framebuffer(void)
{
    const uint16_t *fb = (const uint16_t *)LCD_FB_ADDR;

    if (!panel || !dma_strip) {
        return;
    }

    for (uint32_t y = 0; y < LCD_V_RES; y += LCD_STRIP_LINES) {
        uint32_t lines = LCD_V_RES - y;

        if (lines > LCD_STRIP_LINES) {
            lines = LCD_STRIP_LINES;
        }
        memcpy(dma_strip, fb + y * LCD_H_RES, lines * LCD_H_RES * 2U);
        esp_lcd_panel_draw_bitmap(panel, 0, (int)y, LCD_H_RES,
                                  (int)(y + lines), dma_strip);
    }
}

/*
 * After the handoff the GMAC and Wi-Fi stacks own most of hart 0.  A low
 * priority task keeps scanning the PSRAM frame buffer out over SPI so the
 * Linux simple-framebuffer continues to light the panel without a native
 * SPI display driver.
 */
static void lcd_refresh_task(void *arg)
{
    (void)arg;

    for (;;) {
        lcd_flush_framebuffer();
        vTaskDelay(pdMS_TO_TICKS(LCD_REFRESH_MS));
    }
}

bool display_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = BOARD_LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_STRIP_BYTES + 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_SPI_CLOCK_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_err_t err;

    if (panel) {
        return true;
    }

    dma_strip = heap_caps_malloc(LCD_STRIP_BYTES,
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!dma_strip) {
        ESP_LOGE(TAG, "no DMA strip (%u bytes)", (unsigned)LCD_STRIP_BYTES);
        return false;
    }

    if (BOARD_LCD_PIN_BL >= 0) {
        gpio_config_t bl = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BL,
        };

        gpio_config(&bl);
        gpio_set_level(BOARD_LCD_PIN_BL, 1);
    }

    fill_color_bars();

    err = spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        goto fail_strip;
    }

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                   &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io failed: %s", esp_err_to_name(err));
        goto fail_strip;
    }

    err = esp_lcd_new_panel_ili9341(io, &panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ili9341 panel failed: %s", esp_err_to_name(err));
        goto fail_strip;
    }

    err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_mirror(panel, false, false);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel bring-up failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(panel);
        panel = NULL;
        goto fail_strip;
    }

    lcd_flush_framebuffer();

    if (xTaskCreate(lcd_refresh_task, "ili9341_fb", 4096, NULL, 1,
                    NULL) != pdPASS) {
        ESP_LOGW(TAG, "refresh task missing; panel shows the boot bars only");
    }

    ESP_LOGI(TAG, "ILI9341 framebuffer live: %ux%u RGB565 fb=0x%08" PRIx32
                  " strip=%u lines SPI SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d",
             (unsigned)LCD_H_RES, (unsigned)LCD_V_RES,
             (uint32_t)LCD_FB_ADDR, (unsigned)LCD_STRIP_LINES,
             BOARD_LCD_PIN_SCLK, BOARD_LCD_PIN_MOSI, BOARD_LCD_PIN_CS,
             BOARD_LCD_PIN_DC, BOARD_LCD_PIN_RST, BOARD_LCD_PIN_BL);
    return true;

fail_strip:
    heap_caps_free(dma_strip);
    dma_strip = NULL;
    return false;
}

#else /* RGB Korvo path lives in display_rgb.c via the shared display_init. */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_sys.h"
#include "hal/axi_dma_ll.h"
#include "hal/dma_types.h"
#include "hal/gdma_channel.h"
#include "hal/gdma_ll.h"
#include "hal/lcd_ll.h"
#include "heap_memory_layout.h"
#include "soc/axi_dma_struct.h"
#include "soc/lcd_cam_struct.h"
#include "display.h"

#define LCD_FB_ADDR            BOARD_FB_ADDR
#define LCD_H_RES              BOARD_LCD_H_RES
#define LCD_V_RES              BOARD_LCD_V_RES
#define LCD_FB_SIZE            (LCD_H_RES * LCD_V_RES * 2U)
#define LCD_PCLK_HZ            18000000U

#define LCD_DMA_LINK_ADDR      0x2F079000U
#define LCD_DMA_LINK_SIZE      0x1000U
#define LCD_DMA_CHUNK_SIZE     DMA_DESCRIPTOR_BUFFER_MAX_SIZE_64B_ALIGNED
#define LCD_DMA_NODE_COUNT     ((LCD_FB_SIZE + LCD_DMA_CHUNK_SIZE - 1U) / LCD_DMA_CHUNK_SIZE)
#define LCD_AXI_DMA_PERIPH_ID  SOC_GDMA_TRIG_PERIPH_LCD0
#define LCD_AXI_DMA_CHANNELS   GDMA_LL_AXI_PAIRS_PER_GROUP

_Static_assert(LCD_DMA_NODE_COUNT * sizeof(dma_descriptor_align8_t) <= LCD_DMA_LINK_SIZE,
               "LCD DMA link exceeds its reserved SRAM region");

SOC_RESERVE_MEMORY_REGION(LCD_DMA_LINK_ADDR, LCD_DMA_LINK_ADDR + LCD_DMA_LINK_SIZE, lcd_dma_link);

static const char *TAG = "s31-linux-display";
static esp_lcd_panel_handle_t panel;

static void fill_color_bars(void)
{
    static const uint16_t colors[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    volatile uint16_t *fb = (volatile uint16_t *)LCD_FB_ADDR;

    for (uint32_t y = 0; y < LCD_V_RES; y++) {
        for (uint32_t x = 0; x < LCD_H_RES; x++) {
            fb[y * LCD_H_RES + x] = colors[x * 8U / LCD_H_RES];
        }
    }
}

static void build_dma_link(void)
{
    dma_descriptor_align8_t *link = (dma_descriptor_align8_t *)LCD_DMA_LINK_ADDR;
    uint32_t offset = 0;

    memset(link, 0, LCD_DMA_NODE_COUNT * sizeof(*link));
    for (uint32_t i = 0; i < LCD_DMA_NODE_COUNT; i++) {
        uint32_t chunk = LCD_FB_SIZE - offset;

        if (chunk > LCD_DMA_CHUNK_SIZE) {
            chunk = LCD_DMA_CHUNK_SIZE;
        }
        link[i].dw0.size = chunk;
        link[i].dw0.length = chunk;
        link[i].dw0.suc_eof = (i == LCD_DMA_NODE_COUNT - 1U);
        link[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        link[i].buffer = (void *)(LCD_FB_ADDR + offset);
        link[i].next = &link[(i + 1U) % LCD_DMA_NODE_COUNT];
        offset += chunk;
    }
}

static int find_lcd_dma_channel(void)
{
    for (int channel = 0; channel < LCD_AXI_DMA_CHANNELS; channel++) {
        if (AXI_DMA.out[channel].conf.out_peri_sel.peri_out_sel_chn ==
            LCD_AXI_DMA_PERIPH_ID) {
            return channel;
        }
    }
    return -1;
}

static void silence_lcd_interrupts(int channel)
{
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
    }
    lcd_ll_clear_interrupt_status(&LCD_CAM, UINT32_MAX);
    axi_dma_ll_tx_enable_interrupt(&AXI_DMA, channel, UINT32_MAX, false);
    axi_dma_ll_tx_clear_interrupt_status(&AXI_DMA, channel, UINT32_MAX);
}

static void start_dma_link(int channel)
{
    lcd_ll_enable_auto_next_frame(&LCD_CAM, true);
    lcd_ll_reset(&LCD_CAM);
    lcd_ll_fifo_reset(&LCD_CAM);
    axi_dma_ll_tx_reset_channel(&AXI_DMA, channel);
    axi_dma_ll_tx_set_desc_addr(&AXI_DMA, channel, LCD_DMA_LINK_ADDR);
    axi_dma_ll_tx_start(&AXI_DMA, channel);
    esp_rom_delay_us(1);
    lcd_ll_start(&LCD_CAM);
}

bool display_init(void)
{
    int channel;
    esp_err_t err;

    if (panel) {
        return true;
    }

    fill_color_bars();

    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 40,
            .hsync_back_porch = 40,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 23,
            .vsync_back_porch = 32,
            .vsync_front_porch = 13,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .user_fbs = { (void *)LCD_FB_ADDR },
        .dma_burst_size = 64,
        .hsync_gpio_num = 44,
        .vsync_gpio_num = 45,
        .de_gpio_num = 43,
        .pclk_gpio_num = 40,
        .disp_gpio_num = 38,
        .data_gpio_nums = {
            8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 33, 34, 35, 36,
        },
        .flags = {
            .fb_in_psram = true,
            .refresh_on_demand = true,
        },
    };

    err = esp_lcd_new_rgb_panel(&config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        goto fail;
    }

    channel = find_lcd_dma_channel();
    if (channel < 0) {
        ESP_LOGE(TAG, "no AXI DMA TX channel bound to LCD_CAM");
        goto fail;
    }

    build_dma_link();
    silence_lcd_interrupts(channel);
    start_dma_link(channel);

    ESP_LOGI(TAG, "framebuffer live: %ux%u RGB565 fb=0x%08" PRIx32
                  " dma-link=0x%08" PRIx32 " channel=%d",
             (unsigned)LCD_H_RES, (unsigned)LCD_V_RES,
             (uint32_t)LCD_FB_ADDR, (uint32_t)LCD_DMA_LINK_ADDR, channel);
    return true;

fail:
    esp_lcd_panel_del(panel);
    panel = NULL;
    return false;
}

#endif
