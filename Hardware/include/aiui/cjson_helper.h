#ifndef CJSON_HELPER_H
#define CJSON_HELPER_H

#include "aiui_log.h"
#include "cjson.h"
#include <stdlib.h>
#include <string.h>

#ifndef UNUSED
#define UNUSED(x) (void)x;
#endif // !UNUSED

#ifdef LOG_TAG
#define _CJSON_HELPER_LOG_TAG LOG_TAG
#elif(defined(AIUI_TAG))
#define _CJSON_HELPER_LOG_TAG AIUI_TAG
#else
#define _CJSON_HELPER_LOG_TAG "CJSON_HELPER"
#endif

#define EXTRACT_JSON_INT(target_var, json_item)      \
  if (cJSON_Number == json_item->type)               \
  {                                                  \
      (target_var) = json_item->valueint;            \
  }                                                  \
  else                                               \
  {                                                  \
      (target_var) = atoi(json_item->valuestring);   \
  }

#define EXTRACT_JSON_INT_BY_KEY(target_var, json_node, key)              \
  do {                                                                   \
       cJSON *item_##key = NULL;                                         \
       if (NULL == json_node ||                                          \
           NULL == (item_##key = cJSON_GetObjectItem(json_node, #key)))  \
       {                                                                 \
          LOGE(_CJSON_HELPER_LOG_TAG, "no key \"%s\" in json", #key);    \
          break;                                                         \
       }                                                                 \
       EXTRACT_JSON_INT(target_var, item_##key);                         \
  } while(0)

#define EXTRACT_JSON_STR_BY_KEY(target_var, target_var_size, json_node, key)   \
  do {                                                                         \
       cJSON *item_##key = cJSON_GetObjectItem(json_node, #key);               \
       if (NULL == item_##key)                                                 \
       {                                                                       \
          LOGV(_CJSON_HELPER_LOG_TAG, "no key \"%s\" in json", #key);          \
          break;                                                               \
       }                                                                       \
       strncpy((target_var), item_##key->valuestring, (target_var_size) - 1U); \
  } while(0)


#endif //CJSON_HELPER_H
