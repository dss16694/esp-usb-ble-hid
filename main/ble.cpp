#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "esp_timer.h"

#include "ble.hpp"
#include "bsp.hpp"
#include "controller_manager.hpp"
#include "format.hpp"
#include "status_led.hpp"

#include "gaussian.hpp"

/************* BLE Configuration ****************/

static uint32_t scanTimeMs = 5000; // scan time in milliseconds, 0 = scan forever
static std::unique_ptr<espp::Timer> scanTimer;

// a connection attempt is in flight (the scan was stopped for it): the
// supervisor must not restart the scan underneath it
static std::atomic<bool> connect_pending{false};
static std::atomic<int64_t> connect_started_us{0};
// a pairing scan (accept any controller) stays in effect until this time; after
// that a rescan goes back to reconnecting bonded controllers only
static std::atomic<int64_t> pairing_until_us{0};

// Global link state (simplified for multi-controller: reports state of active controller)
static std::atomic<BleLinkState> link_state{BleLinkState::Idle};
static std::mutex link_detail_mutex;
static std::string link_detail;

static void set_link_detail(std::string detail) {
  std::lock_guard<std::mutex> lk(link_detail_mutex);
  link_detail = std::move(detail);
}

static NimBLEUUID hid_service_uuid(espp::HidService::SERVICE_UUID);
static NimBLEUUID hid_input_uuid(espp::HidService::REPORT_UUID);
static NimBLEUUID report_reference_uuid(espp::HidService::REPORT_DESCRIPTOR_UUID); // 0x2908

static NimBLEUUID battery_service_uuid(espp::BatteryService::BATTERY_SERVICE_UUID);
static NimBLEUUID battery_level_uuid(espp::BatteryService::BATTERY_LEVEL_CHAR_UUID);

static std::atomic<bool> is_pairing{true};
static notify_callback_t notify_callback = nullptr;
static disconnect_callback_t disconnect_callback = nullptr;

// The controller's name, reported to the app once it is connected + subscribed
// (see ble_set_bond_name_callback). The advertised name is captured when we
// decide to connect (scan task) and used as the fallback if the GAP Device Name
// cannot be read (scan-timer task), hence the mutex.
static bond_name_callback_t bond_name_callback = nullptr;
static std::mutex advertised_name_mutex;
static std::string advertised_name;

static NimBLEUUID generic_access_service_uuid(espp::GenericAccessService::SERVICE_UUID);
static NimBLEUUID device_name_uuid(espp::GenericAccessService::NAME_CHAR_UUID);

static espp::Logger ble_logger({.tag = "BLE", .level = espp::Logger::Verbosity::INFO});

// connection parameters requested once the link is up / encrypted
static constexpr uint16_t min_conn_interval = 12;    // 1.25ms units = 15ms
static constexpr uint16_t max_conn_interval = 12;    // 1.25ms units = 15ms
static constexpr uint16_t conn_latency = 4;          // 4 packets at 15ms = 60ms
static constexpr uint16_t supervision_timeout = 400; // 4s

// link bring-up timing (all driven by the 100 ms scan timer)
static constexpr int64_t kAuthTimeoutUs = 15 * 1000 * 1000;    // give up encrypting after this
static constexpr int64_t kSecureRetryUs = 3 * 1000 * 1000;     // re-request security this often
static constexpr int64_t kConnectTimeoutUs = 35 * 1000 * 1000; // > NimBLE's 30 s connect timeout
static constexpr int64_t kScanRetryUs = 500 * 1000;            // don't hammer a failing scan start
static constexpr int64_t kPairingWindowUs = 30 * 1000 * 1000;  // a pairing scan accepts new
                                                               // controllers for this long
static constexpr uint8_t kMaxSubscribeAttempts = 10;           // ~500 ms apart

// Read the connected controller's name: GAP Device Name, else the name it
// advertised, else "". Control characters are stripped and the result capped
// so it is safe to store and display.
static std::string read_controller_name(NimBLEClient *client) {
  std::string name;
  if (auto *gap = client->getService(generic_access_service_uuid)) {
    if (auto *chr = gap->getCharacteristic(device_name_uuid); chr && chr->canRead()) {
      name = chr->readValue();
    }
  }
  if (name.empty()) {
    std::lock_guard<std::mutex> lk(advertised_name_mutex);
    name = advertised_name;
  }
  std::string clean;
  for (const char c : name) {
    if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F)
      clean.push_back(c);
    if (clean.size() >= 31)
      break;
  }
  return clean;
}

