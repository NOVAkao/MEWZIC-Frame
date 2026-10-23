idf_component_register(
    SRCS
        "main.cpp"
        "rtc.cpp"
        "display.cpp"
        "bluetooth.cpp"
        "a2dp_sink.cpp"
        "encoder.cpp"
        "button_handler.cpp"
        "JPEGDEC.cpp"
       

    INCLUDE_DIRS
        "."

    REQUIRES
        btstack
        driver
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_rom
        nvs_flash
)