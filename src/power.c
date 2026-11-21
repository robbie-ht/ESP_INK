#include "power.h"
#include "epd_drv.h"
#include "settings.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define AUTO_WAKE_WINDOW_S 180   /* 定时唤醒后等人的窗口（秒） */

/* 电池分压 1M:1M => Vbat = Vpin * 2.0
   ⚠️ 分压比是板级硬件事实：换板/改电阻必须同步改这里，否则电量整体偏移。
   ⚠️ 1M 高阻源（≈500k 输出阻抗）会让 ADC 偏低：ADC 端建议并 100nF 到地，
      换板后按实测复核 BAT_FULLSCALE_MV。 */
#define BAT_DIV_NUM        200
#define BAT_DIV_DEN        100
#define BAT_ADC_MAX        4095
#define BAT_FULLSCALE_MV   3100   /* 12dB 满量程，按板实测标定 */
#define BAT_MV_MIN         2400
#define BAT_MV_MAX         4600
#define BAT_SAMPLES        12     /* 一次调用内的采样点数（中位数抗坏点） */
#define BAT_SPREAD_MAX     500    /* mV：max-min 超过即判悬空 */

static adc_oneshot_unit_handle_t s_adc;
static int64_t s_last_activity_us;
static portMUX_TYPE s_activity_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_auto_wake = false;
static volatile bool s_busy = false;

static uint8_t s_wake_pin = 5;
static adc_channel_t s_adc_ch = ADC_CHANNEL_0;

/* ---------------- 自锁 ---------------- */

void power_self_lock_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SELF_LOCK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* 先把电平写进输出寄存器，再释放 hold。
       ★ 深睡唤醒后上一次的 pad hold 仍然生效（数字 GPIO 无法在深睡期间保持，
         唤醒后会「恢复」hold 状态），不显式 gpio_hold_dis() 的话：
           - 下面这行 set_level(1) 只是写寄存器，不影响被锁存的 pad；
           - 之后 power_self_lock_off() 拉低同样无效 ⇒ 「关机」永久失效。
         而顺序反过来（先 hold_dis）会在复位输出寄存器为 0 的瞬间让 MOS 断供。 */
    gpio_set_level((gpio_num_t)PIN_SELF_LOCK, 1);
    gpio_hold_dis((gpio_num_t)PIN_SELF_LOCK);
    gpio_deep_sleep_hold_dis();

    ESP_LOGI(TAG, "self-lock GPIO%d high, pad hold released", PIN_SELF_LOCK);
}

/* ---------------- 初始化 ---------------- */

void power_init(void)
{
    const pin_cfg_t *p = settings_pins();
    s_wake_pin = p->v[PIN_IDX_WAKE];
    s_adc_ch   = (adc_channel_t)p->v[PIN_IDX_ADC];

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << s_wake_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    /* 电池 ADC：只用 ADC1（ADC2 与 WiFi 共用），通道随引脚。 */
    adc_oneshot_unit_init_cfg_t u = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_adc_ch, &c));

    ESP_LOGI(TAG, "wake btn GPIO%u, battery ADC GPIO%u (ADC1_CH%d)",
             s_wake_pin, p->v[PIN_IDX_ADC], (int)s_adc_ch);

    power_activity();
}

void power_activity(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_activity_lock);
    s_last_activity_us = now;
    taskEXIT_CRITICAL(&s_activity_lock);
}

void power_set_busy(bool busy)
{
    s_busy = busy;
    if (busy) power_activity();
}

bool power_busy(void) { return s_busy; }

void power_set_auto_wake(bool v) { s_auto_wake = v; }
bool power_auto_wake(void) { return s_auto_wake; }

bool power_woke_by_button(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}

