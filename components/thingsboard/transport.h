#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace esphome {
namespace thingsboard {

// Firmware metadata advertised by ThingsBoard via shared attributes
// (`fw_title`, `fw_version`, `fw_size`, `fw_checksum`, `fw_checksum_algorithm`).
struct FirmwareInfo {
  std::string title;
  std::string version;
  std::string checksum;
  std::string checksum_algorithm;
  size_t size{0};
};

// Optional OTA surface a transport may implement. Implementations report state
// transitions back through telemetry on the owning transport: `fw_state` is
// one of {DOWNLOADING, DOWNLOADED, VERIFIED, UPDATING, UPDATED, FAILED}.
class TBOTATransport {
 public:
  virtual ~TBOTATransport() = default;
  // Invoked by core when a new `fw_*` shared-attribute set arrives.
  virtual void on_firmware_advertised(const FirmwareInfo &info) = 0;
  // Abort an in-flight transfer (e.g., on disconnect).
  virtual void abort() = 0;
};

// Optional inbound gateway-API surface. A `thingsboard_gateway` component
// implements this; the MQTT transport's `v1/gateway/*` router forwards decoded
// events here. Optional and resolved at runtime, exactly like TBOTATransport.
// Every gateway-API message is keyed by the child device *name* (the TB
// Gateway API uses the child name in place of the device API's implicit "me").
class TBGatewayTransport {
 public:
  virtual ~TBGatewayTransport() = default;
  virtual void on_gateway_rpc_request(const std::string &device_name,
                                      const std::string &request_id,
                                      const std::string &method,
                                      const std::string &params) = 0;
  virtual void on_gateway_shared_attributes(
      const std::string &device_name,
      const std::map<std::string, std::string> &attributes) = 0;
  virtual void on_gateway_attribute_response(
      const std::string &device_name, const std::string &request_id,
      const std::map<std::string, std::string> &attributes) = 0;
  // Invoked by core after the device API (re)connects. The underlying MQTT
  // session drops gateway-child routing on every reconnect, so the gateway
  // component uses this hook to replay its child registry.
  virtual void on_core_connected() {}
  // Invoked by core when the device API disconnects (intentional or otherwise).
  // The gateway uses this to gate v1/gateway/* publishes: TB drops child
  // routing on disconnect, so any queued telemetry must wait for the next
  // on_core_connected (which re-seeds gw_connect for every connected child)
  // before going out -- otherwise TB sees telemetry for a child it has no
  // routing for and closes the socket.
  virtual void on_core_disconnected() {}
  // B-layer routing seam. When an ESPHome entity belongs to a sub-device, the
  // core offers its telemetry/attribute datapoints here (one key/value at a
  // time); the gateway component resolves `device_id` to a child name and
  // re-batches onto `v1/gateway/*`. `device_id` is the ESPHome sub-device id
  // (never 0 on this path). `json_value` is a raw JSON fragment spliced
  // verbatim. Returns true if the gateway owns that sub-device and took the
  // datapoint; false (the default) means the core keeps it on the device-API
  // batch — so a registered-but-not-auto-mapping gateway is transparent.
  virtual bool ingest_child_telemetry(uint32_t device_id,
                                      const std::string &key,
                                      const std::string &json_value,
                                      bool is_attribute) {
    return false;
  }
};

// Optional outbound gateway-API surface. The MQTT transport implements this;
// the gateway component calls it. Kept separate from TBTransport because only
// MQTT speaks the gateway API, and separate from the concrete transport class
// so the gateway component depends only on this header (no cross-component
// #include). The transport wraps each payload in the device-name-keyed
// envelope the TB Gateway API expects. Inbound `v1/gateway/*` events are
// delivered through TBGatewayTransport, not here — this interface is purely
// outbound.
class TBGatewayPublisher {
 public:
  virtual ~TBGatewayPublisher() = default;
  // Publishes to v1/gateway/connect — registers/links a child device so TB
  // routes that child's RPC and shared-attribute updates back to us.
  virtual bool gw_connect(const std::string &device_name,
                          const std::string &device_type = "") = 0;
  // Publishes to v1/gateway/disconnect.
  virtual bool gw_disconnect(const std::string &device_name) = 0;
  // `payload` is the per-device value object/array; the transport wraps it as
  // `{"<device_name>": <payload>}`.
  virtual bool gw_publish_telemetry(const std::string &device_name,
                                    const std::string &payload) = 0;
  virtual bool gw_publish_attributes(const std::string &device_name,
                                     const std::string &payload) = 0;
  // `keys` is comma-separated; `client_scope` selects clientKeys vs sharedKeys.
  virtual bool gw_request_attributes(const std::string &device_name,
                                     const std::string &request_id,
                                     const std::string &keys,
                                     bool client_scope) = 0;
  virtual bool gw_publish_rpc_response(const std::string &device_name,
                                       const std::string &request_id,
                                       const std::string &payload) = 0;
  virtual bool gw_publish_claim(const std::string &device_name,
                                const std::string &payload) = 0;
};

// Transport-agnostic interface the core ThingsBoardComponent uses to talk to
// a TB server. Concrete implementations live in sibling components
// (thingsboard_mqtt, thingsboard_http); the core never touches an ESP-MQTT
// client or esp_http_client directly.
class TBTransport {
 public:
  virtual ~TBTransport() = default;

