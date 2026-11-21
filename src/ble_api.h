#ifndef MOINK_BLE_API_H
#define MOINK_BLE_API_H

#include <stdint.h>
#include <stdbool.h>

/*
 * BLE 协议栈（GATT + 帧传输状态机 + 命令分发 + OTA）。
 * 协议权威定义见 docs/API.md，本文件只暴露「固件内其它模块」需要的接口。
 *
 * 线程模型：
 *   - NimBLE host 任务调用 GATT 写回调 -> ble_api 内部状态机（收帧 / OTA / 命令）
 *   - 收满一帧或收到 CACHE_SHOW / GHOST_CLEAR 时，ble_api 投递一个作业到队列，
 *     由 main.c 的 display_task 串行执行（刷屏只在那个任务里发生）
 *   - 作业完成由 display_task 调 ble_api_job_finish()，ble_api 再发 DISPLAY_DONE
 */

/* 事件码（EVT 通知里的 evt 字节，见 docs/API.md §8） */
#define MOINK_EVT_DISPLAY_DONE 0x01   /* arg: 0 成功 / 1 失败 / 2 清理完 / 3 缓存重刷 / 4 图库 */
#define MOINK_EVT_BUTTON       0x02   /* arg: 1 单击 / 2 双击 / 3 长按预警 */
#define MOINK_EVT_LOW_BATTERY  0x03   /* payload: i16 mv */
#define MOINK_EVT_STATE        0x04   /* arg: 状态；payload: u8 gallery read flags */
#define MOINK_EVT_TRANSFER_GAP 0x05   /* payload: u16 expected_seq */
#define MOINK_EVT_STORE_DONE   0x06   /* arg: status; payload: slot,count,current */

/* 状态（与 docs/API.md §6.2 的 state 字段取值一致） */
typedef enum {
    BLE_ST_IDLE        = 0,
    BLE_ST_RECV_FRAME  = 1,
    BLE_ST_RECV_OTA    = 2,
    BLE_ST_REFRESHING  = 3,
    BLE_ST_CLEARING    = 4,
    BLE_ST_SLEEPING    = 6,
} ble_state_t;

/* B2.0.1: appended to GET_STATUS and display-window STATE events. */
#define MOINK_GALLERY_READ_SUPPORTED 0x01u
#define MOINK_GALLERY_READ_WINDOW    0x02u
#define MOINK_GALLERY_APPEND_COMMITTED 0x04u

/* 显示作业类型 */
typedef enum {
    BLE_JOB_FRAME = 1,   /* 刷 ble_api_frame_buf() 里的整帧 */
    BLE_JOB_CACHE = 2,   /* 从 fcache 重刷 */
    BLE_JOB_CLEAR = 3,   /* 残影清理 */
    BLE_JOB_STORE_SHOW = 4,
    BLE_JOB_STORE_DELETE = 5,
    BLE_JOB_STORE_CLEAR = 6,
} ble_job_type_t;

typedef struct {
    uint8_t  type;        /* ble_job_type_t */
    uint8_t  cycles;      /* CLEAR 用 */
    uint8_t  save_cache;  /* FRAME 用：刷完是否落盘 */
    uint8_t  save_store;  /* FRAME: append to gallery */
    uint8_t  no_show;     /* FRAME: save only */
    uint8_t  slot;        /* gallery job; 255 = next / none */
    uint8_t  thumb_format;
    uint16_t thumb_w, thumb_h;
    uint32_t thumb_len;
    uint16_t w;
    uint16_t h;
} ble_job_t;

/* 初始化 NimBLE、注册 GATT、开始广播。 */
void ble_api_start(void);

/* 停止广播并断开当前连接（休眠 / 关机前调用）。 */
void ble_api_stop(void);
/* Atomically claim idle before a power transition; caller then stops BLE. */
bool ble_api_prepare_sleep(void);
/* Idle timeout only: recheck activity under the command/state mutex. */
bool ble_api_prepare_idle_sleep(void);

bool    ble_api_connected(void);
uint8_t ble_api_state(void);
void    ble_api_set_state(uint8_t st);

/* display_task only: open after loading/saving, around the EPD operation only.
 * Closing takes the host command mutex, draining any in-flight gallery read
 * before the caller changes gallery metadata or writes the recent-frame cache.
 * The existing refreshing/clearing state and completion events are unchanged.
 */
void ble_api_set_display_read_window(bool open, bool append_committed);

/* 帧缓冲（长度 EPD_MAX_BUF_LEN）。上传写入它，display_task 读它。 */
uint8_t *ble_api_frame_buf(void);
const uint8_t *ble_api_thumb_buf(void);

/* 主动推一条 EVENT 通知（arg 含义见 MOINK_EVT_*）。payload 超过 37 字节截断。
 * NimBLE 主机栈自带互斥量，从显示任务里调用是安全的。 */
void ble_api_notify_event(uint8_t evt, uint8_t arg, const void *payload, uint8_t len);

/* 唤醒原因（0 上电 / 1 按键 / 2 定时 / 3 其它），只影响 GET_STATUS 回报。 */
void ble_api_set_wake_reason(uint8_t reason);

/* display_task：阻塞取一个作业。返回 false = 超时。 */
bool ble_api_job_wait(ble_job_t *job, uint32_t timeout_ms);

/* display_task：作业完成。result 见 docs/API.md §8（0 成功 / 1 失败 / 2 清理 / 3 缓存重刷）。 */
void ble_api_job_finish(uint8_t result);
/* Release a storage-only job without emitting DISPLAY_DONE. */
void ble_api_job_complete(void);

/* 按键双击：若空闲且缓存有效则请求重刷。返回 false = 当前不可用。 */
bool ble_api_request_cache_show(void);
/* slot 255 selects the next gallery entry; current changes after refresh. */
bool ble_api_request_carousel_show(uint8_t slot);

/* 改名后重启广播（广播包里带设备名）。 */
void ble_api_restart_adv(void);

#endif /* MOINK_BLE_API_H */
