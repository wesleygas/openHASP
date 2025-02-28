/* MIT License - Copyright (c) 2019-2021 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#include "hasplib.h"
#include "dev/device.h"
// #include "lv_datetime.h"

#ifdef ARDUINO
#include "ArduinoLog.h"
#endif

#if defined(WINDOWS) || defined(POSIX)
#include <iostream>
#include <fstream>
#include <sstream>
#endif

#if HASP_USE_EEPROM > 0
#include "StreamUtils.h" // For EEPromStream
#endif

#if HASP_USE_DEBUG > 0
#include "../hasp_debug.h"
#endif

#if HASP_USE_CONFIG > 0
#include "lv_fs_if.h"
#include "hasp_gui.h"
#include "hasp_config.h"
#include "font/hasp_font_loader.h"
//#include "hasp_filesystem.h" included in hasp_conf.h
#endif

#if HASP_USE_EEPROM > 0
#include "EEPROM.h"
#endif

//#if LV_USE_HASP

/*********************
 *      DEFINES
 *********************/
#define PAGE_START_INDEX 1 // Page number of array index 0

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
// void hasp_background(uint16_t pageid, uint16_t imageid);

/**********************
 *  STATIC VARIABLES
 **********************/
#if LV_USE_ANIMATION
// static void kb_hide_anim_end(lv_anim_t * a);
#endif

#if LV_DEMO_WALLPAPER
LV_IMG_DECLARE(img_bubble_pattern)
#endif

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t hasp_sleep_state       = HASP_SLEEP_OFF; // Used in hasp_drv_touch.cpp
static uint16_t sleepTimeShort = 60;             // 1 second resolution
static uint16_t sleepTimeLong  = 120;            // 1 second resolution

uint8_t haspStartDim   = 255;
uint8_t haspStartPage  = 1;
uint8_t haspThemeId    = 2;
uint16_t haspThemeHue  = 200;
char haspPagesPath[32] = "/pages.jsonl";
char haspZiFontPath[32];

lv_style_t style_mbox_bg; /*Black bg. style with opacity*/
lv_obj_t* kb;
// lv_font_t * defaultFont;

static lv_font_t* haspFonts[12] = {nullptr};
uint8_t current_page            = 1;

/**
 * Get Font ID
 */
lv_font_t* hasp_get_font(uint8_t fontid)
{
    if(fontid >= sizeof(haspFonts) / sizeof(haspFonts[0])) {
        return nullptr;
    } else {
        return haspFonts[fontid];
    }
}

/**
 * Check if sleep state needs to be updated
 */
HASP_ATTRIBUTE_FAST_MEM void hasp_update_sleep_state()
{
    uint32_t idle = lv_disp_get_inactive_time(lv_disp_get_default()) / 1000;

    if(sleepTimeLong > 0 && idle >= (sleepTimeShort + sleepTimeLong)) {
        if(hasp_sleep_state != HASP_SLEEP_LONG) {
            hasp_sleep_state = HASP_SLEEP_LONG;
            dispatch_idle(NULL, NULL, TAG_HASP);
        }
    } else if(sleepTimeShort > 0 && idle >= sleepTimeShort) {
        if(hasp_sleep_state != HASP_SLEEP_SHORT) {
            hasp_sleep_state = HASP_SLEEP_SHORT;
            dispatch_idle(NULL, NULL, TAG_HASP);
        }
    } else {
        if(hasp_sleep_state != HASP_SLEEP_OFF) {
            hasp_sleep_state = HASP_SLEEP_OFF;
            dispatch_idle(NULL, NULL, TAG_HASP);
        }
    }

    //  return (hasp_sleep_state != HASP_SLEEP_OFF);
}

void hasp_set_sleep_state(uint8_t state)
{
    hasp_sleep_state = state;
}

void hasp_get_sleep_state(char* payload)
{
    switch(hasp_sleep_state) {
        case HASP_SLEEP_LONG:
            memcpy_P(payload, PSTR("long"), 5);
            break;
        case HASP_SLEEP_SHORT:
            memcpy_P(payload, PSTR("short"), 6);
            break;
        default:
            memcpy_P(payload, PSTR("off"), 4);
    }
}

