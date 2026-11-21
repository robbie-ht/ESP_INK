#ifndef MOINK_POWER_H
#define MOINK_POWER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 电源管理：MOS 自锁、电池 ADC、深睡/唤醒、空闲休眠判定。
 *
 * 休眠策略：
 *   sleep_s = 空闲无活动后深睡的超时（0 = 不休眠）。
 *   wake_s  = 定时自动唤醒间隔（0 = 关闭）。
 *   定时唤醒后进入「等人连」窗口（默认 AUTO_WAKE_WINDOW_S 秒），窗口内无活动则回睡。
 *   timeout_action = OFF 时不是深睡而是释放自锁断电。
 *
 * ★ BLE 版新增 power_set_busy()：刷屏/清理/OTA 期间禁止休眠，
 *   否则长刷新（A1 单次可达 180s）会被空闲休眠从中间打断。
 */

/* ★ 必须在 app_main 第一行调用：配置自锁脚并**释放上次深睡遗留的 pad hold**。
   顺序不能反 —— 复位后输出寄存器默认是 0，先 hold_dis 会让 MOS 瞬间断电。 */
void power_self_lock_init(void);

/* 配置唤醒键输入 + 电池 ADC（依赖 settings_pins()）。 */
void power_init(void);

/* 重置空闲计时（任何 BLE 命令 / 传输 / 刷屏都调用）。 */
void power_activity(void);

/* 置忙：忙期间 power_should_sleep() 恒为 false。 */
void power_set_busy(bool busy);
bool power_busy(void);

/* 本次是否为定时器唤醒（决定是否套用短窗口）。 */
void power_set_auto_wake(bool v);
bool power_auto_wake(void);

/* 当前唤醒原因。ESP-IDF 5.2.2 只有单因接口 esp_sleep_get_wakeup_cause()，
   位图版 esp_sleep_get_wakeup_causes() 是 5.3+ 才加的，这里不做兼容层。
   两个都返回 false ⇒ 上电/复位（或其它非深睡唤醒）。 */
bool power_woke_by_button(void);
bool power_woke_by_timer(void);

/* 空闲是否已超时，应进入深睡/关机。 */
bool power_should_sleep(void);

/* 设唤醒源并进入深睡（不返回）。 */
void power_enter_deep_sleep(void);

/* 超时动作 = 关机：释放自锁脚让 MOS 断电；无自锁电路时 1s 后退回深睡。
   ★ 内部会先 gpio_hold_dis()，否则深睡唤醒后 hold 仍在、拉低无效、关机失效。 */
void power_self_lock_off(void);

/* 电池电压 mV 粗测（分压 1M:1M，×2.0）。未接电池/悬空返回 -1。 */
int power_battery_mv(void);

#endif /* MOINK_POWER_H */