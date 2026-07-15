idf_component_register(
    SRCS "src/comm.cpp" "src/protocol.cpp" "src/frame.cpp"
    INCLUDE_DIRS "include"
    # control — модель параметров; usb_serial_jtag — драйвер ввода; esp_timer — uptime для STAT.
    REQUIRES control esp_driver_usb_serial_jtag esp_timer
)
