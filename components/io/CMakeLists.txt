idf_component_register(
    SRCS "src/io.cpp"
    INCLUDE_DIRS "include"
    # control — контролы транслируются в параметры (энкодеры/матрица, этап 8.2+).
    REQUIRES control
    # esp_driver_i2c — общая I2C-шина + сканер (8.1); esp_driver_gpio — GPIO_NUM_* и INT (8.2+).
    # Приватно: io.h тонкий (только io_init), типы драйвера наружу не торчат.
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio
)