// Forget everything about the current link. Called from the NimBLE host task
// Helper: get controller state from client
static ControllerState *get_controller_by_client(NimBLEClient *client) {
  if (!client)
    return nullptr;
  return ControllerManager::instance().get(client->getPeerAddress());
}

// Helper: get controller state from characteristic
static ControllerState *get_controller_by_characteristic(const NimBLERemoteCharacteristic *chr) {
  if (!chr || !chr->getRemoteService())
    return nullptr;
  auto *client = chr->getRemoteService()->getClient();
  if (!client)
    return nullptr;
  return ControllerManager::instance().get(client->getPeerAddress());
}

// Reset state for a specific controller (called on disconnect)
static void reset_controller_state(const NimBLEAddress &addr, const std::string &why) {
  auto *state = ControllerManager::instance().get(addr);
  if (!state)
    return;

  const bool was_subscribed = state->subscribed;
  state->subscribed = false;
  state->authenticated = false;
  state->subscribe_attempts = 0;
  state->report_ids.clear();
  state->link_detail = why;

  if (was_subscribed) {
    ble_logger.info("controller {} link down ({}); {} notifications received",
                    addr.toString(), why, state->notification_count);
  }

  // Remove from manager
  ControllerManager::instance().remove(addr);

  // If this was the only/active controller, notify app
  if (was_subscribed && ControllerManager::instance().count() == 0) {
    if (disconnect_callback)
      disconnect_callback();
  }

  // Update global link state
  if (ControllerManager::instance().count() == 0) {
    link_state = BleLinkState::Idle;
  }
}

// Legacy function for compatibility - resets active controller
static void reset_link_state(const std::string &why) {
  auto active = ControllerManager::instance().get_active();
  if (active.has_value()) {
    reset_controller_state(active.value(), why);
  }
}

// Every subscribed characteristic notifies through here: count it if it is a
// HID input report (the battery level also notifies through here, and must not
// keep the "controller is sending inputs" diagnostics fresh), then hand it to
// the app.
static void on_notify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool is_notify) {
  auto *state = get_controller_by_characteristic(chr);
  if (!state)
    return;

  // Check if this is an input report for this controller
  bool is_input_report = state->report_ids.contains(chr);

  if (is_input_report) {
    state->notification_count++;
    state->last_notify_us = esp_timer_get_time();
  }

  if (notify_callback)
    notify_callback(chr, data, len, is_notify);
}

// LED configuration for BLE pairing / reconnecting
static constexpr float pairing_breathing_period = 1.0f;
static constexpr float reconnecting_breathing_period = 3.0f;

static float breathing_period = reconnecting_breathing_period;
static auto breathing_start = std::chrono::high_resolution_clock::now();
static espp::Gaussian gaussian({.gamma = 0.1f, .alpha = 1.0f, .beta = 0.5f});
static auto breathe = []() -> float {
  auto now = std::chrono::high_resolution_clock::now();
  auto elapsed = std::chrono::duration<float>(now - breathing_start).count();
  float t = std::fmod(elapsed, breathing_period) / breathing_period;
  return gaussian(t);
};
static auto led_callback = [](auto &m, auto &cv) -> bool {
  using namespace std::chrono_literals;
  static espp::Rgb led_color(0.0f, 0.0f, 1.0f); // blue
  espp::Hsv hsv = led_color.hsv();
  hsv.v = breathe();
  set_led(hsv);
  std::unique_lock<std::mutex> lk(m);
  cv.wait_for(lk, 10ms);
  return false;
};
static auto led_task =
    espp::Task::make_unique({.callback = led_callback, .task_config = {.name = "breathe"}});

// The LED is owned by the scan timer (the link supervisor): it is the only
// place that starts / stops the breathing task and sets the steady "connected"
// level, so a connect or disconnect delivered on the BLE host task can never
// leave the LED in the wrong state. (The optional per-input blink in the app's
// notify callback is the one exception, and only while that setting is on.)
// What the supervisor last wrote while connected: a steady value (>= 0), the
// blink baseline (kLedBlinkBaseline: LED cleared once so the per-input toggle
// starts from a defined state), or nothing since the last scan (kLedNothing).
static constexpr float kLedNothing = -1.0f;
static constexpr float kLedBlinkBaseline = -2.0f;
static float led_connected_applied = kLedNothing;
static uint32_t led_blink_writes_seen = 0;

static void set_led_breathing(bool breathing) {
  if (breathing == led_task->is_running())
    return;
  if (breathing) {
    breathing_start = std::chrono::high_resolution_clock::now();
    led_connected_applied = kLedNothing;
    led_task->start();
  } else {
    led_task->stop();
  }
}

