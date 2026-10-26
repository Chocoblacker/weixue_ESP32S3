#include "translator.h"
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t lock;
static tr_config_t cfg;
static tr_turn_t turns[TR_HISTORY];
static unsigned count;
static uint32_t version;
static nvs_handle_t store;
esp_err_t tr_config_init(void) {
    lock=xSemaphoreCreateMutex(); if(!lock) return ESP_ERR_NO_MEM;
    strcpy(cfg.base,"https://maas.qianwenaiapi.com/compatible-mode/v1");
    strcpy(cfg.ws,"wss://maas.qianwenaiapi.com/api-ws/v1/realtime");
    strcpy(cfg.model,"qwen3.8-livetranslate-flash-realtime");
    esp_err_t err=nvs_open("translator",NVS_READWRITE,&store); if(err) return err;
    /* Public builds never embed a credential. Enter the key on the device. */
    cfg.key[0]='\0';
    size_t n=sizeof(cfg); tr_config_t saved;
    if(nvs_get_blob(store,"config",&saved,&n)==ESP_OK && n==sizeof(saved)) {
        saved.base[TR_URL_MAX]=saved.ws[TR_URL_MAX]=saved.key[TR_KEY_MAX]=saved.model[TR_MODEL_MAX]=0;
        cfg=saved;
    }
    if(nvs_get_blob(store,"config",NULL,&n)==ESP_ERR_NVS_NOT_FOUND) {
        err=nvs_set_blob(store,"config",&cfg,sizeof(cfg));if(!err)err=nvs_commit(store);if(err)return err;
    }
    return ESP_OK;
}
void tr_config_get(tr_config_t *out) { xSemaphoreTake(lock,portMAX_DELAY); *out=cfg; xSemaphoreGive(lock); }
esp_err_t tr_config_set(const tr_config_t *in) {
    if(strncmp(in->base,"https://",8)||strncmp(in->ws,"wss://",6)||!in->model[0]) return ESP_ERR_INVALID_ARG;
    for(const char *p=in->key; *p; p++) if((unsigned char)*p<32||*p==127) return ESP_ERR_INVALID_ARG;
    for(const char *p=in->model; *p; p++) if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'||*p=='.')) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t err=nvs_set_blob(store,"config",in,sizeof(*in)); if(!err) err=nvs_commit(store);
    if(!err) cfg=*in;
    xSemaphoreGive(lock); return err;
}
void tr_history_get(tr_turn_t *out,unsigned *n,uint32_t *v) { xSemaphoreTake(lock,portMAX_DELAY); memcpy(out,turns,sizeof(turns)); *n=count; *v=version; xSemaphoreGive(lock); }
void tr_history_begin(void) {
    xSemaphoreTake(lock,portMAX_DELAY);
    if(count==TR_HISTORY) { memmove(turns,turns+1,(TR_HISTORY-1)*sizeof(turns[0])); count--; }
    memset(&turns[count++],0,sizeof(turns[0])); version++; xSemaphoreGive(lock);
}
void tr_history_abort(void) {
    xSemaphoreTake(lock,portMAX_DELAY);
    if(count && !turns[count-1].original[0] && !turns[count-1].translated[0]) { memset(&turns[--count],0,sizeof(turns[0])); version++; }
    xSemaphoreGive(lock);
}
void tr_history_original(const char *original) {
    xSemaphoreTake(lock,portMAX_DELAY);
    if(count) { strlcpy(turns[count-1].original,original,TR_TEXT_MAX); version++; }
    xSemaphoreGive(lock);
}
void tr_history_translation(const char *translated) {
    xSemaphoreTake(lock,portMAX_DELAY);
    if(count) { strlcpy(turns[count-1].translated,translated,TR_TEXT_MAX); version++; }
    xSemaphoreGive(lock);
}
void tr_history_result(const char *original,const char *translated) {
    xSemaphoreTake(lock,portMAX_DELAY);
    if(count) { strlcpy(turns[count-1].original,original,TR_TEXT_MAX); strlcpy(turns[count-1].translated,translated,TR_TEXT_MAX); version++; }
    xSemaphoreGive(lock);
}
