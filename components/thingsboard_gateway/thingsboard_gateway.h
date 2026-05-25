#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstdint>
#include <deque>
#include <map>
#include <string>

#include "esphome/components/thingsboard/transport.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace thingsboard {
class ThingsBoardComponent;
}  // namespace thingsboard

namespace thingsboard_gateway {

// ThingsBoard Gateway API bridge. Proxies child devices over `v1/gateway/*`,
// keyed by child device name. Additive: the host ESP32 is still an ordinary
// TB device on `v1/devices/me/*` via the core component.
//
// Two layers sit on top of this functional core:
//   A-layer — thin action/trigger surface for children on a local bus
//             (BLE/Modbus/UART); user lambdas marshal the data. YAML surface
//             lands in M4.
//   B-layer — auto-map ESPHome sub-devices to TB gateway children. Wired in M7
//             (discover_sub_devices_ is declared here, body lands then).
class ThingsBoardGatewayComponent : public Component,
                                    public thingsboard::TBGatewayTransport {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Must run after the core component's setup(), which creates the MQTT
  // transport in setup_mqtt_(). Core is WIFI - 1.0f; AFTER_WIFI is strictly
  // later in ESPHome's descending-priority setup order.
  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }

  void set_thingsboard_component(thingsboard::ThingsBoardComponent *p) {
    parent_ = p;
  }
  void set_auto_map_sub_devices(bool enable) { auto_map_sub_devices_ = enable; }

  // --- TBGatewayTransport (inbound, invoked by the MQTT transport) --------
  void on_gateway_rpc_request(const std::string &device_name,
                              const std::string &request_id,
                              const std::string &method,
                              const std::string &params) override;
  void on_gateway_shared_attributes(
      const std::string &device_name,
      const std::map<std::string, std::string> &attributes) override;
  void on_gateway_attribute_response(
      const std::string &device_name, const std::string &request_id,
      const std::map<std::string, std::string> &attributes) override;
  // Core pokes this on every (re)connect; TB drops gateway-child routing on
  // session loss, so we replay the registry from here.
  void on_core_connected() override;
  // Core pokes this on every disconnect; clears per-session state so the next
  // on_core_connected starts clean (and so flush_gw_batch_ holds telemetry
  // until the new session has re-issued gw_connect for each child).
  void on_core_disconnected() override;
  // B-layer: core offers a single sub-device datapoint here. Returns true and
  // batches it onto v1/gateway/* when `device_id` is an auto-mapped child;
  // false otherwise so the core keeps it on its own device-API batch.
  bool ingest_child_telemetry(uint32_t device_id, const std::string &key,
                              const std::string &json_value,
                              bool is_attribute) override;

  // --- A-layer public surface --------------------------------------------
  // Called by generated actions (M4); user lambdas marshal child data from a
  // local bus (BLE/Modbus/UART) into these. Unknown device names passed to the
  // child_send_* methods are lazily registered + connected with an empty type.
  void child_connect(const std::string &device_name,
                     const std::string &device_type = "");
  void child_disconnect(const std::string &device_name);
  void child_send_telemetry(const std::string &device_name,
                            const std::string &payload);
  void child_send_attributes(const std::string &device_name,
                             const std::string &payload);
  void child_send_rpc_response(const std::string &device_name,
                               const std::string &request_id,
                               const std::string &payload);
  void child_request_attributes(const std::string &device_name,
                                const std::string &request_id,
                                const std::string &keys, bool client_scope);

  // --- Codegen accessors (used by __init__.py once the M4 YAML surface lands).
  // The get_*_trigger accessors lazily heap-allocate the Trigger and never free
  // it: it lives for the program's lifetime, like every other ESPHome trigger.
  void add_child(const std::string &device_name,
                 const std::string &device_type, bool auto_connect);
  Trigger<std::string, std::string, std::string> *get_child_rpc_trigger(
      const std::string &device_name);
  Trigger<std::map<std::string, std::string>> *get_child_shared_attr_trigger(
      const std::string &device_name);
  Trigger<std::string, std::string, std::string, std::string>
      *get_global_rpc_trigger();
  Trigger<std::string, std::map<std::string, std::string>>
      *get_global_shared_attr_trigger();

