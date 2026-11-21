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
assert project["project_version"] == "B2.0.0", project["project_version"]
out = root / "firmware" / "B2.0.0"
out.mkdir(parents=True, exist_ok=True)
files = {
    "bootloader.bin": build / "bootloader/bootloader.bin",
    "partition-table.bin": build / "partition_table/partition-table.bin",
    "ota_data_initial.bin": build / "ota_data_initial.bin",
    "moink_B2.0.0_app.bin": build / "moink_ble.bin",
}
for name, source in files.items():
    shutil.copy2(source, out / name)
subprocess.run([
    sys.executable, "-m", "esptool", "--chip", "esp32c3", "merge_bin",
    "-o", str(out / "moink_B2.0.0_merged.bin"), "--flash_mode", "dio",
    "--flash_freq", "40m", "--flash_size", "4MB", "--fill-flash-size", "4MB",
    "0x0", str(out / "bootloader.bin"), "0x8000", str(out / "partition-table.bin"),
    "0xd000", str(out / "ota_data_initial.bin"), "0x10000", str(out / "moink_B2.0.0_app.bin")
], check=True)
hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(out.glob("*.bin"))}
(out / "SHA256SUMS").write_text("".join(f"{sha}  {name}\n" for name, sha in hashes.items()))
(out / "manifest.json").write_text(json.dumps({
    "firmware": "B2.0.0", "api": 1, "idf": idf_version, "target": project["target"],
    "flash_bytes": 0x400000, "ota_slot_bytes": 0x160000,
    "app_bytes": (out / "moink_B2.0.0_app.bin").stat().st_size,
    "usb_migration_required_from": ["R series", "B1.0.0"], "hardware_tested": False,
    "sha256": hashes
}, ensure_ascii=False, indent=2) + "\n")
(out / "README.txt").write_text("""MoInk APP/BLE B2.0.0 - ESP-IDF 5.2.2 - ESP32-C3 4 MB

本目录是设备固件，不是手机 APK。尚未进行真机验证。
R 系列/B1.0.0 首次迁移必须 USB 全量刷入；先备份旧图片和设置。
merged.bin 写入起始地址 0x0。请手动替换 PORT 为实际串口：

python -m esptool --chip esp32c3 --port PORT erase_flash
python -m esptool --chip esp32c3 --port PORT write_flash 0x0 moink_B2.0.0_merged.bin

erase_flash 会清除设备设置、图库和缓存。助手未执行以上硬件命令。
app.bin 仅用于已经采用 B2 分区表的设备后续 BLE OTA；不用于 B1/R 在线迁移。
其余 bin 是合并镜像中的独立构件，地址见工程 README 与分区表。
SHA256SUMS/manifest.json 可核对文件完整性，验证边界见 ../../validation/RESULTS.md。
""")
print(f"Packaged {len(hashes)} binaries in {out}")
