#include "thingsboard_gateway.h"

#ifdef USE_ESP32

#include <vector>

#include "esphome/components/json/json_util.h"
#include "esphome/components/thingsboard/thingsboard_client.h"
#include "esphome/components/thingsboard_mqtt/thingsboard_mqtt_transport.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace thingsboard_gateway {

static const char *const TAG = "thingsboard.gateway";

// Batch window for child telemetry/attributes; mirrors the core's ~100ms.
static const uint32_t DEFAULT_GW_BATCH_DELAY_MS = 100;
// gw_pending_ key separator: `<device>` US `<datapoint key>`. US (0x1f) can't
// occur in a JSON object key, so the split is unambiguous.
static const char GW_KEY_SEP = '\x1f';

void ThingsBoardGatewayComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ThingsBoard Gateway");
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "No ThingsBoard core component bound");
    this->mark_failed();
    return;
  }

  // The MQTT transport is created at runtime by the core component (in its
  // setup_mqtt_()), so we resolve the handle here rather than at codegen time
  // — mirrors thingsboard_mqtt_ota.cpp. The config-time FINAL_VALIDATE
  // guarantees the transport is MQTT.
  auto *core_transport = this->parent_->get_transport();
  auto *mqtt =
      static_cast<thingsboard::ThingsBoardMQTT *>(core_transport);
  this->publisher_ = mqtt;  // ThingsBoardMQTT also implements TBGatewayPublisher

  // Register inbound handler with core...
  this->parent_->register_gateway_transport(this);
  // ...and with the MQTT transport, so v1/gateway/* events route back to us.
  if (mqtt != nullptr) {
    mqtt->set_gateway_handler(this);
  } else {
    ESP_LOGE(TAG, "MQTT transport handle not available; gateway disabled");
  }

  // B-layer: when enabled, auto-register ESPHome sub-devices as children.
  if (this->auto_map_sub_devices_) {
    this->discover_sub_devices_();
  }
}

void ThingsBoardGatewayComponent::loop() {
  // Staggered child-connect replay runs ahead of telemetry so TB has re-linked
  // each child before its datapoints arrive.
  this->drain_pending_replay_();
  if (this->gw_pending_.empty()) return;
  const uint32_t now = millis();
  if (this->last_gw_batch_ == 0) this->last_gw_batch_ = now;
  if (now - this->last_gw_batch_ < DEFAULT_GW_BATCH_DELAY_MS) return;
  this->flush_gw_batch_();
  this->last_gw_batch_ = now;
}

void ThingsBoardGatewayComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ThingsBoard Gateway:");
  ESP_LOGCONFIG(TAG, "  Registered children: %u",
                static_cast<unsigned>(this->children_.size()));
  for (const auto &kv : this->children_) {
    ESP_LOGCONFIG(TAG, "    - %s (type='%s', %s)", kv.second.name.c_str(),
                  kv.second.type.c_str(),
                  kv.second.connected ? "connected" : "disconnected");
  }
  ESP_LOGCONFIG(TAG, "  Auto-map sub-devices: %s",
                this->auto_map_sub_devices_ ? "yes" : "no");
}

// --- Child registry ------------------------------------------------------

void ThingsBoardGatewayComponent::add_child(const std::string &device_name,
                                            const std::string &device_type,
                                            bool auto_connect) {
  auto &child = this->children_[device_name];
  child.name = device_name;
  child.type = device_type;
  // auto_connect children come up via replay_children_ on the first (and every
  // subsequent) core connect; others wait for an explicit child_connect action
  // or the first child_send_* call.
  if (auto_connect) child.connected = true;
  ESP_LOGD(TAG, "Registered child '%s' (type='%s', auto_connect=%s)",
           device_name.c_str(), device_type.c_str(),
           auto_connect ? "true" : "false");
}

