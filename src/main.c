/*
 * 墨印 MoInk · BLE 版固件主装配
 *
 * 职责：热点没了，整机只做「BLE 收图 -> 刷屏 -> 休眠 / 唤醒 / 升级」。
 * 图像合成、排版、风格全部在 APP 侧完成，固件只收已打包的 2bpp 位图。
 *
 * 任务划分：
 *   host_task(NimBLE)   GATT 写回调 -> ble_api 状态机（收帧 / OTA / 命令）
 *   display_task        串行执行刷屏 / 残影清理 / 缓存重刷（唯一的 SPI 使用者）
 *   button_task         消抖 + 单击/双击/长按 20s 恢复出厂
 *   idle_task           空闲超时 -> 深睡或关机、低电量提醒
 *
 * 自锁：GPIO2 上电第一件事拉高并通过 pad hold 维持，深睡前锁存；
 *       唤醒后由 power_self_lock_init() 释放 hold（否则关机永远拉不低）。
 */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_idf_version.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 2, 2)
#error "MoInk APP firmware requires ESP-IDF 5.2.2"
#endif
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "version.h"
#include "epd_drv.h"
#include "settings.h"
#include "power.h"
#include "cache.h"
#include "store.h"
#include "ble_api.h"
#include "crc_util.h"

static const char *TAG = "main";

#define HOLD_RESET_S      20         /* 长按恢复出厂 */
#define BTN_SAMPLE_MS     20
#define BTN_DEBOUNCE_N    3          /* 3 x 20ms = 60ms 消抖 */
#define BTN_DOUBLE_MS     400
#define BTN_WAKE_ARM_MS   1500
#define BTN_SHORT_MAX_MS  1200       /* 超过这个时长算长按（但不恢复出厂） */
#define LOW_BATTERY_MV    3400
#define BAT_CHECK_MS      300000     /* 5 分钟查一次电量 */

/* 本次启动的唤醒来源 */
typedef enum {
    WAKE_SRC_POWER_ON = 0,
    WAKE_SRC_BUTTON   = 1,
    WAKE_SRC_TIMER    = 2,
    WAKE_SRC_OTHER    = 3,
} wake_src_t;

static wake_src_t s_wake_src = WAKE_SRC_POWER_ON;
static volatile bool s_ota_pending;
static bool s_factory_pending;

static const char *wake_src_name(wake_src_t s)
{
    switch (s) {
    case WAKE_SRC_BUTTON: return "button";
    case WAKE_SRC_TIMER:  return "timer";
    case WAKE_SRC_OTHER:  return "other";
    default:              return "power_on";
    }
}

/* 5.2.2 只有 esp_sleep_get_wakeup_cause() 单因接口（位图版是 5.3+ 才有），
   所以这里按「先按键、后定时、其余归其它」判定。 */
static wake_src_t detect_wake_src(void)
{
    if (power_woke_by_button()) return WAKE_SRC_BUTTON;
    if (power_woke_by_timer())  return WAKE_SRC_TIMER;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) return WAKE_SRC_POWER_ON;
    return WAKE_SRC_OTHER;
}

/* ---------------- 显示任务 ---------------- */

static uint8_t storage_status(esp_err_t err)
{
    if (err == ESP_OK) return 0;
    if (err == STORE_ERR_FULL) return 0x0f;
    if (err == ESP_ERR_NOT_FOUND) return 0x0d;
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) return 0x02;
    if (err == ESP_ERR_INVALID_CRC) return 0x08;
    return 0x0b;
}

