idf_component_register(
    SRCS
        "app_main.c"
        "audio_decim.c"
        "audio_input.c"
        "decoder_layer.c"
        "demod_primitives.c"
        "display.c"
        "modem.c"
        "psk_modes.c"
        "spectrum.c"
        "transport.c"
        "ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer esp_driver_gpio esp_driver_i2s esp_driver_usb_serial_jtag esp32_p4_function_ev_board esp_codec_dev lvgl psk_rtty ft8lib
)