ThingsBoardGatewayComponent::ChildDevice &
ThingsBoardGatewayComponent::resolve_child_(const std::string &device_name) {
  auto it = this->children_.find(device_name);
  if (it != this->children_.end()) {
    if (!it->second.connected) {
      // Known but not connected (declared without auto_connect, or previously
      // disconnected) — bring it up now.
      it->second.connected = true;
      if (this->publisher_ != nullptr)
        this->publisher_->gw_connect(device_name, it->second.type);
    }
    return it->second;
  }
  // Unknown name: lazily register + connect with an empty type.
  ESP_LOGD(TAG, "Lazily registering child '%s'", device_name.c_str());
  this->child_connect(device_name, "");
  return this->children_[device_name];
}

void ThingsBoardGatewayComponent::replay_children_() {
  // Queue every connected child for a staggered gw_connect. drain_pending_replay_
  // fires them one per GW_REPLAY_INTERVAL_MS from loop(), so a many-child gateway
  // does not blow TB's per-device message rate limit the instant the session is
  // back. Rebuilt from scratch each (re)connect: a reconnect mid-replay just
  // restarts the queue.
  this->pending_replay_.clear();
  // Sentinel reset: a fresh replay queue starts draining on the next loop()
  // tick rather than waiting out a stale GW_REPLAY_INTERVAL_MS from a prior
  // session. drain_pending_replay_ treats last_replay_drain_ == 0 as "due now".
  this->last_replay_drain_ = 0;
  for (const auto &kv : this->children_) {
    if (kv.second.connected)
      this->pending_replay_.push_back(kv.second.name);
  }
  if (!this->pending_replay_.empty())
    ESP_LOGD(TAG, "Queued %u gateway child(ren) for staggered replay",
             static_cast<unsigned>(this->pending_replay_.size()));
}

void ThingsBoardGatewayComponent::drain_pending_replay_() {
  if (this->pending_replay_.empty() || this->publisher_ == nullptr) return;
  if (this->parent_ == nullptr || !this->parent_->is_connected()) return;
  const uint32_t now = millis();
  if (this->last_replay_drain_ != 0 &&
      now - this->last_replay_drain_ < GW_REPLAY_INTERVAL_MS)
    return;
  // Belt-and-braces: GW_REPLAY_INTERVAL_MS already paces this at ~2.5/sec, but
  // a same-tick burst of child telemetry on the gateway counter could still
  // push us into TB's per-session window. Skip this tick if the gateway
  // counter is at capacity -- the queue stays intact for the next tick. No
  // record() here because gw_connect itself is published through publish() ->
  // the gateway component already records gateway telemetry flushes in
  // flush_gw_batch_; gw_connect is a control message that counts against the
  // session bucket once, which we account for here.
  if (!this->parent_->check_gateway_rate_limits(1, 0)) return;
  this->last_replay_drain_ = now;

  const std::string name = this->pending_replay_.front();
  this->pending_replay_.pop_front();
  auto it = this->children_.find(name);
  if (it != this->children_.end() && it->second.connected) {
    this->publisher_->gw_connect(it->second.name, it->second.type);
    this->parent_->record_gateway_publish(1, 0);
    ESP_LOGD(TAG, "Replayed gateway child '%s' (%u remaining)", name.c_str(),
             static_cast<unsigned>(this->pending_replay_.size()));
  }
}

void ThingsBoardGatewayComponent::on_core_connected() {
  // Seed the replay queue first, then open the flush gate. Order matters:
  // flush_gw_batch_ early-returns while pending_replay_ is non-empty, so the
  // staggered drain finishes before any queued telemetry hits TB -- which
  // means every child has a gw_connect under its belt before its datapoints
  // arrive on v1/gateway/telemetry.
  this->replay_children_();
  this->session_ready_ = true;
}

void ThingsBoardGatewayComponent::on_core_disconnected() {
  // Hold all gateway publishes until the next session has re-issued
  // gw_connect; without this guard the gateway's loop() races the core's
  // bootstrap phase machine on reconnect and flushes telemetry to TB for a
  // child it has no routing for, which trips a server-side socket close.
  this->session_ready_ = false;
  this->pending_replay_.clear();
  this->last_replay_drain_ = 0;
}

