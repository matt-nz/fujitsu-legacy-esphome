import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID

from . import Aot30Climate

CONF_RX_PIN = "rx_pin"
CONF_TX_PIN = "tx_pin"
CONF_DEBUG = "debug"
CONF_REMOTE_RELAY_PIN = "remote_relay_pin"
CONF_REMOTE_RELAY_INVERTED = "remote_relay_inverted"

# Plain pin numbers rather than ESPHome's GPIO pin schema: the pins go straight to HardwareSerial::begin() and
# the bit-banged transmitter. Defaults: GPIO16 = RX (from the LIN transceiver's TXD pin), GPIO17 = TX.
# remote_relay_pin (optional): a relay that cuts the wired remote's +12 V and LIN. With it, Home Assistant commands
# take over from the remote briefly and hand it back (see docs/aot30-master-controller.md).
CONFIG_SCHEMA = climate.climate_schema(Aot30Climate).extend(
    {
        cv.Optional(CONF_RX_PIN, default=16): cv.int_range(min=0, max=39),
        cv.Optional(CONF_TX_PIN, default=17): cv.int_range(min=0, max=39),
        cv.Optional(CONF_DEBUG, default=False): cv.boolean,
        cv.Optional(CONF_REMOTE_RELAY_PIN): cv.int_range(min=0, max=33),
        cv.Optional(CONF_REMOTE_RELAY_INVERTED, default=False): cv.boolean,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    cg.add(var.set_rx_pin(config[CONF_RX_PIN]))
    cg.add(var.set_tx_pin(config[CONF_TX_PIN]))
    cg.add(var.set_debug(config[CONF_DEBUG]))
    if CONF_REMOTE_RELAY_PIN in config:
        cg.add(var.set_remote_relay_pin(config[CONF_REMOTE_RELAY_PIN]))
        cg.add(var.set_remote_relay_inverted(config[CONF_REMOTE_RELAY_INVERTED]))