// While a controller is connected: hold the LED at the configured level, and
// re-apply it when the settings change. With the activity blink enabled the
// app drives the LED per report instead; the supervisor only clears the LED
// once when that mode starts (so the toggle begins from a defined state) and
// otherwise stays out of the way.
static void update_led_connected() {
  if (led_activity_blink()) {
    if (led_connected_applied != kLedBlinkBaseline) {
      static const espp::Rgb off(0.0f, 0.0f, 0.0f);
      set_led(off);
      led_connected_applied = kLedBlinkBaseline;
    }
    return;
  }
  // The app's blink write runs on another task: one that saw the setting still
  // on can land after our steady write. Any blink write since we last wrote
  // means the steady level must be written again.
  const uint32_t blink_writes = g_led_blink_writes.load();
  const float value = led_connected_value();
  if (value == led_connected_applied && blink_writes == led_blink_writes_seen)
    return;
  led_connected_applied = value;
  led_blink_writes_seen = blink_writes;
  show_led_connected();
}

class ClientCallbacks : public NimBLEClientCallbacks {
  espp::Logger logger =
      espp::Logger({.tag = "BLE Client Callbacks", .level = espp::Logger::Verbosity::INFO});
  void onConnect(NimBLEClient *pClient) override {
    logger.info("connected to: {}", pClient->getPeerAddress().toString());
    connect_pending = false;

    const int64_t now = esp_timer_get_time();
    const auto &addr = pClient->getPeerAddress();

    // Create or update controller state
    ControllerState state;
    state.client = pClient;
    state.address = addr;
    state.authenticated = false;
    state.subscribed = false;
    state.subscribe_attempts = 0;
    state.connected_at_us = now;
    state.last_secure_request_us = now;
    state.link_detail = "connected; waiting for the link to be encrypted";

    ControllerManager::instance().add_or_update(addr, state);

    // Update global link state for active controller
    if (ControllerManager::instance().is_active(addr)) {
      link_state = BleLinkState::Encrypting;
      set_link_detail(state.link_detail);
    }

    static constexpr bool async = true;
    // set the connection parameters now that we've connected
    pClient->setConnectionParams(min_conn_interval, max_conn_interval, conn_latency,
                                 supervision_timeout);
    // bond / secure the connection (a bonded controller re-encrypts with the
    // stored key; a new one pairs). If the controller already started this,
    // the request is simply refused; the scan timer re-issues it if nothing
    // happens.
    if (!pClient->secureConnection(async)) {
      logger.warn("security request not accepted (already in progress?)");
    }
  }

  void onConnectFail(NimBLEClient *pClient, int reason) override {
    logger.warn("connection to {} failed, reason = {}", pClient->getPeerAddress().toString(),
                reason);
    connect_pending = false;
    set_link_detail(fmt::format("connection attempt failed (reason {})", reason));
    // the scan timer restarts the scan
  }

  void onDisconnect(NimBLEClient *pClient, int reason) override {
    logger.info("{} disconnected, reason = {}", pClient->getPeerAddress().toString(), reason);
    connect_pending = false;
    // drop the controller state; the scan timer restarts the scan (and the LED) within one period
    reset_controller_state(pClient->getPeerAddress(),
                          fmt::format("disconnected (reason {})", reason));
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    auto *pClient = NimBLEDevice::getClientByHandle(connInfo.getConnHandle());
    if (!pClient)
      return;

    auto *state = get_controller_by_client(pClient);
    if (!state)
      return;

    if (!connInfo.isEncrypted()) {
      logger.error("Encrypt connection failed - disconnecting");
      state->link_detail = "encryption failed; disconnected";
      if (ControllerManager::instance().is_active(state->address)) {
        set_link_detail(state->link_detail);
      }
      pClient->disconnect();
      return;
    } else {
      logger.info("Encryption successful for {}", pClient->getPeerAddress().toString());
      // set the connection parameters
      pClient->updateConnParams(min_conn_interval, max_conn_interval, conn_latency,
                                supervision_timeout);
      // the protected HID characteristics can be discovered + subscribed now
      state->authenticated = true;
    }
  }
};