void ThingsBoardGatewayComponent::discover_sub_devices_() {
#ifdef USE_DEVICES
  size_t mapped = 0;
  for (auto *dev : App.get_devices()) {
    const uint32_t id = dev->get_device_id();
    if (id == 0) continue;  // 0 == the main device (the gateway's own device)
    std::string name = dev->get_name();
    if (name.empty()) {
      ESP_LOGW(TAG, "Sub-device id %u has no name; skipped",
               static_cast<unsigned>(id));
      continue;
    }
    // Register + connect at boot; replay_children_ re-links it on reconnect.
    this->add_child(name, "", /*auto_connect=*/true);
    this->sub_device_ids_[name] = id;
    this->sub_device_names_[id] = name;
    ++mapped;
  }
  ESP_LOGI(TAG, "Auto-mapped %u ESPHome sub-device(s) as gateway children",
           static_cast<unsigned>(mapped));
#else
  ESP_LOGW(TAG,
           "auto_map_sub_devices is set, but this build has no "
           "`esphome: devices:` block (USE_DEVICES undefined); nothing to map");
#endif
}

bool ThingsBoardGatewayComponent::ingest_child_telemetry(
    uint32_t device_id, const std::string &key, const std::string &json_value,
    bool is_attribute) {
  auto it = this->sub_device_names_.find(device_id);
  if (it == this->sub_device_names_.end()) {
    // Not a sub-device we map (auto_map_sub_devices off, or it wasn't
    // discovered). Decline so the core keeps it on its device-API batch.
    return false;
  }
  const std::string &device_name = it->second;
  // Make sure TB is routing this child before its data lands in the batch.
  this->resolve_child_(device_name);

  const uint32_t now = millis();
  // Dedup key includes is_attribute: on_switch_update + on_*_update emit BOTH
  // telemetry (`switch.relay_1` = 1.0) and a matching client attribute
  // (`switch.relay_1` = true) under the same scoped key, so a (child, key)-only
  // map_key would let the attribute overwrite the telemetry. The flush
  // partitions on is_attribute, so the two entries can coexist.
  std::string map_key = device_name;
  map_key += GW_KEY_SEP;
  map_key += is_attribute ? 'a' : 't';
  map_key += GW_KEY_SEP;
  map_key += key;
  GwPending &p = this->gw_pending_[map_key];
  p.device = device_name;
  p.key = key;
  p.json_value = json_value;
  p.is_attribute = is_attribute;
  p.timestamp = now;
  if (this->last_gw_batch_ == 0) this->last_gw_batch_ = now;
  this->bound_gw_pending_();
  return true;
}

// --- A-layer outbound ----------------------------------------------------

void ThingsBoardGatewayComponent::child_connect(
    const std::string &device_name, const std::string &device_type) {
  if (device_name.empty()) {
    ESP_LOGW(TAG, "child_connect: empty device name ignored");
    return;
  }
  auto &child = this->children_[device_name];
  child.name = device_name;
  if (!device_type.empty()) child.type = device_type;
  child.connected = true;
  if (this->publisher_ != nullptr)
    this->publisher_->gw_connect(device_name, child.type);
  ESP_LOGD(TAG, "child_connect('%s', type='%s')", device_name.c_str(),
           child.type.c_str());
}

void ThingsBoardGatewayComponent::child_disconnect(
    const std::string &device_name) {
  auto it = this->children_.find(device_name);
  if (it == this->children_.end()) {
    ESP_LOGW(TAG, "child_disconnect('%s'): not registered",
             device_name.c_str());
    return;
  }
  it->second.connected = false;
  if (this->publisher_ != nullptr)
    this->publisher_->gw_disconnect(device_name);
  ESP_LOGD(TAG, "child_disconnect('%s')", device_name.c_str());
}

