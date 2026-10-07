# 设置持久化方案：从 NVS 迁移到 SPIFFS

## 问题总结

NVS 持久化一直失败，断电后设置恢复默认值。根本原因：
- `ESP_ERR_NVS_NEW_VERSION_FOUND` 导致每次启动自动擦除 NVS
- 即使阻止自动擦除，NVS 仍不稳定

## 解决方案：使用 SPIFFS 文件系统

### 资源情况

分区表中已有 `user_data` 分区：
- **位置**: 0xC60000
- **大小**: 3.2MB (0x3A0000)
- **类型**: SPIFFS
- **当前状态**: 未使用

### 实现步骤

#### 1. 在 `main/services.cpp` 中添加 SPIFFS 初始化

```cpp
#include "esp_spiffs.h"
#include <fstream>
#include <sstream>

// SPIFFS 配置
static constexpr const char *kSpiffsPartition = "user_data";
static constexpr const char *kSpiffsBasePath = "/spiffs";
static constexpr const char *kSettingsFile = "/spiffs/settings.json";

static bool init_spiffs() {
  esp_vfs_spiffs_conf_t conf = {
    .base_path = kSpiffsBasePath,
    .partition_label = kSpiffsPartition,
    .max_files = 5,
    .format_if_mount_failed = true
  };

  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    logger.error("Failed to mount SPIFFS: {}", esp_err_to_name(ret));
    return false;
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(kSpiffsPartition, &total, &used);
  if (ret == ESP_OK) {
    logger.info("SPIFFS: {} KB total, {} KB used", total / 1024, used / 1024);
  }
  return true;
}
```

#### 2. 修改 `load_settings()` - 从文件读取

```cpp
static device_config::Settings load_settings() {
  device_config::Settings s; // defaults

  // 尝试从文件读取
  std::ifstream file(kSettingsFile);
  if (!file.is_open()) {
    logger.warn("Settings file not found, using defaults");
    return s;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string json = buffer.str();
  file.close();

  // 解析 JSON（简单的键值对）
  // 格式: {"swap_ab":1,"swap_xy":0,"deadzone":10,...}
  auto parse_bool = [&](const std::string &key) -> bool {
    size_t pos = json.find("\"" + key + "\":");
    if (pos != std::string::npos) {
      char val = json[pos + key.length() + 3];
      return (val == '1' || val == 't');
    }
    return false;
  };

  auto parse_int = [&](const std::string &key) -> int {
    size_t pos = json.find("\"" + key + "\":");
    if (pos != std::string::npos) {
      size_t start = pos + key.length() + 3;
      size_t end = json.find_first_of(",}", start);
      return std::stoi(json.substr(start, end - start));
    }
    return 0;
  };

  auto parse_string = [&](const std::string &key) -> std::string {
    size_t pos = json.find("\"" + key + "\":\"");
    if (pos != std::string::npos) {
      size_t start = pos + key.length() + 4;
      size_t end = json.find("\"", start);
      return json.substr(start, end - start);
    }
    return "";
  };

  s.invert_left_y = parse_bool("inv_ly");
  s.invert_right_y = parse_bool("inv_ry");
  s.swap_ab = parse_bool("swap_ab");
  s.swap_xy = parse_bool("swap_xy");
  s.deadzone_percent = parse_int("deadzone");
  s.led_brightness = parse_int("led");
  s.ble_name = parse_string("ble_name");
  s.led_connected_brightness = parse_int("led_conn");
  s.led_activity_blink = parse_bool("led_blink");

  logger.info("Settings loaded from file: swap_ab={}, swap_xy={}, deadzone={}",
              s.swap_ab, s.swap_xy, s.deadzone_percent);

  if (auto why = s.validate(); !why.empty()) {
    logger.warn("Stored settings invalid ({}); using defaults", why);
    s = device_config::Settings{};
  }
  return s;
}
```

#### 3. 修改 `save_settings()` - 写入文件