bool power_woke_by_timer(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

bool power_should_sleep(void)
{
    /* ★ 刷屏 / 残影清理 / OTA 期间绝不睡：A1 单次刷新最长 180s，
       清理 4 轮可超 60s，被中途打断会留下半刷新的面板。 */
    if (s_busy) return false;

    const moink_settings_t *s = settings_get();
    uint32_t limit = s->sleep_s;

    if (s_auto_wake) {
        if (limit == 0 || limit > AUTO_WAKE_WINDOW_S) limit = AUTO_WAKE_WINDOW_S;
    }
    if (limit == 0) return false;

    taskENTER_CRITICAL(&s_activity_lock);
    int64_t last_us = s_last_activity_us;
    taskEXIT_CRITICAL(&s_activity_lock);
    int64_t idle_us = esp_timer_get_time() - last_us;
    return idle_us >= (int64_t)limit * 1000000LL;
}

/* allow_timer = false 时不设定时唤醒（用于「关机失败后退回深睡」，
   避免设备每小时醒来空转 3 分钟）。 */
static void enter_deep_sleep(bool allow_timer)
{
    epd_panel_deep_sleep();

    /* 唤醒键：内部上拉、按钮接 GND，低电平唤醒（RTC 域 GPIO0..5）。 */
    gpio_pullup_en((gpio_num_t)s_wake_pin);
    gpio_pulldown_dis((gpio_num_t)s_wake_pin);
    gpio_set_direction((gpio_num_t)s_wake_pin, GPIO_MODE_INPUT);

#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(
        1ULL << s_wake_pin, ESP_GPIO_WAKEUP_GPIO_LOW));
#else
    gpio_wakeup_enable((gpio_num_t)s_wake_pin, GPIO_INTR_LOW_LEVEL);
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
#endif

    /* 自锁脚：深睡期间锁存为高，维持 MOS 持续导通。
       数字域深睡会掉电、普通输出会变高阻，必须用 pad hold 保持。 */
    gpio_hold_en((gpio_num_t)PIN_SELF_LOCK);
    gpio_deep_sleep_hold_en();

    uint32_t wake_s = allow_timer ? settings_get()->wake_s : 0;
    if (wake_s > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_s * 1000000ULL);
    }

    ESP_LOGI(TAG, "deep sleep (btn GPIO%u low, timer %lus, self-lock GPIO%d held)",
             s_wake_pin, (unsigned long)wake_s, PIN_SELF_LOCK);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_deep_sleep_start();
}

void power_enter_deep_sleep(void)
{
    enter_deep_sleep(true);
}

void power_self_lock_off(void)
{
    ESP_LOGW(TAG, "timeout action = off: release self-lock GPIO%d", PIN_SELF_LOCK);

    /* ★ 关键修复（R 系列遗留缺陷）：深睡唤醒后 pad hold 仍然生效，
       不释放的话下面两行是无效写入 —— 表现为「关机只在首次上电那轮有效，
       之后每次都打 did not cut power」。 */
    gpio_hold_dis((gpio_num_t)PIN_SELF_LOCK);
    gpio_deep_sleep_hold_dis();

    /* 自锁 = MOS 一键开机：GPIO2 输出高 -> MOS 导通维持供电，拉低即断电。
       有自锁电路时约 100ms 内失电，下面的代码不会执行到。 */
    gpio_set_direction((gpio_num_t)PIN_SELF_LOCK, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_SELF_LOCK, 0);

    /* 无自锁电路（USB 供电 / 未装 MOS）时拉低无效：1s 后退回深睡。
       这里刻意**不设定时唤醒** —— 否则设备会每小时醒来空转，比不睡更费电。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "self-lock low did not cut power (no self-lock circuit?), "
                  "fallback to deep sleep without timer wake");
    enter_deep_sleep(false);
}

/* ---------------- 电池 ---------------- */

int power_battery_mv(void)
{
    /* 一次调用内连采 12 次取中位数：
       1. 中位数抗单个坏点；
       2. 用极差 max-min 判悬空（悬空时读数靠杂散耦合乱漂，极差远大于真实电池）；
       3. 中位数越界或极差超限 -> 返回 -1（APP 显示「未连接」）。 */
    int v[BAT_SAMPLES];
    int n = 0;

    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_adc_ch, &raw) != ESP_OK) continue;
        /* 注意乘法顺序：先除再乘，避免 raw*3100*200 溢出 int */
        v[n++] = raw * BAT_FULLSCALE_MV / BAT_ADC_MAX * BAT_DIV_NUM / BAT_DIV_DEN;
    }
    if (n < 3) return -1;

    for (int i = 1; i < n; i++) {
        int t = v[i], j = i - 1;
        while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; }
        v[j + 1] = t;
    }
    int med = v[n / 2];
    int spread = v[n - 1] - v[0];

    if (med < BAT_MV_MIN || med > BAT_MV_MAX) return -1;
    if (spread > BAT_SPREAD_MAX) return -1;
    return med;
}