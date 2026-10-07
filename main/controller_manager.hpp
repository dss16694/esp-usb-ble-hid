#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <NimBLEDevice.h>

#include "gamepad_device.hpp"

/// State for a single connected controller
struct ControllerState {
  NimBLEClient *client{nullptr};
  NimBLEAddress address;
  bool subscribed{false};
  bool authenticated{false};
  int64_t connected_at_us{0};
  int64_t last_notify_us{-1};
  int64_t last_secure_request_us{0};
  uint32_t notification_count{0};
  uint8_t subscribe_attempts{0};
  std::string name;
  uint8_t battery_level{100};
  GamepadInputs last_inputs{};
  std::unordered_map<const NimBLERemoteCharacteristic *, uint8_t> report_ids;
  std::string link_detail;
};

/// Manages multiple connected controllers
class ControllerManager {
public:
  static ControllerManager &instance();

  /// Add or update a controller's state
  void add_or_update(const NimBLEAddress &addr, const ControllerState &state);

  /// Get a controller's state (returns nullptr if not found)
  ControllerState *get(const NimBLEAddress &addr);

  /// Remove a controller
  void remove(const NimBLEAddress &addr);

  /// Get all connected controllers
  std::vector<NimBLEAddress> get_all_addresses();

  /// Get the number of connected controllers
  size_t count() const;

  /// Set the active controller (the one whose inputs are forwarded to USB)
  void set_active(const NimBLEAddress &addr);

  /// Get the active controller address
  std::optional<NimBLEAddress> get_active() const;

  /// Auto-switch to a controller when it has input
  void auto_switch_on_input(const NimBLEAddress &addr);

  /// Check if a controller is the active one
  bool is_active(const NimBLEAddress &addr) const;

  /// Clear all controllers
  void clear();

private:
  ControllerManager() = default;
  mutable std::mutex mutex_;
  std::map<NimBLEAddress, ControllerState> controllers_;
  std::optional<NimBLEAddress> active_controller_;
  bool auto_switch_enabled_{true};
};
