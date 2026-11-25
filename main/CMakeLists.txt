idf_component_register(SRCS "main.c" "serial.c" INCLUDE_DIRS "." REQUIRES bt nvs_flash esp_driver_uart esp_app_format)