static void display_task(void *arg)
{
    (void)arg;
    ble_job_t job;
    for (;;) {
        if (!ble_api_job_wait(&job, 1000)) continue;
        power_set_busy(true);
        uint8_t result = 1;
        uint8_t *buf = ble_api_frame_buf();
        esp_err_t store_err = ESP_OK;
        bool store_event = false;
        bool display_event = true;
        int slot = -1;

        if (job.type == BLE_JOB_FRAME) {
            if (job.save_store) {
                store_event = true;
                store_err = store_save_frame(buf, job.w, job.h,
                    crc16_of(buf, (uint32_t)(job.w / 4) * job.h),
                    ble_api_thumb_buf(), job.thumb_len, job.thumb_format,
                    job.thumb_w, job.thumb_h, &slot);
            }
            display_event = !job.no_show;
            if (!job.no_show && store_err == ESP_OK &&
                epd_display_2bpp_wh(buf, job.w, job.h) == 0) {
                epd_panel_deep_sleep();
                result = 0;
                if (job.save_store) store_err = store_set_current(slot);
                else if (store_slots() && store_set_current(-1) != ESP_OK) {
                    result = 1;
                    ESP_LOGE(TAG, "screen changed but gallery current could not be cleared");
                }
                if (job.save_cache && settings_get()->cache_on &&
                    cache_save(buf, job.w, job.h,
                        crc16_of(buf, (uint32_t)(job.w / 4) * job.h)) != ESP_OK)
                    ESP_LOGW(TAG, "cache save failed after display");
            }
        } else if (job.type == BLE_JOB_STORE_SHOW) {
            uint16_t w = 0, h = 0;
            slot = job.slot;
            if (slot == 255) {
                int count = 0, current = -1;
                store_meta_get(&count, &current);
                slot = count ? (current + 1) % count : -1;
            }
            if (store_load_frame(slot, buf, EPD_MAX_BUF_LEN, &w, &h) == ESP_OK &&
                epd_display_2bpp_wh(buf, w, h) == 0) {
                epd_panel_deep_sleep();
                if (store_set_current(slot) == ESP_OK) result = 4;
                if (settings_get()->cache_on &&
                    cache_save(buf, w, h, crc16_of(buf, (uint32_t)(w / 4) * h)) != ESP_OK)
                    ESP_LOGW(TAG, "gallery cache save failed");
            }
        } else if (job.type == BLE_JOB_STORE_DELETE || job.type == BLE_JOB_STORE_CLEAR) {
            store_event = true;
            display_event = false;
            slot = job.type == BLE_JOB_STORE_DELETE ? job.slot : -1;
            store_err = job.type == BLE_JOB_STORE_DELETE ? store_delete(slot) : store_clear();
        } else if (job.type == BLE_JOB_CACHE) {
            uint16_t w = 0, h = 0, crc = 0;
            if (cache_load(buf, EPD_MAX_BUF_LEN, &w, &h, &crc) == ESP_OK &&
                epd_display_2bpp_wh(buf, w, h) == 0) {
                epd_panel_deep_sleep();
                result = 3;
                if (store_slots() && store_set_current(-1) != ESP_OK) {
                    result = 1;
                    ESP_LOGE(TAG, "screen changed but gallery current could not be cleared");
                }
            }
        } else if (job.type == BLE_JOB_CLEAR) {
            if (epd_clear_cycles(job.cycles) == 0) {
                epd_panel_deep_sleep();
                result = 2;
                if (store_slots() && store_set_current(-1) != ESP_OK) {
                    result = 1;
                    ESP_LOGE(TAG, "screen changed but gallery current could not be cleared");
                }
            }
        }

        int count = 0, current = -1;
        if (store_event) store_meta_get(&count, &current);
        uint8_t payload[3] = {slot < 0 ? 255 : (uint8_t)slot,
                             (uint8_t)count, current < 0 ? 255 : (uint8_t)current};
        /* Snapshot metadata before publishing idle; the next command can start
         * immediately after a completion notification. */
        power_activity();
        power_set_busy(false);
        ble_api_job_complete();
        if (store_event)
            ble_api_notify_event(MOINK_EVT_STORE_DONE, storage_status(store_err), payload, 3);
        if (display_event)
            ble_api_notify_event(MOINK_EVT_DISPLAY_DONE, result, NULL, 0);
    }
}

/* ---------------- 按键 ---------------- */

