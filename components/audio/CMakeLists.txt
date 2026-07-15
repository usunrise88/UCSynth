idf_component_register(
    SRCS "src/audio.cpp" "src/wavetable.cpp"
    INCLUDE_DIRS "include"
    # control — модель параметров; esp_driver_i2s — I2S std драйвер (PCM5102);
    # esp_driver_gpio — типы/константы GPIO_NUM_* для распиновки I2S.
    REQUIRES control esp_driver_i2s esp_driver_gpio
)
