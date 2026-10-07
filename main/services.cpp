#include "services.hpp"

#include <cerrno>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>

#include "esp_spiffs.h"
#include "esp_system.h"

#include "coredump.hpp"
#include "coredump_service.hpp"
#include "format.hpp"
#include "logger.hpp"
#include "monitor_service.hpp"
#include "nvs.hpp"
#include "ota.hpp"
#include "ota_service.hpp"
#include "system_service.hpp"

#include "usb.hpp"

using namespace std::chrono_literals;

static espp::Logger logger({.tag = "Services", .level = espp::Logger::Verbosity::INFO});

// --- SPIFFS configuration for settings storage -----------------------------------

static constexpr const char *kSpiffsPartition = "user_data";
static constexpr const char *kSpiffsBasePath = "/spiffs";
static constexpr const char *kSettingsFile = "/spiffs/settings.json";

static bool init_spiffs() {
  esp_vfs_spiffs_conf_t conf = {
    .base_path = kSpiffsBasePath,
    .partition_label = kSpiffsPartition,
    .max_files = 5,
    .format_if_mount_failed = true  // Auto-format if mount fails
  };

  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    logger.error("Failed to mount SPIFFS: {} (0x{:x})", esp_err_to_name(ret), ret);

    // Try to format and retry
    logger.warn("Attempting to format SPIFFS partition...");
    ret = esp_spiffs_format(kSpiffsPartition);
    if (ret != ESP_OK) {
      logger.error("Failed to format SPIFFS: {}", esp_err_to_name(ret));
      return false;
    }

    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
      logger.error("Failed to mount SPIFFS after format: {}", esp_err_to_name(ret));
      return false;
    }
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(kSpiffsPartition, &total, &used);
  if (ret == ESP_OK) {
    logger.info("SPIFFS: {} KB total, {} KB used", total / 1024, used / 1024);
  }

  // Verify write capability by creating a test file
  std::ofstream test_file("/spiffs/.test");
  if (!test_file.is_open()) {
    logger.error("SPIFFS mounted but cannot create files - check partition table");
    return false;
  }
  test_file << "test";
  test_file.close();
  std::remove("/spiffs/.test");

  logger.info("SPIFFS initialized successfully and writable");
  return true;
}

// --- NVS for BLE bond names only -------------------------------------------------

static std::unique_ptr<espp::Nvs> nvs_storage;

// --- settings persistence (SPIFFS) -----------------------------------------------

