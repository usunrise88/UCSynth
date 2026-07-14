// comm — протокол связи с ПК по USB CDC (Serial). Текстовый на старте (проще отлаживать).
// Команды: GET/SET/LIST/NOTE_ON/NOTE_OFF/STAT. LIST даёт GUI строить UI динамически
// из дампа реестра параметров. Реализация — этап 0.3.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void comm_init(void);

#ifdef __cplusplus
}
#endif
