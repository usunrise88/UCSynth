idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    # Слои архитектуры + компоненты IDF для проверки железа на бут-логе.
    REQUIRES audio control io comm display esp_psram spi_flash
)