/**
 * Anti Burn-in protection
 */
static lv_task_t* anti_burnin_task;

void hasp_stop_anti_burn(lv_obj_t* layer)
{
    anti_burnin_task = NULL;
    lv_obj_set_style_local_bg_opa(layer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_TRANSP);
    hasp_set_wakeup_touch(haspDevice.get_backlight_power() == false); // enabled if backlight is OFF
    LOG_INFO(TAG_HASP, F("Antiburn disabled"));
}

void hasp_anti_burnin_cb(lv_task_t* task)
{
    lv_obj_t* layer = lv_disp_get_layer_sys(NULL);
    if(!layer) return;

    lv_color_t color[5] = {LV_COLOR_BLACK, LV_COLOR_WHITE, LV_COLOR_RED, LV_COLOR_LIME, LV_COLOR_BLUE};
    lv_obj_set_style_local_bg_color(layer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, color[task->repeat_count % 5]);
    lv_obj_set_style_local_bg_opa(layer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);

    // task is about to get deleted
    if(task->repeat_count == 1) hasp_stop_anti_burn(layer);
}

/**
 * Enable/Disable Anti Burn-in protection
 */
void hasp_set_anti_burn(int32_t repeat_count, uint32_t period)
{
    lv_obj_t* layer = lv_disp_get_layer_sys(NULL);
    if(!layer) return;

    if(repeat_count != 0) {
        anti_burnin_task = lv_task_create(hasp_anti_burnin_cb, period, LV_TASK_PRIO_LOWEST, NULL);
        if(anti_burnin_task) {
            // hasp_set_wakeup_touch(true);
            lv_obj_set_event_cb(layer, first_touch_event_handler);
            lv_obj_set_click(layer, true);
            lv_task_set_repeat_count(anti_burnin_task, repeat_count);
            LOG_INFO(TAG_HASP, F("Antiburn %s"), D_SETTING_ENABLED);
        } else {
            LOG_INFO(TAG_HASP, F("Antiburn %s"), D_INFO_FAILED);
        }
    } else {
        if(anti_burnin_task) {
            lv_task_del(anti_burnin_task);
            hasp_stop_anti_burn(layer);
        }
    }
}

/**
 * Enable/Disable Wake-up Touch
 */
void hasp_set_wakeup_touch(bool en)
{
    lv_obj_t* layer = lv_disp_get_layer_sys(NULL);
    if(!layer) return;

    if(lv_obj_get_click(layer) != en) {
        lv_obj_set_event_cb(layer, first_touch_event_handler);
        lv_obj_set_click(layer, en);
        LOG_INFO(TAG_HASP, F("First touch %s"), en ? D_SETTING_ENABLED : D_SETTING_DISABLED);
    }
}

/**
 * Return the sleep times
 */
void hasp_get_sleep_time(uint16_t& short_time, uint16_t& long_time)
{
    short_time = sleepTimeShort;
    long_time  = sleepTimeLong;
}

/**
 * Set the sleep times
 */
void hasp_set_sleep_time(uint16_t short_time, uint16_t long_time)
{
    sleepTimeShort = short_time;
    sleepTimeLong  = long_time;
}

/**
 * Checks if we went to sleep, wake up is handled in the event handlers
 */
