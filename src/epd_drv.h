#ifndef EPD_DRV_H
#define EPD_DRV_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * JDY79668 协议四色电子墨水屏驱动（K/W/R/Y，2bpp，4 像素/字节）。
 * 从社区 InkSight 适配移植（epd_driver.cpp + 屏幕修改适配驱动/epd7in3g-A0.cpp）。
 * B 系列（BLE 版）原样沿用 R1.3.4 的真机验证实现，只有三处加固：
 *   ① spi_tx() 增加 s_spi 为空守卫（epd_init 失败后不再解引用空句柄）
 *   ② epd_cycle() 失败重试前补 epd_power_off()，三次全败也收尾断电
 *   ③ 注释同步 BLE 语境
 *
 * 两种拆机屏运行时切换（无需重刷）：
 *   A0 - 768 x 552，隔行半屏 Y 映射（已在真机验证）
 *   A1 - 可见区 768 x 552（与 A0 外观一致）、TRES 768 x 600，完整 JD79665
 *        式初始化，单帧一次 0x83 布防，单次 0x10 顺序连发 552 行。
 *
 * epd_display_2bpp() / epd_display_2bpp_wh() 期望的帧缓冲布局：
 *   行 r 存逻辑行 y = H-1-r（缓冲自下而上存储），
 *   每字节 4 像素、MSB 优先，像素 x 在位 (6 - 2*(x%4))，
 *   色码 0x00=黑 0x01=白 0x02=黄 0x03=红（A0 实测）。
 */

typedef enum {
    EPD_PANEL_A0 = 0,
    EPD_PANEL_A1 = 1,
    EPD_PANEL_COUNT
} epd_panel_t;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint32_t buf_len;        /* w/4*h 字节的 2bpp 载荷 */
    int8_t   lower_y_base;   /* 下半屏 Y 映射偏移（A0: -1） */
    uint8_t  linear;         /* 1 = 整窗 + 单次线性 0x10 写入 */
} epd_profile_t;

extern const epd_profile_t EPD_PROFILES[EPD_PANEL_COUNT];

/* 静态帧缓冲按 800x600/4 = 120000 字节分配（沿用旧上限，留出余量）；
   A0 / A1 的 768x552 帧（105984）用同一块缓冲的前段。 */
#define EPD_MAX_W         800
#define EPD_MAX_H         600
#define EPD_MAX_BUF_LEN   (EPD_MAX_W / 4 * EPD_MAX_H)   /* 120000 */
#define EPD_A0_W          768
#define EPD_A0_H          552
#define EPD_A1_W          768
#define EPD_A1_H          552
#define EPD_A1_GATES      600       /* A1 TRES / 刷新窗栅极行数 */

/*
 * 引脚映射（运行时可配，见 settings.h 的 pin_cfg_t / PIN_PRESETS）：
 *   ADC / 唤醒 / SCK / MOSI / CS / DC / RST / BUSY 八脚由设置决定，
 *   默认档 = SCK4 MOSI6 CS7 DC1 RST3 BUSY10、唤醒 5、ADC 0。
 *   另有「排线」「整合板」两档预设与用户自定义档；改引脚需重启生效。
 *   另有一个固件固定的自锁脚 GPIO2（MOS 一键开机），不在此表中。
 */

/* 初始化 SPI 总线 + GPIO（引脚取自 settings_pins()）。成功返回 0。 */
int epd_init(void);

/* 选择活动屏画像。成功返回 0。 */
int epd_set_panel(epd_panel_t panel);
epd_panel_t epd_get_panel(void);

/* A1 驱动策略（R1.2.2 定案，B 系列不变）：
 *   帧 768x552，TRES/刷新窗 768x600，写入窗口 (0..767, 0..551) +
 *   单次 0x10 连发 552 行 x 192 B，栅极号 = 行号（顺序）。
 *   1:1 铺满可见区：不裁切、不重采样、无白边。行内固定整行镜像。 */

/* 当前画像（永不为 NULL）。 */
const epd_profile_t *epd_profile(void);

/* 画像显示名（"A0" / "A1"）。 */
const char *epd_panel_name(epd_panel_t panel);

/* 行内镜像开关（B 系列保留接口；当前只有 A1 档且固定镜像，不受此开关影响）。 */
void epd_set_hflip(bool on);
bool epd_get_hflip(void);

/*
 * 完整显示周期，最多尝试 3 次：
 * reset -> PSR/TRES/上电 -> 写 2bpp 帧 -> 刷新 -> 断电。
 * `frame` 必须为 epd_profile()->buf_len 字节。成功返回 0。
 */
int epd_display_2bpp(const uint8_t *frame);

/* 同上，但显式给出本帧几何（当前只接受 768x552）。 */
int epd_display_2bpp_wh(const uint8_t *frame, uint16_t w, uint16_t h);

/* 校验帧几何是否被当前画像接受。A0 / A1 都只接受 768x552。 */
bool epd_frame_geom_ok(uint16_t w, uint16_t h, uint32_t len);

/* 残影清理：整屏填单色并刷新 `cycles` 次（黑/白交替）。每次约 15~25 秒。 */
int epd_clear_cycles(int cycles);

/* 发送面板深睡命令（0x07 0xA5）。用完可安全调用。 */
void epd_panel_deep_sleep(void);

#endif /* EPD_DRV_H */