void ThingsBoardGatewayComponent::ingest_child_payload_(
    const std::string &device_name, const std::string &payload,
    bool is_attribute) {
  if (device_name.empty()) {
    ESP_LOGW(TAG, "child_send_*: empty device name ignored");
    return;
  }
  this->resolve_child_(device_name);

  const uint32_t now = millis();
  bool any = false;
  json::parse_json(payload, [&](JsonObject root) -> bool {
    for (JsonPair kv : root) {
      std::string key = kv.key().c_str();
      // Re-serialize the value to a compact JSON fragment for verbatim
      // splicing into the per-device envelope later (handles scalars,
      // strings, and nested objects/arrays alike).
      std::string frag;
      serializeJson(kv.value(), frag);
      // Dedup key includes is_attribute so a user calling
      // child_send_telemetry({"x":1}) then child_send_attributes({"x":"on"})
      // doesn't have the attribute overwrite the telemetry; flush partitions
      // on is_attribute so both entries can coexist.
      std::string map_key = device_name;
      map_key += GW_KEY_SEP;
      map_key += is_attribute ? 'a' : 't';
      map_key += GW_KEY_SEP;
      map_key += key;
      GwPending &p = this->gw_pending_[map_key];
      p.device = device_name;
      p.key = key;
      p.json_value = frag;
      p.is_attribute = is_attribute;
      p.timestamp = now;
      any = true;
    }
    return true;
  });
  if (!any) {
    ESP_LOGW(TAG, "child_send_* for '%s': payload not a JSON object: %s",
             device_name.c_str(), payload.c_str());
    return;
  }
  if (this->last_gw_batch_ == 0) this->last_gw_batch_ = now;
  this->bound_gw_pending_();
}

void ThingsBoardGatewayComponent::child_send_telemetry(
    const std::string &device_name, const std::string &payload) {
  this->ingest_child_payload_(device_name, payload, /*is_attribute=*/false);
}

void ThingsBoardGatewayComponent::child_send_attributes(
    const std::string &device_name, const std::string &payload) {
  this->ingest_child_payload_(device_name, payload, /*is_attribute=*/true);
}

void ThingsBoardGatewayComponent::child_send_rpc_response(
    const std::string &device_name, const std::string &request_id,
    const std::string &payload) {
  if (this->publisher_ == nullptr) {
    ESP_LOGW(TAG, "child_send_rpc_response('%s'): no publisher",
             device_name.c_str());
    return;
  }
  this->publisher_->gw_publish_rpc_response(device_name, request_id, payload);
}

void ThingsBoardGatewayComponent::child_request_attributes(
    const std::string &device_name, const std::string &request_id,
    const std::string &keys, bool client_scope) {
  if (this->publisher_ == nullptr) {
    ESP_LOGW(TAG, "child_request_attributes('%s'): no publisher",
             device_name.c_str());
    return;
  }
  // Make sure TB is routing this child before asking for its attributes.
  this->resolve_child_(device_name);
  this->publisher_->gw_request_attributes(device_name, request_id, keys,
                                          client_scope);
}

void ThingsBoardGatewayComponent::bound_gw_pending_() {
  // Drop the oldest datapoint by timestamp until back under the cap. Wrap-safe
  // age comparison (signed delta), mirroring the core's offline-queue bound.
  while (this->gw_pending_.size() > GW_PENDING_MAX) {
    auto oldest = this->gw_pending_.begin();
    for (auto it = this->gw_pending_.begin(); it != this->gw_pending_.end();
         ++it) {
      if (static_cast<int32_t>(it->second.timestamp -
                               oldest->second.timestamp) < 0)
        oldest = it;
    }
    ESP_LOGW(TAG, "gw_pending_ at cap (%u); dropping oldest key '%s'",
             static_cast<unsigned>(GW_PENDING_MAX), oldest->first.c_str());
    this->gw_pending_.erase(oldest);
  }
}