static device_config::Settings load_settings() {
  device_config::Settings s; // defaults

  // Try to read from file
  std::ifstream file(kSettingsFile);
  if (!file.is_open()) {
    logger.warn("Settings file not found, using defaults");
    return s;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string json = buffer.str();
  file.close();

  // Parse JSON (simple key-value pairs)
  // Format: {"swap_ab":1,"swap_xy":0,"deadzone":10,...}
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

static bool save_settings(const device_config::Settings &s, std::string &error) {
  // Build JSON string
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

  // Write to file
  std::ofstream file(kSettingsFile, std::ios::out | std::ios::trunc);
  if (!file.is_open()) {
    // Provide more diagnostic info
    struct stat st;
    bool dir_exists = (stat(kSpiffsBasePath, &st) == 0);
    logger.error("Failed to open {} for writing. SPIFFS dir exists: {}", kSettingsFile, dir_exists);

    // Try to check SPIFFS status
    size_t total = 0, used = 0;
    esp_err_t info_ret = esp_spiffs_info(kSpiffsPartition, &total, &used);
    if (info_ret == ESP_OK) {
      logger.error("SPIFFS status: {} KB total, {} KB used", total / 1024, used / 1024);
    } else {
      logger.error("Cannot get SPIFFS info: {}", esp_err_to_name(info_ret));
    }

    error = fmt::format("Failed to open settings file for writing (errno: {})", errno);
    logger.error("{}", error);
    return false;
  }

  file << json;
  bool write_ok = file.good();
  file.close();

  if (!write_ok) {
    error = "Failed to write settings to file";
    logger.error("{}", error);
    return false;
  }

  // Sync is handled by the VFS layer automatically on close
  // No need for explicit esp_spiffs_sync in newer ESP-IDF versions

  // Verify: read back immediately
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

// --- paired-controller names (NVS) --------------------------------------------------
//
// One namespace, one key per bond: the 12-hex-digit address (NVS keys are
// limited to 15 characters). Values are the controller's name (<= 31 chars).

static constexpr const char *kBondNamesNamespace = "bondnames";

static std::string bond_key(const std::array<uint8_t, 6> &address) {
  return fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", address[0], address[1], address[2],
                     address[3], address[4], address[5]);
}

std::string services_bond_name(const std::array<uint8_t, 6> &address) {
  if (!nvs_storage)
    return {};
  std::string name;
  std::error_code ec;
  nvs_storage->get_var(kBondNamesNamespace, bond_key(address), name, ec);
  return ec ? std::string{} : name; // not found = unknown
}

void services_set_bond_name(const std::array<uint8_t, 6> &address, const std::string &name) {
  if (!nvs_storage || name.empty())
    return;
  if (services_bond_name(address) == name)
    return; // unchanged: spare the flash
  std::error_code ec;
  nvs_storage->set_var(kBondNamesNamespace, bond_key(address), name, ec);
  if (ec)
    logger.warn("Could not store controller name '{}': {}", name, ec.message());
  else
    logger.info("Stored controller name '{}'", name);
}

void services_forget_bond_name(const std::array<uint8_t, 6> &address) {
  if (!nvs_storage)
    return;
  std::error_code ec;
  nvs_storage->erase(kBondNamesNamespace, bond_key(address), ec); // missing key is fine
}

void services_clear_bond_names() {
  if (!nvs_storage)
    return;
  std::error_code ec;
  nvs_storage->erase(kBondNamesNamespace, ec);
}

// --- service instances -------------------------------------------------------------
//
// Each service is an espp DispatcherModuleConcept: it owns its protocol state
// machine and replies through the link's sender; registration is one call.

static std::unique_ptr<espp::Ota> ota;
static std::unique_ptr<espp::OtaService> ota_service;
static std::unique_ptr<espp::CoreDump> core_dump;
static std::unique_ptr<espp::CoreDumpService> coredump_service;
static std::unique_ptr<espp::SystemService> system_service;
static std::unique_ptr<espp::MonitorService> monitor_service;
static std::unique_ptr<DeviceConfig> device_config_module;
static std::string crash_report;

// --- public API -----------------------------------------------------------------------

void services_init(espp::DispatcherWorker *link, const ServicesCallbacks &callbacks) {
  // SPIFFS (settings storage)
  logger.warn("🔄 Initializing SPIFFS...");
  if (!init_spiffs()) {
    logger.error("❌ SPIFFS init failed - settings will NOT persist!");
  } else {
    logger.warn("✅ SPIFFS mounted successfully");
  }

  // NVS (BLE bonds only)
  nvs_storage = std::make_unique<espp::Nvs>();
  std::error_code ec;
  nvs_storage->init(ec);
  if (ec)
    logger.error("NVS init failed: {}", ec.message());
  const auto settings = load_settings();
  // no transport (plain HID build): replies have nowhere to go
  const auto send = link ? link->sender() : [](std::span<const uint8_t>) {};

  // --- OTA (module 0): espp::OtaService drives the engine; rollback is
  // host-driven (the console confirms a PENDING_VERIFY image) ---
  ota = std::make_unique<espp::Ota>(
      espp::Ota::Config{.reject_same_version = false,
                        .progress_callback =
                            [](size_t written, size_t total) {
                              if (total > 0 && (written % (64 * 1024)) < 1024)
                                logger.info("OTA progress: {} / {} bytes", written, total);
                            },
                        .log_level = espp::Logger::Verbosity::INFO});
  const auto running = ota->running_app_description();
  logger.info("Running '{}' {} (built {} {}) from '{}'; next update -> '{}'", running.project_name,
              running.version, running.date, running.time, ota->running_partition_label(),
              ota->update_partition_label());
  if (ota->is_pending_verify())
    logger.warn("This image is PENDING VERIFY (first boot after an update): confirm it from the "
                "dongle console, or it rolls back on the next reset");
  if (link) {
    ota_service = std::make_unique<espp::OtaService>(
        *ota, espp::OtaService::Config{.send = send, .log_level = espp::Logger::Verbosity::INFO});
    link->register_module(*ota_service);
  }

  // --- crash dumps (module 4) ---
  core_dump = std::make_unique<espp::CoreDump>();
  crash_report = core_dump->format_report();
  if (core_dump->has_core_dump())
    logger.warn("A crash core dump is stored:\n{}", crash_report);
  if (link) {
    coredump_service = std::make_unique<espp::CoreDumpService>(
        *core_dump,
        espp::CoreDumpService::Config{.send = send, .log_level = espp::Logger::Verbosity::INFO});
    link->register_module(*coredump_service);
  }

  // --- system (module 7): identity / status, reboot, reboot into the ROM
  // bootloader (download mode: the dongle re-enumerates as the S3 ROM's USB
  // port, so esptool / `idf.py flash` work without touching the BOOT button,
  // handy for a dongle that lives in a Switch). usb_persist stays off: the
  // composite HID + vendor device is not ROM-CDC/DFU-compatible. ---
  if (link) {
    system_service = std::make_unique<espp::SystemService>(espp::SystemService::Config{
        .send = send,
        .on_reboot_request =
            [](espp::SystemService::RebootKind kind) {
              logger.warn("Console requested a {}; allowing it",
                          kind == espp::SystemService::RebootKind::Bootloader
                              ? "reboot into the bootloader"
                              : "reboot");
              return true;
            },
        .log_level = espp::Logger::Verbosity::INFO});
    link->register_module(*system_service);
    // --- monitor (module 8): heap regions + the task table, on request or
    // streamed (needs the FreeRTOS run-time stats, see sdkconfig.defaults) ---
    monitor_service = std::make_unique<espp::MonitorService>(
        espp::MonitorService::Config{.send = send, .log_level = espp::Logger::Verbosity::INFO});
    link->register_module(*monitor_service);
  }

  // --- device configuration (module 0x10) ---
  device_config_module = std::make_unique<DeviceConfig>(DeviceConfig::Config{
      .send = send,
      .initial = settings,
      .on_settings_changed =
          [on_changed = callbacks.on_settings_changed](const device_config::Settings &s,
                                                       std::string &error) {
            if (!save_settings(s, error))
              return false; // not applied either: the module keeps the old values
            if (on_changed)
              on_changed(s);
            return true;
          },
      .info = callbacks.info,
      .on_action = callbacks.on_action,
      .bonds = callbacks.bonds,
      .forget_bond = callbacks.forget_bond,
      .log_level = espp::Logger::Verbosity::INFO});
  if (!link)
    return; // the module still holds + serves the settings for the app
  link->register_module(*device_config_module);

  // discovery: device name + firmware version, answered over the vendor stream
  link->serve_discovery("ESP USB BLE HID", running.version);
#if CONFIG_DONGLE_USB_VENDOR_INTERFACE
  // bytes dropped on the transport: an in-flight image is unusable; the OTA
  // service aborts its session and tells the host (nothing else is stateful)
  usb_set_rx_overflow_callback([] { ota_service->on_rx_overflow(); });
#endif
}

device_config::Settings services_settings() {
  return device_config_module ? device_config_module->settings() : device_config::Settings{};
}

std::string services_crash_report() { return crash_report; }
