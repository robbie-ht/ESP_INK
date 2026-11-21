#ifndef MOINK_SETTINGS_H
#define MOINK_SETTINGS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * 持久化设置（NVS 命名空间 "moink"）。
 *
 * BLE 版只保留「影响硬件与电源」的项：屏型、引脚、休眠/超时/唤醒、缓存开关、
 * 设备名。图像、排版、风格这些全部留在 APP 侧。
 *
 * 引脚在启动时由 epd_init() / power_init() 读取，改后需重启才生效
 * （SPI 总线与 ADC 通道只能在初始化时绑定），协议见 docs/API.md §6.4。
 */

#define SETT_NAME_MAX 21        /* 20 字节 UTF-8 + NUL，广播名越长越占包 */

#define SETT_SLEEP_3MIN 180

/* 超时动作 */
#define SETT_ACTION_SLEEP 0     /* 深睡（可配定时唤醒） */
#define SETT_ACTION_OFF   1     /* 释放自锁断电（需硬件 MOS 自锁电路） */

/* ---------------- 引脚配置 ----------------
 *
 * ESP32-C3 硬边界：
 *   可用 GPIO   0..10（11..17 片内 flash，18/19 USB-JTAG，20/21 UART0）
 *   电池 ADC    仅 ADC1：GPIO0..4 = ADC1_CH0..CH4
 *   深睡唤醒    仅 RTC 域 GPIO0..5
 *   Strapping   GPIO2 / 8 / 9；GPIO2 固定留给自锁；8/9 可用但须按硬件确认启动电平
 */
enum {
    PIN_IDX_ADC  = 0,
    PIN_IDX_WAKE,
    PIN_IDX_SCK,
    PIN_IDX_MOSI,
    PIN_IDX_CS,
    PIN_IDX_DC,
    PIN_IDX_RST,
    PIN_IDX_BUSY,
    PIN_IDX_COUNT
};

/*
 * MOS 自锁一键开机脚：固件固定 GPIO2，上电第一件事就拉高并 hold，
 * 深睡前锁存电平维持供电。不对外开放配置，自定义引脚菜单里也会排除它。
 */
#define PIN_SELF_LOCK  2

#define GPIO_PRESET_DEFAULT  0   /* 默认档（现役接法） */
#define GPIO_PRESET_LINE     1   /* 排线 */
#define GPIO_PRESET_BOARD    2   /* 整合板 */
#define GPIO_PRESET_CUSTOM   3   /* 自定义 */
#define GPIO_PRESET_COUNT    4
#define GPIO_PRESET_NPRESET  3   /* 前 3 档为固定预设 */

typedef struct { uint8_t v[PIN_IDX_COUNT]; } pin_cfg_t;

extern const pin_cfg_t PIN_PRESETS[GPIO_PRESET_NPRESET];

/* 单脚合法性（白名单 0..10 且避开自锁脚） */
bool pin_io_allowed(uint8_t io);
bool pin_adc_allowed(uint8_t io);    /* GPIO0..4 */
bool pin_wake_allowed(uint8_t io);   /* GPIO0..5 */
/* 整表校验：每脚在各自域内、互不重复、避开自锁脚。合法返回 true。 */
bool pin_cfg_valid(const pin_cfg_t *c);

const pin_cfg_t *settings_pins(void);

typedef struct {
    uint8_t  panel;          /* 0 = A0，1 = A1 */
    uint8_t  gpio_preset;    /* GPIO_PRESET_* */
    pin_cfg_t pins;          /* 生效引脚表（自定义档即用户所填） */
    uint32_t sleep_s;        /* 空闲超时秒；0 = 不超时 */
    uint8_t  timeout_action; /* SETT_ACTION_SLEEP / SETT_ACTION_OFF */
    uint32_t wake_s;         /* 定时自动唤醒秒；0 = 关闭 */
    uint8_t  wake_show;      /* 定时唤醒后切下一张图库，默认 1 */
    uint8_t  cache_on;       /* 1 = 刷屏成功后把整帧写入 fcache */
    char     name[SETT_NAME_MAX];   /* 空 = 默认 MoInk-XXXX */
} moink_settings_t;

void settings_init(void);
const moink_settings_t *settings_get(void);

/* 一份版本化 NVS blob 提交整组设置；持久化成功后才更新 RAM。 */
esp_err_t settings_apply(const moink_settings_t *cfg);
esp_err_t settings_set_wake_show(uint8_t v);
esp_err_t settings_set_panel(uint8_t v);
esp_err_t settings_set_sleep(uint32_t v);
esp_err_t settings_set_timeout_action(uint8_t v);
esp_err_t settings_set_wake(uint32_t v);
esp_err_t settings_set_cache(uint8_t v);
esp_err_t settings_set_name(const char *name);
/* 引脚：preset 0/1/2 取预设表；3 使用 custom 且必须通过 pin_cfg_valid()。 */
esp_err_t settings_set_gpio(uint8_t preset, const pin_cfg_t *custom);

/* 恢复出厂：擦除整个 NVS 命名空间。调用方负责重启。 */
void settings_factory_reset(void);

#endif /* MOINK_SETTINGS_H */