#include "controller_manager.hpp"

#include <algorithm>

ControllerManager &ControllerManager::instance() {
  static ControllerManager instance;
  return instance;
}

void ControllerManager::add_or_update(const NimBLEAddress &addr, const ControllerState &state) {
  std::lock_guard<std::mutex> lock(mutex_);
  controllers_[addr] = state;

  // If this is the first controller, make it active
  if (!active_controller_.has_value()) {
    active_controller_ = addr;
  }
}

ControllerState *ControllerManager::get(const NimBLEAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = controllers_.find(addr);
  if (it == controllers_.end()) {
    return nullptr;
  }
  return &it->second;
}

void ControllerManager::remove(const NimBLEAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);
  controllers_.erase(addr);

  // If the active controller was removed, switch to another one
  if (active_controller_.has_value() && active_controller_.value() == addr) {
    if (!controllers_.empty()) {
      active_controller_ = controllers_.begin()->first;
    } else {
      active_controller_.reset();
    }
  }
}

std::vector<NimBLEAddress> ControllerManager::get_all_addresses() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<NimBLEAddress> addresses;
  addresses.reserve(controllers_.size());
  for (const auto &[addr, _] : controllers_) {
    addresses.push_back(addr);
  }
  return addresses;
}

size_t ControllerManager::count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return controllers_.size();
}

void ControllerManager::set_active(const NimBLEAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (controllers_.find(addr) != controllers_.end()) {
    active_controller_ = addr;
  }
}

std::optional<NimBLEAddress> ControllerManager::get_active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_controller_;
}

void ControllerManager::auto_switch_on_input(const NimBLEAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!auto_switch_enabled_) {
    return;
  }

  // Only switch if this controller exists and is not already active
  if (controllers_.find(addr) != controllers_.end()) {
    active_controller_ = addr;
  }
}

bool ControllerManager::is_active(const NimBLEAddress &addr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_controller_.has_value() && active_controller_.value() == addr;
}

void ControllerManager::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  controllers_.clear();
  active_controller_.reset();
}
