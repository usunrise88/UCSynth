// Host-тест логики протокола (без железа): реестр control + кадрирование frame + диспетчер
// protocol. Компилится обычным g++, потому что ядро не тянет ESP-IDF (control.cpp на не-ESP
// глушит ESP_LOG, frame/protocol вообще без ESP-заголовков). Запуск: tools/run-host-tests.sh
#include "control.h"
#include "protocol.h"
#include "frame.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
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
static Sink run(std::initializer_list<int> bytes, const sys_stats_t *st = nullptr) {
    std::vector<uint8_t> body;
    for (int b : bytes) body.push_back((uint8_t)b);
    Sink s;
    comm_handle_request(body.data(), body.size(), st, sink_emit, &s);
    return s;
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
        auto s = run({ CMD_GET, 0x63, 0x00 });  // id=99
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
