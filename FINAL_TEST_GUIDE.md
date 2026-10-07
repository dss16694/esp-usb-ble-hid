# ESP32-S3 USB BLE HID 设置持久化 - 最终测试指南

## 📦 当前固件版本
- **版本**: v3.1.0-dirty (带 SPIFFS 修复)
- **日期**: 2026-10-07
- **关键修复**: 禁用自动格式化，防止每次启动清空 SPIFFS

## 🔧 关键改动

### 改动 1：从 NVS 迁移到 SPIFFS
- **设置文件**: `/spiffs/settings.json`
- **分区**: `user_data` (3.2MB)
- **格式**: JSON

### 改动 2：防止自动格式化
```cpp
// 之前：format_if_mount_failed = true (每次失败都格式化，丢失数据)
// 现在：format_if_mount_failed = false (手动检测，只在首次格式化)
```

### 改动 3：显式刷新到 Flash
```cpp
esp_spiffs_sync(kSpiffsPartition);  // 强制写入 Flash
```

## 🚀 烧录步骤（非常重要！）

### ⚠️ 必须先完全擦除 Flash！

**为什么必须擦除：**
- 旧固件可能在 SPIFFS 分区留下损坏的数据
- SPIFFS 元数据可能与新代码不兼容
- 确保从干净的状态开始

### Windows 命令：

```cmd
REM 步骤 1：完全擦除 Flash（会删除所有数据）
esptool.py --port COM3 erase_flash

REM 步骤 2：烧录完整固件
cd firmware_n16r8
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x11000 ota_data_initial.bin ^
  0x20000 esp-usb-ble-hid.bin

REM 步骤 3：等待烧录完成
REM 应该看到 "Hash of data verified."
```

### Linux/Mac 命令：

```bash
# 步骤 1：完全擦除
esptool.py --port /dev/ttyUSB0 erase_flash

# 步骤 2：烧录
cd firmware_n16r8
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 write_flash \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x11000 ota_data_initial.bin \
  0x20000 esp-usb-ble-hid.bin
```

## 🧪 测试序列

### 测试 1：首次启动

**操作：**
1. 烧录完成后，拔掉并重新插入 USB
2. 打开 `web/dongle_console.html`
3. 按 F12 打开开发者工具
4. 点击 "Connect dongle"

**预期结果：**
- 成功连接
- Device 标签显示设备信息
- Settings 标签可以看到默认设置

---

### 测试 2：保存设置

**操作：**
1. 进入 Settings 标签
2. 修改一个或多个设置：
   - ✅ **推荐测试**: 勾选 "Swap A / B" (容易识别)
   - 可选：修改 "Stick deadzone" 为 15
3. 点击 "Apply changes"

**预期结果：**
- Settings 标签底部显示：
  - ✅ 绿色 "Saved. " 或 "Saved. The BLE name applies after a reboot."
  - ❌ 如果显示红色错误信息，截图告诉我

**Web Console 日志应显示：**
```
11:XX:XX Applied X setting(s): swap_ab, ...
```

---

### 测试 3：立即验证（不重启）

**操作：**
1. 保存设置后，**不要关闭浏览器**
2. 刷新页面 (F5)
3. 重新点击 "Connect dongle"
4. 进入 Settings 标签

**预期结果：**
- ✅ "Swap A / B" 仍然是勾选状态
- ✅ 其他修改的设置也保持

**如果失败：**
- 说明设置根本没被应用到内存
- 这是代码逻辑问题，与 SPIFFS 无关

---

### 测试 4：软重启测试（关键！）

**操作：**
1. 确认设置已保存（测试 2 完成）
2. 进入 Device 标签
3. 点击 "Reboot dongle" 按钮
4. 等待约 5 秒（设备重启）
5. 刷新浏览器页面
6. 重新连接
7. 检查 Settings 标签

**预期结果：**
- ✅ "Swap A / B" 仍然是勾选状态
- ✅ 所有设置都保持

**如果失败：**
- ❌ 设置恢复默认值
- 说明 SPIFFS 写入失败或重启时被清空
- **这是我们要解决的核心问题**

---

### 测试 5：硬断电测试（终极测试！）

