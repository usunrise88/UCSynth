#include "control.h"

#include <atomic>
#include <cmath>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(tag, ...) ((void)0)  // host-сборка теста: логов нет
#endif

namespace {

// Метаданные реестра — const, во flash, без RAM. Порядок строго по param_id_t.
struct ParamDef {
    const char  *name;
    param_type_t type;
    float        min;
    float        max;
    float        def;
};

constexpr ParamDef kParams[] = {
    /* [PARAM_MASTER_VOLUME] */ { "master_volume", PARAM_TYPE_FLOAT, 0.0f,  1.0f,     0.8f   },
    /* [PARAM_TEST_TONE_HZ]  */ { "test_tone_hz",  PARAM_TYPE_FLOAT, 20.0f, 20000.0f, 440.0f },
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == PARAM_COUNT,
              "таблица kParams разошлась с param_id_t");

// Текущие значения. atomic<float> lock-free на S3 (выровненный 4-байтный доступ):
// пишет Core 1, читает Core 0 — без мьютекса. relaxed: параметры независимы,
// межпараметрических инвариантов нет.
std::atomic<float> g_values[PARAM_COUNT];

[[maybe_unused]] const char *TAG = "control";

float clamp_and_quantize(const ParamDef &d, float v)
{
    if (v < d.min) v = d.min;
    if (v > d.max) v = d.max;
    if (d.type != PARAM_TYPE_FLOAT) v = std::roundf(v);  // INT/ENUM/BOOL дискретны
    return v;
}

}  // namespace

void control_init(void)
{
    for (uint16_t i = 0; i < PARAM_COUNT; ++i) {
        g_values[i].store(kParams[i].def, std::memory_order_relaxed);
    }
    ESP_LOGI(TAG, "реестр: %u параметр(ов)", (unsigned)PARAM_COUNT);
}

uint16_t param_count(void)
{
    return PARAM_COUNT;
}

float set_param(uint16_t id, float value)
{
    if (id >= PARAM_COUNT) return NAN;
    const float v = clamp_and_quantize(kParams[id], value);
    g_values[id].store(v, std::memory_order_relaxed);
    return v;
}

float get_param(uint16_t id)
{
    if (id >= PARAM_COUNT) return NAN;
    return g_values[id].load(std::memory_order_relaxed);
}

bool param_get_info(uint16_t id, param_info_t *out)
{
    if (id >= PARAM_COUNT || out == nullptr) return false;
    const ParamDef &d = kParams[id];
    out->name = d.name;
    out->type = d.type;
    out->min  = d.min;
    out->max  = d.max;
    out->def  = d.def;
    out->cur  = g_values[id].load(std::memory_order_relaxed);
    return true;
}
