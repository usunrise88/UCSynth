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
//
// id хранится явно и проверяется на этапе компиляции (params_in_order ниже). Раньше соответствие
// строка↔id держалось на комментарии-маркере, а машинно проверялось только КОЛИЧЕСТВО строк. Этого
// мало: вставь id в середину param_id_t, а строку допиши в конец — assert проходит, сборка чистая,
// и каждый id после точки вставки начинает отдавать чужие name/type/min/max/def. Так как GUI и
// патчи строятся по LIST, пульт нарисовал бы, скажем, кноб −1..1 привязанным к cutoff, а set_param
// клампил бы входящие значения cutoff в [−1, 1] — DSP уезжает далеко за диапазон без диагностики.
// (Designated array initializers — фича C99, в C++ их нет, поэтому явное поле, а не [PARAM_X] = ….)
struct ParamDef {
    param_id_t   id;
    const char  *name;
    param_type_t type;
    float        min;
    float        max;
    float        def;
};

constexpr ParamDef kParams[] = {
    { PARAM_MASTER_VOLUME,  "master_volume",  PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.8f },
    { PARAM_TEST_TONE_HZ,   "test_tone_hz",   PARAM_TYPE_FLOAT,  20.0f, 20000.0f,   440.0f },
    { PARAM_WAVEFORM,       "waveform",       PARAM_TYPE_ENUM,    0.0f,     3.0f,     0.0f },
    { PARAM_TEST_TONE,      "test_tone",      PARAM_TYPE_BOOL,    0.0f,     1.0f,     1.0f },
    // этап 3.1 — ADSR (VCA) + drone
    { PARAM_AMP_ATTACK,     "amp_attack",     PARAM_TYPE_FLOAT, 0.001f,     5.0f,   0.005f },
    { PARAM_AMP_DECAY,      "amp_decay",      PARAM_TYPE_FLOAT, 0.001f,     5.0f,     0.1f },
    { PARAM_AMP_SUSTAIN,    "amp_sustain",    PARAM_TYPE_FLOAT,   0.0f,     1.0f,     1.0f },
    { PARAM_AMP_RELEASE,    "amp_release",    PARAM_TYPE_FLOAT, 0.001f,     5.0f,    0.02f },
    { PARAM_LATCH,          "latch",          PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    { PARAM_AMP_LOOP,       "amp_loop",       PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    // этап 3.3 — три осц + микшер
    { PARAM_OSC1_LEVEL,     "osc1_level",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     1.0f },
    { PARAM_OSC1_DETUNE,    "osc1_detune",    PARAM_TYPE_FLOAT, -24.0f,    24.0f,     0.0f },
    { PARAM_OSC2_WAVE,      "osc2_wave",      PARAM_TYPE_ENUM,    0.0f,     3.0f,     0.0f },
    { PARAM_OSC2_LEVEL,     "osc2_level",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_OSC2_DETUNE,    "osc2_detune",    PARAM_TYPE_FLOAT, -24.0f,    24.0f,     0.0f },
    { PARAM_OSC3_WAVE,      "osc3_wave",      PARAM_TYPE_ENUM,    0.0f,     3.0f,     0.0f },
    { PARAM_OSC3_LEVEL,     "osc3_level",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_OSC3_DETUNE,    "osc3_detune",    PARAM_TYPE_FLOAT, -24.0f,    24.0f,     0.0f },
    { PARAM_NOISE_LEVEL,    "noise_level",    PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_RING_LEVEL,     "ring_level",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    // этап 3.2 — фильтр (ZDF SVF) + ADSR (VCF)
    { PARAM_CUTOFF,         "cutoff",         PARAM_TYPE_FLOAT,  20.0f, 20000.0f, 20000.0f },
    { PARAM_RESONANCE,      "resonance",      PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_FILTER_MODE,    "filter_mode",    PARAM_TYPE_ENUM,    0.0f,     3.0f,     0.0f },
    { PARAM_FLT_ATTACK,     "flt_attack",     PARAM_TYPE_FLOAT, 0.001f,     5.0f,   0.005f },
    { PARAM_FLT_DECAY,      "flt_decay",      PARAM_TYPE_FLOAT, 0.001f,     5.0f,     0.1f },
    { PARAM_FLT_SUSTAIN,    "flt_sustain",    PARAM_TYPE_FLOAT,   0.0f,     1.0f,     1.0f },
    { PARAM_FLT_RELEASE,    "flt_release",    PARAM_TYPE_FLOAT, 0.001f,     5.0f,    0.02f },
    { PARAM_FLT_ENV_AMT,    "flt_env_amt",    PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_FLT_LOOP,       "flt_loop",       PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    // этап 3.4 — lo-fi
    { PARAM_LOFI,           "lofi",           PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    { PARAM_LOFI_BITS,      "lofi_bits",      PARAM_TYPE_INT,     1.0f,    16.0f,    16.0f },
    // этап 3.5/3.6 — полифония + glide
    { PARAM_POLY_VOICES,    "poly_voices",    PARAM_TYPE_INT,     1.0f,     8.0f,     1.0f },
    { PARAM_GLIDE_TIME,     "glide_time",     PARAM_TYPE_FLOAT,   0.0f,     2.0f,     0.0f },
    { PARAM_LEGATO,         "legato",         PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    // этап 4.1 — LFO×2 + мод-матрица (src 0..7 = ModSource, dst 0..5 = ModDest, depth -1..1)
    { PARAM_LFO1_SHAPE,     "lfo1_shape",     PARAM_TYPE_ENUM,    0.0f,     4.0f,     0.0f },
    { PARAM_LFO1_RATE,      "lfo1_rate",      PARAM_TYPE_FLOAT,  0.05f,    30.0f,     2.0f },
    { PARAM_LFO2_SHAPE,     "lfo2_shape",     PARAM_TYPE_ENUM,    0.0f,     4.0f,     0.0f },
    { PARAM_LFO2_RATE,      "lfo2_rate",      PARAM_TYPE_FLOAT,  0.05f,    30.0f,     4.0f },
    { PARAM_MOD_WHEEL,      "mod_wheel",      PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_MTX1_SRC,       "mtx1_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX1_DST,       "mtx1_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX1_DEPTH,     "mtx1_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX2_SRC,       "mtx2_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX2_DST,       "mtx2_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX2_DEPTH,     "mtx2_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX3_SRC,       "mtx3_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX3_DST,       "mtx3_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX3_DEPTH,     "mtx3_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX4_SRC,       "mtx4_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX4_DST,       "mtx4_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX4_DEPTH,     "mtx4_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX5_SRC,       "mtx5_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX5_DST,       "mtx5_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX5_DEPTH,     "mtx5_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX6_SRC,       "mtx6_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX6_DST,       "mtx6_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX6_DEPTH,     "mtx6_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX7_SRC,       "mtx7_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX7_DST,       "mtx7_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX7_DEPTH,     "mtx7_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    { PARAM_MTX8_SRC,       "mtx8_src",       PARAM_TYPE_ENUM,    0.0f,     7.0f,     0.0f },
    { PARAM_MTX8_DST,       "mtx8_dst",       PARAM_TYPE_ENUM,    0.0f,     5.0f,     0.0f },
    { PARAM_MTX8_DEPTH,     "mtx8_depth",     PARAM_TYPE_FLOAT,  -1.0f,     1.0f,     0.0f },
    // этап 4.2 — wave-огибающая (8 точек 0..1, дефолт = рампа 0→1; rate сек; loop)
    { PARAM_WAVEENV_P1,     "waveenv_p1",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_WAVEENV_P2,     "waveenv_p2",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,   0.143f },
    { PARAM_WAVEENV_P3,     "waveenv_p3",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,   0.286f },
    { PARAM_WAVEENV_P4,     "waveenv_p4",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,   0.429f },
    { PARAM_WAVEENV_P5,     "waveenv_p5",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,   0.571f },
    { PARAM_WAVEENV_P6,     "waveenv_p6",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,   0.714f },
    { PARAM_WAVEENV_P7,     "waveenv_p7",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,   0.857f },
    { PARAM_WAVEENV_P8,     "waveenv_p8",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     1.0f },
    { PARAM_WAVEENV_RATE,   "waveenv_rate",   PARAM_TYPE_FLOAT,  0.05f,    20.0f,     1.0f },
    { PARAM_WAVEENV_LOOP,   "waveenv_loop",   PARAM_TYPE_BOOL,    0.0f,     1.0f,     1.0f },
    // этап 5.1 — overdrive
    { PARAM_OD_ON,          "od_on",          PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    { PARAM_OD_DRIVE,       "od_drive",       PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.5f },
    { PARAM_OD_MIX,         "od_mix",         PARAM_TYPE_FLOAT,   0.0f,     1.0f,     1.0f },
    // этап 5.2 — delay (стерео, PSRAM)
    { PARAM_DELAY_ON,       "delay_on",       PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    { PARAM_DELAY_TIME,     "delay_time",     PARAM_TYPE_FLOAT,   1.0f,  1000.0f,   300.0f },
    { PARAM_DELAY_FEEDBACK, "delay_feedback", PARAM_TYPE_FLOAT,   0.0f,    0.95f,    0.35f },
    { PARAM_DELAY_DAMP,     "delay_damp",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.3f },
    { PARAM_DELAY_MIX,      "delay_mix",      PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.3f },
    // этап 5.3 — reverb (Freeverb, PSRAM)
    { PARAM_REVERB_ON,      "reverb_on",      PARAM_TYPE_BOOL,    0.0f,     1.0f,     0.0f },
    { PARAM_REVERB_SIZE,    "reverb_size",    PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.5f },
    { PARAM_REVERB_DAMP,    "reverb_damp",    PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.5f },
    { PARAM_REVERB_WIDTH,   "reverb_width",   PARAM_TYPE_FLOAT,   0.0f,     1.0f,     1.0f },
    { PARAM_REVERB_MIX,     "reverb_mix",     PARAM_TYPE_FLOAT,   0.0f,     1.0f,    0.25f },
    // этап 6 — модуляция длины гребёнок реверба (дефолт depth=0 → сегодняшний звук)
    { PARAM_REVERB_MODDEPTH,"reverb_moddepth",PARAM_TYPE_FLOAT,   0.0f,     1.0f,     0.0f },
    { PARAM_REVERB_MODRATE, "reverb_modrate", PARAM_TYPE_FLOAT,   0.05f,    5.0f,     0.7f },
    // этап 7 — секвенсор / арпеджиатор (общий темп-клок)
    { PARAM_SEQ_BPM,     "seq_bpm",     PARAM_TYPE_INT,    20.0f,  300.0f,  120.0f },
    { PARAM_SEQ_SWING,   "seq_swing",   PARAM_TYPE_FLOAT,   0.0f,    1.0f,    0.0f },
    { PARAM_SEQ_PLAYING, "seq_playing", PARAM_TYPE_BOOL,    0.0f,    1.0f,    0.0f },
    { PARAM_SEQ_ON,      "seq_on",      PARAM_TYPE_BOOL,    0.0f,    1.0f,    0.0f },
    { PARAM_ARP_ON,      "arp_on",      PARAM_TYPE_BOOL,    0.0f,    1.0f,    0.0f },
    { PARAM_ARP_MODE,    "arp_mode",    PARAM_TYPE_ENUM,    0.0f,    3.0f,    0.0f },
    { PARAM_ARP_OCTAVES, "arp_octaves", PARAM_TYPE_INT,     1.0f,    4.0f,    1.0f },
    { PARAM_ARP_RATE,    "arp_rate",    PARAM_TYPE_ENUM,    0.0f,    3.0f,    2.0f },
    { PARAM_ARP_HOLD,    "arp_hold",    PARAM_TYPE_BOOL,    0.0f,    1.0f,    0.0f },
    // этап 12 — типы осцилляторов
    { PARAM_OSC1_TYPE,   "osc1_type",   PARAM_TYPE_ENUM,    0.0f,    2.0f,    0.0f },
    { PARAM_OSC2_TYPE,   "osc2_type",   PARAM_TYPE_ENUM,    0.0f,    2.0f,    0.0f },
    { PARAM_OSC3_TYPE,   "osc3_type",   PARAM_TYPE_ENUM,    0.0f,    2.0f,    0.0f },
    { PARAM_PD_AMOUNT,   "pd_amount",   PARAM_TYPE_FLOAT,   0.0f,    1.0f,    0.0f },
    { PARAM_VOICE_ENGINE,"voice_engine",PARAM_TYPE_ENUM,    0.0f,    2.0f,    0.0f },
    { PARAM_FM_RATIO,    "fm_ratio",    PARAM_TYPE_FLOAT,   0.5f,    8.0f,    1.0f },
    { PARAM_FM_INDEX,    "fm_index",    PARAM_TYPE_FLOAT,   0.0f,   10.0f,    0.0f },
    { PARAM_KS_DAMP,     "ks_damp",     PARAM_TYPE_FLOAT,   0.0f,    1.0f,    0.5f },
    { PARAM_KS_DECAY,    "ks_decay",    PARAM_TYPE_FLOAT,   0.8f,    0.999f,  0.99f },
    { PARAM_KS_PLUCK,    "ks_pluck",    PARAM_TYPE_FLOAT,   0.0f,    1.0f,    0.5f },
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == PARAM_COUNT,
              "таблица kParams разошлась с param_id_t");

// Индекс строки == её id. Ловит вставку id в середину enum при дописывании строки в конец.
constexpr bool params_in_order()
{
    for (size_t i = 0; i < sizeof(kParams) / sizeof(kParams[0]); ++i) {
        if (kParams[i].id != (param_id_t)i) return false;
    }
    return true;
}
static_assert(params_in_order(), "порядок строк kParams разошёлся с param_id_t");

// audio.cpp читает мод-матрицу как PARAM_MTX1_SRC + slot*3 + {0,1,2}, а точки wave-огибающей — как
// PARAM_WAVEENV_P1 + i. Эта непрерывность нигде не проверялась, хотя от неё зависит чтение
// параметров: сдвинь один id — и слот 5 начнёт читать глубину слота 4.
static_assert(PARAM_MTX1_DST == PARAM_MTX1_SRC + 1 && PARAM_MTX1_DEPTH == PARAM_MTX1_SRC + 2,
              "слот матрицы должен быть (src, dst, depth) подряд");
static_assert(PARAM_MTX8_SRC == PARAM_MTX1_SRC + 7 * 3,
              "слоты матрицы должны идти шагом 3 без разрывов");
static_assert(PARAM_WAVEENV_P8 == PARAM_WAVEENV_P1 + 7,
              "waveenv_p1..p8 должны идти подряд");

// Текущие значения. atomic<float> lock-free на S3 (выровненный 4-байтный доступ):
// пишет Core 1, читает Core 0 — без мьютекса. relaxed: параметры независимы,
// межпараметрических инвариантов нет.
std::atomic<float> g_values[PARAM_COUNT];

[[maybe_unused]] const char *TAG = "control";

float clamp_and_quantize(const ParamDef &d, float v)
{
    // Сравнения инвертированы намеренно: для NaN и `v < min`, и `v > max` ложны, поэтому прямой
    // кламп пропускает NaN как есть. А NaN, попавший в g_values, отравляет аудио-тракт и
    // рециркулирует в кольцах delay/reverb через умножение на feedback — выход остаётся мусором
    // даже после записи корректного значения, до перезагрузки. Проверять значение обязаны мы:
    // set_param — единственные ворота, и в них приходит произвольное 32-битное слово с провода.
    if (!(v >= d.min)) v = d.min;   // ложь для NaN и для v < min
    if (!(v <= d.max)) v = d.max;   // ложь для +Inf и для v > max
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