static void factory_reset_now(void)
{
    if (!ble_api_prepare_sleep()) { s_factory_pending = true; return; }
    s_factory_pending = false;
    ESP_LOGW(TAG, "long press %ds -> factory reset", HOLD_RESET_S);
    ble_api_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    settings_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void button_task(void *arg)
{
    (void)arg;
    const uint8_t pin = settings_pins()->v[PIN_IDX_WAKE];

    int raw = gpio_get_level(pin);
    int stable = raw;
    int same = BTN_DEBOUNCE_N;
    bool pressed = (stable == 0);

    TickType_t press_tick = pressed ? xTaskGetTickCount() : 0;
    TickType_t prev_release = 0;
    bool long_done = false, warn_done = false;
    bool pending_next = false;
    /* A wake click may have ended before this task started. */
    bool wake_armed = s_wake_src == WAKE_SRC_BUTTON && !pressed;
    TickType_t wake_start = xTaskGetTickCount();

    for (;;) {
        if (s_factory_pending && ble_api_state() == BLE_ST_IDLE) factory_reset_now();
        if (pending_next && ble_api_state() == BLE_ST_IDLE) {
            int count = 0;
            store_meta_get(&count, NULL);
            if (!count || ble_api_request_carousel_show(255)) pending_next = false;
        }
        if (wake_armed && (xTaskGetTickCount() - wake_start) > pdMS_TO_TICKS(BTN_WAKE_ARM_MS))
            wake_armed = false;
        int lvl = gpio_get_level(pin);
        if (lvl != raw) {
            raw = lvl;
            same = 0;
        } else if (same < BTN_DEBOUNCE_N) {
            same++;
        }

        if (same == BTN_DEBOUNCE_N && raw != stable) {
            stable = raw;
            if (stable == 0) {
                /* 按下 */
                press_tick = xTaskGetTickCount();
                long_done = false;
                warn_done = false;
                pressed = true;
                power_activity();
            } else {
                /* 松开 */
                pressed = false;
                uint32_t held_ms = (uint32_t)((xTaskGetTickCount() - press_tick) *
                                              portTICK_PERIOD_MS);
                if (!long_done && held_ms < BTN_SHORT_MAX_MS) {
                    TickType_t now = xTaskGetTickCount();
                    if (wake_armed || (prev_release != 0 &&
                        (uint32_t)((now - prev_release) * portTICK_PERIOD_MS) <= BTN_DOUBLE_MS)) {
                        wake_armed = false;
                        /* 双击：设备图库切下一张 */
                        prev_release = 0;
                        ESP_LOGI(TAG, "button: double click");
                        ble_api_notify_event(MOINK_EVT_BUTTON, 2, NULL, 0);
                        if (!ble_api_request_carousel_show(255)) {
                            int count = 0;
                            store_meta_get(&count, NULL);
                            pending_next = count > 0;
                            ESP_LOGI(TAG, "gallery next %s", pending_next ? "deferred" : "empty");
                        }
                    } else {
                        prev_release = now;
                        ESP_LOGI(TAG, "button: click (activity only)");
                        ble_api_notify_event(MOINK_EVT_BUTTON, 1, NULL, 0);
                    }
                }
                power_activity();
            }
        }

        if (pressed) {
            uint32_t held_ms = (uint32_t)((xTaskGetTickCount() - press_tick) *
                                          portTICK_PERIOD_MS);
            /* 长按期间持续续命，否则空闲休眠会先把设备睡掉、20s 长按走不完。 */
            power_activity();
            if (!warn_done && held_ms >= (uint32_t)(HOLD_RESET_S - 3) * 1000) {
                warn_done = true;
                ESP_LOGW(TAG, "button: long press, factory reset in 3s");
                ble_api_notify_event(MOINK_EVT_BUTTON, 3, NULL, 0);
            }
            if (!long_done && held_ms >= (uint32_t)HOLD_RESET_S * 1000) {
                long_done = true;
                factory_reset_now();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_SAMPLE_MS));
    }
}

/* ---------------- 空闲 / 电源 ---------------- */

static void idle_task(void *arg)
{
    (void)arg;
    int64_t last_bat = 0;
    bool low_battery_notified = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_bat > BAT_CHECK_MS) {
            last_bat = now;
            int mv = power_battery_mv();
            if (!low_battery_notified && mv > 0 && mv < LOW_BATTERY_MV) {
                low_battery_notified = true;
                int16_t v = (int16_t)mv;
                ESP_LOGW(TAG, "low battery: %d mV", mv);
                ble_api_notify_event(MOINK_EVT_LOW_BATTERY, 0, &v, 2);
            }
        }

        if (s_ota_pending || !power_should_sleep()) continue;
        if (!ble_api_prepare_idle_sleep()) continue;

        if (settings_get()->timeout_action == SETT_ACTION_OFF) {
            ESP_LOGW(TAG, "idle timeout -> power off (release self-lock)");
            ble_api_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            power_self_lock_off();      /* 有自锁则不返回；无自锁则退回深睡 */
        }

        ESP_LOGW(TAG, "idle timeout -> deep sleep");
        ble_api_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
        power_enter_deep_sleep();
    }
}

