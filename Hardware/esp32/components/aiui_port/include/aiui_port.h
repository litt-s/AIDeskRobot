#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t aiui_port_init(void);
void aiui_port_deinit(void);

#ifdef __cplusplus
}
#endif
