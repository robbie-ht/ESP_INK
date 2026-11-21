#ifndef MOINK_VERSION_H
#define MOINK_VERSION_H

/*
 * 墨印 MoInk · BLE 版版本号
 *
 *   MOINK_VERSION     固件版本（B 系列与 R 系列分开编号：B = Bluetooth）
 *   MOINK_API_VERSION BLE 协议版本 —— **只**在帧/命令字节布局发生不兼容变化时 +1。
 *                     APP 通过 PING / GET_STATUS 读它来决定走哪套解析。
 *
 * 编号规则：
 *   主版本 +1 —— 破坏性变更（分区表 / NVS 布局 / 屏驱动基线 / 协议不兼容）
 *   次版本 +1 —— 固件功能批（新 opcode、新事件）
 *   第三位 +1 —— 修复批（协议不变）
 *
 * ────────────────────────────────────────────────────────────────
 * B2.0.1：刷屏阶段图库只读窗口；兼容尾flags与512字节通知限长。
 * B2.0.0：ESP-IDF 5.2.2；恢复 8 槽图库、双击/定时切图、图库 BLE API。
 * 分区表改变，需 USB 迁移；api=1 的旧命令字节布局保持兼容。
 *
 * B1.0.0（首个 BLE 版，以下是历史记录）
 *   - 与 R1.3.4 的关系：屏驱动（epd_drv）、电源（power）、引脚/设置（settings）
 *     三块沿用 R1.3.4 的真机验证结论；传输层从 HTTP 整体换成 BLE GATT。
 *   - 删除：SoftAP / captive portal / HTTP server / 内嵌控制页 / 页面热更 /
 *     轮播 store 分区。理由：C3 无 PSRAM，WiFi 与 BLE 同时常驻会把堆挤到
 *     无法再分配 120 KB 帧缓冲；且 APP 版不需要设备自带网页。
 *   - 新增 BLE 协议（api=1）：CTRL/DATA/EVT 三特征，命令-序号-状态三段式，
 *     帧传输带 chunk 序号 + 断点续传，OTA 走同一条 DATA 通道。
 *   - OTA 从 HTTP 的「直接写分区」改为 BLE 流式写 + CRC32 校验 + 双槽切启动。
 *   - 新增帧缓存分区 fcache：刷屏成功后落盘，支持「双击按键重刷」。
 *   - 继承 R1.3.4 的两处关键修复与本次移植的两处新修复：
 *       ① 电池电压一次调用内连采 12 次取中位数 + 极差判悬空（R1.3.4 FB-019）
 *       ② 删图 O(1) —— 本版无 store，不适用
 *       ③ 自锁脚进入深睡前 gpio_hold，唤醒后必须 gpio_hold_dis 才能再拉低
 *          （否则「关机」在第一次休眠后永久失效）—— 本版已修
 *       ④ 传图状态机的所有提前返回路径统一走 out: 释放锁，不再漏放
 * ────────────────────────────────────────────────────────────────
 */
#define MOINK_API_VERSION   1
#define MOINK_VERSION       "B2.0.1"

#endif /* MOINK_VERSION_H */
