// control — единая модель параметров (ядро архитектуры).
// ВСЕ источники управления (GUI/Serial, энкодеры, тач) пишут ТОЛЬКО сюда,
// никогда напрямую в DSP. Реализация реестра + set_param/get_param — этап 0.2.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void control_init(void);

#ifdef __cplusplus
}
#endif
