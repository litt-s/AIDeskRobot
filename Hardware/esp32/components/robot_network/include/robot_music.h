#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve an AIUI musicX itemid through iFlytek's production NetEase API. */
esp_err_t robot_music_resolve_play_url(const char *item_id,
                                       char *play_url,
                                       size_t play_url_size);

/* Search the bound NetEase account and return the first playable song. */
esp_err_t robot_music_search_first(const char *keyword,
                                   char *item_id,
                                   size_t item_id_size,
                                   char *title,
                                   size_t title_size);

#ifdef __cplusplus
}
#endif
