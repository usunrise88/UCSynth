idf_component_register(
    SRCS "src/audio.cpp" "src/wavetable.cpp" "src/env.cpp" "src/filter.cpp" "src/voice.cpp" "src/synth.cpp" "src/lfo.cpp" "src/waveenv.cpp" "src/fx.cpp" "src/seq_engine.cpp" "src/osc_types.cpp"
    INCLUDE_DIRS "include"
    # Всё приватное: audio.h тянет только stdint. Публичные REQUIRES протекали транзитивно —
    # display и comm (оба REQUIRES audio) наследовали include-пути esp_driver_i2s/gpio, а main
    # получал вообще всё. Файл в display мог сделать #include "driver/i2s_std.h" и собраться —
    # ровно то межслойное сцепление, ради предотвращения которого и разделены audio/io/control.
    # control — модель параметров; esp_driver_i2s — I2S std (PCM5102); esp_driver_gpio — GPIO_NUM_*;
    # esp_timer — метрики (время блока); freertos — задача/очередь нот.
    PRIV_REQUIRES control esp_driver_i2s esp_driver_gpio esp_timer freertos
)
