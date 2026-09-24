idf_component_register(SRCS "main.c" "i2c_bus.c" "tca9555.c" "es8311.c" "es7210.c" "audio.c" "console.c"
                            "settings.c" "wifi.c" "web.c" "ota.c" "netconsole.c" "buttons.c" "leds.c" "status.c" "voice.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram spi_flash app_update esp_system esp_driver_i2c
                                esp_driver_i2s esp_driver_gpio esp_driver_rmt console nvs_flash esp_wifi
                                esp_netif esp_event esp_http_server esp_timer lwip)
