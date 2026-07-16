// env — ADSR-огибающая, gate-driven, control-rate (шаг = один аудио-блок).
// Экспоненциальные сегменты (один полюс к цели). latch/drone делается снаружи удержанием gate;
// loop — циклит A→D→A для эволюции дрона. Чистый DSP (float, без ESP-IDF) → host-тестируем.
#pragma once

#include <cstdint>

enum EnvStage : uint8_t { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

// Параметры огибающей: A/D/R — секунды, S — уровень [0,1]. loop — циклить A→D при удержании gate.
struct EnvParams {
    float a, d, s, r;
    bool  loop;
};

struct Env {
    EnvStage stage;
    float    level;
    bool     prev_gate;
};

void env_reset(Env *e);

// Продвинуть на dt секунд (длительность блока), вернуть текущий уровень [0,1].
// gate — удерживается ли нота (фронт 0→1 = attack из текущего уровня, 1→0 = release).
float env_tick(Env *e, const EnvParams *p, bool gate, float dt);

// Форсировать атаку из текущего уровня (ретригер при удержанном gate — polyphony/mono restrike).
// prev_gate не трогаем: gate уже высокий → env_tick не увидит фронта и не сбросит стадию.
void env_trigger(Env *e);
