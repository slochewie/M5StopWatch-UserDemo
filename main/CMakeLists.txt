file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
)

set(MY_INCS
    "."
)

idf_component_register(
    SRCS
        "main.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS
        ${MY_INCS}
    REQUIRES
        lvgl
        mqtt
        esp_wifi
        esp_netif
        esp_http_server
        nvs_flash
        esp_timer
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
    EMBED_TXTFILES
        "hal/utils/config_ap/assets/badge_config_ap.html"
)
