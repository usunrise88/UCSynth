idf_component_register(
    SRCS "src/preset_codec.cpp" "src/preset_store.cpp" "src/seq_store.cpp"
    INCLUDE_DIRS "include"
    # control — кодек читает/пишет реестр параметров; nvs_flash — хранилище пресетов.
    # (preset.h — кодек — публично тянет только stdint; control/nvs приватны.)
    PRIV_REQUIRES control nvs_flash
)
