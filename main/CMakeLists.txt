idf_component_register(
    SRCS "dolos_main.c" "display.c" "usb_hid.c" "payload.c"
         "net_wifi.c" "console_server.c"
    INCLUDE_DIRS "."
    REQUIRES ducky ui dconsole esp_tinyusb fatfs sdmmc esp_driver_sdspi
             esp_lcd driver esp_driver_gpio esp_driver_spi nvs_flash esp_timer
             esp_wifi esp_netif esp_event esp_http_server mbedtls esp_hw_support
    EMBED_TXTFILES "console.html"
)
