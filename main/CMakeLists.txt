idf_component_register(
    SRCS
        "app_main.c"
        "audio_input.c"
        "modem/psk_modem.c"
        "modem/psk_modes.c"
    INCLUDE_DIRS "." "modem"
    REQUIRES esp_timer esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_codec_dev psk_rtty
)