void ThingsBoardGatewayComponent::flush_gw_batch_() {
  if (this->gw_pending_.empty() || this->publisher_ == nullptr) return;
  // Leave datapoints pending across a disconnect; they drain once the session
  // is back. Mirrors the core's process_batch_ connectivity guard.
  if (this->parent_ == nullptr || !this->parent_->is_connected()) return;
  // Wait for on_core_connected: the core flips connection_active_ to true
  // several loop ticks before the bootstrap phase machine reaches
  // BOOT_GW_REPLAY (which seeds the replay queue). Flushing in that window
  // sends gateway telemetry for children TB has no routing for on this
  // session -> server-side close.
  if (!this->session_ready_) return;
  // Hold telemetry until the staggered child-connect replay has drained, so a
  // child is re-linked on TB before its datapoints are published.
  if (!this->pending_replay_.empty()) return;

  // Partition pending datapoints into per-device telemetry/attribute buckets.
  struct Bucket {
    std::vector<std::string> telemetry;
    std::vector<std::string> attributes;
  };
  std::map<std::string, Bucket> by_device;
  for (const auto &kv : this->gw_pending_) {
    Bucket &b = by_device[kv.second.device];
    if (kv.second.is_attribute)
      b.attributes.push_back(kv.first);
    else
      b.telemetry.push_back(kv.first);
  }

  // One bucket publish: rate-gate, build envelope, publish, drain pending,
  // record. Telemetry counts as n_points; attributes count as 0 data points
  // (still 1 message). On any non-fatal stop (rate limit deferral or publish
  // failure) the keys stay in gw_pending_ for the next batch window.
  auto publish_bucket = [&](const std::string &device_name,
                            const std::vector<std::string> &keys,
                            bool is_attribute) {
    if (keys.empty()) return;
    const uint32_t n_keys = static_cast<uint32_t>(keys.size());
    const uint32_t n_points = is_attribute ? 0 : n_keys;
    const char *kind = is_attribute ? "attributes" : "telemetry";

    if (!this->parent_->check_gateway_rate_limits(1, n_points)) {
      ESP_LOGD(TAG, "Gateway rate limit: deferring %s for '%s' (%u keys)",
               kind, device_name.c_str(), static_cast<unsigned>(n_keys));
      return;
    }

    std::string body = json::build_json([&](JsonObject root) {
      for (const std::string &map_key : keys) {
        auto it = this->gw_pending_.find(map_key);
        if (it != this->gw_pending_.end())
          root[it->second.key] = serialized(it->second.json_value);
      }
    });

    const bool ok = is_attribute
                        ? this->publisher_->gw_publish_attributes(device_name, body)
                        : this->publisher_->gw_publish_telemetry(device_name, body);
    if (!ok) {
      ESP_LOGW(TAG, "gw_publish_%s('%s') failed; %u keys still pending",
               kind, device_name.c_str(), static_cast<unsigned>(n_keys));
      return;
    }
    for (const std::string &map_key : keys) this->gw_pending_.erase(map_key);
    this->parent_->record_gateway_publish(1, n_points);
    ESP_LOGD(TAG, "Flushed %s for '%s' (%u keys, %u bytes)", kind,
             device_name.c_str(), static_cast<unsigned>(n_keys),
             static_cast<unsigned>(body.size()));
  };

  for (const auto &dev : by_device) {
    publish_bucket(dev.first, dev.second.telemetry, /*is_attribute=*/false);
    publish_bucket(dev.first, dev.second.attributes, /*is_attribute=*/true);
  }
}

// --- Codegen trigger accessors (lazy heap-alloc, program-lifetime) -------

Trigger<std::string, std::string, std::string> *
ThingsBoardGatewayComponent::get_child_rpc_trigger(
    const std::string &device_name) {
  auto it = this->child_rpc_triggers_.find(device_name);
  if (it != this->child_rpc_triggers_.end()) return it->second;
  auto *trig = new Trigger<std::string, std::string, std::string>();  // NOLINT
  this->child_rpc_triggers_[device_name] = trig;
  return trig;
}

