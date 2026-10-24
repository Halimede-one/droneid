idf_component_register(
    SRCS "app_main.c" "radio.c" "ble_radio.c" "wifi_radio.c" "fc_uart.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_usb_serial_jtag esp_system esp_timer
                  esp_hw_support esp_event esp_wifi esp_netif bt nvs_flash rid esp_driver_uart fc_link
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
