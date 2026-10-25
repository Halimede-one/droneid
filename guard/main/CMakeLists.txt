idf_component_register(SRCS "guard_main.c" INCLUDE_DIRS "."
    PRIV_REQUIRES uart_guard esp_driver_uart esp_driver_gpio esp_driver_usb_serial_jtag esp_timer esp_hw_support esp_app_format)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