Trigger<std::map<std::string, std::string>> *
ThingsBoardGatewayComponent::get_child_shared_attr_trigger(
    const std::string &device_name) {
  auto it = this->child_shared_attr_triggers_.find(device_name);
  if (it != this->child_shared_attr_triggers_.end()) return it->second;
  auto *trig = new Trigger<std::map<std::string, std::string>>();  // NOLINT
  this->child_shared_attr_triggers_[device_name] = trig;
  return trig;
}

Trigger<std::string, std::string, std::string, std::string> *
ThingsBoardGatewayComponent::get_global_rpc_trigger() {
  if (this->global_rpc_trigger_ == nullptr) {
    this->global_rpc_trigger_ =
        new Trigger<std::string, std::string, std::string,  // NOLINT
                    std::string>();
  }
  return this->global_rpc_trigger_;
}

Trigger<std::string, std::map<std::string, std::string>> *
ThingsBoardGatewayComponent::get_global_shared_attr_trigger() {
  if (this->global_shared_attr_trigger_ == nullptr) {
    this->global_shared_attr_trigger_ =
        new Trigger<std::string,  // NOLINT
                    std::map<std::string, std::string>>();
  }
  return this->global_shared_attr_trigger_;
}

// --- TBGatewayTransport inbound ------------------------------------------

void ThingsBoardGatewayComponent::handle_service_rpc_(
    const std::string &request_id, const std::string &method,
    const std::string &params) {
  (void) request_id;
  if (method == "gateway_device_renamed") {
    // params: {"Old name":"New name"} — a single-entry object.
    std::string old_name, new_name;
    json::parse_json(params, [&](JsonObject root) -> bool {
      for (JsonPair kv : root) {
        old_name = kv.key().c_str();
        if (kv.value().is<const char *>())
          new_name = kv.value().as<const char *>();
        break;
      }
      return true;
    });
    if (old_name.empty() || new_name.empty()) {
      ESP_LOGW(TAG, "gateway_device_renamed: could not parse '%s'",
               params.c_str());
      return;
    }
    auto it = this->children_.find(old_name);
    if (it != this->children_.end()) {
      ChildDevice child = it->second;
      child.name = new_name;
      this->children_.erase(it);
      this->children_[new_name] = child;
    }
    // Carry any per-child triggers across to the new key.
    auto rpc_it = this->child_rpc_triggers_.find(old_name);
    if (rpc_it != this->child_rpc_triggers_.end()) {
      this->child_rpc_triggers_[new_name] = rpc_it->second;
      this->child_rpc_triggers_.erase(rpc_it);
    }
    auto attr_it = this->child_shared_attr_triggers_.find(old_name);
    if (attr_it != this->child_shared_attr_triggers_.end()) {
      this->child_shared_attr_triggers_[new_name] = attr_it->second;
      this->child_shared_attr_triggers_.erase(attr_it);
    }
    ESP_LOGI(TAG, "Gateway child renamed: '%s' -> '%s'", old_name.c_str(),
             new_name.c_str());
  } else if (method == "gateway_device_deleted") {
    // params is a bare JSON string ("Removed name"), not an object.
    std::string name = params;
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
      name = name.substr(1, name.size() - 2);
    this->children_.erase(name);
    this->child_rpc_triggers_.erase(name);
    this->child_shared_attr_triggers_.erase(name);
    ESP_LOGI(TAG, "Gateway child deleted: '%s'", name.c_str());
  } else {
    ESP_LOGD(TAG, "Unhandled gateway service RPC: %s", method.c_str());
  }
}

