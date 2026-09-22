idf_component_register(SRCS main.c display.c sd.c wifi_ipc.c eth_ipc.c
                       REQUIRES freertos spi_flash esp_partition esp_psram esp_hw_support esp_rom
                                hal esp_mm esp_hal_wdt esp_lcd esp_hal_lcd esp_hal_dma esp_hal_sd
                                esp_driver_gpio esp_driver_spi heap esp_wifi esp_event esp_eth
                       INCLUDE_DIRS "." "../../shared")

if(DEFINED ENV{BOARD} AND "$ENV{BOARD}" STREQUAL "function-coreboard-1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_FUNCTION_COREBOARD_1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_KORVO_1)
endif()
