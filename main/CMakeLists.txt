idf_component_register(
    SRCS "dolos_main.c" "display.c" "usb_hid.c" "payload.c"
    INCLUDE_DIRS "."
    REQUIRES ducky ui esp_tinyusb fatfs sdmmc esp_driver_sdspi
             esp_lcd driver esp_driver_gpio esp_driver_spi nvs_flash esp_timer
)
