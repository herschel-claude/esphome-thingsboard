# `thingsboard_mqtt`

MQTT transport for the [`thingsboard`](../thingsboard/) ESPHome custom component.
Implements ThingsBoard's MQTT device API as documented at
<https://thingsboard.io/docs/reference/mqtt-api/>.

End firmware uses **either** `thingsboard_mqtt:` **or** `thingsboard_http:`,
never both. The core component enforces this via its `FINAL_VALIDATE_SCHEMA`.

## YAML

```yaml
thingsboard:
  server_url: !secret thingsboard_server_url
  device_name: ${name}

thingsboard_mqtt:
  thingsboard_id: thingsboard_component
  broker: !secret thingsboard_mqtt_broker
  # port: 8883            # optional: defaults to 8883 when TLS material
                          # below is present, 1883 otherwise.
  device_token: !secret tb_device_token # optional if `provisioning:` set
  # qos: 1                # default 1 (TB-recommended at-least-once); 0/2 also accepted.
  # retain: false         # default false. Retained TB topics are rarely useful,
                          # so leave off unless your broker pipeline requires it.
  # Optional TLS material. `server_ca_pem` enables TLS with ACCESS_TOKEN or
  # MQTT_BASIC auth; X509_CERTIFICATE credentials require both
  # certificate_pem and private_key_pem. See the top-level README for the
  # full TLS surface.
  server_ca_pem: !secret tb_server_ca
  credentials:
    type: X509_CERTIFICATE
    certificate_pem: !secret tb_client_cert
    private_key_pem: !secret tb_client_key
  provisioning:
    key: !secret tb_provisioning_key
    secret: !secret tb_provisioning_secret
    # credentials variants (see TB device-provisioning docs)
```

## TB device API coverage

| Operation                         | Status        | Notes                                          |
| --------------------------------- | ------------- | ---------------------------------------------- |
| Telemetry upload (server-ts)      | live          | pub `v1/devices/me/telemetry`                  |
| Telemetry upload (client-ts)      | live          | same topic, `{ts, values}` shape               |
| Client-attribute upload           | live          | pub `v1/devices/me/attributes`                 |
| Attribute request                 | live          | request_id round-trip                          |
| Shared-attribute push             | live          | sub `v1/devices/me/attributes`                 |
| Server-side RPC (TB→device)       | live          | request/response topics                        |
| Client-side RPC (device→TB)       | live          | `send_rpc_request` action                      |
| `getSessionLimits`                | live          | called automatically post-connect              |
| Device claim                      | live          | `claim_device` action                          |
| Provisioning, server token        | live          |                                                |
| Provisioning, device token        | payload wired | `credentialsType:"ACCESS_TOKEN"` + `token`     |
| Provisioning, `MQTT_BASIC`        | payload wired | `clientId`/`username`/`password`               |
| Provisioning, `X509_CERTIFICATE`  | payload wired | `hash` (cert PEM)                              |
| OTA over MQTT (`v2/fw/*`)         | live          | chunked binary, see `../thingsboard_mqtt_ota/` |
| SOTA (software updates)           | unsupported   |                                                |
| Gateway protocol (`v1/gateway/*`) | live          | opt-in, see `../thingsboard_gateway/`          |
| Auth, access token                | live          | `username = $TOKEN` in CONNECT                 |
| Auth, X.509 mTLS                  | live          | `set_client_certificate` + `set_server_ca`     |
| Auth, `MQTT_BASIC`                | live          | `set_basic_credentials`                        |

## Gateway API topics

When a [`thingsboard_gateway`](../thingsboard_gateway/) component is present the
transport also speaks the `v1/gateway/*` topic family over the same MQTT
connection. These are handled by `ThingsBoardMQTT` but driven entirely by the
gateway component; plain device firmware never touches them.

| Topic                          | Direction | Purpose                                          |
| ------------------------------ | --------- | ------------------------------------------------ |
| `v1/gateway/connect`           | publish   | register / link a child device by name          |
| `v1/gateway/disconnect`        | publish   | unlink a child device                            |
| `v1/gateway/telemetry`         | publish   | per-child telemetry, name-keyed envelope         |
| `v1/gateway/attributes`        | publish   | per-child client-attribute upload                |
| `v1/gateway/attributes/request`| publish   | request a child's shared/client attributes       |
| `v1/gateway/rpc`               | both      | inbound child RPC request / outbound response    |
| `v1/gateway/attributes`        | subscribe | server-pushed shared attributes for a child      |
| `v1/gateway/attributes/response`| subscribe| reply to an attribute request                    |

The `gateway_device_renamed` / `gateway_device_deleted` service RPCs arrive on
the ordinary device-API RPC topic (`v1/devices/me/rpc/request/+`) and the core
component forwards them to the gateway component.

## Credential rotation

`ThingsBoardMQTT::connect()` re-pushes the esp-mqtt client config via
`esp_mqtt_set_config()` whenever the configured device token differs from the
last value pushed into the live client. This covers provisioning →
device-token transitions and admin-driven token rotations. Without this the
existing client would simply reconnect with stale credentials and the broker
would reject the CONNECT with an auth error.
