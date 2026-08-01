// Host-тест чистого кодека пресетов (preset_serialize/apply/read_path) против реального реестра control.
// NVS не мокаем (нужен таргет) — тут только (де)сериализация и её гарды. Запуск: tools/run-host-tests.sh
#include "preset.h"
#include "control.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_f32(uint8_t *p, float f) { std::memcpy(p, &f, 4); }
static bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main()
{
    control_init();

    // --- round-trip: снимок → изменить реестр → применить → значения восстановились ---
    {
        set_param(PARAM_MASTER_VOLUME, 0.30f);
        set_param(PARAM_REVERB_SIZE,   0.90f);
        uint8_t blob[PRESET_BLOB_MAX];
        const size_t len = preset_serialize(blob, sizeof(blob), "Leads/Saw");
        check(len > 0, "serialize → непустой блоб");
        check(blob[0] == PRESET_FMT_VERSION, "версия в заголовке");

        set_param(PARAM_MASTER_VOLUME, 0.60f);   // «уехали»
        set_param(PARAM_REVERB_SIZE,   0.20f);
        check(preset_apply(blob, len), "apply валидного блоба");
        check(approx(get_param(PARAM_MASTER_VOLUME), 0.30f), "master_volume восстановлен");
        check(approx(get_param(PARAM_REVERB_SIZE),   0.90f), "reverb_size восстановлен");
    }

    // --- путь: read_path извлекает путь без изменения реестра ---
    {
        uint8_t blob[PRESET_BLOB_MAX];
        const size_t len = preset_serialize(blob, sizeof(blob), "Bank A/Bass 1");
        char path[PRESET_PATH_MAX + 1] = {0};
        check(preset_read_path(blob, len, path, sizeof(path)), "read_path ok");
        check(std::strcmp(path, "Bank A/Bass 1") == 0, "путь совпал");

        // путь > PRESET_PATH_MAX → serialize отказывает
        char longp[PRESET_PATH_MAX + 8];
        std::memset(longp, 'a', sizeof(longp)); longp[sizeof(longp) - 1] = '\0';
        check(preset_serialize(blob, sizeof(blob), longp) == 0, "слишком длинный путь → serialize=0");
    }

    // --- apply сбрасывает в дефолт параметры, которых НЕТ в блобе (forward-compat) ---
    {
        // Минимальный блоб: только master_volume=0.25 (без остальных пар).
        param_info_t cut{}; param_get_info(PARAM_CUTOFF, &cut);
        set_param(PARAM_CUTOFF, cut.def == cut.max ? cut.min : cut.max);   // заведомо НЕ дефолт
        check(!approx(get_param(PARAM_CUTOFF), cut.def), "cutoff уведён от дефолта");

        uint8_t b[16]; size_t o = 0;
        b[o++] = PRESET_FMT_VERSION;
        b[o++] = 0;                       // path_len = 0
        put_u16(b + o, 1); o += 2;        // count = 1
        put_u16(b + o, PARAM_MASTER_VOLUME); o += 2;
        put_f32(b + o, 0.25f); o += 4;
        check(preset_apply(b, o), "apply минимального блоба");
        check(approx(get_param(PARAM_MASTER_VOLUME), 0.25f), "master_volume из блоба");
        check(approx(get_param(PARAM_CUTOFF), cut.def), "cutoff (нет в блобе) сброшен в дефолт");
    }

    // --- транзиентный test_tone: не пишется в снимок и не сбрасывается загрузкой ---
    {
        set_param(PARAM_TEST_TONE, 1.0f);          // включили отладочный тон
        uint8_t blob[PRESET_BLOB_MAX];
        const size_t len = preset_serialize(blob, sizeof(blob), "");
        set_param(PARAM_TEST_TONE, 0.0f);          // выключили
        check(preset_apply(blob, len), "apply (снят при test_tone=1)");
        check(approx(get_param(PARAM_TEST_TONE), 0.0f), "test_tone НЕ дёрнут загрузкой (транзиентный)");
    }

    // --- гарды битых/усечённых блобов ---
    {
        uint8_t blob[PRESET_BLOB_MAX];
        const size_t len = preset_serialize(blob, sizeof(blob), "x");
        check(!preset_apply(blob, 1), "apply len<2 → false");
        uint8_t z[8] = {0}; z[0] = 0;                       // version 0
        check(!preset_apply(z, sizeof(z)), "apply version=0 → false");
        check(!preset_apply(blob, len - 1), "apply усечённого блоба → false");
        check(!preset_read_path(blob, 1, (char *)z, sizeof(z)), "read_path len<2 → false");

        // неизвестный id в блобе → apply не крашит, id игнорируется set_param
        uint8_t b[16]; size_t o = 0;
        b[o++] = PRESET_FMT_VERSION; b[o++] = 0;
        put_u16(b + o, 1); o += 2;
        put_u16(b + o, 9999); o += 2;                       // id вне реестра
        put_f32(b + o, 0.5f); o += 4;
        check(preset_apply(b, o), "apply с неизвестным id → true (id тихо скипнут)");
    }

    // --- нехватка cap → serialize=0 (не переполняет буфер) ---
    {
        uint8_t tiny[4];
        check(preset_serialize(tiny, sizeof(tiny), "Name") == 0, "малый буфер → serialize=0");
    }

    if (g_fail == 0) { std::printf("OK: preset — все проверки пройдены\n"); return 0; }
    std::printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
