// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include "board.h"

#if BOARD_HAS_GMAC_ETH

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_intr_alloc.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/hp_system_reg.h"
#include "soc/interrupts.h"
#include "esp32s31-eth-ipc.h"
#include "loader.h"

SOC_RESERVE_MEMORY_REGION(ESP32S31_ETH_IPC_SRAM_ADDR,
                          ESP32S31_ETH_IPC_SRAM_ADDR + ESP32S31_ETH_IPC_SRAM_SIZE,
                          eth_ipc);

_Static_assert(sizeof(struct esp32s31_eth_ipc) <= ESP32S31_ETH_IPC_SRAM_SIZE,
               "Ethernet IPC block exceeds its reserved SRAM window");

#define IPC_DOORBELL_TO_LINUX_REG    HP_SYSTEM_CPU_INT_FROM_CPU_2_REG
#define IPC_DOORBELL_TO_FIRMWARE     ETS_CPU_INTR_FROM_CPU_3_SOURCE
#define IPC_DOORBELL_TO_FIRMWARE_REG HP_SYSTEM_CPU_INT_FROM_CPU_3_REG

static const char *TAG = "s31-linux-eth";

static struct esp32s31_eth_ipc *const ipc =
    (struct esp32s31_eth_ipc *)ESP32S31_ETH_IPC_SRAM_ADDR;

static esp_eth_handle_t eth_handle;
static TaskHandle_t ipc_tx_task_handle;

static bool ipc_ring_pop(struct esp32s31_eth_ipc_ring *ring, void *buf,
                         uint32_t *len)
{
    uint32_t tail = ring->tail;
    struct esp32s31_eth_ipc_slot *slot;

    if (tail == ring->head) {
        return false;
    }

    slot = &ring->slot[tail % ESP32S31_ETH_IPC_SLOTS];
    *len = slot->len;
    if (*len > ESP32S31_ETH_IPC_SLOT_DATA) {
        *len = ESP32S31_ETH_IPC_SLOT_DATA;
    }
    memcpy(buf, slot->data, *len);
    __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

static bool ipc_ring_push(struct esp32s31_eth_ipc_ring *ring, const void *buf,
                          uint32_t len)
{
    uint32_t head = ring->head;
    struct esp32s31_eth_ipc_slot *slot;

    if (len > ESP32S31_ETH_IPC_SLOT_DATA ||
        head - __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) >=
        ESP32S31_ETH_IPC_SLOTS) {
        return false;
    }

    slot = &ring->slot[head % ESP32S31_ETH_IPC_SLOTS];
    memcpy(slot->data, buf, len);
    slot->len = len;
    __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

static esp_err_t ipc_eth_rx(esp_eth_handle_t hdl, uint8_t *buffer, uint32_t len,
                            void *priv)
{
    bool queued;

    (void)hdl;
    (void)priv;

    queued = ipc_ring_push(&ipc->to_linux, buffer, len);
    free(buffer);
    if (queued) {
        REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
    }
    return ESP_OK;
}

static void ipc_from_linux_isr(void *arg)
{
    BaseType_t higher_priority_woken = pdFALSE;

    (void)arg;
    REG_WRITE(IPC_DOORBELL_TO_FIRMWARE_REG, 0);
    vTaskNotifyGiveFromISR(ipc_tx_task_handle, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

static void ipc_tx_task(void *arg)
{
    static uint8_t frame[ESP32S31_ETH_IPC_SLOT_DATA];
    uint32_t len;
    bool drained;

    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        drained = false;
        while (ipc_ring_pop(&ipc->to_firmware, frame, &len)) {
            if (esp_eth_transmit(eth_handle, frame, len) != ESP_OK) {
                ESP_LOGW(TAG, "transmit failed (%" PRIu32 " bytes)", len);
            }
            drained = true;
        }
        if (drained) {
            REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
        }
    }
}

static void ipc_set_link(uint32_t up)
{
    __atomic_store_n(&ipc->link_up, up, __ATOMIC_RELEASE);
    REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
}

static void ipc_eth_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg;
    (void)data;

    if (base != ETH_EVENT) {
        return;
    }

    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        ipc_set_link(1);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
        ipc_set_link(0);
        break;
    default:
        break;
    }
}

static esp_err_t start_ipc(void)
{
    esp_err_t err;

    memset(ipc, 0, sizeof(*ipc));
    err = esp_read_mac(ipc->mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        err = esp_read_mac(ipc->mac, ESP_MAC_WIFI_STA);
    }
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(ipc_tx_task, "eth_ipc_tx", 4096, NULL, 5,
                    &ipc_tx_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_intr_alloc(IPC_DOORBELL_TO_FIRMWARE, 0, ipc_from_linux_isr,
                         NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     ipc_eth_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_eth_update_input_path(eth_handle, ipc_eth_rx, NULL);
    if (err != ESP_OK) {
        return err;
    }

    ipc->version = ESP32S31_ETH_IPC_VERSION;
    __atomic_store_n(&ipc->magic, ESP32S31_ETH_IPC_MAGIC, __ATOMIC_RELEASE);

    ESP_LOGI(TAG, "eth ipc at 0x%08" PRIx32 ", mac %02x:%02x:%02x:%02x:%02x:%02x",
             (uint32_t)ESP32S31_ETH_IPC_SRAM_ADDR, ipc->mac[0], ipc->mac[1],
             ipc->mac[2], ipc->mac[3], ipc->mac[4], ipc->mac[5]);
    return ESP_OK;
}

void start_eth(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    esp_eth_config_t config;
    esp_err_t err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return;
    }

    esp32_emac.smi_gpio.mdc_num = BOARD_ETH_MDC_GPIO;
    esp32_emac.smi_gpio.mdio_num = BOARD_ETH_MDIO_GPIO;

    phy_config.phy_addr = BOARD_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = BOARD_ETH_PHY_RST_GPIO;

    mac = esp_eth_mac_new_esp32(&esp32_emac, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "failed to create EMAC");
        return;
    }

    /*
     * The Function-CoreBoard-1 ships a Motorcomm YT8531.  IDF's generic
     * IEEE 802.3 PHY driver covers the basic clause-22 bring-up used here.
     */
    phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "failed to create PHY");
        mac->del(mac);
        return;
    }

    config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
        phy->del(phy);
        mac->del(mac);
        return;
    }

    err = start_ipc();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ipc setup failed: %s", esp_err_to_name(err));
        esp_eth_driver_uninstall(eth_handle);
        eth_handle = NULL;
        return;
    }

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "RGMII Ethernet started (MDC=%d MDIO=%d RST=%d addr=%d)",
             BOARD_ETH_MDC_GPIO, BOARD_ETH_MDIO_GPIO,
             BOARD_ETH_PHY_RST_GPIO, BOARD_ETH_PHY_ADDR);
}

#else /* !BOARD_HAS_GMAC_ETH */

void start_eth(void)
{
}

#endif
