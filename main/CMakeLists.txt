idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    # Слои архитектуры + компоненты IDF для проверки железа на бут-логе.
    # preset + nvs_flash — инициализация хранилища пресетов (этап 6).
    REQUIRES audio control io comm display preset nvs_flash esp_psram spi_flash
)