  // True when the transport believes it can publish.
  virtual bool is_connected() const = 0;

  virtual bool publish_telemetry(const std::string &payload) = 0;

  // publish_attributes() is retained for historical call sites;
  // publish_client_attributes() is the canonical one.
  virtual bool publish_attributes(const std::string &payload) = 0;
  virtual bool publish_client_attributes(const std::string &payload) = 0;

  virtual bool publish_rpc_response(const std::string &request_id,
                                    const std::string &payload) = 0;

  virtual bool publish_rpc_request(const std::string &request_id,
                                   const std::string &method,
                                   const std::string &params) = 0;

  // `keys` is the raw MQTT payload body (e.g.
  // `{"clientKeys":"a,b","sharedKeys":"c,d"}`); the HTTP transport re-emits it
  // as a query string.
  virtual bool publish_attribute_request(const std::string &request_id,
                                         const std::string &keys) = 0;

  virtual bool publish_provision_request(const std::string &payload) = 0;

  virtual bool publish_claim(const std::string &payload) = 0;

  // Concrete transports invoke these when an inbound event arrives so the same
  // core dispatch logic runs regardless of transport.
  using ConnectedCb = std::function<void()>;
  using DisconnectedCb = std::function<void()>;
  using RpcRequestCb = std::function<void(const std::string &request_id,
                                          const std::string &method,
                                          const std::string &params)>;
  using SharedAttributesCb =
      std::function<void(const std::map<std::string, std::string> &)>;
  using AttributeResponseCb =
      std::function<void(const std::string &request_id,
                         const std::map<std::string, std::string> &)>;
  using RpcResponseCb = std::function<void(const std::string &request_id,
                                           const std::string &response)>;
  using ProvisionResponseCb =
      std::function<void(const std::string &response_json)>;

  void set_on_connected(ConnectedCb cb) { on_connected_ = std::move(cb); }
  void set_on_disconnected(DisconnectedCb cb) {
    on_disconnected_ = std::move(cb);
  }
  void set_on_rpc_request(RpcRequestCb cb) { on_rpc_request_ = std::move(cb); }
  void set_on_shared_attributes(SharedAttributesCb cb) {
    on_shared_attributes_ = std::move(cb);
  }
  void set_on_attribute_response(AttributeResponseCb cb) {
    on_attribute_response_ = std::move(cb);
  }
  void set_on_rpc_response(RpcResponseCb cb) {
    on_rpc_response_ = std::move(cb);
  }
  void set_on_provision_response(ProvisionResponseCb cb) {
    on_provision_response_ = std::move(cb);
  }

 protected:
  ConnectedCb on_connected_;
  DisconnectedCb on_disconnected_;
  RpcRequestCb on_rpc_request_;
  SharedAttributesCb on_shared_attributes_;
  AttributeResponseCb on_attribute_response_;
  RpcResponseCb on_rpc_response_;
  ProvisionResponseCb on_provision_response_;
};

}  // namespace thingsboard
}  // namespace esphome