```cpp
static bool save_settings(const device_config::Settings &s, std::string &error) {
  // 构建 JSON 字符串
  std::string json = fmt::format(
    "{{"
    "\"inv_ly\":{},"
    "\"inv_ry\":{},"
    "\"swap_ab\":{},"
    "\"swap_xy\":{},"
    "\"deadzone\":{},"
    "\"led\":{},"
    "\"ble_name\":\"{}\"," 
    "\"led_conn\":{},"
    "\"led_blink\":{}"
    "}}",
    s.invert_left_y ? 1 : 0,
    s.invert_right_y ? 1 : 0,
    s.swap_ab ? 1 : 0,
    s.swap_xy ? 1 : 0,
    s.deadzone_percent,
    s.led_brightness,
    s.ble_name,
    s.led_connected_brightness,
    s.led_activity_blink ? 1 : 0
  );

  // 写入文件
  std::ofstream file(kSettingsFile);
  if (!file.is_open()) {
    error = "Failed to open settings file for writing";
    logger.error("{}", error);
    return false;
  }

  file << json;
  file.close();

  // 验证：立即读回
  std::ifstream verify_file(kSettingsFile);
  if (!verify_file.is_open()) {
    error = "VERIFICATION FAILED: could not read back settings file";
    logger.error("{}", error);
    return false;
  }

  std::stringstream buffer;
  buffer << verify_file.rdbuf();
  std::string verify_json = buffer.str();
  verify_file.close();

  if (verify_json != json) {
    error = "VERIFICATION FAILED: readback mismatch";
    logger.error("{}", error);
    return false;
  }

  logger.warn("✅ Settings saved and verified: swap_ab={}, swap_xy={}, deadzone={}",
              s.swap_ab, s.swap_xy, s.deadzone_percent);
  return true;
}
```

#### 4. 修改 `services_init()` - 初始化 SPIFFS

```cpp
void services_init(espp::DispatcherWorker *link, const ServicesCallbacks &callbacks) {
  // SPIFFS (settings storage)
  logger.warn("🔄 Initializing SPIFFS...");
  if (!init_spiffs()) {
    logger.error("❌ SPIFFS init failed - settings will NOT persist!");
  } else {
    logger.warn("✅ SPIFFS mounted successfully");
  }

  const auto settings = load_settings();
  
  // ... 其余代码保持不变
}
```

#### 5. 移除 NVS 相关代码

可以完全移除：
- `nvs_storage` 变量
- `kNvsNamespace` 
- NVS 初始化代码
- BLE bonds 名称存储（如果不需要，或迁移到 SPIFFS）

### CMakeLists.txt 修改

确保链接 SPIFFS 组件：

```cmake
idf_component_register(
    SRCS "main.cpp" "services.cpp" "usb.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        # ... 其他组件
        spiffs  # 添加这一行
)
```

### 优势

1. ✅ **完全绕过 NVS 问题**
2. ✅ **文件系统成熟可靠**
3. ✅ **JSON 格式人类可读**，方便调试
4. ✅ **3.2MB 空间充足**
5. ✅ **支持备份/导出**（未来可扩展）

### 注意事项

1. **首次烧录**：SPIFFS 会自动格式化
2. **断电保护**：文件系统写入默认有缓存，`close()` 会刷新
3. **性能**：读写比 NVS 慢，但对配置文件完全够用
4. **BLE Bonds**：如需保留配对信息，也可迁移到 SPIFFS

### 测试步骤

1. 擦除 Flash：`esptool.py erase_flash`
2. 烧录新固件
3. 首次启动会创建 `/spiffs/settings.json`
4. 修改设置，文件会更新
5. 断电重启，从文件读取设置

### 进一步优化（可选）

如果需要更高可靠性：
- 使用**双备份**：`settings.json` + `settings.json.bak`
- 写入时先写备份，再覆盖主文件
- 读取时如果主文件损坏，使用备份

## 实施建议

1. 先实现基础版本（上述步骤 1-4）
2. 测试验证持久化是否工作
3. 如果成功，再考虑优化（双备份等）

## 预期结果

- ✅ 设置断电后保留
- ✅ 可通过文件系统工具查看 `/spiffs/settings.json`
- ✅ 不再依赖 NVS
