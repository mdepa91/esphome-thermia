"""ESPHome component for the Danfoss/Thermia heat pump "EXT" port.

The pump is the I2C master; this component emulates the I2C slave at address 0x2E in software (ESP8266/ESP32) and
answers the pump's polling with "please send me register N" (reads) or, for the handful of registers exposed as
`number`/`select` entities below, "please WRITE this value to register N". There is no protocol-level write
confirmation - see queue_write() in thermia_slave.h - so every writable entity also polls its own register for
reading, and its displayed state is corrected by what the pump actually reports, not just what was last sent.

Register map: ThermIQ (https://github.com/ThermIQ/thermiq_mqtt-ha .../heatpump/thermiq_regs.py). The main_mode
option labels come from the same repo's mode0..mode4 translation strings - registers 5-16 of that field are not
documented anywhere we found, so they are deliberately not offered as a select option (use set_registers: for
those, at your own risk and with your own known-good min/max).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, number, select, sensor
from esphome.core import CORE
from esphome.const import (
    CONF_ID,
    CONF_INVERTED,
    CONF_NUMBER,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_STEP,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HOUR,
    UNIT_PERCENT,
)

CODEOWNERS = []
AUTO_LOAD = ["sensor", "binary_sensor", "number", "select"]

thermia_ns = cg.esphome_ns.namespace("thermia")
ThermiaComponent = thermia_ns.class_("ThermiaComponent", cg.PollingComponent)
ThermiaNumber = thermia_ns.class_("ThermiaNumber", number.Number)
ThermiaSelect = thermia_ns.class_("ThermiaSelect", select.Select)

CONF_SDA_PIN = "sda_pin"
CONF_SCL_PIN = "scl_pin"
CONF_STALE_TIMEOUT = "stale_timeout"
CONF_REQUEST_TRIES = "request_tries"
CONF_DEBUG_FRAMES = "debug_frames"
CONF_SNIFF = "sniff"
CONF_SELFTEST = "selftest"
CONF_LINK = "link"
CONF_REGISTERS = "registers"
CONF_SET_REGISTERS = "set_registers"
CONF_REGISTER = "register"
CONF_SCALE = "scale"
CONF_SIGNED = "signed"

# Plausibility window for temperatures; anything outside is treated as a corrupted frame.
_T_MIN, _T_MAX = -60.0, 200.0
_ANY = 1e30


def _temp(reg, tmin=_T_MIN, tmax=_T_MAX, diagnostic=False):
    return dict(
        reg=reg,
        min=tmin,
        max=tmax,
        schema=dict(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            **({"entity_category": ENTITY_CATEGORY_DIAGNOSTIC} if diagnostic else {}),
        ),
    )


def _num(reg, unit=None, icon=None, device_class=None, state_class=STATE_CLASS_MEASUREMENT,
         diagnostic=False, tmin=-_ANY, tmax=_ANY):
    schema = dict(accuracy_decimals=0)
    if unit:
        schema["unit_of_measurement"] = unit
    if icon:
        schema["icon"] = icon
    if device_class:
        schema["device_class"] = device_class
    if state_class:
        schema["state_class"] = state_class
    if diagnostic:
        schema["entity_category"] = ENTITY_CATEGORY_DIAGNOSTIC
    return dict(reg=reg, min=tmin, max=tmax, schema=schema)


# key -> register definition. All values are signed 16 bit, unscaled (whole degrees etc.).
SENSORS = {
    # --- measurements -------------------------------------------------------------------------
    "temp_outdoor": _temp(0x00),
    "temp_indoor": _temp(0x01),
    "temp_indoor_target": _temp(0x03),
    "temp_supply": _temp(0x05),  # heating supply line ("T1")
    "temp_return": _temp(0x06),  # heating return line ("T2")
    "temp_hotwater": _temp(0x07),  # hot water tank ("T3")
    "temp_brine_out": _temp(0x08),
    "temp_brine_in": _temp(0x09),
    "temp_cooling": _temp(0x0A),
    "temp_supply_shunt": _temp(0x0B),
    "temp_supply_target": _temp(0x0E),
    "temp_supply_shunt_target": _temp(0x0F),
    "temp_hot_gas": _temp(0x17),  # compressor discharge ("pressure pipe")
    "temp_hotwater_supply": _temp(0x18),
    "current": _num(0x0C, UNIT_AMPERE, device_class=DEVICE_CLASS_CURRENT),
    "supply_pump_speed": _num(0x1E, UNIT_PERCENT, icon="mdi:pump"),
    "brine_pump_speed": _num(0x1F, UNIT_PERCENT, icon="mdi:pump"),
    "integral": _num(0x19, "°C·min", icon="mdi:sigma"),
    "sw_version": _num(0x1D, icon="mdi:chip", state_class=None, diagnostic=True),
    # --- run time counters --------------------------------------------------------------------
    "runtime_compressor": _num(0x68, UNIT_HOUR, device_class=DEVICE_CLASS_DURATION,
                               state_class=STATE_CLASS_TOTAL_INCREASING, diagnostic=True),
    "runtime_aux_3kw": _num(0x6A, UNIT_HOUR, device_class=DEVICE_CLASS_DURATION,
                            state_class=STATE_CLASS_TOTAL_INCREASING, diagnostic=True),
    "runtime_hotwater": _num(0x6C, UNIT_HOUR, device_class=DEVICE_CLASS_DURATION,
                             state_class=STATE_CLASS_TOTAL_INCREASING, diagnostic=True),
    "runtime_aux_6kw": _num(0x72, UNIT_HOUR, device_class=DEVICE_CLASS_DURATION,
                            state_class=STATE_CLASS_TOTAL_INCREASING, diagnostic=True),
}


def _bit(reg, mask, device_class=DEVICE_CLASS_RUNNING, icon=None):
    return dict(reg=reg, mask=mask, schema=dict(device_class=device_class, **({"icon": icon} if icon else {})))


def _alarm(reg, mask):
    return _bit(reg, mask, DEVICE_CLASS_PROBLEM)


# key -> (register, bit mask)
BINARY_SENSORS = {
    "compressor": _bit(0x10, 0x0002, icon="mdi:engine"),
    "brine_pump": _bit(0x10, 0x0001, icon="mdi:pump"),
    "supply_pump": _bit(0x10, 0x0004, icon="mdi:pump"),
    "hotwater_production": _bit(0x10, 0x0008, icon="mdi:water-boiler"),
    "aux1_heating": _bit(0x10, 0x0080, icon="mdi:heating-coil"),
    "aux2_heating": _bit(0x10, 0x0010, icon="mdi:heating-coil"),
    "aux_3kw": _bit(0x0D, 0x0001, icon="mdi:heating-coil"),
    "aux_6kw": _bit(0x0D, 0x0002, icon="mdi:heating-coil"),
    "alarm": _alarm(0x11, 0x0040),
    "alarm_high_pressure": _alarm(0x13, 0x0001),
    "alarm_low_pressure": _alarm(0x13, 0x0002),
    "alarm_motor_breaker": _alarm(0x13, 0x0004),
    "alarm_brine_flow": _alarm(0x13, 0x0008),
    "alarm_brine_temperature": _alarm(0x13, 0x0010),
    "alarm_outdoor_sensor": _alarm(0x14, 0x0001),
    "alarm_supply_sensor": _alarm(0x14, 0x0002),
    "alarm_return_sensor": _alarm(0x14, 0x0004),
    "alarm_hotwater_sensor": _alarm(0x14, 0x0008),
    "alarm_indoor_sensor": _alarm(0x14, 0x0010),
    "alarm_phase_order": _alarm(0x14, 0x0020),
    "alarm_overheating": _alarm(0x14, 0x0040),
}


def _writable_temp(reg, tmin, tmax, icon=None):
    return dict(
        reg=reg,
        min=tmin,
        max=tmax,
        schema=dict(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            **({"icon": icon} if icon else {}),
        ),
    )


def _writable_num(reg, tmin, tmax, unit=None, icon=None, device_class=None, step=1.0):
    schema = {}
    if unit:
        schema["unit_of_measurement"] = unit
    if icon:
        schema["icon"] = icon
    if device_class:
        schema["device_class"] = device_class
    return dict(reg=reg, min=tmin, max=tmax, step=step, schema=schema)


# key -> writable register definition (exposed as a `number`). Confirmed against the pump's own display on
# 2026-09-22 (r32 tracked 28 -> 18 -> 21 while the target was changed on the pump itself).
NUMBERS = {
    "setting_indoor_temp": _writable_temp(0x32, 10.0, 30.0, icon="mdi:home-thermometer"),
    # Heating curve (integral1): slope + the three fixed offset points, min/max clamp. The slope range 22-56 is
    # the one the pump's own menu allows; the remaining 0-200 ranges are raw register limits from the ThermIQ map,
    # not confirmed on a real pump (unlike r32/r33) - change them carefully, in small steps, with the pump's
    # display at hand, and narrow min_value/max_value in your YAML.
    "setting_curve_slope": _writable_num(0x34, 22.0, 56.0, icon="mdi:chart-line"),
    "setting_curve_min": _writable_temp(0x35, 0.0, 200.0, icon="mdi:thermometer-chevron-down"),
    "setting_curve_max": _writable_temp(0x36, 0.0, 200.0, icon="mdi:thermometer-chevron-up"),
    "setting_curve_offset_p5": _writable_temp(0x37, -5.0, 5.0, icon="mdi:thermometer-plus"),
    "setting_curve_offset_0": _writable_temp(0x38, -5.0, 5.0, icon="mdi:thermometer"),
    "setting_curve_offset_n5": _writable_temp(0x39, -5.0, 5.0, icon="mdi:thermometer-minus"),
    # How much weight the pump gives to room temperature in the heating curve (0-4). Only has an effect when a
    # real room sensor is connected to the pump's "Room sensor" port (writing r01 over EXT is ignored by the pump).
    "setting_room_factor": _writable_num(0x3C, 0.0, 4.0, icon="mdi:home-percent"),
    "setting_heating_stop_temp": _writable_temp(0x3A, 0.0, 200.0, icon="mdi:thermometer-off"),
    "setting_hotwater_start_temp": _writable_temp(0x44, 0.0, 100.0, icon="mdi:water-boiler"),
    "setting_hotwater_stop_temp": _writable_temp(0x54, 0.0, 100.0, icon="mdi:water-boiler"),
    "setting_max_electric_steps": _writable_num(0x51, 0.0, 3.0, icon="mdi:stairs"),
    "setting_max_current": _writable_num(0x52, 0.0, 100.0, UNIT_AMPERE, device_class=DEVICE_CLASS_CURRENT),
    # r42 (outdoor_stop_t) deliberately left out: the ThermIQ label "Outdoor stop temp. (20=-20C)" suggests an
    # inverted sign encoding (positive register value = negative temperature), which is unconfirmed.
}

# key -> (register, option labels in value order - option i is written as raw value i). Labels are ThermIQ's
# mode0..mode4 (see module docstring); values 5-16 that the register nominally allows are not offered here.
SELECTS = {
    "setting_main_mode": dict(
        reg=0x33,
        options=["Off", "Auto", "Heatpump only", "Heater only", "Hot water only"],
        schema=dict(icon="mdi:cog"),
    ),
}

_REGISTER = cv.int_range(min=0, max=0x7F)

RAW_SENSOR_SCHEMA = sensor.sensor_schema(accuracy_decimals=0).extend(
    {
        cv.Required(CONF_REGISTER): _REGISTER,
        cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        cv.Optional(CONF_SIGNED, default=True): cv.boolean,
    }
)

# Generic escape hatch for any other writable register: unlike the named entries above, min/max are NOT
# defaulted - you must state the pump's actual valid range yourself. Get it wrong and the pump receives a value
# outside what it expects.
RAW_NUMBER_SCHEMA = number.number_schema(ThermiaNumber).extend(
    {
        cv.Required(CONF_REGISTER): _REGISTER,
        cv.Required(CONF_MIN_VALUE): cv.float_,
        cv.Required(CONF_MAX_VALUE): cv.float_,
        cv.Optional(CONF_STEP, default=1.0): cv.positive_float,
        cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        cv.Optional(CONF_SIGNED, default=True): cv.boolean,
    }
)

_schema = {
    cv.GenerateID(): cv.declare_id(ThermiaComponent),
    cv.Required(CONF_SDA_PIN): pins.internal_gpio_input_pullup_pin_schema,
    cv.Required(CONF_SCL_PIN): pins.internal_gpio_input_pullup_pin_schema,
    cv.Optional(CONF_STALE_TIMEOUT, default="120s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_REQUEST_TRIES, default=50): cv.int_range(min=1, max=255),
    cv.Optional(CONF_DEBUG_FRAMES, default=False): cv.boolean,
    cv.Optional(CONF_SNIFF, default=False): cv.boolean,
    cv.Optional(CONF_SELFTEST, default=False): cv.boolean,
    cv.Optional(CONF_LINK): binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    cv.Optional(CONF_REGISTERS): cv.ensure_list(RAW_SENSOR_SCHEMA),
    cv.Optional(CONF_SET_REGISTERS): cv.ensure_list(RAW_NUMBER_SCHEMA),
}
for _key, _spec in SENSORS.items():
    _schema[cv.Optional(_key)] = sensor.sensor_schema(**_spec["schema"])
for _key, _spec in BINARY_SENSORS.items():
    _schema[cv.Optional(_key)] = binary_sensor.binary_sensor_schema(**_spec["schema"])
for _key, _spec in NUMBERS.items():
    _schema[cv.Optional(_key)] = number.number_schema(ThermiaNumber, **_spec["schema"]).extend(
        {
            cv.Optional(CONF_MIN_VALUE, default=_spec["min"]): cv.float_,
            cv.Optional(CONF_MAX_VALUE, default=_spec["max"]): cv.float_,
            cv.Optional(CONF_STEP, default=_spec.get("step", 1.0)): cv.positive_float,
        }
    )
for _key, _spec in SELECTS.items():
    _schema[cv.Optional(_key)] = select.select_schema(ThermiaSelect, **_spec["schema"])


# The ISR samples both lines with one register read (see thermia_hw.h), so both must be in the register it reads:
# GPI covers GPIO0-15 on the ESP8266 (GPIO16 is on a separate RTC register), GPIO_IN covers GPIO0-31 on the ESP32.
_MAX_PIN = {"esp8266": 15, "esp32": 31}


def _validate_pins(config):
    max_pin = _MAX_PIN[CORE.target_platform]
    for key in (CONF_SDA_PIN, CONF_SCL_PIN):
        pin = config[key]
        if pin[CONF_NUMBER] > max_pin:
            raise cv.Invalid(
                f"GPIO{pin[CONF_NUMBER]} is not supported here, use GPIO0-{max_pin} on {CORE.target_platform}",
                path=[key],
            )
        if pin.get(CONF_INVERTED, False):
            raise cv.Invalid("inverted pins are not supported (the bus is read straight from the registers)",
                             path=[key])
    if config[CONF_SDA_PIN][CONF_NUMBER] == config[CONF_SCL_PIN][CONF_NUMBER]:
        raise cv.Invalid("sda_pin and scl_pin must be different pins")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(_schema).extend(cv.polling_component_schema("10s")),
    cv.only_on(["esp8266", "esp32"]),
    _validate_pins,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_sda_pin(await cg.gpio_pin_expression(config[CONF_SDA_PIN])))
    cg.add(var.set_scl_pin(await cg.gpio_pin_expression(config[CONF_SCL_PIN])))
    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT]))
    cg.add(var.set_request_tries(config[CONF_REQUEST_TRIES]))
    cg.add(var.set_debug_frames(config[CONF_DEBUG_FRAMES]))
    cg.add(var.set_sniff(config[CONF_SNIFF]))
    cg.add(var.set_selftest(config[CONF_SELFTEST]))

    if CONF_LINK in config:
        cg.add(var.set_link_sensor(await binary_sensor.new_binary_sensor(config[CONF_LINK])))

    for key, spec in SENSORS.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(var.add_sensor(spec["reg"], sens, 1.0, True, spec["min"], spec["max"]))

    for key, spec in BINARY_SENSORS.items():
        if key in config:
            bsens = await binary_sensor.new_binary_sensor(config[key])
            cg.add(var.add_binary_sensor(spec["reg"], spec["mask"], bsens))

    for raw in config.get(CONF_REGISTERS, []):
        sens = await sensor.new_sensor(raw)
        cg.add(var.add_sensor(raw[CONF_REGISTER], sens, raw[CONF_SCALE], raw[CONF_SIGNED], -_ANY, _ANY))

    for key, spec in NUMBERS.items():
        if key in config:
            conf = config[key]
            num = await number.new_number(
                conf, min_value=conf[CONF_MIN_VALUE], max_value=conf[CONF_MAX_VALUE], step=conf[CONF_STEP]
            )
            cg.add(num.set_parent(var))
            cg.add(num.set_register(spec["reg"]))
            cg.add(num.set_scale(1.0))
            cg.add(num.set_signed(True))
            cg.add(var.add_number(spec["reg"], num, 1.0, True, conf[CONF_MIN_VALUE], conf[CONF_MAX_VALUE]))

    for key, spec in SELECTS.items():
        if key in config:
            sel = await select.new_select(config[key], options=spec["options"])
            cg.add(sel.set_parent(var))
            cg.add(sel.set_register(spec["reg"]))
            cg.add(var.add_select(spec["reg"], sel))

    for raw in config.get(CONF_SET_REGISTERS, []):
        num = await number.new_number(
            raw, min_value=raw[CONF_MIN_VALUE], max_value=raw[CONF_MAX_VALUE], step=raw[CONF_STEP]
        )
        cg.add(num.set_parent(var))
        cg.add(num.set_register(raw[CONF_REGISTER]))
        cg.add(num.set_scale(raw[CONF_SCALE]))
        cg.add(num.set_signed(raw[CONF_SIGNED]))
        cg.add(
            var.add_number(raw[CONF_REGISTER], num, raw[CONF_SCALE], raw[CONF_SIGNED], raw[CONF_MIN_VALUE],
                            raw[CONF_MAX_VALUE])
        )