void ThingsBoardGatewayComponent::on_gateway_rpc_request(
    const std::string &device_name, const std::string &request_id,
    const std::string &method, const std::string &params) {
  // Empty device_name = TB service RPC (gateway_device_renamed /
  // gateway_device_deleted), forwarded by the core from the device-API RPC
  // topic. Anything else is a per-child RPC from v1/gateway/rpc.
  if (device_name.empty()) {
    this->handle_service_rpc_(request_id, method, params);
    return;
  }

  // B-layer: an auto-mapped ESPHome sub-device. A `domain.method` RPC resolves
  // against that sub-device's own entities through the core control iterator
  // (device-id-scoped), and the result is published straight back on
  // v1/gateway/rpc. Falls through to the A-layer triggers for non-sub-devices
  // or non-`domain.method` payloads.
  auto sub_it = this->sub_device_ids_.find(device_name);
  if (sub_it != this->sub_device_ids_.end() && this->parent_ != nullptr &&
      method.find('.') != std::string::npos) {
    std::string response;
    this->parent_->handle_child_rpc(method, params, sub_it->second, response);
    if (this->publisher_ != nullptr)
      this->publisher_->gw_publish_rpc_response(device_name, request_id,
                                                response);
    ESP_LOGD(TAG, "B-layer RPC '%s' for sub-device '%s' -> %s", method.c_str(),
             device_name.c_str(), response.c_str());
    return;
  }

  bool handled = false;
  auto trig_it = this->child_rpc_triggers_.find(device_name);
  if (trig_it != this->child_rpc_triggers_.end()) {
    trig_it->second->trigger(request_id, method, params);
    handled = true;
  }
  if (this->global_rpc_trigger_ != nullptr) {
    this->global_rpc_trigger_->trigger(device_name, request_id, method,
                                       params);
    handled = true;
  }
  if (!handled) {
    // No automation wired for this child — auto-decline so TB's RPC call
    // doesn't hang waiting for a response.
    ESP_LOGD(TAG, "Gateway RPC for '%s' (%s) unhandled; auto-declining",
             device_name.c_str(), method.c_str());
    if (this->publisher_ != nullptr)
      this->publisher_->gw_publish_rpc_response(device_name, request_id,
                                                "{\"success\":false}");
  }
}

void ThingsBoardGatewayComponent::on_gateway_shared_attributes(
    const std::string &device_name,
    const std::map<std::string, std::string> &attributes) {
  auto it = this->child_shared_attr_triggers_.find(device_name);
  if (it != this->child_shared_attr_triggers_.end())
    it->second->trigger(attributes);
  if (this->global_shared_attr_trigger_ != nullptr)
    this->global_shared_attr_trigger_->trigger(device_name, attributes);

  // B-layer: when the child is an auto-mapped ESPHome sub-device, route the
  // attribute map through the existing per-domain register_shared_attributes
  // callbacks so a TB shared-attr write to a sub-device child actually toggles
  // / writes to that entity. Mirrors the RPC path in on_gateway_rpc_request
  // (handle_child_rpc) above.
  auto sub_it = this->sub_device_ids_.find(device_name);
  if (sub_it != this->sub_device_ids_.end() && this->parent_ != nullptr) {
    this->parent_->handle_child_shared_attributes(attributes, sub_it->second);
  }

  ESP_LOGD(TAG, "Gateway shared attributes for '%s' (%u keys)",
           device_name.c_str(), static_cast<unsigned>(attributes.size()));
}

void ThingsBoardGatewayComponent::on_gateway_attribute_response(
    const std::string &device_name, const std::string &request_id,
    const std::map<std::string, std::string> &attributes) {
  (void) request_id;
  // Attribute-request responses carry the same shape as a shared-attribute
  // push; route them to the same per-child + global triggers.
  auto it = this->child_shared_attr_triggers_.find(device_name);
  if (it != this->child_shared_attr_triggers_.end())
    it->second->trigger(attributes);
  if (this->global_shared_attr_trigger_ != nullptr)
    this->global_shared_attr_trigger_->trigger(device_name, attributes);
  ESP_LOGD(TAG, "Gateway attribute response for '%s' (%u keys)",
           device_name.c_str(), static_cast<unsigned>(attributes.size()));
}

}  // namespace thingsboard_gateway
}  // namespace esphome

#endif  // USE_ESP32
