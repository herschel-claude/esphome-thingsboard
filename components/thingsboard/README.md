# `thingsboard` core component

Transport-agnostic core of the ThingsBoard integration. Owns entity discovery,
the batch builder, attribute cache, RPC dispatch, OTA state machine, and the
ESPHome action surface (`thingsboard.send_telemetry`, etc.). The actual wire
protocol lives in sibling components:

- [`thingsboard_mqtt`](../thingsboard_mqtt/): MQTT device API
- [`thingsboard_http`](../thingsboard_http/): HTTP device API

End firmware uses **exactly one** transport. The core's `FINAL_VALIDATE_SCHEMA`
rejects the YAML if zero or both transports are configured.

## YAML (minimum viable, MQTT)

```yaml
thingsboard:
  server_url: !secret thingsboard_server_url
  device_name: ${name}

thingsboard_mqtt:
  broker: !secret thingsboard_mqtt_broker
  port: 1883
  device_token: !secret tb_device_token
```

## Tuning knobs

| Key | Default | Notes |
| --- | --- | --- |
| `telemetry_interval` | 100 ms | Floor on how often `process_batch_` emits. Raise this to coalesce bursty entity updates into fewer MQTT publishes. |
| `telemetry_throttle` | 0 (off) | Per-key rate limit. Last write within the window wins; the dropped intermediates never hit the wire. |
| `periodic_sync_interval` | 30 s | Cadence for the full re-publish that covers post-reconnect / dropped-packet replay. |
| `offline_queue_max` | 200 | When the broker is unreachable, `pending_messages_` keeps queuing up to this many entries (FIFO, last-write-wins per key). On reconnect the queue drains naturally. |
| `use_client_timestamps` | false | Emit `[{"ts": <ms>, "values": {...}}]` so replays after a disconnect retain the original capture timestamp rather than inheriting TB's server-receive time. |

The core also parses the `getSessionLimits` tier strings
(`"200:1,6000:60,14000:3600"`) into sliding-window counters and defers
publishes that would exceed any tier; payloads larger than the broker's
`maxPayloadSize` are split into chunks across multiple publishes on the
same topic.

## Telemetry shape

Per-entity telemetry keys are namespaced as `<domain>.<object_id>` (e.g.
`switch.heating_1`, `sensor.environmental_temperature`). Rich-domain
entities (`climate`, `light`, `cover`, `fan`, `valve`, `media_player`,
`lock`, `alarm_control_panel`) route through their `DomainHandler` and
emit one key per state field, so a climate entity publishes
`climate.<id>.mode`, `climate.<id>.action`,
`climate.<id>.current_temperature`, `climate.<id>.target_temperature`,
etc., rather than a single `climate.<id>` value. The on-connect
snapshot, the steady-state `on_*_update` callbacks, and the periodic
sync all route through the same handler so all three sources emit the
same shape.

## Architecture

```
                +-------------------+
   YAML actions |   thingsboard     |   action surface, batch builder,
       --->     |   (core)          |   entity discovery, RPC dispatch
                +---------+---------+
                          | TBTransport *
                          v
                +-------------------+
                | TBTransport       |   abstract interface (transport.h)
                +---------+---------+
                          |
        +-----------------+-----------------+
        |                                   |
+-----------------+              +-------------------+
| thingsboard_mqtt|              | thingsboard_http  |
| (esp-mqtt)      |              | (esp_http_client) |
+-----------------+              +-------------------+
```

Core never touches an ESP-MQTT client or `esp_http_client` directly. Every
protocol-level operation goes through `TBTransport`. Transport instances
register themselves with the core during their own `to_code()` /
`setup()` and are owned by their respective components.

## Protocol coverage

The core + transports implement the following ThingsBoard device-API surfaces:

- Telemetry upload
- Client-attribute upload
- Attribute request (with response dispatch)
- Shared-attribute push
- Server-side RPC (request + response)
- Client-side RPC
- `getSessionLimits` (MQTT-only; called automatically post-connect)
- Device claim
- Device provisioning (server-generated `ACCESS_TOKEN`; payload builder also
  emits `credentialsType` + variant fields for `ACCESS_TOKEN` / `MQTT_BASIC` /
  `X509_CERTIFICATE` strategies)