static ClientCallbacks clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
  espp::Logger logger =
      espp::Logger({.tag = "BLE Scan Callbacks", .level = espp::Logger::Verbosity::INFO});
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    // logger.info("Advertised Device found: {}", advertisedDevice->toString());

    const auto &addr = advertisedDevice->getAddress();

    // Check if already connected to this device
    auto pClients = NimBLEDevice::getConnectedClients();
    for (auto *client : pClients) {
      if (client && client->getPeerAddress() == addr) {
        logger.debug("Device {} already connected, skipping", addr.toString());
        return;
      }
    }

    bool should_connect = false;
    bool is_pairable_device =
        advertisedDevice->isAdvertisingService(hid_service_uuid) ||
        advertisedDevice->getAppearance() == (uint16_t)espp::BleAppearance::GAMEPAD;
    if (is_pairing && is_pairable_device) {
      // if we're pairing, then simply connect to the first device that advertises
      // the HID service. The connection callback will try to bond to it.
      should_connect = true;
    } else if (!is_pairing && NimBLEDevice::isBonded(advertisedDevice->getAddress())) {
      // if we're not pairing, then we're reconnecting, so we need to check if
      // the device is bonded
      should_connect = true;
    }
    if (should_connect) {
      // tell the supervisor a connection is in flight
      connect_pending = true;
      connect_started_us = esp_timer_get_time();
      link_state = BleLinkState::Connecting;
      set_link_detail(fmt::format("connecting to {}", advertisedDevice->getAddress().toString()));

      // DON'T stop scan for multi-controller support - let it continue discovering other devices
      // Note: NimBLE can handle multiple async connections

      logger.info("Connecting to device {}", addr.toString());
      {
        // remember what it called itself, for the paired-controller list
        std::lock_guard<std::mutex> lk(advertised_name_mutex);
        advertised_name = advertisedDevice->haveName() ? advertisedDevice->getName() : "";
      }

      /** Async connections can be made directly in the scan callbacks */
      auto pClient = NimBLEDevice::getDisconnectedClient();
      if (!pClient) {
        pClient = NimBLEDevice::createClient(advertisedDevice->getAddress());
        if (!pClient) {
          logger.error("Failed to create client");
          set_link_detail("could not create a BLE client");
          connect_pending = false;
          return;
        }
      }

      // and set our callbacks
      pClient->setClientCallbacks(&clientCallbacks, false);
      static constexpr bool delete_on_disconnect = true;
      static constexpr bool delete_on_connect_fail = true;
      pClient->setSelfDelete(delete_on_disconnect, delete_on_connect_fail);
      if (!pClient->connect(true, true,
                            false)) { // delete attributes, async connect, no MTU exchange
        logger.error("Failed to connect");
        set_link_detail("could not start the connection attempt");
        connect_pending = false;
        return;
      }
    }
  }

  void onScanEnd(const NimBLEScanResults &results, int reason) override {
    // a scan ends because it was stopped to connect, or because its 5 s window
    // ran out; the scan timer starts the next one if nothing is connected
    logger.debug("scan ended (reason {}), {} devices seen", reason, results.getCount());
  }
};

std::string get_connected_client_serial_number() {
  auto clients = NimBLEDevice::getConnectedClients();
  if (clients.size() == 0) {
    return "";
  }
  auto client = clients[0];
  // get the device info service
  auto svc = client->getService(espp::DeviceInfoService::SERVICE_UUID);
  if (!svc) {
    return "";
  }
  // get the serial number characteristic
  auto chr = svc->getCharacteristic(espp::DeviceInfoService::SERIAL_NUMBER_CHAR_UUID);
  // make sure we can read it
  if (!chr || !chr->canRead()) {
    return {};
  }
  // and read it
  auto value = chr->readValue();
  return value;
}

static ScanCallbacks scanCallbacks;

