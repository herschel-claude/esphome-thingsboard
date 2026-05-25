"""ThingsBoard Gateway API component (MQTT-only).

Proxies child devices over the `v1/gateway/*` topic family. Additive to the
device API: a gateway is still an ordinary device on `v1/devices/me/*` for
itself. See https://thingsboard.io/docs/reference/gateway-mqtt-api/.

A-layer surface (this module): a `devices:` list of statically declared
children, per-child and global `on_child_rpc` / `on_child_shared_attributes`
automations, and `child_*` actions for marshalling data from a local bus.
B-layer surface: `auto_map_sub_devices` auto-registers every ESPHome sub-device
(`esphome: devices:`) as a TB gateway child and routes its telemetry / RPC
through `v1/gateway/*`.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components.thingsboard import ThingsBoardComponent
from esphome.const import CONF_ID, CONF_NAME
from esphome.core import CORE, coroutine_with_priority

CODEOWNERS = ["@rjt-rockx"]
# Depends on the core component and the MQTT transport. No AUTO_LOAD: the user
# declares the transport themselves, and the core's final-validator enforces
# exactly one transport — the gateway must not pull a second one in.
DEPENDENCIES = ["thingsboard", "thingsboard_mqtt"]

CONF_THINGSBOARD_ID = "thingsboard_id"
CONF_DEVICES = "devices"
CONF_DEVICE_TYPE = "device_type"
CONF_AUTO_CONNECT = "auto_connect"
CONF_ON_CHILD_RPC = "on_child_rpc"
CONF_ON_CHILD_SHARED_ATTRIBUTES = "on_child_shared_attributes"
CONF_DEVICE_NAME = "device_name"
CONF_REQUEST_ID = "request_id"
CONF_DATA = "data"
CONF_KEYS = "keys"
CONF_CLIENT_SCOPE = "client_scope"
CONF_AUTO_MAP_SUB_DEVICES = "auto_map_sub_devices"

thingsboard_gateway_ns = cg.esphome_ns.namespace("thingsboard_gateway")
ThingsBoardGatewayComponent = thingsboard_gateway_ns.class_(
    "ThingsBoardGatewayComponent", cg.Component
)

ChildConnectAction = thingsboard_gateway_ns.class_(
    "ChildConnectAction", automation.Action
)
ChildDisconnectAction = thingsboard_gateway_ns.class_(
    "ChildDisconnectAction", automation.Action
)
ChildSendTelemetryAction = thingsboard_gateway_ns.class_(
    "ChildSendTelemetryAction", automation.Action
)
ChildSendAttributesAction = thingsboard_gateway_ns.class_(
    "ChildSendAttributesAction", automation.Action
)
ChildSendRpcResponseAction = thingsboard_gateway_ns.class_(
    "ChildSendRpcResponseAction", automation.Action
)
ChildRequestAttributesAction = thingsboard_gateway_ns.class_(
    "ChildRequestAttributesAction", automation.Action
)

# Per-child `devices:` entry: a name, an optional TB device type, whether to
# bring it up at connect time, and optional per-child inbound automations.
DEVICE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string,
        cv.Optional(CONF_DEVICE_TYPE, default=""): cv.string,
        cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
        cv.Optional(CONF_ON_CHILD_RPC): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_CHILD_SHARED_ATTRIBUTES): automation.validate_automation(
            single=True
        ),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ThingsBoardGatewayComponent),
        cv.Required(CONF_THINGSBOARD_ID): cv.use_id(ThingsBoardComponent),
        cv.Optional(CONF_DEVICES): cv.ensure_list(DEVICE_SCHEMA),
        # Top-level any-child variants: fire for every child, with device_name
        # as the leading trigger argument.
        cv.Optional(CONF_ON_CHILD_RPC): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_CHILD_SHARED_ATTRIBUTES): automation.validate_automation(
            single=True
        ),
        # B-layer: auto-register every ESPHome sub-device (`esphome: devices:`)
        # as a TB gateway child. No-op unless the build defines USE_DEVICES.
        cv.Optional(CONF_AUTO_MAP_SUB_DEVICES, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    """The ThingsBoard Gateway API is MQTT-only — reject an HTTP transport."""
    if "thingsboard_mqtt" not in CORE.loaded_integrations:
        raise cv.Invalid(
            "thingsboard_gateway: requires the MQTT transport. Add a "
            "`thingsboard_mqtt:` block — the ThingsBoard Gateway API is "
            "MQTT-only and has no HTTP equivalent."
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

# Trigger argument lists, kept here so the per-child and global wiring stay in
# sync with the C++ Trigger<> template arguments.
_STD_MAP = cg.std_ns.class_("map").template(cg.std_string, cg.std_string)
_CHILD_RPC_ARGS = [
    (cg.std_string, "request_id"),
    (cg.std_string, "method"),
    (cg.std_string, "params"),
]
_GLOBAL_RPC_ARGS = [(cg.std_string, "device_name")] + _CHILD_RPC_ARGS
_CHILD_ATTR_ARGS = [(_STD_MAP, "attributes")]
_GLOBAL_ATTR_ARGS = [(cg.std_string, "device_name"), (_STD_MAP, "attributes")]


@coroutine_with_priority(40.0)
async def to_code(config):
    # Gates the B-layer code in the core component (entity-resolution and
    # child-telemetry routing in thingsboard_client.{h,cpp}) and the gateway
    # rate-limit counters. The A-layer needs no define -- it routes through
    # the always-present TBGatewayTransport / TBGatewayPublisher virtuals.
    cg.add_define("USE_THINGSBOARD_GATEWAY")

    var = cg.new_Pvariable(config[CONF_ID])

    tb_component = await cg.get_variable(config[CONF_THINGSBOARD_ID])
    cg.add(var.set_thingsboard_component(tb_component))
    cg.add(var.set_auto_map_sub_devices(config[CONF_AUTO_MAP_SUB_DEVICES]))

    await cg.register_component(var, config)

    # Statically declared children + their per-child inbound automations.
    for dev in config.get(CONF_DEVICES, []):
        name = dev[CONF_NAME]
        cg.add(var.add_child(name, dev[CONF_DEVICE_TYPE], dev[CONF_AUTO_CONNECT]))
        if CONF_ON_CHILD_RPC in dev:
            await automation.build_automation(
                var.get_child_rpc_trigger(name),
                _CHILD_RPC_ARGS,
                dev[CONF_ON_CHILD_RPC],
            )
        if CONF_ON_CHILD_SHARED_ATTRIBUTES in dev:
            await automation.build_automation(
                var.get_child_shared_attr_trigger(name),
                _CHILD_ATTR_ARGS,
                dev[CONF_ON_CHILD_SHARED_ATTRIBUTES],
            )

    # Top-level any-child automations.
    if CONF_ON_CHILD_RPC in config:
        await automation.build_automation(
            var.get_global_rpc_trigger(),
            _GLOBAL_RPC_ARGS,
            config[CONF_ON_CHILD_RPC],
        )
    if CONF_ON_CHILD_SHARED_ATTRIBUTES in config:
        await automation.build_automation(
            var.get_global_shared_attr_trigger(),
            _GLOBAL_ATTR_ARGS,
            config[CONF_ON_CHILD_SHARED_ATTRIBUTES],
        )


# --- A-layer Actions -----------------------------------------------------
# Each follows the core component's ThingsBoardSendTelemetryAction pattern:
# Parented<> to the gateway, templatable string fields, play() forwards to a
# child_* method.

_CHILD_ACTION_BASE = {
    cv.GenerateID(): cv.use_id(ThingsBoardGatewayComponent),
    cv.Required(CONF_DEVICE_NAME): cv.templatable(cv.string),
}


@automation.register_action(
    "thingsboard_gateway.child_connect",
    ChildConnectAction,
    cv.Schema(
        {
            **_CHILD_ACTION_BASE,
            cv.Optional(CONF_DEVICE_TYPE, default=""): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def child_connect_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_device_name(
            await cg.templatable(config[CONF_DEVICE_NAME], args, cg.std_string)
        )
    )
    cg.add(
        var.set_device_type(
            await cg.templatable(config[CONF_DEVICE_TYPE], args, cg.std_string)
        )
    )
    return var


@automation.register_action(
    "thingsboard_gateway.child_disconnect",
    ChildDisconnectAction,
    cv.Schema(_CHILD_ACTION_BASE),
    synchronous=True,
)
async def child_disconnect_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_device_name(
            await cg.templatable(config[CONF_DEVICE_NAME], args, cg.std_string)
        )
    )
    return var


@automation.register_action(
    "thingsboard_gateway.child_send_telemetry",
    ChildSendTelemetryAction,
    cv.Schema(
        {
            **_CHILD_ACTION_BASE,
            cv.Required(CONF_DATA): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def child_send_telemetry_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_device_name(
            await cg.templatable(config[CONF_DEVICE_NAME], args, cg.std_string)
        )
    )
    cg.add(var.set_data(await cg.templatable(config[CONF_DATA], args, cg.std_string)))
    return var


@automation.register_action(
    "thingsboard_gateway.child_send_attributes",
    ChildSendAttributesAction,
    cv.Schema(
        {
            **_CHILD_ACTION_BASE,
            cv.Required(CONF_DATA): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def child_send_attributes_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_device_name(
            await cg.templatable(config[CONF_DEVICE_NAME], args, cg.std_string)
        )
    )
    cg.add(var.set_data(await cg.templatable(config[CONF_DATA], args, cg.std_string)))
    return var


@automation.register_action(
    "thingsboard_gateway.child_send_rpc_response",
    ChildSendRpcResponseAction,
    cv.Schema(
        {
            **_CHILD_ACTION_BASE,
            cv.Required(CONF_REQUEST_ID): cv.templatable(cv.string),
            cv.Required(CONF_DATA): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def child_send_rpc_response_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_device_name(
            await cg.templatable(config[CONF_DEVICE_NAME], args, cg.std_string)
        )
    )
    cg.add(
        var.set_request_id(
            await cg.templatable(config[CONF_REQUEST_ID], args, cg.std_string)
        )
    )
    cg.add(var.set_data(await cg.templatable(config[CONF_DATA], args, cg.std_string)))
    return var


@automation.register_action(
    "thingsboard_gateway.child_request_attributes",
    ChildRequestAttributesAction,
    cv.Schema(
        {
            **_CHILD_ACTION_BASE,
            cv.Required(CONF_REQUEST_ID): cv.templatable(cv.string),
            cv.Required(CONF_KEYS): cv.templatable(cv.string),
            cv.Optional(CONF_CLIENT_SCOPE, default=False): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def child_request_attributes_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_device_name(
            await cg.templatable(config[CONF_DEVICE_NAME], args, cg.std_string)
        )
    )
    cg.add(
        var.set_request_id(
            await cg.templatable(config[CONF_REQUEST_ID], args, cg.std_string)
        )
    )
    cg.add(var.set_keys(await cg.templatable(config[CONF_KEYS], args, cg.std_string)))
    cg.add(
        var.set_client_scope(
            await cg.templatable(config[CONF_CLIENT_SCOPE], args, cg.bool_)
        )
    )
    return var
