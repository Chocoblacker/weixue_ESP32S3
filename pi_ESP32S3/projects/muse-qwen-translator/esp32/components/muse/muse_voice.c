/* Local push-to-talk translator. Hardware remains owned by muse_audio. */
#include "muse_voice.h"
#include "muse_input.h"
#include "muse_audio.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"
#include "muse_mem.h"
#include "translator.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#define REC_MAX (16000*12)
#define OUT_MAX (24000*12) /* bounded per target: 12 seconds of 24 kHz PCM */
#define MESSAGE_MAX (64*1024)
#define CREATED BIT0
#define UPDATED BIT1
#define FINISHED BIT2
#define FAILED BIT3
/* Phase 1: validate the device-side ASR path before coupling it to translation. */
#define TRANSLATOR_ASR_PHASE1 0
/* Two target sessions open asynchronously while recording proceeds. */
#define TRANSLATOR_PARALLEL_TARGETS 1
static QueueHandle_t queue;
static volatile bool monitor, resting, chirp;
static volatile float monitor_db=-100;
static bool pending_press;
static int16_t *recording, *output, *output_alt, *pair_left, *pair_right;
static const char *TAG="translator";
static bool s_time_started;
static SemaphoreHandle_t s_time_lock;
static portMUX_TYPE s_record_mux=portMUX_INITIALIZER_UNLOCKED;
static volatile int s_parallel_selected=-1;
static volatile size_t s_capture_frames;
static volatile bool s_recording_done;
typedef struct {
    EventGroupHandle_t events;
    char *message;
    char *packet;
    size_t message_n;
    int16_t *audio;
    size_t audio_capacity;
    size_t frames;
    size_t played_frames;
    size_t uploaded_frames;
    esp_websocket_client_handle_t ws;
    uint8_t source_language; /* 0 unknown, 1 Chinese, 2 English */
    volatile int *selection; /* -1 unknown, 0 target=en, 1 target=zh */
    int target_index;
    bool audio_enabled;
    int64_t started_us, created_us, updated_us, first_transcript_us;
    int64_t first_translation_us, first_audio_us, finished_us;
    TaskHandle_t opener;
    volatile bool open_done, open_ok, send_done, cancel;
    volatile size_t *captured_frames;
    volatile bool *recording_done;
    tr_config_t config;
    char original[TR_TEXT_MAX], translated[TR_TEXT_MAX];
} response_t;
static uint8_t detect_language_prefix(const char *text) {
    const unsigned char *p=(const unsigned char *)text;
    while(*p) {
        if(*p<0x80) {
            if((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')) return 2;
            p++; continue;
        }
        if((p[0]&0xf0)==0xe0 && p[1] && p[2]) {
            unsigned cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63);
            if(cp>=0x3400 && cp<=0x9fff) return 1;
            p+=3; continue;
        }
        if((p[0]&0xe0)==0xc0 && p[1]) p+=2;
        else if((p[0]&0xf8)==0xf0 && p[1]&&p[2]&&p[3]) p+=4;
        else p++;
    }
    return 0;
}
static void append_text(char *out,const char *text) {
    if(!text) return;
    size_t have=strlen(out), n=strlen(text), room=TR_TEXT_MAX-have-1;
    if(n>room) { n=room; while(n && ((unsigned char)text[n]&0xc0)==0x80) n--; }
    memcpy(out+have,text,n); out[have+n]=0;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    response_t *r=arg; esp_websocket_event_data_t *e=data;
    if(id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG,"WS event target=%d id=%ld connected=%d bits=0x%x",r->target_index,(long)id,(int)esp_websocket_client_is_connected(r->ws),(unsigned)xEventGroupGetBits(r->events));
        if(!(xEventGroupGetBits(r->events)&FINISHED)) xEventGroupSetBits(r->events,FAILED);
        return;
    }
    if(id!=WEBSOCKET_EVENT_DATA || (e->op_code!=1 && e->op_code!=0)) return;
    /* payload_offset==0 starts every new WebSocket message. Do not require
     * opcode 1: the client may report text fragments with continuation opcode. */
    if(e->payload_offset==0) r->message_n=0;
    if(e->data_len<0 || r->message_n+(size_t)e->data_len>=MESSAGE_MAX) { ESP_LOGW(TAG,"WS message overflow target=%d data=%d accumulated=%u",r->target_index,(int)e->data_len,(unsigned)r->message_n); xEventGroupSetBits(r->events,FAILED);return; }
    memcpy(r->message+r->message_n,e->data_ptr,e->data_len);r->message_n+=e->data_len;
    if(e->payload_offset+e->data_len!=e->payload_len || !e->fin) return;
    r->message[r->message_n]=0;
    cJSON *j=cJSON_Parse(r->message); if(!j) { ESP_LOGW(TAG,"JSON parse failed target=%d bytes=%u offset=%u payload=%u",r->target_index,(unsigned)r->message_n,(unsigned)e->payload_offset,(unsigned)e->payload_len); xEventGroupSetBits(r->events,FAILED);return; }
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(j,"type"));
    const char *delta=cJSON_GetStringValue(cJSON_GetObjectItem(j,"delta"));
    if(!strcmp(type?type:"","session.finished")) ESP_LOGI(TAG,"WS finished target=%d",r->target_index);
    if(type) {
        if(!strcmp(type,"session.created")) { r->created_us=esp_timer_get_time(); ESP_LOGI(TAG,"timeline target=%d created +%lldms",r->target_index,(long long)((r->created_us-r->started_us)/1000)); xEventGroupSetBits(r->events,CREATED); }
        else if(!strcmp(type,"session.updated")) { r->updated_us=esp_timer_get_time(); ESP_LOGI(TAG,"timeline target=%d updated +%lldms",r->target_index,(long long)((r->updated_us-r->started_us)/1000)); xEventGroupSetBits(r->events,UPDATED); }
        else if(!strcmp(type,"session.finished")) { r->finished_us=esp_timer_get_time(); ESP_LOGI(TAG,"timeline target=%d finished +%lldms",r->target_index,(long long)((r->finished_us-r->started_us)/1000)); xEventGroupSetBits(r->events,FINISHED); }
        else if(!strcmp(type,"error")) {
            cJSON *err=cJSON_GetObjectItem(j,"error");
            const char *code=err?cJSON_GetStringValue(cJSON_GetObjectItem(err,"code")):NULL;
            const char *msg=err?cJSON_GetStringValue(cJSON_GetObjectItem(err,"message")):NULL;
            ESP_LOGW(TAG,"API error target=%d code=%s message=%s",r->target_index,code?code:"?",msg?msg:"?");
            xEventGroupSetBits(r->events,FAILED);
        }
        else if(!strcmp(type,"conversation.item.input_audio_transcription.delta")) {
            if(!r->first_transcript_us) { r->first_transcript_us=esp_timer_get_time(); ESP_LOGI(TAG,"timeline target=%d transcript +%lldms",r->target_index,(long long)((r->first_transcript_us-r->started_us)/1000)); }
            append_text(r->original,delta);
            if(r->selection && *r->selection==r->target_index) tr_history_original(r->original);
            if(!r->source_language) {
                r->source_language=detect_language_prefix(r->original);
                if(r->source_language && r->selection && *r->selection<0)
                    *r->selection=(r->source_language==1)?0:1;
            }
        }
        else if(!strcmp(type,"conversation.item.input_audio_transcription.completed")) {
            const char *text=cJSON_GetStringValue(cJSON_GetObjectItem(j,"transcript"));
            if(!text) text=cJSON_GetStringValue(cJSON_GetObjectItem(j,"text"));
            if(text) {
                r->original[0]=0;append_text(r->original,text);r->source_language=detect_language_prefix(r->original);
                if(r->source_language && r->selection && *r->selection<0)
                    *r->selection=(r->source_language==1)?0:1;
                if(r->selection && *r->selection==r->target_index) tr_history_original(r->original);
            }
        } else if(!strcmp(type,"conversation.item.input_audio_transcription.text")) {
            const char *text=cJSON_GetStringValue(cJSON_GetObjectItem(j,"text"));
            if(text) { append_text(r->original,text); if(r->selection && *r->selection==r->target_index) tr_history_original(r->original); }
        } else if(!strcmp(type,"response.audio_transcript.delta") || !strcmp(type,"response.text.delta")) {
            if(!r->first_translation_us) { r->first_translation_us=esp_timer_get_time(); ESP_LOGI(TAG,"timeline target=%d translation +%lldms",r->target_index,(long long)((r->first_translation_us-r->started_us)/1000)); }
            append_text(r->translated,delta);
            if(r->selection && *r->selection==r->target_index) tr_history_translation(r->translated);
        }
        else if(!strcmp(type,"response.audio.delta") && delta && r->audio_enabled) {
            if(!r->first_audio_us) { r->first_audio_us=esp_timer_get_time(); ESP_LOGI(TAG,"timeline target=%d audio +%lldms",r->target_index,(long long)((r->first_audio_us-r->started_us)/1000)); }
            size_t bytes=0, len=strlen(delta), required=(len/4)*3;
            if(required>(r->audio_capacity-r->frames)*2) {
                ESP_LOGW(TAG,"audio buffer full target=%d delta=%u frames=%u cap=%u",r->target_index,(unsigned)len,(unsigned)r->frames,(unsigned)r->audio_capacity);
                xEventGroupSetBits(r->events,FAILED);
            } else if(!r->selection || *r->selection<0 || *r->selection==r->target_index) {
                int rc=mbedtls_base64_decode((unsigned char *)(r->audio+r->frames),(r->audio_capacity-r->frames)*2,&bytes,(const unsigned char *)delta,len);
                if(rc!=0 || (bytes&1)) {
                    ESP_LOGW(TAG,"audio base64 failed target=%d rc=%d len=%u bytes=%u",r->target_index,rc,(unsigned)len,(unsigned)bytes);
                    xEventGroupSetBits(r->events,FAILED);
                } else r->frames+=bytes/2;
            }
        }
    }
    cJSON_Delete(j);
}
static bool interrupted(void) {
    muse_input_event_t ev;
    while(xQueueReceive(queue,&ev,0)==pdTRUE) if(ev.type==MUSE_PTT_DOWN) { pending_press=true; return true; }
    return false;
}
static bool wait_event_internal(response_t *r,EventBits_t bit,int seconds,bool watch_ptt) {
    int64_t until=esp_timer_get_time()+seconds*1000000LL;
    while(esp_timer_get_time()<until) {
        if(r->cancel || (watch_ptt && interrupted())) return false;
        EventBits_t b=xEventGroupWaitBits(r->events,bit|FAILED,pdFALSE,pdFALSE,pdMS_TO_TICKS(50));
        /* session.finished may be followed immediately by DISCONNECTED;
         * completion wins over the transport teardown notification. */
        if(b&bit) return true;
        if(b&FAILED) return false;
    }
    return false;
}
static bool wait_event(response_t *r,EventBits_t bit,int seconds) { return wait_event_internal(r,bit,seconds,true); }
static bool wait_event_background(response_t *r,EventBits_t bit,int seconds) { return wait_event_internal(r,bit,seconds,false); }
static bool send_text(esp_websocket_client_handle_t ws,const char *s) { return esp_websocket_client_send_text(ws,s,strlen(s),pdMS_TO_TICKS(3000))==(int)strlen(s); }
static void close_session(response_t *r);
static bool open_session(const tr_config_t *cfg,const char *language,response_t *r) {
    if(s_time_lock) xSemaphoreTake(s_time_lock,portMAX_DELAY);
    if(!s_time_started) { esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0,"ntp.aliyun.com"); esp_sntp_init(); s_time_started=true; }
    if(s_time_lock) xSemaphoreGive(s_time_lock);
    int64_t deadline=esp_timer_get_time()+20000000;
    while(time(NULL)<1700000000 && esp_timer_get_time()<deadline) { vTaskDelay(pdMS_TO_TICKS(100)); }
    if(time(NULL)<1700000000) return false;
    char uri[TR_URL_MAX+TR_MODEL_MAX+32], *header=heap_caps_malloc(TR_KEY_MAX+40,MUSE_BIG_CAPS);
    if(!header) return false;
    snprintf(uri,sizeof(uri),"%s%smodel=%s",cfg->ws,strchr(cfg->ws,'?')?"&":"?",cfg->model);
    snprintf(header,TR_KEY_MAX+40,"Authorization: Bearer %s\r\n",cfg->key);
    r->message_n=r->frames=0; r->source_language=0; r->audio_capacity=OUT_MAX; r->original[0]=r->translated[0]=0;
    r->started_us=esp_timer_get_time(); r->created_us=r->updated_us=r->first_transcript_us=r->first_translation_us=r->first_audio_us=r->finished_us=0;
    ESP_LOGI(TAG,"timeline target=%d connect.start",r->target_index);
    xEventGroupClearBits(r->events,CREATED|UPDATED|FINISHED|FAILED);
    esp_websocket_client_config_t wc={.uri=uri,.headers=header,.disable_auto_reconnect=true,.crt_bundle_attach=esp_crt_bundle_attach,.task_stack=8192,.buffer_size=4096,.network_timeout_ms=10000};
    r->ws=esp_websocket_client_init(&wc); free(header);
    if(!r->ws) return false;
    esp_websocket_register_events(r->ws,WEBSOCKET_EVENT_ANY,event,r);
    if(esp_websocket_client_start(r->ws)!=ESP_OK || !wait_event_background(r,CREATED,8)) {
        ESP_LOGW(TAG,"session create timeout target=%d bits=0x%x",r->target_index,(unsigned)xEventGroupGetBits(r->events));
        close_session(r); return false;
    }
    char setup[180]; snprintf(setup,sizeof(setup),"{\"type\":\"session.update\",\"session\":{\"output_modalities\":[\"text\"%s],\"translation\":{\"language\":\"%s\"}}}",r->audio_enabled?",\"audio\"":"",language);
    if(!send_text(r->ws,setup) || !wait_event_background(r,UPDATED,3)) {
        ESP_LOGW(TAG,"session setup timeout target=%d created=%d updated=%d bits=0x%x",r->target_index,
                 r->created_us!=0,r->updated_us!=0,(unsigned)xEventGroupGetBits(r->events));
        close_session(r); return false;
    }
    return true;
}
static void close_session(response_t *r) {
    if(r->ws) {
        esp_websocket_client_stop(r->ws);
        /* The websocket task owns internal-RAM resources; let it leave its
         * event loop before destroy, especially on repeated ASR sessions. */
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_destroy(r->ws);
        r->ws=NULL;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static bool send_audio_data(response_t *r,const int16_t *pcm,size_t n) {
    size_t b64=0; const char *prefix="{\"type\":\"input_audio_buffer.append\",\"audio\":\""; size_t start=strlen(prefix);
    memcpy(r->packet,prefix,start);
    if(mbedtls_base64_encode((unsigned char *)r->packet+start,4400-start-4,&b64,(const unsigned char *)pcm,n*2)) return false;
    memcpy(r->packet+start+b64,"\"}",3); return send_text(r->ws,r->packet);
}
static bool send_audio_chunk(response_t *r,size_t offset,size_t n) {
    int16_t snapshot[1600]; if(n>1600)return false;
    portENTER_CRITICAL(&s_record_mux); memcpy(snapshot,recording+offset,n*sizeof(int16_t)); portEXIT_CRITICAL(&s_record_mux);
    return send_audio_data(r,snapshot,n);
}
static void open_worker(void *arg) {
    response_t *r=arg;
    r->open_ok=false;
    for(int attempt=0; attempt<2 && !r->cancel; attempt++) {
        if(attempt) vTaskDelay(pdMS_TO_TICKS(250));
        if(open_session(&r->config,r->target_index==0?"en":"zh",r)) { r->open_ok=true; break; }
        ESP_LOGW(TAG,"connect retry target=%d attempt=%d",r->target_index,attempt+1);
    }
    r->open_done=true;
    if(r->open_ok) {
        while(!r->cancel && (!*r->recording_done || r->uploaded_frames<*r->captured_frames)) {
            size_t available=*r->captured_frames;
            if(r->uploaded_frames<available) {
                size_t n=available-r->uploaded_frames<1600?available-r->uploaded_frames:1600;
                if(!send_audio_chunk(r,r->uploaded_frames,n)) { r->open_ok=false; break; }
                r->uploaded_frames+=n;
            } else vTaskDelay(pdMS_TO_TICKS(5));
        }
        if(r->open_ok && !r->cancel) r->open_ok=send_text(r->ws,"{\"type\":\"session.finish\"}");
    }
    if(r->cancel) close_session(r);
    r->send_done=true;
    r->opener=NULL;
    vTaskDelete(NULL);
}
static bool finish_session(response_t *r) {
    if(!r->ws || !send_text(r->ws,"{\"type\":\"session.finish\"}") || !wait_event(r,FINISHED,45)) { close_session(r); return false; }
    close_session(r); return true;
}
static bool asr_phase1(const tr_config_t *cfg,size_t frames,response_t *r) {
    char uri[TR_URL_MAX+TR_MODEL_MAX+32], *header=heap_caps_malloc(TR_KEY_MAX+40,MUSE_BIG_CAPS);
    if(!header) return false;
    snprintf(uri,sizeof(uri),"%s?model=qwen3-asr-flash-realtime",cfg->ws);
    snprintf(header,TR_KEY_MAX+40,"Authorization: Bearer %s\r\n",cfg->key);
    r->message_n=r->frames=0;r->source_language=0;r->audio_capacity=OUT_MAX;r->original[0]=r->translated[0]=0;
    xEventGroupClearBits(r->events,CREATED|UPDATED|FINISHED|FAILED);
    esp_websocket_client_config_t wc={.uri=uri,.headers=header,.disable_auto_reconnect=true,.crt_bundle_attach=esp_crt_bundle_attach,.task_stack=8192,.buffer_size=4096,.network_timeout_ms=10000};
    esp_websocket_client_handle_t ws=esp_websocket_client_init(&wc);free(header);
    if(!ws)return false;
    r->ws=ws;esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,event,r);
    if(esp_websocket_client_start(ws)!=ESP_OK||!wait_event(r,CREATED,20)){close_session(r);return false;}
    const char *setup="{\"type\":\"session.update\",\"event_id\":\"phase1\",\"session\":{\"input_audio_format\":\"pcm\",\"sample_rate\":16000,\"input_audio_transcription\":{},\"turn_detection\":null}}";
    if(!send_text(ws,setup)||!wait_event(r,UPDATED,10)){close_session(r);return false;}
    for(size_t i=0;i<frames;i+=1600){size_t n=frames-i<1600?frames-i:1600;if(!send_audio_chunk(r,i,n)){close_session(r);return false;}}
    if(!send_text(ws,"{\"type\":\"input_audio_buffer.commit\",\"event_id\":\"commit\"}")){close_session(r);return false;}
    bool ok=send_text(ws,"{\"type\":\"session.finish\",\"event_id\":\"finish\"}")&&wait_event(r,FINISHED,45);
    close_session(r);return ok;
}
static response_t *create_parallel_session(const tr_config_t *cfg,int index,volatile int *selected) {
    response_t *r=heap_caps_calloc(1,sizeof(*r),MUSE_BIG_CAPS);
    if(!r) return NULL;
    r->events=xEventGroupCreate();r->message=heap_caps_malloc(MESSAGE_MAX,MUSE_BIG_CAPS);r->packet=heap_caps_malloc(4400,MUSE_BIG_CAPS);
    r->audio=index==0?output:output_alt;r->audio_capacity=OUT_MAX;r->selection=selected;r->target_index=index;r->config=*cfg;r->audio_enabled=muse_settings_speaker_on();
    r->captured_frames=&s_capture_frames;r->recording_done=&s_recording_done;
    if(!r->events||!r->message||!r->packet) { if(r->events)vEventGroupDelete(r->events);free(r->message);free(r->packet);free(r);return NULL; }
    if(xTaskCreatePinnedToCore(open_worker,index==0?"qwen_en":"qwen_zh",8192,r,5,&r->opener,MUSE_AUDIO_CORE)!=pdPASS) { vEventGroupDelete(r->events);free(r->message);free(r->packet);free(r);return NULL; }
    return r;
}
static bool parallel_translate(response_t *a,response_t *b,size_t frames,response_t *out) {
    response_t *rs[2]={a,b}; volatile int *selected=&s_parallel_selected;
    a->selection=selected;b->selection=selected;
    int64_t ready_deadline=esp_timer_get_time()+10000000;
    while((!a->send_done || !b->send_done) && esp_timer_get_time()<ready_deadline) {
        if(a->open_done && b->open_done && !a->open_ok && !b->open_ok) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool senders_done=a->send_done && b->send_done;
    /* Transcription is delivered asynchronously after upload completion. Do not
     * reject a healthy session just because language selection has not arrived
     * in the same scheduler tick as the sender tasks finishing. */
    bool both_setup_failed=(a->open_done && !a->open_ok && b->open_done && !b->open_ok);
    int64_t select_deadline=esp_timer_get_time()+3000000;
    while(senders_done && !both_setup_failed && *selected<0 && esp_timer_get_time()<select_deadline) {
        EventBits_t ba=xEventGroupGetBits(a->events), bb=xEventGroupGetBits(b->events);
        if((ba&FAILED) && (bb&FAILED)) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    int selected_now=*selected;
    bool ready=senders_done && selected_now>=0 && rs[selected_now]->open_ok &&
               rs[selected_now]->uploaded_frames==frames;
    ESP_LOGI(TAG,"timeline upload.result done=%d selected=%d en_ok=%d en_frames=%u zh_ok=%d zh_frames=%u expected=%u free_int=%u",
             senders_done,selected_now,a->open_ok,(unsigned)a->uploaded_frames,b->open_ok,(unsigned)b->uploaded_frames,
             (unsigned)frames,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if(!ready) {
        ESP_LOGW(TAG,"parallel upload failed selected=%d en_bits=0x%x zh_bits=0x%x en_done=%d zh_done=%d",
                 selected_now,(unsigned)xEventGroupGetBits(a->events),(unsigned)xEventGroupGetBits(b->events),
                 a->send_done,b->send_done);
        for(int i=0;i<2;i++) { rs[i]->cancel=true; while(!rs[i]->send_done)vTaskDelay(pdMS_TO_TICKS(5)); close_session(rs[i]); if(rs[i]->events)vEventGroupDelete(rs[i]->events); memset(&rs[i]->config,0,sizeof(rs[i]->config)); free(rs[i]->message);free(rs[i]->packet);free(rs[i]); }
        return false;
    }
    ESP_LOGI(TAG,"timeline upload.done frames=%u en=%u zh=%u",(unsigned)frames,(unsigned)a->uploaded_frames,(unsigned)b->uploaded_frames);
    bool ok=true; int winner_index=selected_now; int64_t wait_start=esp_timer_get_time();
    bool playback_started=false; size_t play_cursor=0;
    if(ok) {
        /* Do not wait for target 0 then target 1. The first useful
         * transcription chooses the winner; the loser is cancelled. */
        while(esp_timer_get_time()-wait_start < 45000000) {
            int choice=*selected;
            if(choice>=0) {
                winner_index=choice;
                int loser=choice^1;
                if(rs[loser]->ws) {
                    ESP_LOGI(TAG,"parallel cancel loser=%d after language decision",loser);
                    rs[loser]->cancel=true;
                    while(!rs[loser]->send_done) vTaskDelay(pdMS_TO_TICKS(5));
                    close_session(rs[loser]);
                }
                EventBits_t bits=xEventGroupGetBits(rs[choice]->events);
                if(bits&FAILED) { ok=false; break; }
                if(bits&FINISHED) break;
            } else {
                EventBits_t ba=xEventGroupGetBits(a->events), bb=xEventGroupGetBits(b->events);
                if((ba&FAILED)&&(bb&FAILED)) { ok=false; break; }
                if((ba&FINISHED)||(bb&FINISHED)) {
                    /* A completed response normally has a transcription;
                     * use it if the callback did not publish selection yet. */
                    if(a->source_language) *selected=a->source_language==1?0:1;
                    else if(b->source_language) *selected=b->source_language==1?0:1;
                    else if(ba&FINISHED) *selected=0; else *selected=1;
                }
            }
            /* Play the selected target as soon as a safe PCM prefix exists.
             * The websocket callback continues filling the buffer meanwhile. */
            if(*selected>=0 && rs[*selected]->audio_enabled && !playback_started && rs[*selected]->frames>=8000) {
                playback_started=true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
                muse_state_set_caption("Playing translation");
                ESP_LOGI(TAG,"timeline playback.early target=%d pcm_frames=%u",*selected,(unsigned)rs[*selected]->frames);
            }
            if(playback_started && *selected>=0) {
                response_t *playing=rs[*selected];
                size_t available=playing->frames;
                while(play_cursor+2<available) {
                    int16_t pcm[MUSE_AUDIO_CHUNK]; size_t n=0;
                    while(n+2<=MUSE_AUDIO_CHUNK && play_cursor+2<available) {
                        pcm[n++]=playing->audio[play_cursor];
                        pcm[n++]=((int32_t)playing->audio[play_cursor+1]+playing->audio[play_cursor+2])/2;
                        play_cursor+=3;
                    }
                    if(n && muse_audio_write(pcm,n)!=ESP_OK) { ok=false; break; }
                }
                playing->played_frames=play_cursor;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if(winner_index<0) winner_index=*selected;
        if(winner_index>=0 && ok) {
            EventBits_t bits=xEventGroupGetBits(rs[winner_index]->events);
            ok=(bits&FINISHED)!=0 && !(bits&FAILED);
        } else ok=false;
    }
    if(*selected<0) *selected=a->source_language==1?0:(b->source_language==1?0:1);
    response_t *winner=*selected==0?a:b;
    /* Once FINISHED is observed, the winning PCM buffer is complete. Drain its
     * remaining tail here, before websocket teardown, so the caller does not
     * insert close_session() delays between early playback and the tail. */
    if(ok && winner->audio_enabled && playback_started && winner->played_frames<winner->frames) {
        size_t cursor=winner->played_frames;
        int16_t pcm[MUSE_AUDIO_CHUNK];
        while(cursor+2<winner->frames && !interrupted()) {
            size_t n=0;
            while(n+2<=MUSE_AUDIO_CHUNK && cursor+2<winner->frames) {
                pcm[n++]=winner->audio[cursor];
                pcm[n++]=((int32_t)winner->audio[cursor+1]+winner->audio[cursor+2])/2;
                cursor+=3;
            }
            if(n && muse_audio_write(pcm,n)!=ESP_OK) { ok=false; break; }
        }
        winner->played_frames=cursor;
    }
    for(int i=0;i<2;i++) {
        rs[i]->cancel=true;
        ESP_LOGI(TAG,"timeline target=%d summary bits=0x%x transcript_ms=%lld translation_ms=%lld audio_ms=%lld finished_ms=%lld chars=%u/%u pcm_frames=%u",i,(unsigned)xEventGroupGetBits(rs[i]->events),rs[i]->first_transcript_us?(long long)((rs[i]->first_transcript_us-rs[i]->started_us)/1000):-1LL,rs[i]->first_translation_us?(long long)((rs[i]->first_translation_us-rs[i]->started_us)/1000):-1LL,rs[i]->first_audio_us?(long long)((rs[i]->first_audio_us-rs[i]->started_us)/1000):-1LL,rs[i]->finished_us?(long long)((rs[i]->finished_us-rs[i]->started_us)/1000):-1LL,(unsigned)strlen(rs[i]->original),(unsigned)strlen(rs[i]->translated),(unsigned)rs[i]->frames);
        while(!rs[i]->send_done) vTaskDelay(pdMS_TO_TICKS(5));
        close_session(rs[i]);
    }
    if(winner->audio!=output) memcpy(output,winner->audio,winner->frames*sizeof(int16_t));
    strlcpy(out->original,winner->original,TR_TEXT_MAX);strlcpy(out->translated,winner->translated,TR_TEXT_MAX);out->frames=winner->frames;out->played_frames=winner->played_frames;
    ESP_LOGI(TAG,"parallel targets selected=%s en_orig=\"%s\" zh_orig=\"%s\" en_audio=%u zh_audio=%u",*selected==0?"en":"zh",a->original,b->original,(unsigned)a->frames,(unsigned)b->frames);
    for(int i=0;i<2;i++){if(rs[i]->events)vEventGroupDelete(rs[i]->events);free(rs[i]->message);free(rs[i]->packet);free(rs[i]);}
    return ok && out->original[0] && out->translated[0];
}
static bool translate(const tr_config_t *cfg,size_t frames,const char *language,response_t *r) {
    if(!s_time_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0,"ntp.aliyun.com");esp_sntp_init();s_time_started=true;
    }
    int64_t time_deadline=esp_timer_get_time()+20000000;
    while(time(NULL)<1700000000 && esp_timer_get_time()<time_deadline) {
        if(interrupted())return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(time(NULL)<1700000000)return false;
    char uri[TR_URL_MAX+TR_MODEL_MAX+32], header[TR_KEY_MAX+40];
    snprintf(uri,sizeof(uri),"%s%smodel=%s",cfg->ws,strchr(cfg->ws,'?')?"&":"?",cfg->model);
    snprintf(header,sizeof(header),"Authorization: Bearer %s\r\n",cfg->key);
    r->message_n=r->frames=0;r->original[0]=r->translated[0]=0;
    xEventGroupClearBits(r->events,CREATED|UPDATED|FINISHED|FAILED);
    esp_websocket_client_config_t wc={.uri=uri,.headers=header,.disable_auto_reconnect=true,
        .crt_bundle_attach=esp_crt_bundle_attach,.task_stack=8192,.buffer_size=4096,.network_timeout_ms=10000};
    esp_websocket_client_handle_t ws=esp_websocket_client_init(&wc); bool ok=false;
    if(!ws) return false;
    esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,event,r);
    if(esp_websocket_client_start(ws)!=ESP_OK || !wait_event(r,CREATED,20)) goto done;
    char setup[180];snprintf(setup,sizeof(setup),"{\"type\":\"session.update\",\"session\":{\"output_modalities\":[\"text\",\"audio\"],\"translation\":{\"language\":\"%s\"}}}",language);
    if(!send_text(ws,setup)||!wait_event(r,UPDATED,10)) goto done;
    char *packet=r->packet;
    for(size_t i=0;i<frames;i+=1600) {
        if(interrupted() || (xEventGroupGetBits(r->events)&FAILED)) goto done;
        size_t n=frames-i<1600?frames-i:1600, b64=0;
        const char *prefix="{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
        size_t start=strlen(prefix);memcpy(packet,prefix,start);
        if(mbedtls_base64_encode((unsigned char *)packet+start,4400-start-4,&b64,(unsigned char *)(recording+i),n*2)) goto done;
        memcpy(packet+start+b64,"\"}",3);
        if(!send_text(ws,packet)) goto done;
        /* Upload as fast as the socket allows; the old 100 ms pacing doubled latency. */
        if(((i / 1600) & 3) == 3) vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(!send_text(ws,"{\"type\":\"session.finish\"}")||!wait_event(r,FINISHED,45)) goto done;
    ok=true;
done:
    esp_websocket_client_stop(ws);esp_websocket_client_destroy(ws);
    memset(header,0,sizeof(header));return ok;
}
static bool chinese(const char *s) {
    /* UTF-8 Han range U+3400..U+9FFF. Mixed speech: any Han selects zh->en. */
    const unsigned char *p=(const unsigned char *)s;
    while(*p) {
        if((p[0]&0xf0)==0xe0 && p[1] && p[2]) {
            unsigned cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63);
            if(cp>=0x3400 && cp<=0x9fff) return true;
            p+=3;
        } else p++;
    }
    return false;
}
static void voice_task(void *arg) {
    response_t *r=heap_caps_calloc(1,sizeof(*r),MUSE_BIG_CAPS);
    tr_config_t *cfg=heap_caps_malloc(sizeof(*cfg),MUSE_BIG_CAPS);
    if(r) {r->events=xEventGroupCreate();r->message=heap_caps_malloc(MESSAGE_MAX,MUSE_BIG_CAPS);r->packet=heap_caps_malloc(4400,MUSE_BIG_CAPS);r->audio=output;r->audio_capacity=OUT_MAX;}
    if(!r||!cfg||!r->events||!r->message||!r->packet) { muse_state_set_mode(MUSE_MODE_ERROR);muse_state_set_caption("No memory");vTaskDelete(NULL);return; }
    int16_t chunk[MUSE_AUDIO_CHUNK];
    for(;;) {
        muse_input_event_t ev;
        bool have_event=false;
        if(pending_press) { ev.type=MUSE_PTT_DOWN; ev.wake=false; pending_press=false; have_event=true; }
        else have_event=xQueueReceive(queue,&ev,pdMS_TO_TICKS(20))==pdTRUE;
        if(!have_event) {
            if(muse_state_asleep() && !monitor) {
                if(!resting) {muse_audio_power(false);resting=true;}
                muse_state_wait_awake(100);continue;
            }
            if(resting) {muse_audio_power(true);resting=false;}
            /* Keep capture DMA drained even when the level meter is hidden. */
            if(muse_audio_read(chunk,MUSE_AUDIO_CHUNK)==ESP_OK && monitor) monitor_db=muse_audio_dbfs(chunk,MUSE_AUDIO_CHUNK);
            if(chirp) {chirp=false;muse_audio_chirp(1);}
            continue;
        }
        if(ev.type!=MUSE_PTT_DOWN) continue;
        if(resting) {muse_audio_power(true);resting=false;}
        tr_config_get(cfg);tr_history_begin();
        muse_state_set_progress(0.0f);
        muse_state_set_asleep(false);muse_state_set_mode(MUSE_MODE_LISTENING);muse_state_set_caption("Recording...");
        ESP_LOGI(TAG,"PTT start reset=%d int=%u psram=%u stack=%u",(int)esp_reset_reason(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),(unsigned)uxTaskGetStackHighWaterMark(NULL));
        response_t *pre_a=NULL,*pre_b=NULL;
#if TRANSLATOR_PARALLEL_TARGETS
        s_parallel_selected=-1; s_capture_frames=0; s_recording_done=false;
        if(cfg->key[0] && muse_wifi_connected()) {
            pre_a=create_parallel_session(cfg,0,&s_parallel_selected);
            pre_b=create_parallel_session(cfg,1,&s_parallel_selected);
            ESP_LOGI(TAG,"timeline preconnect started en=%d zh=%d",pre_a!=NULL,pre_b!=NULL);
        }
#endif
        size_t frames=0,speech_frames=0;bool released=false;bool streamed=false;const char *target="en";
        const size_t max_record_frames=REC_MAX;
        while(!released && frames<max_record_frames) {
            if(muse_audio_read(chunk,MUSE_AUDIO_CHUNK)!=ESP_OK) {released=true;frames=0;break;}
            portENTER_CRITICAL(&s_record_mux);
            memcpy(recording+frames,chunk,MUSE_AUDIO_CHUNK*sizeof(int16_t));
            frames+=MUSE_AUDIO_CHUNK; s_capture_frames=frames;
            portEXIT_CRITICAL(&s_record_mux);
            muse_state_set_progress((float)frames/(float)max_record_frames);
            float chunk_db=muse_audio_dbfs(chunk,MUSE_AUDIO_CHUNK);
            if(chunk_db>-55.0f) speech_frames+=MUSE_AUDIO_CHUNK;
            muse_state_set_level(muse_audio_level(chunk,MUSE_AUDIO_CHUNK));
#if TRANSLATOR_PARALLEL_TARGETS
            /* Session workers observe s_capture_frames and upload independently. */
#endif
            while(!released && xQueueReceive(queue,&ev,0)==pdTRUE) {
                if(ev.type==MUSE_PTT_UP) released=true;
                else if(ev.type==MUSE_PTT_DOWN) { pending_press=true; released=true; }
            }
        }
        bool hit_record_limit=!released && frames>=max_record_frames;
        if(hit_record_limit)
            ESP_LOGW(TAG,"recording limit reached; auto-stopping at %u ms",(unsigned)(frames*1000/16000));
        if(!released && !hit_record_limit)
            while(xQueueReceive(queue,&ev,pdMS_TO_TICKS(1000))!=pdTRUE || ev.type!=MUSE_PTT_UP) {}
        s_recording_done=true;
        muse_state_set_progress(0.0f);
        ESP_LOGI(TAG,"PTT end frames=%u duration_ms=%u int=%u psram=%u stack=%u",(unsigned)frames,(unsigned)(frames*1000/16000),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),(unsigned)uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG,"recording speech_frames=%u limit=%u hit_limit=%d",(unsigned)speech_frames,(unsigned)max_record_frames,(int)hit_record_limit);
        if(frames<1600 || speech_frames<1600 || !cfg->key[0] || !muse_wifi_connected()) {
#if TRANSLATOR_PARALLEL_TARGETS
            response_t *unused[2]={pre_a,pre_b};
            for(int ui=0;ui<2;ui++) if(unused[ui]) { unused[ui]->cancel=true; while(!unused[ui]->send_done)vTaskDelay(pdMS_TO_TICKS(5)); close_session(unused[ui]); if(unused[ui]->events)vEventGroupDelete(unused[ui]->events); free(unused[ui]->message);free(unused[ui]->packet);free(unused[ui]); }
#endif
#if TRANSLATOR_ASR_PHASE1
            tr_history_abort();
#endif
            if(frames<1600 || speech_frames<1600) tr_history_abort();
            muse_state_set_progress(0.0f);
            muse_state_set_mode(MUSE_MODE_IDLE);muse_state_set_caption(!muse_wifi_connected()?"Set up Wi-Fi":!cfg->key[0]?"Set API key":"No speech detected");continue;
        }
#if TRANSLATOR_ASR_PHASE1
        muse_state_set_mode(MUSE_MODE_THINKING);muse_state_set_caption("Recognizing speech...");
        bool ok=asr_phase1(cfg,frames,r);
        memset(cfg->key,0,sizeof(cfg->key));
        if(!ok || !r->original[0]) { if(!r->original[0]) tr_history_abort(); muse_state_set_mode(MUSE_MODE_IDLE);muse_state_set_caption("ASR failed; try again");continue; }
        tr_history_original(r->original);
        ESP_LOGI(TAG,"ASR phase1 completed: original=\"%s\"",r->original);
        muse_state_set_mode(MUSE_MODE_IDLE);muse_state_set_caption("ASR OK; translation paused for phase 1");
        continue;
#else
#if TRANSLATOR_PARALLEL_TARGETS
        muse_state_set_mode(MUSE_MODE_THINKING);muse_state_set_caption("Translating...");
        bool ok=false;
        if(pre_a && pre_b) ok=parallel_translate(pre_a,pre_b,frames,r);
        else {
            response_t *partial[2]={pre_a,pre_b}; s_recording_done=true;
            for(int pi=0;pi<2;pi++) if(partial[pi]) {
                partial[pi]->cancel=true;
                while(!partial[pi]->send_done)vTaskDelay(pdMS_TO_TICKS(10));
                close_session(partial[pi]);if(partial[pi]->events)vEventGroupDelete(partial[pi]->events);
                memset(&partial[pi]->config,0,sizeof(partial[pi]->config));free(partial[pi]->message);free(partial[pi]->packet);free(partial[pi]);
            }
        }
        ESP_LOGI(TAG,"parallel translation ok=%d",(int)ok);
#else
        muse_state_set_mode(MUSE_MODE_THINKING);muse_state_set_caption("Recognizing language...");
        bool detected=asr_phase1(cfg,frames,r); bool source_chinese=chinese(r->original); target=source_chinese?"en":"zh";
        if(!detected || !r->original[0]) { if(!r->original[0]) tr_history_abort(); muse_state_set_mode(MUSE_MODE_IDLE);muse_state_set_caption("Language recognition failed");continue; }
        r->translated[0]=0;r->frames=0; muse_state_set_caption(target[0]=='e'?"Translating to English":"Translating to Chinese");
        bool ok=translate(cfg,frames,target,r);
#endif
        memset(cfg->key,0,sizeof(cfg->key));
        if(pending_press) continue;
        if(!ok || !r->original[0] || !r->translated[0]) {
            if(!r->original[0]) tr_history_abort();
            muse_state_set_progress(0.0f);
            muse_state_set_mode(MUSE_MODE_IDLE);
            muse_state_set_caption(!r->original[0]?"No speech detected":"Translation failed; try again");continue;
        }
        tr_history_result(r->original,r->translated);
        ESP_LOGI(TAG,"translation done: audio frames=%u",(unsigned)r->frames);
        muse_state_set_mode(MUSE_MODE_SPEAKING);muse_state_set_caption(muse_settings_speaker_on()?"Playing translation":"Translation ready (muted)");
        ESP_LOGI(TAG,"timeline playback.start enabled=%d pcm_frames=%u played=%u",(int)muse_settings_speaker_on(),(unsigned)r->frames,(unsigned)r->played_frames);
        /* 24k output -> existing 16k duplex bus. Linear 3:2 resampling. */
        for(size_t i=r->played_frames;i+2<r->frames && muse_settings_speaker_on() && !interrupted();) {
            size_t n=0;
            while(n+2<=MUSE_AUDIO_CHUNK && i+2<r->frames) {
                chunk[n++]=r->audio[i];chunk[n++]=((int32_t)r->audio[i+1]+r->audio[i+2])/2;i+=3;
            }
            if(muse_audio_write(chunk,n)!=ESP_OK) break;
        }
        muse_state_set_progress(0.0f);
        muse_state_set_mode(MUSE_MODE_IDLE);muse_state_set_caption("Hold top button to translate");
#endif
    }
}
esp_err_t muse_voice_start(QueueHandle_t q) {
    queue=q;
    recording=heap_caps_malloc(REC_MAX*2,MUSE_BIG_CAPS);output=heap_caps_malloc(OUT_MAX*2,MUSE_BIG_CAPS);output_alt=heap_caps_malloc(OUT_MAX*2,MUSE_BIG_CAPS);
    pair_left=heap_caps_malloc(MUSE_AUDIO_CHUNK*2,MUSE_BIG_CAPS);pair_right=heap_caps_malloc(MUSE_AUDIO_CHUNK*2,MUSE_BIG_CAPS);
    if(!recording||!output||!output_alt||!pair_left||!pair_right) { ESP_LOGE(TAG,"buffer allocation failed recording=%p output=%p alt=%p",recording,output,output_alt); return ESP_ERR_NO_MEM; }
    s_time_lock=xSemaphoreCreateMutex();
    esp_err_t err=muse_audio_init(muse_settings_volume(),muse_settings_mic_gain());if(err) return err;
    return xTaskCreatePinnedToCore(voice_task,"translate",12288,NULL,6,NULL,MUSE_AUDIO_CORE)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void muse_voice_set_monitor(bool on) {monitor=on;}
float muse_voice_monitor_db(void) {return monitor_db;}
void muse_voice_request_chirp(void) {chirp=true;}
void muse_voice_request_loopback(void) {}
void muse_voice_request_mp3test(void) {}
bool muse_voice_resting(void) {return resting;}
bool muse_voice_notes_waiting(void) {return false;}