 protected:
  // name -> {type, connected}. The lifecycle is replayed on every MQTT
  // reconnect (TB drops gateway child routing on disconnect) and gateway RPC
  // is routed per child against this registry.
  struct ChildDevice {
    std::string name;
    std::string type;
    bool connected{false};
  };

  // One pending telemetry/attribute datapoint. Keyed in gw_pending_ by
  // `device + "\x1f" + key` (per-(child,key) dedup, mirrors the core's
  // pending_messages_). `json_value` is a raw JSON fragment spliced verbatim.
  struct GwPending {
    std::string device;
    std::string key;
    std::string json_value;
    bool is_attribute{false};
    uint32_t timestamp{0};
  };

  // Queue gw_connect for every connected child. TB drops gateway-child routing
  // on every MQTT reconnect, so this runs from on_core_connected(). The connects
  // are not fired here: they go into pending_replay_ and drain one per
  // GW_REPLAY_INTERVAL_MS so a many-child gateway does not blow TB's per-device
  // message rate limit the instant the session comes back.
  void replay_children_();
  // Fires one queued gw_connect per GW_REPLAY_INTERVAL_MS from loop().
  void drain_pending_replay_();
  // Drains gw_pending_ once per batch window: partitions by device, builds one
  // JSON object per device, publishes via the v1/gateway/* outbound surface.
  void flush_gw_batch_();
  // Caps gw_pending_ at GW_PENDING_MAX, dropping the oldest datapoint first.
  // Mirrors the core's bounded offline queue: a long disconnect (or an A-layer
  // lambda spraying ever-new child/key pairs) must not grow memory unbounded.
  void bound_gw_pending_();
  // Lazily registers + connects an unknown child name (empty type); returns the
  // registry entry. Backs the child_send_* lazy path.
  ChildDevice &resolve_child_(const std::string &device_name);
  // Parses a per-child telemetry/attribute payload object and fans each
  // key/value into gw_pending_; `device_name` is lazily resolved.
  void ingest_child_payload_(const std::string &device_name,
                             const std::string &payload, bool is_attribute);
  // Routes an inbound TB-reserved service RPC (gateway_device_renamed /
  // gateway_device_deleted) against the child registry.
  void handle_service_rpc_(const std::string &request_id,
                           const std::string &method,
                           const std::string &params);
  // B-layer: walks App.get_devices() (guarded by USE_DEVICES) and
  // auto-registers each ESPHome sub-device as a gateway child, recording the
  // name <-> device-id mapping below. Called from setup() when
  // auto_map_sub_devices_ is set.
  void discover_sub_devices_();

  std::map<std::string, ChildDevice> children_;
  std::map<std::string, GwPending> gw_pending_;
  uint32_t last_gw_batch_{0};
  // Hard cap on pending child datapoints; see bound_gw_pending_().
  static constexpr size_t GW_PENDING_MAX = 256;

  // Staggered gw_connect replay queue (child names) + its drain clock. A
  // std::deque drained front-to-back so children re-link in registration
  // order. Interval set so a many-child gateway's gw_connect burst (each
  // gw_connect is two TB messages) stays well under TB's per-device rate.
  std::deque<std::string> pending_replay_;
  uint32_t last_replay_drain_{0};
  static constexpr uint32_t GW_REPLAY_INTERVAL_MS = 400;

  // Gates v1/gateway/* publishes. False until on_core_connected has seeded the
  // replay queue for this session; cleared by on_core_disconnected. Without
  // this gate the gateway can flush queued telemetry in the window between
  // dispatch_connected setting connection_active_ = true and the bootstrap
  // phase machine reaching BOOT_GW_REPLAY (5+ loop ticks), which TB sees as
  // telemetry for a child it has no routing for and closes the socket.
  bool session_ready_{false};

