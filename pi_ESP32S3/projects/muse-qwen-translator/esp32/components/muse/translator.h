#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define TR_URL_MAX 255
#define TR_KEY_MAX 511
#define TR_MODEL_MAX 95
#define TR_TEXT_MAX 2048
#define TR_HISTORY 4
typedef struct { char base[TR_URL_MAX+1], ws[TR_URL_MAX+1], key[TR_KEY_MAX+1], model[TR_MODEL_MAX+1]; } tr_config_t;
typedef struct { char original[TR_TEXT_MAX], translated[TR_TEXT_MAX]; } tr_turn_t;
void tr_config_get(tr_config_t *out);
esp_err_t tr_config_set(const tr_config_t *config);
esp_err_t tr_config_init(void);
void tr_history_get(tr_turn_t *out, unsigned *count, uint32_t *version);
void tr_history_begin(void);
void tr_history_abort(void);
void tr_history_original(const char *original);
void tr_history_translation(const char *translated);
void tr_history_result(const char *original, const char *translated);
