#!/usr/bin/env python3
"""Package the verified IDF build; never connects to a board."""
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = root / "build-idf522"
project = json.loads((build / "project_description.json").read_text())
idf_version = re.search(r'^CONFIG_IDF_INIT_VERSION="([^"]+)"$', (build / "sdkconfig").read_text(), re.MULTILINE).group(1)
assert idf_version == "5.2.2", idf_version
version = re.search(r'^#define MOINK_VERSION\s+"(B\d+\.\d+\.\d+)"$',
                    (root / "src/version.h").read_text(), re.MULTILINE).group(1)
assert project["project_version"] == version, project["project_version"]
app_name = f"moink_{version}_app.bin"
merged_name = f"moink_{version}_merged.bin"
out = root / "firmware" / version
out.mkdir(parents=True, exist_ok=True)
files = {
    "bootloader.bin": build / "bootloader/bootloader.bin",
    "partition-table.bin": build / "partition_table/partition-table.bin",
    "ota_data_initial.bin": build / "ota_data_initial.bin",
    app_name: build / "moink_ble.bin",
}
for name, source in files.items():
    shutil.copy2(source, out / name)
subprocess.run([
    sys.executable, "-m", "esptool", "--chip", "esp32c3", "merge_bin",
    "-o", str(out / merged_name), "--flash_mode", "dio",
    "--flash_freq", "40m", "--flash_size", "4MB", "--fill-flash-size", "4MB",
    "0x0", str(out / "bootloader.bin"), "0x8000", str(out / "partition-table.bin"),
    "0xd000", str(out / "ota_data_initial.bin"), "0x10000", str(out / app_name)
], check=True)
hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(out.glob("*.bin"))}
(out / "SHA256SUMS").write_text("".join(f"{sha}  {name}\n" for name, sha in hashes.items()))
(out / "manifest.json").write_text(json.dumps({
    "firmware": version, "api": 1, "idf": idf_version, "target": project["target"],
    "variant": "integrated-board", "default_gpio_preset": 2,
    "default_pins": {"ADC": 0, "WAKE": 1, "SCK": 4, "MOSI": 3,
                     "CS": 5, "DC": 6, "RST": 7, "BUSY": 10},
    "existing_valid_nvs_preserved_on_app_ota": True,
    "full_erase_or_merged_flash_preserves_data": False,
    "gallery_read_flags": {"supported": 1, "window_open": 2, "append_committed": 4},
    "flash_bytes": 0x400000, "ota_slot_bytes": 0x160000,
    "app_bytes": (out / app_name).stat().st_size,
    "usb_migration_required_from": ["R series", "B1.0.0"], "hardware_tested": False,
    "sha256": hashes
}, ensure_ascii=False, indent=2) + "\n")
(out / "README.txt").write_text(f"""MoInk APP/BLE {version} - ESP-IDF 5.2.2 - ESP32-C3 4 MB

本目录是设备固件，不是手机 APK。尚未进行真机验证。
R 系列/B1.0.0 首次迁移必须 USB 全量刷入；先备份旧图片和设置。
merged.bin 写入起始地址 0x0。请手动替换 PORT 为实际串口：

python -m esptool --chip esp32c3 --port PORT erase_flash
python -m esptool --chip esp32c3 --port PORT write_flash 0x0 {merged_name}

erase_flash 会清除设备设置、图库和缓存。助手未执行以上硬件命令。
app.bin 仅用于已经采用 B2 分区表的设备后续 BLE OTA；不用于 B1/R 在线迁移。
默认整合板preset2：[ADC,WAKE,SCK,MOSI,CS,DC,RST,BUSY]=[0,1,4,3,5,6,7,10]。
已有有效NVS配置优先；升级不强制改接线，也不要求清空图库或缓存。
本版新增显式刷屏图库只读窗口flags；需配合支持该扩展的客户端。
其余 bin 是合并镜像中的独立构件，地址见工程 README 与分区表。
SHA256SUMS/manifest.json 可核对文件完整性，验证边界见 ../../validation/RESULTS.md。
""")
print(f"Packaged {len(hashes)} binaries in {out}")
