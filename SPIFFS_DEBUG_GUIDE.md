# SPIFFS 设置持久化调试指南

## 已完成的重构

### 1. SPIFFS 初始化
- 位置：`main/services.cpp` 中的 `init_spiffs()`
- 挂载点：`/spiffs`
- 分区：`user_data` (0xC60000, 3.2MB)
- 配置：`format_if_mount_failed = true`

### 2. 设置读取
- 函数：`load_settings()`
- 文件：`/spiffs/settings.json`
- 格式：简单的 JSON 键值对
- 时机：`services_init()` 启动时调用

### 3. 设置保存
- 函数：`save_settings()`
- 文件：`/spiffs/settings.json`
- 验证：立即读回并比对
- 时机：Web Console 修改设置时通过 `on_settings_changed_` 回调触发

## 调试步骤

### 步骤 1：检查启动日志

烧录新固件后，查看串口日志，应该看到：

```
🔄 Initializing SPIFFS...
SPIFFS: XXXX KB total, XXXX KB used
✅ SPIFFS mounted successfully
```

如果看到 `❌ SPIFFS init failed`，说明 SPIFFS 挂载失败。

### 步骤 2：测试设置保存

1. 通过 Web Console 连接 dongle
2. 修改任意设置（例如 swap_ab）
3. 点击 "Apply changes"
4. 查看串口日志，应该看到：

```
✅ Settings saved and verified: swap_ab=1, swap_xy=0, deadzone=10
```

如果看到 `VERIFICATION FAILED`，说明文件写入有问题。

### 步骤 3：测试断电持久化

1. 修改设置并保存（确认看到 ✅ 日志）
2. 拔掉 dongle 电源（完全断电）
3. 重新上电
4. 查看启动日志：

```
Settings loaded from file: swap_ab=1, swap_xy=0, deadzone=10
```

如果看到 `Settings file not found, using defaults`，说明文件丢失。

### 步骤 4：手动检查文件系统

可以在代码中添加调试代码来列出 SPIFFS 文件：

```cpp
// 在 init_spiffs() 后添加
DIR *dir = opendir("/spiffs");
if (dir) {
  struct dirent *entry;
  logger.warn("📁 SPIFFS files:");
  while ((entry = readdir(dir)) != nullptr) {
    logger.warn("  - {}", entry->d_name);
  }
  closedir(dir);
}
```

## 常见问题和解决方案

### 问题 1：SPIFFS 挂载失败

**症状：** 启动时看到 `❌ SPIFFS init failed`

**可能原因：**
- 分区表没有正确烧录
- Flash 损坏

**解决方案：**
```bash
# 完全擦除 Flash 并重新烧录
esptool.py --port /dev/ttyUSB0 erase_flash
# 重新烧录固件（会自动烧录分区表）
```

### 问题 2：文件写入失败

**症状：** 保存设置时看到 `Failed to open settings file for writing`

**可能原因：**
- SPIFFS 空间已满（不太可能，3.2MB 很大）
- 文件系统损坏

**解决方案：**
- 格式化 SPIFFS 分区重新开始

### 问题 3：断电后文件丢失

**症状：** 
- 保存成功（看到 ✅）
- 重启后文件不存在（`Settings file not found`）

**可能原因：**
1. **SPIFFS 缓存未刷新** - `file.close()` 应该会刷新，但可能需要显式调用
2. **断电时机** - 在文件写入过程中断电
3. **Flash 写入问题** - 硬件问题

**解决方案 A：添加显式 flush**

在 `save_settings()` 中：

```cpp
file << json;
file.flush();  // 添加这一行
file.close();
```

**解决方案 B：添加 SPIFFS 显式提交**

```cpp
file.close();
esp_spiffs_sync(kSpiffsPartition);  // 添加这一行
```

### 问题 4：format_if_mount_failed 导致数据丢失

**症状：** 每次启动都自动格式化

**可能原因：**
- SPIFFS 元数据损坏
- 首次烧录时分区未初始化

**解决方案：**

将 `format_if_mount_failed` 改为 `false`，手动观察挂载失败原因：

```cpp
esp_vfs_spiffs_conf_t conf = {
  .base_path = kSpiffsBasePath,
  .partition_label = kSpiffsPartition,
  .max_files = 5,
  .format_if_mount_failed = false  // 改为 false
};
```

如果挂载失败，手动格式化一次：

```cpp
if (ret == ESP_FAIL) {
  logger.warn("SPIFFS not formatted, formatting now...");
  esp_spiffs_format(kSpiffsPartition);
  ret = esp_vfs_spiffs_register(&conf);
}
```

## 推荐的增强方案

### 1. 双备份机制

在 `save_settings()` 中实现双备份：

```cpp
static constexpr const char *kSettingsFile = "/spiffs/settings.json";
static constexpr const char *kSettingsBackup = "/spiffs/settings.json.bak";

// 保存时：先写备份，再覆盖主文件
// 读取时：如果主文件损坏，使用备份
```

### 2. 增加写入日志

在关键操作添加更详细的日志：

```cpp
logger.warn("💾 Writing settings to {}...", kSettingsFile);
// ... 写入 ...
logger.warn("✅ Settings file written: {} bytes", json.length());
// ... 验证 ...
logger.warn("✅ Settings verified successfully");
```

### 3. 添加 CRC 校验

在 JSON 中添加 CRC 字段，确保文件完整性：

```json
{
  "swap_ab": 1,
  "swap_xy": 0,
  "crc": "a1b2c3d4"
}
```

## 测试清单

- [ ] 首次烧录后 SPIFFS 挂载成功
- [ ] 设置保存后立即验证成功
- [ ] 软重启（Reboot）后设置保留
- [ ] 硬重启（拔电源）后设置保留
- [ ] 连续多次修改设置都能保存
- [ ] 断电后重新上电，文件仍然存在

## 当前代码位置

- **SPIFFS 初始化**: `main/services.cpp:36-54`
- **设置加载**: `main/services.cpp:59-91`
- **设置保存**: `main/services.cpp:129-190`
- **服务初始化**: `main/services.cpp:256-271`
- **CMakeLists.txt**: `main/CMakeLists.txt:3`

## 下一步

1. **先测试基本功能**：烧录固件，修改设置，查看日志
2. **如果断电后丢失**：添加 `esp_spiffs_sync()` 显式刷新
3. **如果仍有问题**：实现双备份机制
4. **长期方案**：添加 CRC 校验和更详细的错误处理