// Subscribe to every HID *input* report the controller notifies (a gamepad may
// expose several: e.g. the Xbox controller's gamepad report and a separate
// report for the Xbox button). The report id of each comes from its Report
// Reference descriptor (0x2908: [report_id][report_type], type 1 = input);
// without that descriptor the characteristic is assumed to be the main input
// report. All-or-nothing: if any input report cannot be subscribed the attempt
// fails (a partial subscription could leave the gamepad report itself silent
// while the link is reported up), and the next attempt starts over. Returns the
// number of characteristics subscribed; `why` explains a zero result.
static size_t subscribe_input_reports(NimBLEClient *client, std::string &why) {
  static constexpr bool refresh = true;

  auto *state = get_controller_by_client(client);
  if (!state) {
    why = "controller state not found";
    return 0;
  }

  // Clear previous attempt's entries for this controller
  state->report_ids.clear();

  const auto &services = client->getServices(refresh);
  auto *svc = client->getService(hid_service_uuid);
  if (!svc) {
    why = fmt::format("HID service not found ({} services discovered)", services.size());
    return 0;
  }
  size_t count = 0, report_chars = 0, failed = 0;
  for (auto *chr : svc->getCharacteristics(refresh)) {
    if (chr->getUUID() != hid_input_uuid || !chr->canNotify())
      continue;
    ++report_chars;
    uint8_t report_id = 1;
    if (auto *ref = chr->getDescriptor(report_reference_uuid)) {
      const auto value = ref->readValue();
      if (value.size() >= 2) {
        report_id = value.data()[0];
        const uint8_t report_type = value.data()[1];
        if (report_type != 1) // 1 = input; skip output / feature reports
          continue;
      }
    }
    if (!chr->subscribe(true, on_notify)) {
      ble_logger.warn("could not subscribe to input report {} for {}",
                      report_id, client->getPeerAddress().toString());
      ++failed;
      continue;
    }
    state->report_ids[chr] = report_id;
    ble_logger.info("subscribed to HID input report id {} (handle {:#06x}) for {}",
                    report_id, chr->getHandle(), client->getPeerAddress().toString());
    ++count;
  }
  if (failed) {
    why = fmt::format("subscribing to {} of {} input reports failed", failed, count + failed);
    state->report_ids.clear(); // the next attempt subscribes them all again
    return 0;
  }
  if (count == 0) {
    if (report_chars == 0)
      why = "HID service has no notifying Report characteristic";
    else
      why = fmt::format("no input report among {} Report characteristics", report_chars);
  }
  return count;
}

static void start_scan(bool pairing);

// (Re)start the scan when nothing is connected, at most every kScanRetryUs.
static void ensure_scanning(int64_t now) {
  if (NimBLEDevice::getScan()->isScanning()) {
    link_state = BleLinkState::Scanning;
    return;
  }
  static int64_t last_scan_start_us = 0;
  if (now - last_scan_start_us < kScanRetryUs)
    return;
  last_scan_start_us = now;
  // a pairing scan stays a pairing scan for its window; with no bonds there is
  // nothing to reconnect to, so that is a pairing scan as well
  const bool pairing = NimBLEDevice::getNumBonds() == 0 || now < pairing_until_us.load();
  start_scan(pairing);
}

