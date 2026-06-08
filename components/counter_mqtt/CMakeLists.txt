idf_component_register(
    SRCS "counter_mqtt.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        mqtt
        esp_event
        esp_netif
        nvs_flash
        esp_wifi
        device_config
        main
)
