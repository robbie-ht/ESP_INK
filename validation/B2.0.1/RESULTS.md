# B2.0.1 刷屏图库只读窗口验证

日期：2026-11-21。权威基线为用户指定`robbie-ht/ESP_INK`的
`4909e4c36172daa1d54a21ae4d4f0d6998eac007`；最新仓库中的ble_api.c/main.c与修改前备份逐字节一致，store.c/epd_drv.c与未修改恢复源码一致。没有把此前中断中的草稿当作已验证固件。

## 实现与边界

- 固件及CMake版本均为B2.0.1，api=1；GET_STATUS版本字符串后兼容追加一字节flags，旧字段不移动。bit0支持、bit1当前只读窗口、bit2本窗口前已成功追加；flags分别为1、3、7。STATE窗口通知保留原arg=3/4并附同样flags，旧客户端可忽略尾字段。
- main仅在保存/加载完成后，在实际EPD操作及面板deep_sleep周围开放窗口。普通立即显示、图库显示、缓存重刷、清残影为flags3；FRAME保存成功再刷屏为flags7；仅存、存储失败和加载失败不打开。STORE_DONE仍在完整作业末尾，不因bit2提前发成功回执。
- INFO/LIST/READ仅允许idle或明确窗口。关闭窗口使用与host回调相同的s_lock，等待在途读取结束，再允许current/NVS/cache后处理。窗口关闭与complete均清bit1/bit2。所有修改操作继续要求idle；未放开上传、设置、删除、清空或OTA。
- 下载使用独立小响应缓冲，不改共享上传/显示帧；store_read保留存储互斥、偏移与CRC检查。READ数据最多min(MTU-19,496)，RSP属性值最多512字节。
- bit2不包含slot/count/current，不等于最终STORE_DONE。APP若在自己存并显示的窗口迁移旧缓存，仍须同连接/会话、pending追加、count+1、单步revision与全部旧prefix元数据一致，最终回执异常必须撤销推定。参考SDK只增加可选flags解析及496限长，未声称它们已具备APP的并行调度。

## 已通过的离线检查

最终[统一离线日志](offline-tests.log)：

- 实际store.c/settings.c/cache.c的主机模拟及ASan/UBSan检查通过，原容量、CRC、事务、失败保护、preset2默认与有效NVS行为保留。
- 原生产BLE解析函数提取测试通过；新增测试编译实际窗口/GET_STATUS/INFO/LIST/READ/RSP函数及main显示任务，用真实pthread互斥验证关窗等待在途回调排空。覆盖6种状态门禁、flags1/3/7、窗口开关、修改命令BUSY、complete复位、8种MTU/512边界，以及15条显示任务成功/失败路径中读窗口与存储写入的隔离。
- 帧编码27项通过；Python SDK16项、TypeScript SDK15项通过，包括旧GET_STATUS缺失尾字段兼容、flags7解析及MTU517下496请求上限。TypeScript使用Node experimental transform执行，未声称独立tsc静态分析。

这些检查模拟Flash、NVS、BLE和屏幕外设，不代表真实无线链路、面板刷新并发、运行堆余量或物理掉电验收。

## 实际构建与打包

[Docker构建日志](build-idf522.log)：固定官方`espressif/idf:v5.2.2`镜像digest构建和链接成功，目标ESP32-C3、固件B2.0.1。配置阶段保留OpenThread子仓库Git ownership诊断，构建命令最终exit0，未改变全局Git配置。没有隐去该诊断或称作日志完全无警告。

APP镜像445136字节，OTA槽0x160000，约69%余量；编译期静态D/IRAM102162字节（31.8%），不等于运行期堆测量。

[打包日志](package.log)与[产物核对](artifact-check.json)：Docker环境生成5个bin及4MiB合并镜像，独立核对SHA、合并镜像4个组件偏移及字节、当前分区表与旧B2.0.0一致。[新固件manifest](../../firmware/B2.0.1/manifest.json)标记variant=integrated-board、默认preset2及引脚[0,1,4,3,5,6,7,10]、hardware_tested=false。

- `moink_B2.0.1_app.bin` SHA256：`1cadab640ee3e09e268f604f1614dbbeeda9d9321b31d647a3fa25036bad2f26`。
- `moink_B2.0.1_merged.bin` SHA256：`b70146d2c589049884001289db6da8c8a2d3febf0c46fe7cd5d9f9a8d1b97db2`。
- 其余bin与完整清单见[SHA256SUMS](../../firmware/B2.0.1/SHA256SUMS)。

13份最终构建输入的SHA前后一致；16份受保护文件（含旧B2.0.0目录全部文件及settings/store/cache/epd/power/分区/defaults）未变。证据为[构建输入](build-input-sha256.json)、[受保护基线](preserved-baseline.json)与产物核对。受影响原始16文件及清单保留在[修改前备份](../before-B2.0.1-display-read-20261121T093919Z/manifest.json)。

## 硬件与升级边界

本轮没有连接、擦除或刷写ESP，没有在手机实测刷屏并行高清读取。已有B2分区设备可使用APP镜像按原OTA流程升级；有效NVS配置优先，升级不强制改为preset2。R/B1首次迁移才需USB完整分区迁移。完整merged镜像或erase会覆盖用户数据，不能把“应用升级保留有效NVS”的源码行为解释为全量刷写也保存图片和设置。

首次实机应检查窗口开关通知/GET_STATUS一致性、刷屏中完整HD与CRC、窗口收尾BUSY暂停恢复、存并显示的append证明和最终回执、断线重连，以及A0/A1与电源稳定性。离线通过不替代这些结果。
