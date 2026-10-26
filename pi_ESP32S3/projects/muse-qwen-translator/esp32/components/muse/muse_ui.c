/* Local bilingual chat UI; hardware/settings continue using the original adapters. */
#include "muse_ui.h"
#include "muse_board.h"
#include "muse_state.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#include "translator.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
LV_FONT_DECLARE(muse_font_cjk_16)
static lv_obj_t *tv,*home,*settings,*status,*wifi_mark,*chat,*speaker,*cover,*record_ring;
static lv_indev_t *indev;
static lv_font_t font;
static bool dark,ready;
static int brightness=-1,preview=-1;
static uint32_t shown_version=UINT32_MAX;
static tr_turn_t snapshot[TR_HISTORY];
static void muted(lv_event_t *e) { (void)e;muse_settings_set_speaker_on(!muse_settings_speaker_on()); }
static void poke(lv_event_t *e) {(void)e;muse_state_poke();}
static void wake(lv_event_t *e) {
    if(lv_event_get_code(e)==LV_EVENT_PRESSED) {muse_state_set_asleep(false);lv_obj_set_style_bg_opa(cover,LV_OPA_TRANSP,0);}
    else if(!muse_state_asleep()) lv_obj_add_flag(cover,LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *text(lv_obj_t *p,const char *s,uint32_t color) {
    lv_obj_t *l=lv_label_create(p);lv_obj_set_style_text_font(l,&font,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);
    lv_label_set_text(l,s);return l;
}
static void bubble(const char *s,bool original) {
    lv_obj_t *box=lv_obj_create(chat);lv_obj_remove_style_all(box);
    lv_obj_set_size(box,324,LV_SIZE_CONTENT);lv_obj_set_style_pad_all(box,10,0);lv_obj_set_style_radius(box,22,0);
    lv_obj_set_style_bg_opa(box,LV_OPA_COVER,0);lv_obj_set_style_bg_color(box,lv_color_hex(original?0x183b5b:0x4b263f),0);
    lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(box,6,0);
    text(box,original?"我说 / Original":"译文 / Translation",original?0x55c8ff:0xff8ab4);
    lv_obj_t *l=text(box,s[0]?s:"…",0xf2efff);lv_obj_set_width(l,300);lv_label_set_long_mode(l,LV_LABEL_LONG_MODE_WRAP);
}
static void tick(lv_timer_t *t) {
    (void)t;
    bool asleep=muse_state_asleep();
    if(asleep&&!dark) {
        muse_board->set_brightness(0);if(muse_board->panel_sleep) muse_board->panel_sleep(true);
        lv_obj_set_style_bg_opa(cover,LV_OPA_COVER,0);lv_obj_remove_flag(cover,LV_OBJ_FLAG_HIDDEN);dark=true;brightness=0;
    } else if(!asleep&&dark) {
        if(muse_board->panel_sleep) muse_board->panel_sleep(false);
        dark=false;
        if(lv_obj_get_style_bg_opa(cover,0)==LV_OPA_COVER)lv_obj_add_flag(cover,LV_OBJ_FLAG_HIDDEN);
    }
    if(dark) return;
    /* Brightness is system state: restore it before any app-specific return. */
    int target=muse_settings_brightness();if(preview==target)preview=-1;if(preview>=0)target=preview;
    if(target!=brightness){muse_board->set_brightness(target);brightness=target;}
    muse_settings_ui_tick(lv_obj_get_scroll_x(tv)>0);
    lv_label_set_text(speaker,muse_settings_speaker_on()?LV_SYMBOL_VOLUME_MAX:LV_SYMBOL_MUTE);
    static int last_ring_value=-1;
    if(muse_state_mode(NULL)==MUSE_MODE_LISTENING) {
        int remain=100-(int)(muse_state_progress()*100.0f);
        if(remain<0) remain=0;
        if(remain>100) remain=100;
        if(remain!=last_ring_value) { lv_arc_set_value(record_ring,remain); last_ring_value=remain; }
        lv_obj_clear_flag(record_ring,LV_OBJ_FLAG_HIDDEN);
    } else { lv_obj_add_flag(record_ring,LV_OBJ_FLAG_HIDDEN); last_ring_value=-1; }
    static uint32_t caption_version;static char caption[MUSE_CAPTION_MAX];
    bool changed=muse_state_caption(caption,sizeof(caption),&caption_version);
    static bool was_wifi;
    bool wifi=muse_wifi_connected();
    if(changed||wifi!=was_wifi){lv_label_set_text(status,caption);lv_label_set_text(wifi_mark,wifi?LV_SYMBOL_WIFI:"offline");was_wifi=wifi;}
    unsigned n;uint32_t v;tr_history_get(snapshot,&n,&v);
    static uint32_t last_chat_ms;
    uint32_t now_ms=lv_tick_get();
    if(v!=shown_version && (uint32_t)(now_ms-last_chat_ms)>=120){
        shown_version=v;last_chat_ms=now_ms;lv_obj_clean(chat);
        if(!n){lv_obj_t *hint=text(chat,"按住上方按键说话\n松开后自动中英互译\n\n向左滑动打开设置",0x8b84a8);lv_obj_set_width(hint,320);}
        for(unsigned i=0;i<n;i++){bubble(snapshot[i].original,true);if(snapshot[i].translated[0])bubble(snapshot[i].translated,false);}
        lv_obj_update_layout(chat);lv_obj_scroll_to_y(chat,LV_COORD_MAX,LV_ANIM_OFF);
    }
}
esp_err_t muse_ui_start(void) {
    font=lv_font_montserrat_16;font.fallback=&muse_font_cjk_16;
    lv_display_t *disp=muse_board->display_start(&indev);if(!disp)return ESP_FAIL;
    muse_board->display_lock(-1);
    lv_obj_t *scr=lv_screen_active();lv_obj_set_style_bg_color(scr,lv_color_black(),0);lv_obj_remove_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    tv=lv_tileview_create(scr);lv_obj_set_style_bg_color(tv,lv_color_black(),0);lv_obj_set_scrollbar_mode(tv,LV_SCROLLBAR_MODE_OFF);
    home=lv_tileview_add_tile(tv,0,0,LV_DIR_RIGHT);settings=lv_tileview_add_tile(tv,1,0,LV_DIR_LEFT);
    lv_obj_set_style_bg_color(home,lv_color_black(),0);lv_obj_set_style_bg_opa(home,LV_OPA_COVER,0);
    lv_obj_t *title=text(home,"中英互译",0xa77dff);lv_obj_align(title,LV_ALIGN_TOP_MID,0,20);
    lv_obj_t *button=lv_button_create(home);lv_obj_set_size(button,42,38);lv_obj_align(button,LV_ALIGN_TOP_MID,150,58);
    lv_obj_set_style_bg_color(button,lv_color_hex(0x2e2552),0);lv_obj_add_event_cb(button,muted,LV_EVENT_CLICKED,NULL);
    speaker=text(button,LV_SYMBOL_VOLUME_MAX,0xf2efff);lv_obj_center(speaker);
    wifi_mark=text(home,LV_SYMBOL_WIFI,0x8b84a8);lv_obj_align(wifi_mark,LV_ALIGN_TOP_MID,-150,68);
    status=text(home,"正在启动…",0x8b84a8);lv_obj_set_size(status,220,42);lv_obj_align(status,LV_ALIGN_TOP_MID,12,62);lv_label_set_long_mode(status,LV_LABEL_LONG_MODE_WRAP);
    /* Wide centered bubbles use the circular panel width; the panel itself
     * clips them naturally at the upper/lower edges of the round display. */
    chat=lv_obj_create(home);lv_obj_remove_style_all(chat);lv_obj_set_size(chat,360,328);lv_obj_align(chat,LV_ALIGN_TOP_MID,0,116);
    lv_obj_set_flex_flow(chat,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(chat,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(chat,5,0);lv_obj_set_style_pad_left(chat,8,0);lv_obj_set_style_pad_right(chat,8,0);lv_obj_set_style_pad_bottom(chat,32,0);lv_obj_set_scroll_dir(chat,LV_DIR_VER);lv_obj_set_scrollbar_mode(chat,LV_SCROLLBAR_MODE_OFF);
    record_ring=lv_arc_create(lv_layer_top());lv_obj_set_size(record_ring,458,458);lv_obj_center(record_ring);
    lv_arc_set_bg_angles(record_ring,0,360);lv_arc_set_rotation(record_ring,270);lv_arc_set_range(record_ring,0,100);lv_arc_set_value(record_ring,100);
    lv_obj_remove_style(record_ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(record_ring,2,LV_PART_MAIN);lv_obj_set_style_arc_color(record_ring,lv_color_hex(0x203820),LV_PART_MAIN);lv_obj_set_style_arc_opa(record_ring,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_arc_width(record_ring,2,LV_PART_INDICATOR);lv_obj_set_style_arc_color(record_ring,lv_color_hex(0x35ff63),LV_PART_INDICATOR);lv_obj_set_style_arc_opa(record_ring,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_obj_add_flag(record_ring,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(record_ring,LV_OBJ_FLAG_CLICKABLE);
    muse_settings_ui_build(settings);
    cover=lv_obj_create(lv_layer_top());lv_obj_remove_style_all(cover);lv_obj_set_size(cover,lv_pct(100),lv_pct(100));lv_obj_set_style_bg_color(cover,lv_color_black(),0);
    lv_obj_add_flag(cover,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(cover,wake,LV_EVENT_PRESSED,NULL);lv_obj_add_event_cb(cover,wake,LV_EVENT_RELEASED,NULL);lv_obj_add_event_cb(cover,wake,LV_EVENT_PRESS_LOST,NULL);
    if(indev)lv_indev_add_event_cb(indev,poke,LV_EVENT_PRESSED,NULL);
    lv_timer_create(tick,100,NULL);ready=true;muse_board->display_unlock();return ESP_OK;
}
bool muse_ui_dark(void){return dark;}
void muse_ui_show_face(void){if(tv){lv_obj_add_flag(tv,LV_OBJ_FLAG_SCROLLABLE);lv_tileview_set_tile(tv,home,LV_ANIM_ON);}}
void muse_ui_set_swipe_enabled(bool enabled){if(tv)lv_obj_set_flag(tv,LV_OBJ_FLAG_SCROLLABLE,enabled);}
void muse_ui_preview_brightness(int pct){preview=pct;muse_board->set_brightness(pct);brightness=pct;}
bool muse_ui_image_size(int *w,int *h){(void)w;(void)h;return false;}
bool muse_ui_image_draw(int x,int y,int w,int h,const uint16_t *p){(void)x;(void)y;(void)w;(void)h;(void)p;return false;}
void muse_ui_image_hide(void){}
void muse_ui_camera_hint(bool visible){(void)visible;}
void muse_ui_request_snapshot(void){(void)ready;}
