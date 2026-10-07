# ESP32-S3 N16R8 适配说明

## 硬件规格
- **芯片**: ESP32-S3 (revision v0.2)
- **Flash**: 16MB (N16)
- **PSRAM**: 8MB OPI/Octal PSRAM (R8)
- **显示屏**: 无（SuperMini / 通用开发板）

## 已解决的问题

### ✅ 1. PSRAM 识别问题
- **原因**: 原配置未启用 PSRAM 支持
- **解决**: 在 `sdkconfig.n16r8` 中启用 OPI PSRAM 配置
- **结果**: 系统可用堆内存从 ~50KB 提升至 8MB+

### ✅ 2. 显示驱动死锁问题
- **原因**: 官方固件默认启用 T-Dongle-S3 的 ST7735 屏幕驱动
- **解决**: 切换硬件目标为 `CONFIG_TARGET_HARDWARE_QTPY_ESP32_S3` (无屏版)
- **结果**: 完全禁用 LVGL 和 LCD 驱动，避免 WDT 超时

### ✅ 3. 存储空间不足
- **原因**: 4MB Flash 布局下 OTA 分区仅 1.86MB
- **解决**: 创建 16MB Flash 专用分区表，每个 OTA 分区扩展至 6MB
- **结果**: NVS 从 24KB 扩展至 32KB，彻底解决配置丢失问题

## 配置文件说明

### 1. `sdkconfig.n16r8` - 主配置文件
关键配置项：
```ini
# 硬件目标：无屏版
CONFIG_TARGET_HARDWARE_QTPY_ESP32_S3=y

# Flash: 16MB
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# PSRAM: 8MB OPI 模式
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SIZE=8388608
CONFIG_SPIRAM_USE_MALLOC=y

# 分区表：16MB 专用
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_16mb.csv"
```

### 2. `partitions_16mb.csv` - 16MB 分区表
```
nvs,       data, nvs,      0x9000,   0x8000    # 32KB NVS
otadata,   data, ota,      0x11000,  0x2000    # 8KB OTA data
phy_init,  data, phy,      0x13000,  0x1000    # 4KB PHY init
ota_0,     app,  ota_0,    0x20000,  0x600000  # 6MB OTA 分区 0
ota_1,     app,  ota_1,    0x620000, 0x600000  # 6MB OTA 分区 1
coredump,  data, coredump, 0xC20000, 0x40000   # 256KB 崩溃转储
user_data, data, spiffs,   0xC60000, 0x3A0000  # 3.2MB 用户数据
```

## 编译方法

### 方案 A: GitHub Actions 自动编译（推荐）

1. **Push 代码触发自动构建**:
   ```bash
   git add .github/workflows/build_n16r8.yml sdkconfig.n16r8 partitions_16mb.csv
   git commit -m "feat: Add N16R8 board support"
   git push
   ```

2. **手动触发工作流**:
   - 访问 GitHub 仓库 → Actions 标签
   - 选择 "Build for N16R8 Board"
   - 点击 "Run workflow"

3. **下载固件**:
   - 构建完成后在 Artifacts 下载 `firmware-n16r8-<commit-sha>`
   - 解压得到 4 个 bin 文件和烧录说明

### 方案 B: Docker 本地编译

1. **确保 Docker 已安装**:
   ```bash
   docker --version
   ```

2. **运行编译脚本**:
   ```bash
   chmod +x build_docker.sh
   ./build_docker.sh
   ```

3. **编译产物**:
   - `build/bootloader/bootloader.bin`
   - `build/partition_table/partition-table.bin`
   - `build/ota_data_initial.bin`
   - `build/esp-usb-ble-hid.bin`

### 方案 C: 本地 ESP-IDF 环境

```bash
# 设置 ESP-IDF 环境
. $HOME/esp/esp-idf/export.sh

# 复制配置
cp sdkconfig.n16r8 sdkconfig.defaults

# 配置目标
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash

# 监控
idf.py -p /dev/ttyUSB0 monitor
```

## 烧录固件

### 使用 esptool.py（通用方法）

```bash
# 安装 esptool
pip install esptool

# 烧录命令（Linux/Mac）
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 \
  --before default_reset --after hard_reset write_flash \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x11000 ota_data_initial.bin \
  0x20000 esp-usb-ble-hid.bin

# Windows
esptool.py --chip esp32s3 --port COM3 --baud 921600 ^
  --before default_reset --after hard_reset write_flash ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x11000 ota_data_initial.bin ^
  0x20000 esp-usb-ble-hid.bin
```

