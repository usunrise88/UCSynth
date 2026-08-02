// Host-тест логики протокола (без железа): реестр control + кадрирование frame + диспетчер
// protocol. Компилится обычным g++, потому что ядро не тянет ESP-IDF (control.cpp на не-ESP
// глушит ESP_LOG, frame/protocol вообще без ESP-заголовков). Запуск: tools/run-host-tests.sh
#include "control.h"
#include "protocol.h"
#include "frame.h"
#include "preset.h"
#include "seq_codec.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <map>
#include <string>
#include <utility>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_fail; } } while (0)

struct Sink { std::vector<std::vector<uint8_t>> frames; };
static void sink_emit(void *ctx, const uint8_t *body, size_t len) {
    static_cast<Sink *>(ctx)->frames.emplace_back(body, body + len);
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float f32(const uint8_t *p) { uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }

// Прогнать тело запроса (список int → байты, без narrowing) через диспетчер.
static Sink run(std::initializer_list<int> bytes, const sys_stats_t *st = nullptr,
                const preset_backend_t *pb = nullptr, const seq_backend_t *sb = nullptr) {
    std::vector<uint8_t> body;
    for (int b : bytes) body.push_back((uint8_t)b);
    Sink s;
    comm_handle_request(body.data(), body.size(), st, pb, sb, sink_emit, &s);
    return s;
}

// Вариант с телом-вектором (когда байты собираются в рантайме, напр. seq-блоб шага).
static Sink run_v(const std::vector<uint8_t> &body, const seq_backend_t *sb) {
    Sink s;
    comm_handle_request(body.data(), body.size(), nullptr, nullptr, sb, sink_emit, &s);
    return s;
}

// In-memory seq backend: настоящий кодек паттерна + std::map «NVS».
struct FakeSeq {
    SeqStep pattern[SEQ_STEPS];
    std::map<uint16_t, std::pair<std::string, std::vector<uint8_t>>> store;
    uint16_t next = 0;
    FakeSeq() { for (int i = 0; i < SEQ_STEPS; ++i) pattern[i] = SeqStep{}; }
};
static int fseq_set_step(void *ctx, uint8_t step, const uint8_t *blob, size_t len) {
    auto *f = static_cast<FakeSeq *>(ctx);
    if (step >= SEQ_STEPS) return ERR_BAD_LEN;
    SeqStep st;
    if (seq_step_deserialize(blob, len, &st) == 0) return ERR_BAD_LEN;
    f->pattern[step] = st;
    return 0;
}
static size_t fseq_get_step(void *ctx, uint8_t step, uint8_t *out, size_t cap) {
    auto *f = static_cast<FakeSeq *>(ctx);
    if (step >= SEQ_STEPS) return 0;
    return seq_step_serialize(out, cap, &f->pattern[step]);
}
static int fseq_save(void *ctx, uint16_t slot, const char *path, uint16_t *out) {
    auto *f = static_cast<FakeSeq *>(ctx);
    const uint16_t use = (slot == 0xFFFF) ? f->next++ : slot;
    uint8_t buf[SEQ_BLOB_MAX];
    const size_t n = seq_serialize(buf, sizeof(buf), f->pattern);
    if (!n) return ERR_STORAGE;
    f->store[use] = { std::string(path), std::vector<uint8_t>(buf, buf + n) };
    if (out) *out = use;
    return 0;
}
static int fseq_load(void *ctx, uint16_t slot) {
    auto *f = static_cast<FakeSeq *>(ctx);
    auto it = f->store.find(slot);
    if (it == f->store.end()) return ERR_NO_PRESET;
    return seq_deserialize(it->second.second.data(), it->second.second.size(), f->pattern) ? 0 : ERR_STORAGE;
}
static int fseq_del(void *ctx, uint16_t slot) {
    return static_cast<FakeSeq *>(ctx)->store.erase(slot) ? 0 : ERR_NO_PRESET;
}
static int fseq_rename(void *ctx, uint16_t slot, const char *path) {
    auto *f = static_cast<FakeSeq *>(ctx);
    auto it = f->store.find(slot);
    if (it == f->store.end()) return ERR_NO_PRESET;
    it->second.first = path;
    return 0;
}
static int fseq_list(void *ctx, preset_list_emit_fn emit, void *ectx) {
    auto *f = static_cast<FakeSeq *>(ctx);
    int n = 0;
    for (auto &kv : f->store) { emit(ectx, kv.first, kv.second.first.c_str()); ++n; }
    return n;
}

// In-memory preset backend: настоящий кодек (preset_serialize/apply/read_path), «NVS» = map slot→blob.
// Тестирует фрейминг preset-опкодов в диспетчере, не таща NVS в host-сборку.
struct FakeStore { std::map<uint16_t, std::vector<uint8_t>> blobs; uint16_t next = 0; };
static int fake_save(void *ctx, uint16_t slot, const char *path, uint16_t *out) {
    auto *s = static_cast<FakeStore *>(ctx);
    const uint16_t use = (slot == PRESET_SLOT_NEW) ? s->next++ : slot;
    uint8_t blob[PRESET_BLOB_MAX];
    const size_t len = preset_serialize(blob, sizeof(blob), path);
    if (!len) return ERR_STORAGE;
    s->blobs[use].assign(blob, blob + len);
    if (out) *out = use;
    return 0;
}
static int fake_load(void *ctx, uint16_t slot) {
    auto *s = static_cast<FakeStore *>(ctx);
    auto it = s->blobs.find(slot);
    if (it == s->blobs.end()) return ERR_NO_PRESET;
    return preset_apply(it->second.data(), it->second.size()) ? 0 : ERR_STORAGE;
}
static int fake_del(void *ctx, uint16_t slot) {
    return static_cast<FakeStore *>(ctx)->blobs.erase(slot) ? 0 : ERR_NO_PRESET;
}
static int fake_rename(void *ctx, uint16_t slot, const char *path) {
    auto *s = static_cast<FakeStore *>(ctx);
    auto it = s->blobs.find(slot);
    if (it == s->blobs.end()) return ERR_NO_PRESET;
    auto &old = it->second;
    if (old.size() < 2 || (size_t)2 + old[1] > old.size()) return ERR_STORAGE;
    const size_t toff = (size_t)2 + old[1];
    std::vector<uint8_t> nb;
    nb.push_back(old[0]);
    const size_t np = std::strlen(path);
    nb.push_back((uint8_t)np);
    nb.insert(nb.end(), path, path + np);
    nb.insert(nb.end(), old.begin() + toff, old.end());
    old.swap(nb);
    return 0;
}
static int fake_list(void *ctx, preset_list_emit_fn emit, void *ectx) {
    auto *s = static_cast<FakeStore *>(ctx);
    int n = 0;
    for (auto &kv : s->blobs) {
        char path[PRESET_PATH_MAX + 1];
        if (preset_read_path(kv.second.data(), kv.second.size(), path, sizeof(path))) { emit(ectx, kv.first, path); ++n; }
    }
    return n;
}

// float → 4 младших int-байта LE (для передачи в run()).
static void f32_bytes(float v, int out[4]) {
    uint8_t b[4]; std::memcpy(b, &v, 4);
    for (int i = 0; i < 4; ++i) out[i] = b[i];
}

int main() {
    control_init();

    // --- реестр --- (счётчик берём из enum — тест не ломается при добавлении параметров)
    CHECK(param_count() == PARAM_COUNT, "param_count == PARAM_COUNT");
    CHECK(std::fabs(get_param(PARAM_MASTER_VOLUME) - 0.8f) < 1e-6f, "master_volume def 0.8");

    // --- GET id0 ---
    {
        auto s = run({ CMD_GET, 0x00, 0x00 });
        CHECK(s.frames.size() == 1, "GET -> 1 ответ");
        auto &f = s.frames[0];
        CHECK(f[0] == RSP_VALUE && u16(&f[1]) == 0, "GET -> VALUE id0");
        CHECK(std::fabs(f32(&f[3]) - 0.8f) < 1e-6f, "GET value 0.8");
    }

    // --- SET 0.5 (в диапазоне) ---
    {
        int v[4]; f32_bytes(0.5f, v);
        auto s = run({ CMD_SET, 0x00, 0x00, v[0], v[1], v[2], v[3] });
        CHECK(s.frames[0][0] == RSP_VALUE && std::fabs(f32(&s.frames[0][3]) - 0.5f) < 1e-6f, "SET 0.5 -> VALUE 0.5");
        CHECK(std::fabs(get_param(0) - 0.5f) < 1e-6f, "реестр обновился");
    }

    // --- SET 9 -> кламп 1.0 ---
    {
        int v[4]; f32_bytes(9.0f, v);
        auto s = run({ CMD_SET, 0x00, 0x00, v[0], v[1], v[2], v[3] });
        CHECK(std::fabs(f32(&s.frames[0][3]) - 1.0f) < 1e-6f, "SET 9 -> кламп 1.0");
    }

    // --- GET неверный id ---
    {
        // PARAM_COUNT — всегда первый несуществующий id (растёт с реестром), в отличие от литерала,
        // который становился валидным при добавлении параметров (этап 12 сделал id 99 реальным).
        auto s = run({ CMD_GET, (uint8_t)(PARAM_COUNT & 0xFF), (uint8_t)(PARAM_COUNT >> 8) });
        CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_BAD_ID, "GET bad id -> ERR_BAD_ID");
    }

    // --- LIST ---
    {
        auto s = run({ CMD_LIST });
        CHECK(s.frames.size() == PARAM_COUNT + 1, "LIST -> PARAM_COUNT×PARAM + LISTEND");
        bool all_param = true;
        for (int i = 0; i < PARAM_COUNT; ++i) if (s.frames[i][0] != RSP_PARAM) all_param = false;
        CHECK(all_param, "PARAM_COUNT строк PARAM");
        auto &e = s.frames[PARAM_COUNT];
        CHECK(e[0] == RSP_LISTEND && u16(&e[1]) == PARAM_COUNT, "LISTEND count=PARAM_COUNT");
        auto &p0 = s.frames[0];
        const uint8_t namelen = p0[1 + 2 + 1 + 16];  // opcode+id+type+4*f32
        CHECK(namelen == 13 && std::memcmp(&p0[1 + 2 + 1 + 16 + 1], "master_volume", 13) == 0, "имя master_volume");
    }

    // --- STAT ---
    {
        sys_stats_t st = { 12345, 6789, 42, 500, 3 };
        auto s = run({ CMD_STAT }, &st);
        auto &f = s.frames[0];
        CHECK(f[0] == RSP_STAT && u32(&f[1]) == 12345 && u32(&f[5]) == 6789 && u32(&f[9]) == 42
              && u32(&f[13]) == 500 && u32(&f[17]) == 3, "STAT поля");
    }

    // --- NOTE_ON -> ACK ---
    {
        auto s = run({ CMD_NOTE_ON, 60, 100 });
        CHECK(s.frames[0][0] == RSP_ACK, "NOTE_ON -> ACK");
    }

    // --- неизвестная команда ---
    {
        auto s = run({ 0x77 });
        CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_UNKNOWN_CMD, "unknown -> ERR_UNKNOWN_CMD");
    }

    // --- пресеты: фрейминг опкодов через in-memory backend (кодек настоящий) ---
    {
        FakeStore store;
        const preset_backend_t be{ fake_save, fake_load, fake_del, fake_rename, fake_list, &store };

        // SAVE новый (slot=0xFFFF) путь "A/B" → RSP_PRESET_SAVED slot=0
        set_param(PARAM_MASTER_VOLUME, 0.42f);
        {
            auto s = run({ CMD_PRESET_SAVE, 0xFF, 0xFF, 3, 'A', '/', 'B' }, nullptr, &be);
            CHECK(s.frames.size() == 1 && s.frames[0][0] == RSP_PRESET_SAVED, "SAVE -> RSP_PRESET_SAVED");
            CHECK(u16(&s.frames[0][1]) == 0, "первый слот = 0");
        }
        // LIST → RSP_PRESET(slot0,"A/B") + RSP_PRESET_END count=1
        {
            auto s = run({ CMD_PRESET_LIST }, nullptr, &be);
            CHECK(s.frames.size() == 2, "LIST -> 1 пресет + END");
            auto &p = s.frames[0];
            CHECK(p[0] == RSP_PRESET && u16(&p[1]) == 0 && p[3] == 3 && std::memcmp(&p[4], "A/B", 3) == 0, "RSP_PRESET slot0 A/B");
            auto &e = s.frames[1];
            CHECK(e[0] == RSP_PRESET_END && u16(&e[1]) == 1, "PRESET_END count=1");
        }
        // LOAD slot0 после ухода реестра → значение восстановлено
        set_param(PARAM_MASTER_VOLUME, 0.11f);
        {
            auto s = run({ CMD_PRESET_LOAD, 0x00, 0x00 }, nullptr, &be);
            CHECK(s.frames[0][0] == RSP_ACK, "LOAD -> ACK");
            CHECK(std::fabs(get_param(PARAM_MASTER_VOLUME) - 0.42f) < 1e-4f, "LOAD применил пресет");
        }
        // RENAME slot0 → "X/Y"
        {
            auto s = run({ CMD_PRESET_RENAME, 0x00, 0x00, 3, 'X', '/', 'Y' }, nullptr, &be);
            CHECK(s.frames[0][0] == RSP_ACK, "RENAME -> ACK");
            auto l = run({ CMD_PRESET_LIST }, nullptr, &be);
            CHECK(l.frames[0][3] == 3 && std::memcmp(&l.frames[0][4], "X/Y", 3) == 0, "RENAME сменил путь");
        }
        // LOAD несуществующего слота → ERR_NO_PRESET
        {
            auto s = run({ CMD_PRESET_LOAD, 0x09, 0x00 }, nullptr, &be);
            CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_NO_PRESET, "LOAD пустого -> ERR_NO_PRESET");
        }
        // DELETE slot0 → ACK, затем LIST пуст
        {
            auto s = run({ CMD_PRESET_DELETE, 0x00, 0x00 }, nullptr, &be);
            CHECK(s.frames[0][0] == RSP_ACK, "DELETE -> ACK");
            auto l = run({ CMD_PRESET_LIST }, nullptr, &be);
            CHECK(l.frames.size() == 1 && l.frames[0][0] == RSP_PRESET_END && u16(&l.frames[0][1]) == 0, "после DELETE LIST пуст");
        }
        // preset-опкод без backend (pb=nullptr) → ERR_UNKNOWN_CMD
        {
            auto s = run({ CMD_PRESET_LIST });
            CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_UNKNOWN_CMD, "preset без backend -> ERR_UNKNOWN_CMD");
        }
        // SAVE с усечённым телом (path_len=5, а байтов пути 2) → ERR_BAD_LEN
        {
            auto s = run({ CMD_PRESET_SAVE, 0x00, 0x00, 5, 'A', 'B' }, nullptr, &be);
            CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_BAD_LEN, "SAVE усечённый путь -> ERR_BAD_LEN");
        }
    }

    // --- секвенсор: фрейминг опкодов через in-memory backend (кодек паттерна настоящий) ---
    {
        FakeSeq fs;
        const seq_backend_t sb{ fseq_set_step, fseq_get_step, fseq_save, fseq_load, fseq_del, fseq_rename, fseq_list, &fs };

        // SET_STEP 3: активный шаг с нотой 62
        SeqStep st = SeqStep{}; st.active = true; st.n_notes = 1; st.notes[0] = 62; st.velocity = 100; st.trig_prob = 1.0f;
        uint8_t stblob[64]; const size_t sn = seq_step_serialize(stblob, sizeof(stblob), &st);
        std::vector<uint8_t> body; body.push_back(CMD_SEQ_SET_STEP); body.push_back(3);
        for (size_t i = 0; i < sn; ++i) body.push_back(stblob[i]);
        { auto s = run_v(body, &sb); CHECK(s.frames.size() == 1 && s.frames[0][0] == RSP_ACK, "SEQ_SET_STEP -> ACK"); }
        CHECK(fs.pattern[3].active && fs.pattern[3].notes[0] == 62, "SET_STEP применён к паттерну");

        // GET → 16 RSP_SEQ_STEP; шаг 3 несёт активную ноту 62
        {
            auto s = run({ CMD_SEQ_GET }, nullptr, nullptr, &sb);
            CHECK(s.frames.size() == 16, "SEQ_GET -> 16 шагов");
            CHECK(s.frames[3][0] == RSP_SEQ_STEP && s.frames[3][1] == 3, "RSP_SEQ_STEP #3");
            SeqStep got; const size_t c = seq_step_deserialize(&s.frames[3][2], s.frames[3].size() - 2, &got);
            CHECK(c > 0 && got.active && got.notes[0] == 62, "GET шаг 3 = нота 62");
        }

        // SAVE new → RSP_SEQ_SAVED slot 0
        { auto s = run({ CMD_SEQ_SAVE, 0xFF, 0xFF, 1, 'A' }, nullptr, nullptr, &sb);
          CHECK(s.frames[0][0] == RSP_SEQ_SAVED && u16(&s.frames[0][1]) == 0, "SEQ_SAVE -> SAVED slot0"); }

        // затереть шаг 3, LOAD slot0 → ACK, паттерн восстановлен
        fs.pattern[3] = SeqStep{};
        { auto s = run({ CMD_SEQ_LOAD, 0x00, 0x00 }, nullptr, nullptr, &sb);
          CHECK(s.frames[0][0] == RSP_ACK, "SEQ_LOAD -> ACK");
          CHECK(fs.pattern[3].active && fs.pattern[3].notes[0] == 62, "LOAD восстановил шаг 3"); }

        // LIST → RSP_SEQ_ENTRY + RSP_SEQ_END count 1
        { auto s = run({ CMD_SEQ_LIST }, nullptr, nullptr, &sb);
          CHECK(s.frames.size() == 2 && s.frames[0][0] == RSP_SEQ_ENTRY && u16(&s.frames[0][1]) == 0, "SEQ_LIST entry slot0");
          CHECK(s.frames[1][0] == RSP_SEQ_END && u16(&s.frames[1][1]) == 1, "SEQ_LIST END count 1"); }

        // RENAME + DELETE, затем LOAD удалённого → ERR
        { auto s = run({ CMD_SEQ_RENAME, 0x00, 0x00, 1, 'B' }, nullptr, nullptr, &sb);
          CHECK(s.frames[0][0] == RSP_ACK, "SEQ_RENAME -> ACK"); }
        { auto s = run({ CMD_SEQ_DELETE, 0x00, 0x00 }, nullptr, nullptr, &sb);
          CHECK(s.frames[0][0] == RSP_ACK, "SEQ_DELETE -> ACK"); }
        { auto s = run({ CMD_SEQ_LOAD, 0x00, 0x00 }, nullptr, nullptr, &sb);
          CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_NO_PRESET, "LOAD удалённого -> ERR"); }

        // seq-опкод без backend → ERR_UNKNOWN_CMD
        { auto s = run({ CMD_SEQ_GET });
          CHECK(s.frames[0][0] == RSP_ERR && s.frames[0][1] == ERR_UNKNOWN_CMD, "seq без backend -> ERR_UNKNOWN_CMD"); }
    }

    // --- id ↔ строка реестра ---
    // В control.cpp это проверяется static_assert'ом (params_in_order), но сдвиг таблицы — самый
    // дорогой из возможных здесь дефектов: GUI и патчи строятся по LIST, поэтому чужие min/max
    // молча уводят DSP за диапазон. Дублируем несколько якорей рантаймом, чтобы проверка выжила,
    // даже если static_assert когда-нибудь снесут.
    {
        struct { uint16_t id; const char *name; } anchors[] = {
            { PARAM_MASTER_VOLUME, "master_volume" },
            { PARAM_TEST_TONE,     "test_tone"     },
            { PARAM_CUTOFF,        "cutoff"        },
            { PARAM_POLY_VOICES,   "poly_voices"   },
            { PARAM_MTX1_SRC,      "mtx1_src"      },
            { PARAM_MTX8_DEPTH,    "mtx8_depth"    },
            { PARAM_WAVEENV_P1,    "waveenv_p1"    },
            { PARAM_REVERB_MIX,    "reverb_mix"    },
        };
        for (const auto &a : anchors) {
            param_info_t info{};
            const bool ok = param_get_info(a.id, &info);
            CHECK(ok && info.name && std::strcmp(info.name, a.name) == 0, a.name);
        }
        CHECK(param_count() == PARAM_COUNT, "param_count() == PARAM_COUNT");
    }

    // --- NaN/Inf с провода не должны попадать в параметр ---
    // set_param — единственные ворота в DSP, а в них приходит произвольное 32-битное слово.
    // Прямой кламп (v < min / v > max) пропускает NaN: оба сравнения для него ложны. NaN в
    // g_values отравляет тракт и рециркулирует в кольцах delay/reverb через feedback — выход
    // остаётся мусором даже после записи корректного значения, до перезагрузки.
    {
        const float before = get_param(PARAM_CUTOFF);
        int nan_b[4]; f32_bytes(std::nanf(""), nan_b);
        run({ CMD_SET, PARAM_CUTOFF & 0xFF, (PARAM_CUTOFF >> 8) & 0xFF,
              nan_b[0], nan_b[1], nan_b[2], nan_b[3] });
        const float after = get_param(PARAM_CUTOFF);
        CHECK(std::isfinite(after), "SET NaN → параметр остался конечным");

        int inf_b[4]; f32_bytes(INFINITY, inf_b);
        run({ CMD_SET, PARAM_CUTOFF & 0xFF, (PARAM_CUTOFF >> 8) & 0xFF,
              inf_b[0], inf_b[1], inf_b[2], inf_b[3] });
        CHECK(std::isfinite(get_param(PARAM_CUTOFF)), "SET +Inf → параметр остался конечным");

        int neg_b[4]; f32_bytes(-INFINITY, neg_b);
        run({ CMD_SET, PARAM_CUTOFF & 0xFF, (PARAM_CUTOFF >> 8) & 0xFF,
              neg_b[0], neg_b[1], neg_b[2], neg_b[3] });
        CHECK(std::isfinite(get_param(PARAM_CUTOFF)), "SET -Inf → параметр остался конечным");

        set_param(PARAM_CUTOFF, before);   // вернуть как было для остальных проверок
    }

    // --- CRC совпадает со стандартным check value CRC-16/CCITT-FALSE ---
    // Гарантирует, что прошивка и GUI/скрипт считают CRC одинаково (иначе связь не сойдётся).
    CHECK(frame_crc16(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0x29B1,
          "CRC check value = 0x29B1");

    // --- golden-кадры: ПОЛНЫЕ литеральные байты с CRC ---
    // Зеркало таблиц из docs/serial-protocol.md и того же набора в app/proto/proto_test.go
    // (TestEncodeGoldenFrames). Это и есть настоящий кросс-якорь (T-007): байты CRC ниже —
    // жёстко зашитые литералы, посчитанные независимой ссылкой, а НЕ выводом frame_crc16. Поэтому
    // смена покрытия CRC (LEN+BODY→BODY), порядка байт CRC или раскладки полей ловится ЗДЕСЬ —
    // хотя frame_encode и его собственный frame_crc16 остались бы согласованы между собой.
    // Три источника (док + оба теста) держим байт-в-байт одинаковыми; настоящая смена протокола —
    // это правка всех трёх разом (точка, где контракт провода осознанно переподписывается).
    {
        struct Golden { const char *name; std::vector<uint8_t> body; std::vector<uint8_t> frame; };
        const std::vector<Golden> golden = {
            { "LIST",           { 0x03 },
              { 0x55, 0xAA, 0x01, 0x03, 0x5D, 0x1E } },
            { "GET id0",        { 0x02, 0x00, 0x00 },
              { 0x55, 0xAA, 0x03, 0x02, 0x00, 0x00, 0x7C, 0x71 } },
            { "SET id0=0.5",    { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F },
              { 0x55, 0xAA, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFB, 0x89 } },
            { "STAT",           { 0x06 },
              { 0x55, 0xAA, 0x01, 0x06, 0xF8, 0x4E } },
            { "NOTE_ON 60,100", { 0x04, 0x3C, 0x64 },
              { 0x55, 0xAA, 0x03, 0x04, 0x3C, 0x64, 0x06, 0xAF } },
            { "NOTE_OFF 60",    { 0x05, 0x3C },
              { 0x55, 0xAA, 0x02, 0x05, 0x3C, 0xD6, 0xAA } },
            { "VALUE 0.8",      { 0x81, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3F },
              { 0x55, 0xAA, 0x07, 0x81, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3F, 0x17, 0xB3 } },
            { "empty body",     {},
              { 0x55, 0xAA, 0x00, 0xF0, 0xE1 } },
        };
        for (const auto &g : golden) {
            // Энкодер должен выдать ровно эти байты, включая CRC.
            uint8_t out[FRAME_MAX_SIZE];
            const size_t n = frame_encode(g.body.data(), g.body.size(), out, sizeof(out));
            bool enc_ok = (n == g.frame.size()) &&
                          std::memcmp(out, g.frame.data(), n) == 0;
            CHECK(enc_ok, g.name);

            // И декодер должен принять литеральные байты и вернуть точно то же тело — значит CRC,
            // который несут эти литералы, ровно тот, что проверяет наш декодер.
            frame_decoder_t d; frame_decoder_init(&d);
            const uint8_t *dbody = nullptr; size_t dlen = 0; bool got = false;
            for (uint8_t b : g.frame)
                if (frame_decoder_push(&d, b, &dbody, &dlen)) got = true;
            bool dec_ok = got && dlen == g.body.size() &&
                          (g.body.empty() || std::memcmp(dbody, g.body.data(), dlen) == 0);
            CHECK(dec_ok, g.name);
        }
    }

    // --- кадр: encode -> decode round-trip ---
    {
        const uint8_t body[] = { CMD_SET, 0x01, 0x00, 0xAA, 0xBB, 0xCC, 0xDD };
        uint8_t frame[FRAME_MAX_SIZE];
        const size_t n = frame_encode(body, sizeof(body), frame, sizeof(frame));
        CHECK(n == 2 + 1 + sizeof(body) + 2, "encode длина");
        frame_decoder_t d; frame_decoder_init(&d);
        const uint8_t *out = nullptr; size_t outlen = 0; bool got = false;
        for (size_t i = 0; i < n; ++i) if (frame_decoder_push(&d, frame[i], &out, &outlen)) got = true;
        CHECK(got && outlen == sizeof(body) && std::memcmp(out, body, sizeof(body)) == 0, "decode совпал");
    }

    // --- кадр: битый CRC -> игнор ---
    {
        const uint8_t body[] = { CMD_GET, 0x00, 0x00 };
        uint8_t frame[FRAME_MAX_SIZE];
        const size_t n = frame_encode(body, sizeof(body), frame, sizeof(frame));
        frame[n - 1] ^= 0xFF;  // портим старший байт CRC
        frame_decoder_t d; frame_decoder_init(&d);
        const uint8_t *out; size_t outlen; bool got = false;
        for (size_t i = 0; i < n; ++i) if (frame_decoder_push(&d, frame[i], &out, &outlen)) got = true;
        CHECK(!got, "битый CRC -> кадр отброшен");
    }

    // --- декодер ресинхронизируется после ASCII-лога ---
    {
        const char *log = "I (123) audio: init\n";
        const uint8_t body[] = { CMD_STAT };
        uint8_t frame[FRAME_MAX_SIZE];
        const size_t n = frame_encode(body, sizeof(body), frame, sizeof(frame));
        frame_decoder_t d; frame_decoder_init(&d);
        const uint8_t *out = nullptr; size_t outlen = 0; bool got = false;
        for (const char *c = log; *c; ++c) frame_decoder_push(&d, (uint8_t)*c, &out, &outlen);
        for (size_t i = 0; i < n; ++i) if (frame_decoder_push(&d, frame[i], &out, &outlen)) got = true;
        CHECK(got && out && out[0] == CMD_STAT, "кадр после ASCII-лога распознан");
    }

    // --- ресинхронизация после ЛОЖНОГО синка ---
    // Прежняя версия этой проверки кормила декодер строкой "I (123) audio: init\n", в которой нет
    // байта 0x55 — автомат не покидал S_SYNC0, и весь механизм ресинка оставался непроверенным.
    // Ложный синк — совсем другое дело: старый байтовый декодер брал следующий байт за LEN и
    // необратимо съедал LEN+2 байта, уничтожая настоящие кадры внутри этого окна.
    {
        const uint8_t body[] = { CMD_STAT };
        uint8_t frame[FRAME_MAX_SIZE];
        const size_t n = frame_encode(body, sizeof(body), frame, sizeof(frame));

        frame_decoder_t d; frame_decoder_init(&d);
        const uint8_t *out = nullptr; size_t outlen = 0;
        const uint8_t junk[] = { 'x', FRAME_SYNC0, FRAME_SYNC1 };   // ложный синк без кадра за ним
        for (uint8_t b : junk) frame_decoder_push(&d, b, &out, &outlen);

        int decoded = 0;
        for (int rep = 0; rep < 20; ++rep)
            for (size_t i = 0; i < n; ++i)
                if (frame_decoder_push(&d, frame[i], &out, &outlen)) ++decoded;
        CHECK(decoded == 20, "после ложного синка ни один из 20 кадров не потерян");
    }

    // Ложный синк, объявивший огромную длину, не должен задерживать уже пришедший целый кадр
    // (head-of-line stall): если дальше в буфере лежит кадр с сошедшимся CRC, синк был ложный.
    {
        const uint8_t body[] = { CMD_GET, 0x05, 0x00 };
        uint8_t frame[FRAME_MAX_SIZE];
        const size_t n = frame_encode(body, sizeof(body), frame, sizeof(frame));

        frame_decoder_t d; frame_decoder_init(&d);
        const uint8_t *out = nullptr; size_t outlen = 0; bool got = false;
        frame_decoder_push(&d, FRAME_SYNC0, &out, &outlen);
        frame_decoder_push(&d, FRAME_SYNC1, &out, &outlen);   // LEN возьмётся из тела кадра ниже
        for (size_t i = 0; i < n; ++i) if (frame_decoder_push(&d, frame[i], &out, &outlen)) got = true;
        CHECK(got && out && out[0] == CMD_GET, "ложный синк не задерживает готовый кадр");
    }

    if (g_fail == 0) { std::printf("OK: все проверки пройдены\n"); return 0; }
    std::printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
