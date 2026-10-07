# SPIFFS 设置持久化问题排查

## 问题描述

1. **没有 COM 设备出现** - USB CDC 控制台未生效
2. **设置总是重置** - 无论 reboot 还是断电，设置都会恢复默认值

## 诊断步骤

### 步骤 1：通过 Web Console 查看错误信息

1. 打开 `web/dongle_console.html`
2. 连接 dongle
3. 打开浏览器的开发者工具（F12）
4. 进入 Console 标签
5. 修改一个设置并点击 "Apply changes"
6. 查看是否有错误信息：
   - `"Apply failed: xxx"` - 保存失败
   - `"Dongle rejected the settings: xxx"` - 设备拒绝

### 步骤 2：检查 SPIFFS 是否挂载成功

由于无法看到串口日志，我们需要通过其他方式验证：

**方案 A：添加一个测试 Action**

在 Web Console 中，可以看到 Actions 区域有：
- Start pairing
- Forget all controllers  
- Reboot dongle

如果 SPIFFS 挂载失败，保存设置时应该会在 Web Console 显示错误。

**方案 B：使用外部 USB 转 TTL 模块**

如果你有 USB 转 TTL 模块（CP2102/CH340/FT232）：

连接方式：
```
USB转TTL     ESP32-S3
  GND    →     GND
  RX     →     GPIO43 (UART TX)
```

然后用 PuTTY 连接查看日志。

### 步骤 3：可能的原因分析

#### 原因 1：SPIFFS 分区未初始化

**症状：** 首次烧录后 SPIFFS 挂载失败

**解决：** 完全擦除 Flash
```bash
esptool.py --port COM3 erase_flash
# 然后重新烧录完整固件
```

#### 原因 2：文件写入失败但未显示错误

**可能性：** 
- SPIFFS 空间已满（不太可能，3.2MB 很大）
- 文件系统损坏
- 权限问题

**验证：** 在 Web Console 的 Settings 标签保存设置时，注意右下角的提示信息：
- 绿色 "Saved" - 表示保存成功
- 红色错误信息 - 表示失败原因

#### 原因 3：`esp_spiffs_sync()` 失败

**可能性：** Flash 硬件问题或分区配置错误

**验证方法：** 如果有 USB 转 TTL，在保存设置后应该看到：
```
✅ Settings saved and verified: swap_ab=1, ...
```

如果看到错误，会显示具体原因。

#### 原因 4：`load_settings()` 总是读取不到文件

**可能性：** 
- 文件路径错误
- SPIFFS 每次启动都被格式化（`format_if_mount_failed = true`）

### 步骤 4：临时调试固件

我可以为你编译一个特殊的调试版本，它会：
1. 在启动时创建一个测试文件 `/spiffs/test.txt`
2. 每次保存设置时，同时写入一个时间戳文件
3. 在 Web Console 的 Device Info 中显示 SPIFFS 状态

要不要我创建这个调试版本？

### 步骤 5：硬件 UART 连接（推荐）

这是最可靠的诊断方法：

**所需硬件：**
- USB 转 TTL 模块（约 10-20 元）

**淘宝搜索：**
- "CP2102 USB 转 TTL"
- "CH340 USB 转 TTL"
- "FT232 USB 转 TTL"

**连接后能看到：**
```
🔄 Initializing SPIFFS...
SPIFFS: 3276 KB total, 0 KB used
✅ SPIFFS mounted successfully
Settings loaded from file: swap_ab=0, swap_xy=0, deadzone=10
```

或者错误信息：
```
❌ SPIFFS init failed - settings will NOT persist!
Failed to mount SPIFFS: ESP_ERR_NOT_FOUND
```

## 快速测试方案

### 测试 1：验证设置是否真的被发送到设备

1. 打开 `web/dongle_console.html`
2. F12 打开开发者工具 → Network 标签
3. 修改设置并保存
4. 查看是否有 WebUSB 数据传输
5. 查看 Console 标签是否有 JavaScript 错误

### 测试 2：验证设备是否返回了错误

在 Settings 标签保存设置后：
- 绿色提示 = 设备确认保存成功
- 红色提示 = 设备返回错误（查看具体错误信息）
- 无提示/卡住 = 通信问题

### 测试 3：多次保存同一个设置

1. 修改 "Swap A/B"
2. 保存
3. 立即刷新页面重新连接
4. 检查设置是否保持

如果**立即重连设置保持**，但**断电后丢失**：
- 说明内存中的设置正常
- 但 SPIFFS 写入可能有问题

如果**立即重连也丢失**：
- 说明保存根本没成功
- 或者 `load_settings()` 有问题

## 我的建议

1. **立即尝试：** 打开 Web Console，保存设置，截图给我看提示信息
2. **短期方案：** 购买 USB 转 TTL 模块（10-20 元，1-2 天到货）
3. **长期方案：** 我可以编译一个 USB CDC 真正可用的版本

你想先尝试哪个？