void haspEverySecond()
{
    hasp_update_sleep_state();
    dispatchEverySecond();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void haspDisconnect()
{

    /* Create a dark plain style for a message box's background (modal)*/
    // lv_style_copy(&style_mbox_bg, &lv_style_plain);
    // style_mbox_bg.body.main_color = LV_COLOR_BLACK;
    // style_mbox_bg.body.grad_color = LV_COLOR_BLACK;
    // style_mbox_bg.body.opa        = LV_OPA_60;

    // lv_obj_set_style(lv_disp_get_layer_sys(NULL), &style_mbox_bg);

    /*
        lv_obj_set_click(lv_disp_get_layer_sys(NULL), true);
        lv_obj_set_event_cb(lv_disp_get_layer_sys(NULL), NULL);
        lv_obj_set_user_data(lv_disp_get_layer_sys(NULL), 255);
        */

    /*
        lv_obj_t * obj = lv_obj_get_child(lv_disp_get_layer_sys(NULL), NULL);
        lv_obj_set_hidden(obj, false);
        obj = lv_obj_get_child(lv_disp_get_layer_sys(NULL), obj);
        lv_obj_set_hidden(obj, false);*/
}

void haspReconnect()
{
    /*Revert the top layer to not block*/
    // lv_obj_set_style(lv_disp_get_layer_sys(NULL), &lv_style_transp);
    // lv_obj_set_click(lv_disp_get_layer_sys(NULL), false);
    // lv_obj_set_event_cb(lv_disp_get_layer_sys(NULL), btn_event_handler);
    /*
        lv_obj_t * obj = lv_obj_get_child(lv_disp_get_layer_sys(NULL), NULL);
        lv_obj_set_hidden(obj, true);
        obj = lv_obj_get_child(lv_disp_get_layer_sys(NULL), obj);
        lv_obj_set_hidden(obj, true);*/
}

// String progress_str((char *)0);

// Shows/hides the the global progress bar and updates the value
void haspProgressVal(uint8_t val)
{
    lv_obj_t* layer = lv_disp_get_layer_sys(NULL);
    lv_obj_t* bar   = hasp_find_obj_from_page_id(255U, 10U);
    if(layer && bar) {
        if(val == 255) {
            if(!lv_obj_get_hidden(bar)) {
                lv_obj_set_hidden(bar, true);
                lv_obj_set_style_local_bg_opa(layer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_0);

                // lv_obj_set_style_local_value_str(bar, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, "");
                // lv_obj_set_value_str_txt(bar, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, NULL);
                // TODO: call our custom function to free the memory

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
                // progress_str.clear();
#endif
            }
        } else {
            if(lv_obj_get_hidden(bar)) {
                lv_obj_set_hidden(bar, false);
                lv_obj_set_style_local_bg_opa(layer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_100);
            }
            lv_bar_set_value(bar, val, LV_ANIM_OFF);
        }
        lv_task_handler(); // needed to let the GUI do its work during long updates
    }
}

// Sets the value string of the global progress bar
void haspProgressMsg(const char* msg)
{
    if(lv_obj_t* bar = hasp_find_obj_from_page_id(255U, 10U)) {
        char value_str[10];
        snprintf_P(value_str, sizeof(value_str), PSTR("value_str"));
        hasp_process_obj_attribute(bar, value_str, msg, true);
    }

    lv_task_handler(); // needed to let the GUI do its work during long updates

    /* if(bar) {
         progress_str.reserve(64);
         progress_str = msg;
         lv_obj_set_style_local_value_str(bar, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, progress_str.c_str());

        // lv_task_handler(); // let the GUI do its work
     } */
}

#ifdef ARDUINO
// Sets the value string of the global progress bar
void haspProgressMsg(const __FlashStringHelper* msg)
{
    haspProgressMsg(String(msg).c_str());
}
#endif

/*Add a custom apply callback*/
static void custom_font_apply_cb(lv_theme_t* th, lv_obj_t* obj, lv_theme_style_t name)
{
    /*    lv_style_list_t* list;

        switch(name) {
            case LV_THEME_BTN:
                list = lv_obj_get_style_list(obj, LV_BTN_PART_MAIN);
                // _lv_style_list_add_style(list, &my_style);
                break;
            default:
                // nothing
                ;
        } */
}

void hasp_set_theme(uint8_t themeid)
{
    lv_theme_t* th             = NULL;
    lv_color_t color_primary   = lv_color_hsv_to_rgb(haspThemeHue, 100, 100);
    lv_color_t color_secondary = lv_color_hsv_to_rgb(haspThemeHue, 100, 100);

    /* ********** Theme Initializations ********** */
    if(themeid == 8) themeid = 1;          // update old HASP id
    if(themeid == 9) themeid = 5;          // update old material id
    if(themeid < 0 || themeid > 5) return; // check bounds

#if(LV_USE_THEME_HASP == 1)
    uint32_t hasp_flags = LV_THEME_HASP_FLAG_LIGHT;
#endif

#if(LV_USE_THEME_MATERIAL == 1)
    // LV_THEME_MATERIAL_FLAG_NO_TRANSITION : disable transitions(state change animations)
    // LV_THEME_MATERIAL_FLAG_NO_FOCUS: disable indication of focused state)
    uint32_t material_flags = LV_THEME_MATERIAL_FLAG_LIGHT + LV_THEME_MATERIAL_FLAG_NO_TRANSITION;
#endif

    switch(themeid) {
#if(LV_USE_THEME_EMPTY == 1)
        case 0:
            th = lv_theme_empty_init(lv_color_hsv_to_rgb(haspThemeHue, 100, 100),
                                     lv_color_hsv_to_rgb(haspThemeHue, 100, 100), LV_THEME_DEFAULT_FLAGS, haspFonts[0],
                                     haspFonts[1], haspFonts[2], haspFonts[3]);
            break;
#endif

#if(LV_USE_THEME_HASP == 1)
        case 2: // Dark
            hasp_flags = LV_THEME_HASP_FLAG_DARK;
        case 1: // Light
        case 8: // Light (old id)
            th = lv_theme_hasp_init(color_primary, color_secondary, hasp_flags + LV_THEME_HASP_FLAG_NO_FOCUS,
                                    haspFonts[0], haspFonts[1], haspFonts[2], haspFonts[3]);
            break;
#endif

#if(LV_USE_THEME_MONO == 1)
        case 3:
            th = lv_theme_mono_init(color_primary, color_secondary, LV_THEME_DEFAULT_FLAGS, haspFonts[0], haspFonts[1],
                                    haspFonts[2], haspFonts[3]);
            break;
#endif

#if LV_USE_THEME_MATERIAL == 1
        case 5: // Dark
            material_flags = LV_THEME_MATERIAL_FLAG_DARK + LV_THEME_MATERIAL_FLAG_NO_TRANSITION;
        case 4: // Light
        case 9: // Light (old id)
            th =
                lv_theme_material_init(color_primary, color_secondary, material_flags + LV_THEME_MATERIAL_FLAG_NO_FOCUS,
                                       haspFonts[0], haspFonts[1], haspFonts[2], haspFonts[3]);
            break;
#endif

#if LV_USE_THEME_TEMPLATE == 1
        case 7:
            th = lv_theme_template_init(LV_COLOR_PURPLE, LV_COLOR_ORANGE, LV_THEME_DEFAULT_FLAGS, haspFonts[0],
                                        haspFonts[1], haspFonts[2], haspFonts[3]);
            break;
#endif

        default:

            LOG_ERROR(TAG_HASP, F("Unknown theme selected"));
    }

    if(th) {
        lv_theme_set_act(th);
        LOG_INFO(TAG_HASP, F("Custom theme loaded"));
    } else {
        LOG_ERROR(TAG_HASP, F("Theme could not be loaded"));
    }
}

/**
 * Create a demo application
 */
void haspSetup(void)
{
    haspDevice.set_backlight_level(haspStartDim);

    /******* File System Test ********************************************************************/
    // lv_fs_file_t f;
    // lv_fs_res_t res;
    // res = lv_fs_open(&f, "L:/config.json", LV_FS_MODE_RD);
    // if(res == LV_FS_RES_OK)
    //     LOG_VERBOSE(TAG_HASP, F("Opening config.json OK"));
    // else
    //     LOG_ERROR(TAG_HASP, F("Opening config.json from FS failed %d"), res);

    // uint32_t btoread = 128;
    // uint32_t bread   = 0;
    // char buffer[129];

    // res = lv_fs_read(&f, buffer, btoread, &bread);
    // if(res == LV_FS_RES_OK) {
    //     LOG_VERBOSE(TAG_HASP, F("Reading config.json OK %u"), bread);
    //     buffer[127] = '\0';
    //     LOG_INFO(TAG_HASP, buffer);
    // } else
    //     LOG_ERROR(TAG_HASP, F("Reading config.json from FS failed %d"), res);

    // res = lv_fs_close(&f);
    // if(res == LV_FS_RES_OK)
    //     LOG_VERBOSE(TAG_HASP, F("Closing config.json OK"));
    // else
    //     LOG_ERROR(TAG_HASP, F("Closing config.json on FS failed %d"), res);
    /******* File System Test ********************************************************************/

    /* ********** Font Initializations ********** */

    LOG_WARNING(TAG_ATTR, "%s %d %x", __FILE__, __LINE__, nullptr);
    LOG_WARNING(TAG_ATTR, "%s %d %x", __FILE__, __LINE__, haspFonts[0]);
    // LOG_WARNING(TAG_ATTR, "%s %d %x", __FILE__, __LINE__, &robotocondensed_regular_16);

#if HASP_USE_SPIFFS > 0 || HASP_USE_LITTLEFS > 0
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)

    lv_font_t* hasp_font = nullptr; // required or font init will crash
    lv_zifont_init();

    // WARNING: hasp_font needs to be null !
    if(lv_zifont_font_init(&hasp_font, haspZiFontPath, 32) != 0) {
        if(strlen(haspZiFontPath) > 0) LOG_WARNING(TAG_HASP, F("Failed to set font to %s"), haspZiFontPath);
        haspFonts[0] = LV_THEME_DEFAULT_FONT_SMALL;
    } else {
        // defaultFont = haspFonts[0];
        haspFonts[0] = hasp_font; // save it
    }

    //  LOG_WARNING(TAG_ATTR, "%s %d %x", __FILE__, __LINE__, robotocondensed_regular_16);
    LOG_WARNING(TAG_ATTR, "%s %d %x", __FILE__, __LINE__, *hasp_font);

#endif
#endif

    if(haspFonts[0] == nullptr) haspFonts[0] = LV_THEME_DEFAULT_FONT_SMALL;
    if(haspFonts[1] == nullptr) haspFonts[1] = LV_THEME_DEFAULT_FONT_NORMAL;
    if(haspFonts[2] == nullptr) haspFonts[2] = LV_THEME_DEFAULT_FONT_SUBTITLE;
    if(haspFonts[3] == nullptr) haspFonts[3] = LV_THEME_DEFAULT_FONT_TITLE;

    // haspFonts[0] = lv_font_load("E:/font_1.fnt");
    // haspFonts[2] = lv_font_load("E:/font_2.fnt");

    // haspFonts[3] = hasp_font_load("L:/robotocondensed_60_latin1.bin");

    hasp_set_theme(haspThemeId);

    /* Create all screens using the theme */

#if HASP_USE_WIFI > 0
    if(!wifiShowAP()) {
        haspDisconnect();
    }
#endif

    hasp_init();
    hasp_load_json();
    haspPages.set(haspStartPage, LV_SCR_LOAD_ANIM_FADE_ON);

    // lv_obj_t* obj        = lv_datetime_create(haspPages.get_obj(haspPages.get()), NULL);
    // obj->user_data.objid = LV_HASP_DATETIME;
    // obj->user_data.id    = 199;
    // lv_datetime_set_text_fmt(obj,"%A, %B %d");
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

IRAM_ATTR void haspLoop(void)
{
    dispatchLoop();
}

void hasp_init(void)
{
    haspPages.init(haspStartPage);
}

void hasp_load_json(void)
{
    haspPages.load_jsonl(haspPagesPath);
}

/*
void hasp_background(uint16_t pageid, uint16_t imageid)
{
    lv_obj_t * page = get_page_obj(pageid);
    if(!page) return;

    return;

    page               = lv_scr_act();
    lv_obj_t * thisobj = lv_obj_get_child_back(page, NULL);

    if(!thisobj) return;

    switch (imageid)
    {
    case 0:
        lv_img_set_src(thisobj, &frame00);
        break;
    case 1:
        lv_img_set_src(thisobj, &frame02);
        break;
    case 2:
        lv_img_set_src(thisobj, &frame04);
        break;
    case 3:
        lv_img_set_src(thisobj, &frame06);
        break;
    case 4:
        lv_img_set_src(thisobj, &frame08);
        break;
    case 5:
        lv_img_set_src(thisobj, &frame10);
        break;
    case 6:
        lv_img_set_src(thisobj, &frame12);
        break;
    case 7:
        lv_img_set_src(thisobj, &frame14);
        break;
    }
    //printf("Image set to %u\n", imageid);

    lv_img_set_auto_size(thisobj, false);
    lv_obj_set_width(thisobj, lv_disp_get_hor_res(NULL));
    lv_obj_set_height(thisobj, lv_disp_get_ver_res(NULL));
    // lv_obj_set_protect(wp, LV_PROTECT_POS);
    // lv_obj_invalidate(thisobj);
}*/

///////////////////////////////////////////////////////////////////////////////////////////////////////////

// void haspGetVersion(char* version, size_t len)
// {
//     snprintf_P(version, len, PSTR("%u.%u.%u"), HASP_VER_MAJ, HASP_VER_MIN, HASP_VER_REV);
// }

void haspClearPage(uint16_t pageid)
{
    lv_obj_t* page = haspPages.get_obj(pageid);
    if(!page || (pageid > HASP_NUM_PAGES)) {
        LOG_WARNING(TAG_HASP, F(D_HASP_INVALID_PAGE), pageid);
    } else if(page == lv_layer_sys() /*|| page == lv_layer_top()*/) {
        LOG_WARNING(TAG_HASP, F(D_HASP_INVALID_LAYER));
    } else {
        LOG_TRACE(TAG_HASP, F(D_HASP_CLEAR_PAGE), pageid);
        lv_obj_clean(page);
    }
}

uint8_t haspGetPage()
{
    return current_page;
}

void haspSetPage(uint8_t pageid)
{
    lv_obj_t* page = haspPages.get_obj(pageid);
    if(!page || pageid == 0 || pageid > HASP_NUM_PAGES) {
        LOG_WARNING(TAG_HASP, F(D_HASP_INVALID_PAGE), pageid);
    } else {
        LOG_TRACE(TAG_HASP, F(D_HASP_CHANGE_PAGE), pageid);
        current_page = pageid;
        lv_scr_load(page);
        hasp_object_tree(page, pageid, 0);
    }
}

void hasp_get_info(JsonDocument& doc)
{
    std::string buffer;
    buffer.reserve(64);
    char size_buf[32];
    JsonObject info = doc.createNestedObject(F(D_MANUFACTURER));

    info[F(D_INFO_VERSION)] = haspDevice.get_version();

    buffer = __DATE__;
    buffer += (" ");
    buffer += __TIME__;
    buffer += (" UTC"); // Github buildservers are in UTC
    info[F(D_INFO_BUILD_DATETIME)] = buffer;

    unsigned long time = millis() / 1000;
    uint16_t day       = time / 86400;
    time               = time % 86400;
    uint8_t hour       = time / 3600;
    time               = time % 3600;
    uint8_t min        = time / 60;
    time               = time % 60;
    uint8_t sec        = time;

    buffer.clear();
    if(day > 0) {
        itoa(day, size_buf, DEC);
        buffer += size_buf;
        buffer += "d ";
    }
    if(day > 0 || hour > 0) {
        itoa(hour, size_buf, DEC);
        buffer += size_buf;
        buffer += "h ";
    }
    if(day > 0 || hour > 0 || min > 0) {
        itoa(min, size_buf, DEC);
        buffer += size_buf;
        buffer += "m ";
    }
    itoa(sec, size_buf, DEC);
    buffer += size_buf;
    buffer += "s";
    info[F(D_INFO_UPTIME)] = buffer;

    info = doc.createNestedObject(F(D_INFO_DEVICE_MEMORY));
    Parser::format_bytes(haspDevice.get_free_heap(), size_buf, sizeof(size_buf));
    info[F(D_INFO_FREE_HEAP)] = size_buf;
    Parser::format_bytes(haspDevice.get_free_max_block(), size_buf, sizeof(size_buf));
    info[F(D_INFO_FREE_BLOCK)]    = size_buf;
    info[F(D_INFO_FRAGMENTATION)] = haspDevice.get_heap_fragmentation();

#if ARDUINO_ARCH_ESP32
    if(psramFound()) {
        Parser::format_bytes(ESP.getFreePsram(), size_buf, sizeof(size_buf));
        info[F(D_INFO_PSRAM_FREE)] = size_buf;
        Parser::format_bytes(ESP.getPsramSize(), size_buf, sizeof(size_buf));
        info[F(D_INFO_PSRAM_SIZE)] = size_buf;
    }
#endif

    info = doc.createNestedObject(F(D_INFO_LVGL_MEMORY));
    lv_mem_monitor_t mem_mon;
    lv_mem_monitor(&mem_mon);
    Parser::format_bytes(mem_mon.total_size, size_buf, sizeof(size_buf));
    info[F(D_INFO_TOTAL_MEMORY)] = size_buf;
    Parser::format_bytes(mem_mon.free_size, size_buf, sizeof(size_buf));
    info[F(D_INFO_FREE_MEMORY)]   = size_buf;
    info[F(D_INFO_FRAGMENTATION)] = mem_mon.frag_pct;

    info = doc.createNestedObject(F("HASP State"));
    hasp_get_sleep_state(size_buf);
    info[F("Idle")]        = size_buf;
    info[F("Active Page")] = haspPages.get();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if HASP_USE_CONFIG > 0
bool haspGetConfig(const JsonObject& settings)
{
    bool changed = false;

    if(haspStartPage != settings[FPSTR(FP_CONFIG_STARTPAGE)].as<uint8_t>()) changed = true;
    settings[FPSTR(FP_CONFIG_STARTPAGE)] = haspStartPage;

    if(haspStartDim != settings[FPSTR(FP_CONFIG_STARTDIM)].as<uint8_t>()) changed = true;
    settings[FPSTR(FP_CONFIG_STARTDIM)] = haspStartDim;

    if(haspThemeId != settings[FPSTR(FP_CONFIG_THEME)].as<uint8_t>()) changed = true;
    settings[FPSTR(FP_CONFIG_THEME)] = haspThemeId;

    if(haspThemeHue != settings[FPSTR(FP_CONFIG_HUE)].as<uint16_t>()) changed = true;
    settings[FPSTR(FP_CONFIG_HUE)] = haspThemeHue;

    if(strcmp(haspZiFontPath, settings[FPSTR(FP_CONFIG_ZIFONT)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_ZIFONT)] = haspZiFontPath;

    if(strcmp(haspPagesPath, settings[FPSTR(FP_CONFIG_PAGES)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_PAGES)] = haspPagesPath;

    if(changed) configOutput(settings, TAG_HASP);
    return changed;
}

/** Set HASP Configuration.
 *
 * Read the settings from json and sets the application variables.
 *
 * @note: data pixel should be formated to uint32_t RGBA. Imagemagick requirements.
 *
 * @param[in] settings    JsonObject with the config settings.
 **/
bool haspSetConfig(const JsonObject& settings)
{
    configOutput(settings, TAG_HASP);
    bool changed = false;

    changed |= configSet(haspStartPage, settings[FPSTR(FP_CONFIG_STARTPAGE)], F("haspStartPage"));
    changed |= configSet(haspStartDim, settings[FPSTR(FP_CONFIG_STARTDIM)], F("haspStartDim"));
    changed |= configSet(haspThemeId, settings[FPSTR(FP_CONFIG_THEME)], F("haspThemeId"));
    changed |= configSet(haspThemeHue, settings[FPSTR(FP_CONFIG_HUE)], F("haspThemeHue"));

    if(haspStartPage == 0) { // TODO: fase out migration code
        haspStartPage = 1;
        changed       = true;
    }

    if(!settings[FPSTR(FP_CONFIG_PAGES)].isNull()) {
        changed |= strcmp(haspPagesPath, settings[FPSTR(FP_CONFIG_PAGES)]) != 0;
        strncpy(haspPagesPath, settings[FPSTR(FP_CONFIG_PAGES)], sizeof(haspPagesPath));
    }

    if(!settings[FPSTR(FP_CONFIG_ZIFONT)].isNull()) {
        changed |= strcmp(haspZiFontPath, settings[FPSTR(FP_CONFIG_ZIFONT)]) != 0;
        strncpy(haspZiFontPath, settings[FPSTR(FP_CONFIG_ZIFONT)], sizeof(haspZiFontPath));
    }

    return changed;
}
#endif // HASP_USE_CONFIG
