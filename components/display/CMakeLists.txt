idf_component_register(
    SRCS "src/display.cpp" "src/ssd1306.cpp" "src/gfx.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # control — параметры (читаем); audio — scope-буфер формы волны;
    # esp_driver_i2c — OLED по I2C; esp_driver_gpio — GPIO_NUM_*; esp_timer — тайминг splash/fps.
    REQUIRES control audio esp_driver_i2c esp_driver_gpio esp_timer
)