  // Per-child + global automation triggers; heap-allocated lazily by the
  // codegen accessors, never freed (program-lifetime, like all ESPHome
  // triggers). Empty/null until the M4 YAML surface populates them.
  std::map<std::string, Trigger<std::string, std::string, std::string> *>
      child_rpc_triggers_;
  std::map<std::string, Trigger<std::map<std::string, std::string>> *>
      child_shared_attr_triggers_;
  Trigger<std::string, std::string, std::string, std::string>
      *global_rpc_trigger_{nullptr};
  Trigger<std::string, std::map<std::string, std::string>>
      *global_shared_attr_trigger_{nullptr};

  bool auto_map_sub_devices_{false};
  // B-layer name <-> ESPHome-sub-device-id mapping, populated by
  // discover_sub_devices_(). Empty unless auto_map_sub_devices_ is set and the
  // config has an `esphome: devices:` block (USE_DEVICES). ingest_child_telemetry
  // resolves id -> name; on_gateway_rpc_request resolves name -> id.
  std::map<std::string, uint32_t> sub_device_ids_;
  std::map<uint32_t, std::string> sub_device_names_;

  thingsboard::ThingsBoardComponent *parent_{nullptr};
  // Outbound `v1/gateway/*` surface; resolved at runtime from the core's
  // transport in setup() (the MQTT transport is created at core setup() time).
  thingsboard::TBGatewayPublisher *publisher_{nullptr};
};

// --- A-layer Actions -----------------------------------------------------
// Generated by @automation.register_action in __init__.py. Each holds the
// component via Parented<> and forwards to a child_* method. Every field is a
// TemplatableValue so YAML lambdas can compute it per-invocation.

template<typename... Ts>
class ChildConnectAction : public Action<Ts...>,
                           public Parented<ThingsBoardGatewayComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, device_name)
  TEMPLATABLE_VALUE(std::string, device_type)
  void play(const Ts &...x) override {
    this->parent_->child_connect(this->device_name_.value(x...),
                                 this->device_type_.value(x...));
  }
};

template<typename... Ts>
class ChildDisconnectAction : public Action<Ts...>,
                              public Parented<ThingsBoardGatewayComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, device_name)
  void play(const Ts &...x) override {
    this->parent_->child_disconnect(this->device_name_.value(x...));
  }
};

template<typename... Ts>
class ChildSendTelemetryAction
    : public Action<Ts...>, public Parented<ThingsBoardGatewayComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, device_name)
  TEMPLATABLE_VALUE(std::string, data)
  void play(const Ts &...x) override {
    this->parent_->child_send_telemetry(this->device_name_.value(x...),
                                        this->data_.value(x...));
  }
};

template<typename... Ts>
class ChildSendAttributesAction
    : public Action<Ts...>, public Parented<ThingsBoardGatewayComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, device_name)
  TEMPLATABLE_VALUE(std::string, data)
  void play(const Ts &...x) override {
    this->parent_->child_send_attributes(this->device_name_.value(x...),
                                         this->data_.value(x...));
  }
};

template<typename... Ts>
class ChildSendRpcResponseAction
    : public Action<Ts...>, public Parented<ThingsBoardGatewayComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, device_name)
  TEMPLATABLE_VALUE(std::string, request_id)
  TEMPLATABLE_VALUE(std::string, data)
  void play(const Ts &...x) override {
    this->parent_->child_send_rpc_response(this->device_name_.value(x...),
                                           this->request_id_.value(x...),
                                           this->data_.value(x...));
  }
};

template<typename... Ts>
class ChildRequestAttributesAction
    : public Action<Ts...>, public Parented<ThingsBoardGatewayComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, device_name)
  TEMPLATABLE_VALUE(std::string, request_id)
  TEMPLATABLE_VALUE(std::string, keys)
  TEMPLATABLE_VALUE(bool, client_scope)
  void play(const Ts &...x) override {
    this->parent_->child_request_attributes(this->device_name_.value(x...),
                                            this->request_id_.value(x...),
                                            this->keys_.value(x...),
                                            this->client_scope_.value(x...));
  }
};

}  // namespace thingsboard_gateway
}  // namespace esphome

#endif  // USE_ESP32