// The link supervisor, run every 100 ms on its own task: it owns the LED, the
// scan restart, the encryption retry and the HID subscription, so the whole
// bring-up is driven from one place regardless of which BLE callback did (or
// did not) fire. Now handles multiple controllers.
static bool timer_callback() {
  const int64_t now = esp_timer_get_time();
  auto pClients = NimBLEDevice::getConnectedClients();

  if (pClients.empty()) {
    // A disconnect callback can be missed (e.g. the client object went away):
    // never leave the link marked up when nothing is connected.
    if (ControllerManager::instance().count() > 0) {
      ble_logger.info("no BLE clients connected but controller manager has entries; clearing");
      ControllerManager::instance().clear();
      if (disconnect_callback)
        disconnect_callback();
    }
    if (connect_pending) {
      // a connection attempt is in flight: leave the radio alone (bounded, in
      // case its result callback never arrives)
      if (now - connect_started_us.load() < kConnectTimeoutUs) {
        link_state = BleLinkState::Connecting;
        return false;
      }
      ble_logger.warn("connection attempt did not complete; scanning again");
      set_link_detail("connection attempt timed out");
      connect_pending = false;
    }
    set_led_breathing(true);
    ensure_scanning(now);
    return false; // don't stop the timer
  }

  set_led_breathing(false);
  update_led_connected();

  // Keep scanning to discover additional controllers (unless in pairing mode with max connections)
  // This allows multiple controllers to connect over time
  if (!connect_pending && ControllerManager::instance().count() < 3) {  // Limit to 3 controllers
    ensure_scanning(now);
  }

  // Process each connected controller
  for (auto *pClient : pClients) {
    if (!pClient || !pClient->isConnected())
      continue;

    const auto &addr = pClient->getPeerAddress();
    auto *state = ControllerManager::instance().get(addr);

    if (!state) {
      // Controller not in manager yet, might be mid-connection
      if (connect_pending) {
        link_state = BleLinkState::Connecting;
        if (now - connect_started_us.load() < kConnectTimeoutUs)
          continue;
        ble_logger.warn("connected to {}, but the connect callback never arrived; carrying on",
                        addr.toString());
        connect_pending = false;

        // Create state manually
        ControllerState new_state;
        new_state.client = pClient;
        new_state.address = addr;
        new_state.connected_at_us = now;
        new_state.last_secure_request_us = now;
        ControllerManager::instance().add_or_update(addr, new_state);
        state = ControllerManager::instance().get(addr);
      } else {
        continue;
      }
    }

    if (state->subscribed) {
      continue; // already fully set up
    }

    // The HID input reports are protected: wait for the bond/encryption to
    // complete before discovering and subscribing
    if (!state->authenticated && pClient->getConnInfo().isEncrypted()) {
      ble_logger.info("link to {} is encrypted", addr.toString());
      pClient->updateConnParams(min_conn_interval, max_conn_interval, conn_latency,
                                supervision_timeout);
      state->authenticated = true;
      ControllerManager::instance().add_or_update(addr, *state);
    }

    if (!state->authenticated) {
      const int64_t waited_us = now - state->connected_at_us;
      if (waited_us > kAuthTimeoutUs) {
        ble_logger.warn("controller {} did not complete encryption in time; disconnecting",
                        addr.toString());
        state->link_detail = fmt::format("controller did not encrypt the link within {} s; disconnected",
                                        kAuthTimeoutUs / 1000000);
        ControllerManager::instance().add_or_update(addr, *state);
        pClient->disconnect();
        continue;
      }
      if (now - state->last_secure_request_us > kSecureRetryUs) {
        state->last_secure_request_us = now;
        ble_logger.warn("link to {} not encrypted after {} ms; requesting security again",
                        addr.toString(), waited_us / 1000);
        state->link_detail = fmt::format("waiting for encryption ({} s); security requested again",
                                        waited_us / 1000000);
        ControllerManager::instance().add_or_update(addr, *state);
        pClient->secureConnection(true);
      }
      continue;
    }

    // Discovery + subscribe takes a while; try every ~500 ms (this timer runs at
    // 100 ms) and bounded, then drop the connection so the scan can start over.
    static uint8_t throttle = 0;
    if (++throttle % 5 != 1)
      continue;

    const uint8_t attempt = ++state->subscribe_attempts;
    if (attempt > kMaxSubscribeAttempts) {
      ble_logger.error("could not subscribe to controller {} HID reports after {} attempts; "
                       "disconnecting (bond kept)", addr.toString(), kMaxSubscribeAttempts);
      state->link_detail = fmt::format("could not subscribe to the HID input reports after {} attempts; "
                                      "disconnected (bond kept)", kMaxSubscribeAttempts);
      ControllerManager::instance().add_or_update(addr, *state);
      pClient->disconnect();
      continue;
    }

    std::string why;
    if (subscribe_input_reports(pClient, why) == 0) {
      ble_logger.warn("no HID input report subscribed for {} (attempt {}/{}): {}",
                      addr.toString(), attempt, kMaxSubscribeAttempts, why);
      state->link_detail = fmt::format("{} (attempt {}/{})", why, attempt, kMaxSubscribeAttempts);
      ControllerManager::instance().add_or_update(addr, *state);
      continue;
    }

    state->notification_count = 0;
    state->last_notify_us = -1;
    state->subscribed = true;
    state->link_detail = fmt::format("subscribed on attempt {}", attempt);

    // Subscribe to battery service if it exists
    if (auto *pBatterySvc = pClient->getService(battery_service_uuid)) {
      pBatterySvc->getCharacteristics(true);
      if (auto *pBatteryChr = pBatterySvc->getCharacteristic(battery_level_uuid)) {
        pBatteryChr->subscribe(pBatteryChr->canNotify(), on_notify);
      }
    }

    // Report the controller's name
    if (bond_name_callback) {
      const NimBLEAddress id = pClient->getConnInfo().getIdAddress();
      std::array<uint8_t, 6> address{};
      std::copy(id.getVal(), id.getVal() + address.size(), address.begin());
      state->name = read_controller_name(pClient);
      bond_name_callback(address, id.getType(), state->name);
    }

    ControllerManager::instance().add_or_update(addr, *state);
    ble_logger.info("Controller {} fully connected ({} total)", addr.toString(),
                    ControllerManager::instance().count());
  }

  // Update global link state based on active controller
  auto active_addr = ControllerManager::instance().get_active();
  if (active_addr.has_value()) {
    auto *active_state = ControllerManager::instance().get(active_addr.value());
    if (active_state) {
      if (active_state->subscribed) {
        link_state = BleLinkState::Subscribed;
      } else if (active_state->authenticated) {
        link_state = BleLinkState::Subscribing;
      } else {
        link_state = BleLinkState::Encrypting;
      }
      set_link_detail(active_state->link_detail);
    }
  }

  return false; // don't stop the timer
}

