MoInk APP/BLE B2.0.0 - ESP-IDF 5.2.2 - ESP32-C3 4 MB

本目录是设备固件，不是手机 APK。尚未进行真机验证。
R 系列/B1.0.0 首次迁移必须 USB 全量刷入；先备份旧图片和设置。
merged.bin 写入起始地址 0x0。请手动替换 PORT 为实际串口：

python -m esptool --chip esp32c3 --port PORT erase_flash
python -m esptool --chip esp32c3 --port PORT write_flash 0x0 moink_B2.0.0_merged.bin

erase_flash 会清除设备设置、图库和缓存。助手未执行以上硬件命令。
app.bin 仅用于已经采用 B2 分区表的设备后续 BLE OTA；不用于 B1/R 在线迁移。
其余 bin 是合并镜像中的独立构件，地址见工程 README 与分区表。
SHA256SUMS/manifest.json 可核对文件完整性，验证边界见 ../../validation/RESULTS.md。
