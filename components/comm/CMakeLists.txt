idf_component_register(
    SRCS "src/comm.cpp" "src/protocol.cpp" "src/frame.cpp"
    INCLUDE_DIRS "include"
    # comm.h тянет только stdint → всё приватное (см. комментарий в components/audio).
    # control — модель параметров; audio — нотный путь; preset — хранилище пресетов (NVS);
    # nvs_flash — коды ошибок NVS (ESP_ERR_NVS_NOT_FOUND) при трактовке ответов store; preset требует
    # nvs_flash приватно, поэтому путь к nvs.h сюда не протекает — нужен явный require.
    # esp_driver_usb_serial_jtag — транспорт; esp_timer — uptime для STAT; freertos — задачи RX/TX.
    PRIV_REQUIRES control audio preset nvs_flash esp_driver_usb_serial_jtag esp_timer freertos
)