void init_ble(const std::string &device_name) {
  NimBLEDevice::init(device_name);
  // NOTE: you must create a server if you want the GAP services to be available
  // and the device name to be readable by connected peers.
  static auto server_ = NimBLEDevice::createServer();
  server_->start();

  // // and some i/o config
  auto io_capabilities = BLE_HS_IO_NO_INPUT_OUTPUT;
  NimBLEDevice::setSecurityIOCap(io_capabilities);

  // // set security parameters
  bool bonding = true;
  bool mitm = false;
  bool secure_connections = true;
  NimBLEDevice::setSecurityAuth(bonding, mitm, secure_connections);
}

// Start (or restart) the scan. Called from the main task at boot, from the
// console's actions, and from the scan timer; the LED is handled by the timer.
static void start_scan(bool pairing) {
  is_pairing = pairing;
  breathing_period = pairing ? pairing_breathing_period : reconnecting_breathing_period;

  NimBLEScan *pScan = NimBLEDevice::getScan();

  // if scanning, then stop
  if (pScan->isScanning()) {
    pScan->stop();
  }

  // Set the callbacks to call when scan events occur, no duplicates
  pScan->setScanCallbacks(&scanCallbacks);

  // Set scan interval (how often) and window (how long) in milliseconds
  pScan->setInterval(100);
  pScan->setWindow(100);

  // Active scan will gather scan response data from advertisers
  // but will use more energy from both devices
  pScan->setActiveScan(true);

  // Start scanning for advertisers
  if (pScan->start(scanTimeMs)) {
    link_state = BleLinkState::Scanning;
    ble_logger.debug("{} scan started", pairing ? "pairing" : "reconnection");
  } else {
    ble_logger.error("could not start the {} scan", pairing ? "pairing" : "reconnection");
    set_link_detail("scan could not be started; retrying");
  }

  if (!scanTimer) {
    // the link supervisor: (re)scans when nothing is connected, drives the
    // encryption + HID subscription of a connected controller, owns the LED
    using namespace std::chrono_literals;
    scanTimer = std::make_unique<espp::Timer>(
        espp::Timer::Config{.name = "Scan Timer",
                            .period = 100ms,
                            .callback = timer_callback,
                            .log_level = espp::Logger::Verbosity::INFO});
  }
}

void start_ble_reconnection_thread(notify_callback_t callback) {
  // save the callback
  notify_callback = callback;
  pairing_until_us = 0;
  // if there are no bonded devices, there is nothing to reconnect to: pair instead
  start_scan(NimBLEDevice::getNumBonds() == 0);
}

void start_ble_pairing_thread(notify_callback_t callback) {
  // save the callback
  notify_callback = callback;
  // accept new controllers for a while (the scan itself cycles every 5 s)
  pairing_until_us = esp_timer_get_time() + kPairingWindowUs;
  start_scan(true);
}

bool is_ble_subscribed() {
  auto active = ControllerManager::instance().get_active();
  if (!active.has_value())
    return false;
  auto *state = ControllerManager::instance().get(active.value());
  return state && state->subscribed;
}

bool is_ble_scanning() { return NimBLEDevice::getScan()->isScanning(); }

bool is_ble_pairing() { return is_pairing.load(); }

uint8_t ble_bond_count() { return static_cast<uint8_t>(NimBLEDevice::getNumBonds()); }

void ble_set_bond_name_callback(bond_name_callback_t callback) {
  bond_name_callback = std::move(callback); // set once at startup, before scanning
}

void ble_set_disconnect_callback(disconnect_callback_t callback) {
  disconnect_callback = std::move(callback); // set once at startup, before scanning
}

std::optional<uint8_t> ble_report_id_for(const NimBLERemoteCharacteristic *chr) {
  auto *state = get_controller_by_characteristic(chr);
  if (!state)
    return std::nullopt;
  const auto it = state->report_ids.find(chr);
  if (it == state->report_ids.end())
    return std::nullopt;
  return it->second;
}

uint32_t ble_notification_count() {
  auto active = ControllerManager::instance().get_active();
  if (!active.has_value())
    return 0;
  auto *state = ControllerManager::instance().get(active.value());
  return state ? state->notification_count : 0;
}

uint32_t ble_ms_since_last_notification() {
  auto active = ControllerManager::instance().get_active();
  if (!active.has_value())
    return UINT32_MAX;
  auto *state = ControllerManager::instance().get(active.value());
  if (!state || state->last_notify_us < 0)
    return UINT32_MAX;
  const int64_t age_ms = (esp_timer_get_time() - state->last_notify_us) / 1000;
  return age_ms > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(age_ms);
}