### 使用自动脚本（Docker 编译后）

```bash
cd firmware_n16r8
./flash.sh /dev/ttyUSB0
```

## 验证步骤

### 1. 检查 PSRAM 识别
连接串口监视器（115200 波特率），启动日志应显示：
```
I (xxx) cpu_start: Pro cpu start user code
I (xxx) spiram: Found 8MB PSRAM device
I (xxx) spiram: Speed: 80MHz
I (xxx) spiram: Initialized successfully
```

### 2. 检查系统状态
通过 USB 控制台（web/dongle_console.html）查看：
- **PSRAM**: 应显示 8192 KB 或 "8MB OPI"
- **可用堆内存**: 应 > 8000 KB
- **NVS 存储**: 按键映射配置在重启后应保持

### 3. 测试手柄连接
1. 长按板载按钮 3 秒进入配对模式（LED 闪烁）
2. 打开手柄配对模式（如 Xbox/PS 手柄）
3. 配对成功后 LED 常亮
4. 连接 Switch，手柄输入应正常工作
5. 重启后手柄应自动重连（验证 NVS 持久化）

## 故障排查

### 问题 1: 烧录失败
**解决方法**:
1. 按住板载 BOOT 按钮，同时插入 USB
2. 降低波特率：`--baud 115200`
3. 完全擦除后重新烧录：
   ```bash
   esptool.py --chip esp32s3 --port /dev/ttyUSB0 erase_flash
   ```

### 问题 2: PSRAM 仍显示 "none"
**可能原因**:
- PSRAM 型号不兼容（非 OPI 模式）
- 硬件问题

**验证方法**:
```bash
esptool.py --port /dev/ttyUSB0 flash_id
# 查看输出的 Flash 和 PSRAM 信息
```

### 问题 3: 手柄连接后立即复位
**检查项**:
1. 确认使用的是 `sdkconfig.n16r8` 而非原始配置
2. 确认硬件目标为 `QTPY_ESP32_S3` 而非 `T3_DONGLE`
3. 查看串口输出的具体错误信息

## 与原版固件对比

| 项目 | 官方固件 (4MB) | N16R8 适配版 (16MB) |
|------|----------------|---------------------|
| Flash 大小 | 4MB | 16MB |
| PSRAM | 未启用 | 8MB OPI |
| 显示屏驱动 | 启用 (ST7735) | 禁用 |
| OTA 分区大小 | 1.86MB × 2 | 6MB × 2 |
| NVS 分区 | 24KB | 32KB |
| 可用堆内存 | ~50KB | ~8MB |
| 配置持久化 | 不稳定 | 稳定 |
| WDT 超时 | 手柄连接时触发 | 无 |

## 技术细节

### PSRAM 配置原理
- **OPI (Octal) 模式**: 8 条数据线，带宽更高
- **80MHz 频率**: 平衡性能与稳定性
- **MALLOC 策略**: >16KB 的分配自动使用 PSRAM
- **BSS 段外置**: 将静态变量放入 PSRAM，节省内部 DRAM

### 分区表设计考量
1. **NVS 扩容**: 从 24KB → 32KB，为多设备配对预留空间
2. **OTA 分区**: 6MB 可容纳未来功能扩展（如固件 > 2MB）
3. **用户数据**: 预留 3.2MB SPIFFS 分区，可存储自定义配置或日志
4. **地址对齐**: 遵循 0x1000 (4KB) 对齐要求

### 硬件目标选择
项目通过 `bsp.hpp` 根据 Kconfig 选择硬件抽象层：
- `CONFIG_TARGET_HARDWARE_QTPY_ESP32_S3` → 无屏版，纯 LED 指示
- `CONFIG_TARGET_HARDWARE_T3_DONGLE` → 带屏版，包含 LVGL GUI

切换到 QtPy 后：
- `HAS_DISPLAY` 宏定义为 0
- 编译时跳过所有 LCD/LVGL 相关代码
- `main.cpp` 中的 `#if HAS_DISPLAY` 块全部失效

## 许可证
本适配配置遵循原项目 MIT License。

## 鸣谢
- 原项目: [finger563/esp-usb-ble-hid](https://github.com/finger563/esp-usb-ble-hid)
- ESP-IDF: Espressif Systems