**操作：**
1. 确认设置已保存（测试 2 完成）
2. **直接拔掉 USB 线**（完全断电）
3. 等待 10 秒
4. 重新插入 USB 线
5. 打开浏览器，连接 dongle
6. 检查 Settings 标签

**预期结果：**
- ✅ "Swap A / B" 仍然是勾选状态
- ✅ 所有设置都保持

**如果失败：**
- ❌ 设置恢复默认值
- 说明 SPIFFS 数据在断电后丢失
- 可能是 Flash 硬件问题或其他原因

---

### 测试 6：多次修改测试

**操作：**
1. 修改 "Swap A / B" 为勾选，保存
2. 重启（Reboot dongle）
3. 确认保持
4. 修改 "Swap A / B" 为不勾选，保存
5. 重启
6. 确认保持

**预期结果：**
- ✅ 每次修改都能正确保存和恢复

---

## 📊 结果分析

### 场景 A：所有测试都通过 ✅
**恭喜！** 问题已解决，SPIFFS 工作正常。

### 场景 B：测试 3 失败，其他未测试 ❌
**问题：** 设置没有应用到内存
**解决：** 检查 Web Console 是否有错误信息

### 场景 C：测试 3 通过，测试 4 失败 ❌
**问题：** 软重启后设置丢失
**原因：** SPIFFS 写入失败或被清空

**下一步诊断：**
1. 在 Web Console 的浏览器 Console 中查看是否有错误
2. 重复测试 2，截图保存时的提示信息
3. 如果有 USB 转 TTL 模块，连接 GPIO43 查看真实日志

### 场景 D：测试 4 通过，测试 5 失败 ❌
**问题：** 断电后设置丢失
**原因：** `esp_spiffs_sync()` 可能失败，或 Flash 写入未完成

**可能的解决方案：**
1. 降低 Flash 速度（从 80MHz 改为 40MHz）
2. 增加延迟，确保写入完成
3. 实现双备份机制

---

## 🔍 故障排除

### 问题：Web Console 显示红色错误

**错误示例：**
- "Failed to open settings file for writing"
- "SPIFFS sync failed: ESP_ERR_xxx"
- "VERIFICATION FAILED"

**解决方案：**
1. 再次完全擦除并重新烧录
2. 检查 Flash 是否有硬件问题

---

### 问题：无法连接到 dongle

**检查：**
1. 设备管理器中是否出现新设备
2. 浏览器是否支持 WebUSB (Chrome/Edge)
3. 是否在 HTTPS 或 localhost 下打开页面

---

### 问题：需要查看 ESP32 的真实日志

**方案 1：使用 USB 转 TTL 模块**
```
购买：淘宝搜 "CP2102 USB 转 TTL"（10-20 元）
连接：
  USB转TTL RX  → ESP32-S3 GPIO43 (TX)
  USB转TTL GND → ESP32-S3 GND
查看：PuTTY, 115200 波特率
```

**方案 2：我修改代码启用 USB CDC**
需要更多代码改动，但可以通过 USB 线直接查看日志

---

## 📝 测试记录表

请填写测试结果：

| 测试项 | 操作 | 预期 | 实际结果 | 通过? |
|--------|------|------|----------|-------|
| 测试 1 | 首次启动 | 成功连接 | | ☐ |
| 测试 2 | 保存设置 | 显示 "Saved" | | ☐ |
| 测试 3 | 刷新重连 | 设置保持 | | ☐ |
| 测试 4 | Reboot | 设置保持 | | ☐ |
| 测试 5 | 断电 | 设置保持 | | ☐ |
| 测试 6 | 多次修改 | 每次保持 | | ☐ |

---

## 🎯 下一步

根据测试结果：

1. **所有通过** → 问题解决，可以正常使用
2. **测试 4 或 5 失败** → 告诉我具体哪个失败，以及 Web Console 的错误信息
3. **需要日志** → 我可以帮你编译一个真正启用 USB CDC 的版本

---

## 📞 需要帮助？

提供以下信息：
1. 哪个测试失败了
2. Web Console 底部的提示（绿色/红色）
3. 浏览器 Console (F12) 的错误信息截图
4. 是否完全擦除了 Flash

我会根据这些信息进一步诊断！
