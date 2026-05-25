# `thingsboard_gateway`

ThingsBoard Gateway API bridge for the [`thingsboard_mqtt`](../thingsboard_mqtt/)
transport. Proxies many child devices over one MQTT connection using the
`v1/gateway/*` topic family, as documented at
<https://thingsboard.io/docs/reference/gateway-mqtt-api/>.

Requires the MQTT transport: the ThingsBoard Gateway API is MQTT-only and has no
HTTP equivalent. Configuring `thingsboard_gateway` alongside `thingsboard_http`
is rejected at validation time.

## How it works

The gateway is **additive**: the host ESP32 stays an ordinary ThingsBoard device
on `v1/devices/me/*` (via the core `thingsboard` component) and *also* proxies
named child devices on `v1/gateway/*` over the same MQTT connection. Every
gateway-API payload is keyed by child device **name** instead of the device
API's implicit "me".

The host device must be flagged as a gateway in ThingsBoard
(`additionalInfo.gateway = true`, the "Is gateway" checkbox) for the
`v1/gateway/*` topics to be accepted. This is a server-side setup step, not
something the firmware can do over the device API. If the flag is unset the
broker silently drops every `v1/gateway/*` publish.

Two layers sit on top of the component:

- **A-layer** -- a thin action/trigger surface for child devices reached over a
  local bus (BLE, Modbus, UART). User lambdas marshal each child's data into the
  `child_send_*` actions and respond to the `on_child_*` triggers.
- **B-layer** -- opt-in auto-mapping of ESPHome sub-devices (`esphome: devices:`
  entries) to TB gateway children. Telemetry, attributes, and RPC for any entity
  tagged with a `device_id:` route through the gateway child of the same name,
  with no marshalling lambdas at all.

The two layers compose: a single `thingsboard_gateway:` block can declare
A-layer `devices:` *and* set `auto_map_sub_devices: true`.

## Minimal YAML

```yaml
thingsboard:
  id: thingsboard_component
  server_url: !secret thingsboard_server_url

thingsboard_mqtt:
  thingsboard_id: thingsboard_component
  broker: !secret thingsboard_mqtt_broker
  device_token: !secret tb_device_token

thingsboard_gateway:
  thingsboard_id: thingsboard_component
```

## A-layer: children on a local bus

Declare known children under `devices:`, attach per-child inbound automations,
and marshal outbound data with the `child_*` actions. An unknown device name
passed to a `child_send_*` action is lazily registered and connected with an
empty device type, so a `devices:` entry is optional -- use it to pin a device
type or to bring a child up at boot.

```yaml
thingsboard_gateway:
  thingsboard_id: thingsboard_component
  devices:
    - name: "sensor-hub-1"
      device_type: "Generic Sensor"   # optional, default ""
      auto_connect: true              # optional, default true
      on_child_rpc:
        then:
          - logger.log:
              format: "RPC for sensor-hub-1: %s (id=%s)"
              args: ["method.c_str()", "request_id.c_str()"]
          - thingsboard_gateway.child_send_rpc_response:
              device_name: "sensor-hub-1"
              request_id: !lambda "return request_id;"
              data: '{"success":true}'
      on_child_shared_attributes:
        then:
          - logger.log: "Shared attributes for sensor-hub-1"
  # Global any-child variants: device_name is the leading trigger argument.
  on_child_rpc:
    then:
      - logger.log:
          format: "Any-child RPC: %s -> %s"
          args: ["device_name.c_str()", "method.c_str()"]
  on_child_shared_attributes:
    then:
      - logger.log:
          format: "Any-child shared attributes: %s"
          args: ["device_name.c_str()"]
```

### Actions

All `device_name` / `device_type` / `request_id` / `keys` / `data` fields are
templatable.