/* ---------------- OTA 确认 ---------------- */

static void ota_confirm_task(void *arg)
{
    (void)arg;
    /* 新固件跑满 45s 才确认；期间断电/复位会被 bootloader 回滚到旧版本。 */
    vTaskDelay(pdMS_TO_TICKS(45000));

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA confirmation failed: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "firmware confirmed (rollback cancelled)");
    }
    s_ota_pending = false;
    vTaskDelete(NULL);
}

/* ---------------- 入口 ---------------- */

void app_main(void)
{
    /* ★ 必须第一行：先钉高自锁脚，再释放上次深睡遗留的 pad hold。
       顺序反了会在复位瞬间让 MOS 失去驱动、电池供电下直接断电。 */
    power_self_lock_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));

    settings_init();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    s_ota_pending = running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
                    ota_state == ESP_OTA_IMG_PENDING_VERIFY;

    s_wake_src = detect_wake_src();
    power_set_auto_wake(s_wake_src == WAKE_SRC_TIMER);

    ESP_LOGI(TAG, "%s (api %d) boot: panel=%s wake=%s cause=%d reset=%d",
             MOINK_VERSION, MOINK_API_VERSION,
             epd_panel_name((epd_panel_t)settings_get()->panel),
             wake_src_name(s_wake_src), (int)esp_sleep_get_wakeup_cause(),
             (int)esp_reset_reason());

    if (epd_set_panel((epd_panel_t)settings_get()->panel) != 0)
        ESP_LOGE(TAG, "bad panel id %u", settings_get()->panel);

    /* 屏初始化失败也继续跑：让 APP 仍能连上来改引脚设置，而不是直接失联。 */
    if (epd_init() != 0)
        ESP_LOGE(TAG, "EPD init failed, check GPIO settings via BLE");

    if (store_init() != ESP_OK) ESP_LOGW(TAG, "gallery unavailable; verify B2 partition table");
    cache_init();                       /* 分区缺失只告警，不阻塞 */
    power_init();

    ble_api_set_wake_reason((uint8_t)s_wake_src);
    ble_api_start();

    configASSERT(xTaskCreate(button_task, "button", 3072, NULL, 3, NULL) == pdPASS);
    configASSERT(xTaskCreate(display_task, "display", 6144, NULL, 4, NULL) == pdPASS);
    configASSERT(xTaskCreate(idle_task, "idle", 3072, NULL, 2, NULL) == pdPASS);
    configASSERT(xTaskCreate(ota_confirm_task, "ota_ok", 2048, NULL, 1, NULL) == pdPASS);

    if (s_wake_src == WAKE_SRC_TIMER && settings_get()->wake_show)
        ble_api_request_carousel_show(255);

    ESP_LOGI(TAG, "ready: BLE '%s', heap %" PRIu32,
             settings_get()->name[0] ? settings_get()->name : "MoInk-XXXX",
             esp_get_free_heap_size());
}