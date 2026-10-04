import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import (
    CONF_HEIGHT,
    CONF_ID,
    CONF_PORT,
    CONF_TIMEOUT,
    CONF_TRIGGER_ID,
    CONF_WIDTH,
)

CODEOWNERS = ["@cosmicDustOfLightLength"]
DEPENDENCIES = ["network", "display"]

CONF_ON_REDRAW = "on_redraw"

ddp_display_ns = cg.esphome_ns.namespace("ddp_display")
DDPDisplay = ddp_display_ns.class_("DDPDisplay", cg.Component)
DDPRedrawTrigger = ddp_display_ns.class_(
    "DDPRedrawTrigger", automation.Trigger.template()
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DDPDisplay),
            cv.Required(CONF_WIDTH): cv.int_range(min=1, max=1024),
            cv.Required(CONF_HEIGHT): cv.int_range(min=1, max=1024),
            cv.Optional(CONF_PORT, default=4048): cv.port,
            # Auto mode: how long after the last frame the normal display comes back.
            # 0s = never time out (last DDP frame stays until mode changes).
            cv.Optional(
                CONF_TIMEOUT, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_REDRAW): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DDPRedrawTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_size(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))

    for conf in config.get(CONF_ON_REDRAW, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