- Access-token auth

OTA is provided by the per-transport sibling components
([`thingsboard_mqtt_ota`](../thingsboard_mqtt_ota/) for MQTT chunked-binary,
[`thingsboard_http_ota`](../thingsboard_http_ota/) for streaming HTTPS GET);
see their READMEs for endpoint detail.

## Shared-attribute commands

Every controllable ESPHome entity registers a shared-attribute key derived
from its domain + object id:

| Domain    | Outbound telemetry / state key | Inbound command key (default)   |
| --------- | ------------------------------ | ------------------------------- |
| switch    | `switch.<object_id>`           | `set.switch.<object_id>`        |
| light     | `light.<object_id>`            | `set.light.<object_id>`         |
| number    | `number.<object_id>`           | `set.number.<object_id>`        |
| select    | `select.<object_id>`           | `set.select.<object_id>`        |
| climate   | `climate.<object_id>`          | `set.climate.<object_id>`      |
| fan       | `fan.<object_id>`              | `set.fan.<object_id>`           |
| cover     | `cover.<object_id>`            | `set.cover.<object_id>`         |
| valve     | `valve.<object_id>`            | `set.valve.<object_id>`         |
| lock      | `lock.<object_id>`             | `set.lock.<object_id>`          |
| button    | -                              | `set.button.<object_id>`        |
| text      | `text.<object_id>`             | `set.text.<object_id>`          |
| media_player | `media_player.<object_id>`  | `set.media_player.<object_id>` |
| alarm_control_panel | `alarm.<object_id>`   | `set.alarm.<object_id>`         |

Writing a shared attribute on the SHARED_SCOPE side (`set.switch.heating_1 =
true`) dispatches into the matching domain handler. Telemetry / state echoes
stay on the bare key. Per-domain command grammar:

- Switch / lock: `true`/`false` or `ON`/`OFF` (case-insensitive).
- Number / climate (`target_temperature`) / cover / valve: float scalar, or
  JSON object for multi-field setpoints.
- Light: JSON object (state, brightness, color, ...).
- Select / text: bare string.

Non-numeric values on float-typed handlers are logged and dropped rather
than crashing the dispatch (see `number_handler.cpp:register_shared_attributes`).

### Configurable prefixes

- `command_prefix` (default `"set."`): inbound command-attribute prefix.
  Override at the YAML level when an existing TB-side convention is in use.
- `state_prefix` (default `""`): outbound client-attribute (state echo)
  prefix. Default `""` keeps state echoes on the same scoped id as
  telemetry. Set to e.g. `"state."` to namespace state echoes separately
  on TB so scope-agnostic widgets read a single deterministic value per
  key. Mirrors the wire-level `gw_pending_` dedup separation in commit
  592a565 at the TB key level.

```yaml
thingsboard:
  server_url: !secret thingsboard_server_url
  device_name: ${name}
  command_prefix: "set."   # default
  state_prefix: ""         # default; set "state." to namespace state echoes
```

Connect-time, every registered `<command_prefix><domain>.<object_id>` key
is folded into the bootstrap `sharedKeys` request so setpoints survive a
reboot before the first server-side push. Non-scalar attribute values
(JSON objects, arrays) are now serialised through to handlers rather than
being dropped as `"unknown"`.

**Breaking change vs. pre-1.0:** handlers used to register bare object_ids.
SHARED_SCOPE writes targeting bare `heating_1` no longer dispatch; migrate
to `set.switch.heating_1` (or set `command_prefix: ""` if you must keep
the old convention -- collisions with telemetry keys then become your
responsibility).

## Files

- `transport.h`: `TBTransport` interface implemented by each transport
- `thingsboard_client.h/.cpp`: core component (batch builder, RPC dispatch,
  OTA state machine, action surface)
- `*_handler.cpp`: transport-agnostic domain handlers (switch, climate, etc.)
- `control_iterator.cpp`: RPC dispatch + shared-attribute fan-out
