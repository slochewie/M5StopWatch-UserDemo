idf_component_register(
    SRCS "configure_mqtt_test.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        mqtt
        esp_wifi
        esp_netif
        esp_event
)
