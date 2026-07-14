// io — физическая периферия: I2C (MCP23017 ×3, VL53L0X), энкодеры, матрица, дисплей, тач.
// Контролы транслируются в параметры ТОЛЬКО через control (тот же путь, что и GUI). Этапы 8–10.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void io_init(void);

#ifdef __cplusplus
}
#endif