| Action                                       | Fields                                    | Effect                                                            |
| -------------------------------------------- | ----------------------------------------- | ----------------------------------------------------------------- |
| `thingsboard_gateway.child_connect`          | `device_name`, `device_type` (opt)        | `gw_connect` -- registers/links the child with TB                 |
| `thingsboard_gateway.child_disconnect`       | `device_name`                             | `gw_disconnect` -- TB stops routing for the child                 |
| `thingsboard_gateway.child_send_telemetry`   | `device_name`, `data` (JSON object)       | batched onto `v1/gateway/telemetry`                               |
| `thingsboard_gateway.child_send_attributes`  | `device_name`, `data` (JSON object)       | batched onto `v1/gateway/attributes`                              |
| `thingsboard_gateway.child_send_rpc_response`| `device_name`, `request_id`, `data`       | replies on `v1/gateway/rpc`                                       |
| `thingsboard_gateway.child_request_attributes`| `device_name`, `request_id`, `keys`, `client_scope` (opt, default false) | requests attributes on `v1/gateway/attributes/request` |

`data` for the `child_send_*` actions must be a JSON **object**; each key/value
is fanned into a per-child batch (per-(child, key) dedup) and flushed about
every 100 ms.

### Triggers

| Trigger                          | Scope     | Lambda variables                                              |
| -------------------------------- | --------- | ------------------------------------------------------------- |
| `on_child_rpc` (under a device)  | one child | `request_id`, `method`, `params` (all `std::string`)          |
| `on_child_rpc` (top level)       | any child | `device_name`, `request_id`, `method`, `params`               |
| `on_child_shared_attributes` (under a device) | one child | `attributes` (`std::map<std::string, std::string>`) |
| `on_child_shared_attributes` (top level)      | any child | `device_name`, `attributes`                          |

If no `on_child_rpc` automation is wired for a child, an inbound gateway RPC for
that child is auto-declined with `{"success":false}` so TB's call does not hang.

## B-layer: ESPHome sub-devices

Set `auto_map_sub_devices: true` and the component walks `App.get_devices()` at
`setup()`, registering every ESPHome sub-device (`esphome: devices:` entry) as a
gateway child of the same name. Any entity tagged with a `device_id:` then has
its telemetry, attributes, and RPC routed through that child automatically.

```yaml
esphome:
  name: my-gateway
  devices:
    - id: sub_relay_1
      name: "Relay 1"
    - id: sub_climate
      name: "Climate Sensors"

thingsboard_gateway:
  thingsboard_id: thingsboard_component
  auto_map_sub_devices: true

sensor:
  - platform: ...
    name: "Room Temperature"
    device_id: sub_climate      # routes to the "Climate Sensors" TB child

switch:
  - platform: ...
    name: "Relay"
    device_id: sub_relay_1      # telemetry + RPC route to the "Relay 1" child
```

Entities with **no** `device_id:` stay on the host's own device API
(`v1/devices/me/*`) -- the gateway mapping is additive, not a replacement.

`auto_map_sub_devices` is inert unless the build defines `USE_DEVICES`, which
ESPHome only does when the config has a non-empty `esphome: devices:` block. With
no sub-devices the option is a harmless no-op.

A B-layer RPC arriving on `v1/gateway/rpc` for a mapped sub-device, in
`domain.method` form (for example `switch.turn_on`), is resolved against that
sub-device's own entities through the core component's control iterator and the
result is published straight back -- no `on_child_rpc` lambda needed. Anything
else falls through to the A-layer triggers.

## Rate limits

ThingsBoard returns a separate `gatewayRateLimits` budget from `getSessionLimits`
(sliding-window tiers for messages, telemetry messages, and telemetry data
points). The component captures it and gates every `v1/gateway/*` publish: when
the budget is exhausted the affected keys stay pending and drain on the next
batch window. If TB returns no gateway limits the gate is a graceful no-op.

## Reconnect behavior

TB drops gateway-child routing whenever the MQTT session drops. On every core
(re)connect the component replays `gw_connect` for every child it considers
connected, so child telemetry keeps flowing after a WiFi blip without a reboot.