BleLinkState ble_link_state() { return link_state.load(); }

std::string ble_link_detail() {
  std::lock_guard<std::mutex> lk(link_detail_mutex);
  return link_detail;
}

// The identity addresses of the connected clients (a bond is keyed by the
// identity address, not the possibly-random connection address).
static std::vector<NimBLEAddress> connected_identity_addresses() {
  std::vector<NimBLEAddress> out;
  for (auto *client : NimBLEDevice::getConnectedClients()) {
    if (client->isConnected())
      out.push_back(client->getConnInfo().getIdAddress());
  }
  return out;
}

std::vector<BleBond> ble_bonds() {
  std::vector<BleBond> bonds;
  const auto connected = connected_identity_addresses();
  const int count = NimBLEDevice::getNumBonds();
  for (int i = 0; i < count; ++i) {
    const NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
    BleBond b;
    std::copy(addr.getVal(), addr.getVal() + b.address.size(), b.address.begin());
    b.address_type = addr.getType();
    b.connected = std::find(connected.begin(), connected.end(), addr) != connected.end();
    bonds.push_back(b);
  }
  return bonds;
}

bool ble_forget_bond(const std::array<uint8_t, 6> &address, uint8_t address_type) {
  static espp::Logger logger({.tag = "ble_forget_bond", .level = espp::Logger::Verbosity::INFO});

  // Log the address we're trying to forget
  logger.info("Attempting to forget bond: {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X} type={}",
              address[5], address[4], address[3], address[2], address[1], address[0], address_type);

  // Check all bonded addresses to see what we have
  const int count = NimBLEDevice::getNumBonds();
  logger.info("Total bonds in NimBLE: {}", count);
  for (int i = 0; i < count; ++i) {
    const NimBLEAddress stored = NimBLEDevice::getBondedAddress(i);
    const uint8_t* val = stored.getVal();
    logger.info("Bond #{}: {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X} type={}",
                i, val[5], val[4], val[3], val[2], val[1], val[0], stored.getType());
  }

  // IMPORTANT: address bytes come from ble_bonds() which uses getVal(), which returns
  // the INTERNAL (already reversed) representation. NimBLEAddress constructor does
  // reverse_copy, so we must NOT pass it through the constructor again.
  // Instead, construct a ble_addr_t directly with the bytes as-is.
  ble_addr_t addr_struct;
  std::copy(address.begin(), address.end(), addr_struct.val);
  addr_struct.type = address_type;
  const NimBLEAddress addr(addr_struct);

  logger.info("Constructed NimBLEAddress: {}", addr.toString());

  if (!NimBLEDevice::isBonded(addr)) {
    logger.warn("Address not found in bonded list");
    return false;
  }

  logger.info("Address found, proceeding with deletion");

  // drop the live connection first if it is this controller (the client would
  // otherwise re-bond)
  for (auto *client : NimBLEDevice::getConnectedClients()) {
    if (client->isConnected() && client->getConnInfo().getIdAddress() == addr) {
      client->disconnect();
      reset_link_state("bond forgotten");
    }
  }
  return NimBLEDevice::deleteBond(addr);
}

bool ble_disconnect_bond(const std::array<uint8_t, 6> &address, uint8_t address_type) {
  static espp::Logger logger({.tag = "ble_disconnect_bond", .level = espp::Logger::Verbosity::INFO});

  // Construct address using ble_addr_t to avoid double reversal
  ble_addr_t addr_struct;
  std::copy(address.begin(), address.end(), addr_struct.val);
  addr_struct.type = address_type;
  const NimBLEAddress addr(addr_struct);

  logger.info("Attempting to disconnect bond: {}", addr.toString());

  // Find and disconnect this controller if it's connected
  bool found = false;
  for (auto *client : NimBLEDevice::getConnectedClients()) {
    if (client->isConnected() && client->getConnInfo().getIdAddress() == addr) {
      logger.info("Disconnecting controller");
      client->disconnect();
      reset_link_state("disconnected by user");
      found = true;
      break;
    }
  }

  if (!found) {
    logger.warn("Controller not currently connected");
    return false;
  }

  return true;
}

bool ble_clear_bonds() {
  // drop the live connection first (its client would otherwise re-bond)
  for (auto *client : NimBLEDevice::getConnectedClients()) {
    client->disconnect();
  }
  reset_link_state("all bonds cleared");
  return NimBLEDevice::deleteAllBonds();
}
