# ESPHome component for Thermia / Danfoss heat pumps (EXT port)

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-ffdd00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/mdepa91)

Read **and write** Thermia / Danfoss ground-source heat pump parameters from Home Assistant with a plain
**ESP8266** (Wemos D1 mini) or **ESP32** (any variant) wired to the pump's **EXT** connector. No ThermIQ box, no Arduino bridge.

- 40+ ready-made entities: temperatures, compressor / pump / heater status, alarms, run-time counters.
- Writable settings as `number` / `select` entities: room temperature setpoint, operating mode, heating curve,
  hot water thresholds, …
- Generic `registers:` (read) and `set_registers:` (write) for anything else in the register map.
- Software I2C **slave** on ESP8266 and ESP32, running entirely in IRAM, with host-side unit tests (203 checks).

> **Tested on:** Thermia Duo 400V 10 kW, ESPHome 2026.9.0, Wemos D1 mini.
> Other Thermia / Danfoss models using the same EXT protocol (the one ThermIQ talks to) will most likely work,
> but register meanings may differ – compare with your pump's display before trusting a value.

> ⚠️ **Writing to your heat pump is at your own risk.** Only change settings you understand, in small steps,
> with the pump's display at hand. See [Writing settings](#writing-settings).

## Hardware

```
Thermia EXT (5 V)      bi-directional level shifter      Wemos D1 mini (3.3 V)
  SDA  ────────────────  HV ─── LV  ─────────────────────  D2 (GPIO4)   = sda_pin
  SCL  ────────────────  HV ─── LV  ─────────────────────  D1 (GPIO5)   = scl_pin
  GND  ────────────────  GND ── GND ─────────────────────  GND
```

- EXT is a 4-pin connector: VCC 5 V, SCL, SDA, GND. Verify the pin order on your own unit.
- The bus is **5 V**; use a bi-directional level shifter (BSS138 type). The pump has its own pull-ups.
- Power the ESP from its own USB supply; connect only GND + SDA + SCL to EXT. Wire it with the pump switched off.
- ESP8266: D1/D2 are recommended: they are not boot-strapping pins. GPIO0–15 are accepted.
- ESP32: GPIO0–31 are accepted; avoid boot-strapping pins (GPIO0/2/5/12/15 on the classic ESP32) and the
  flash pins. GPIO21/22 work well on an ESP32 DevKit. The ESP32 is 3.3 V too – keep the level shifter.
  Minimal config: [`example-esp32.yaml`](example-esp32.yaml).

## Installation

```yaml
external_components:
  - source: github://mdepa91/esphome-thermia
    components: [thermia]

thermia:
  sda_pin: D2
  scl_pin: D1
  update_interval: 10s
  link:
    name: "EXT link"
  temp_outdoor:
    name: "Outdoor temperature"
  temp_supply:
    name: "Supply (T1)"
  compressor:
    name: "Compressor"
  alarm:
    name: "Alarm"
  setting_indoor_temp:
    name: "Room temperature setpoint"
  setting_main_mode:
    name: "Operating mode"
```

A complete configuration with every entity is in [`example.yaml`](example.yaml)
(secrets template: [`secrets.example.yaml`](secrets.example.yaml)).

Do **not** add an `i2c:` block – the ESPHome I2C component can only be a master, and here the pump is the master.

## Configuration variables

| Option | Default | Description |
|---|---|---|
| `sda_pin`, `scl_pin` | **required** | GPIO pins wired to EXT via the level shifter: GPIO0–15 on ESP8266, GPIO0–31 on ESP32. |
| `update_interval` | `10s` | How often entity states are published. |
| `stale_timeout` | `120s` | A value older than this is published as unavailable. Some registers are answered rarely – `1h` works well. |
| `debug_frames` | `false` | Log every bus frame. Useful for first start-up only; it costs CPU and UART time. |
| `sniff` | `false` | Hardware diagnostics: record raw SDA/SCL waveforms. **Disables the protocol.** See [Diagnostics](#diagnostics). |
| `selftest` | `false` | Hardware diagnostics: probe the SDA line at boot and log level changes. |
| `link` | – | Binary sensor (connectivity): on while the pump is talking to us. |
| `registers` | – | List of extra read-only sensors: `register` (0x00–0x7F), `scale` (default 1), `signed` (default true) + any sensor option. |
| `set_registers` | – | List of extra writable numbers: `register`, **`min_value`, `max_value` (required)**, `step`, `scale`, `signed` + any number option. |

### Sensors

All values are whole numbers as reported by the pump (temperatures in °C, signed 16-bit).

| Key | Register | Unit |
|---|---|---|
| `temp_outdoor` | r00 | °C |
| `temp_indoor` | r01 | °C (virtual indoor temperature) |
| `temp_indoor_target` | r03 | °C |
| `temp_supply` | r05 | °C (supply line, T1) |
| `temp_return` | r06 | °C (return line, T2) |
| `temp_hotwater` | r07 | °C (hot water tank, T3) |
| `temp_brine_out` / `temp_brine_in` | r08 / r09 | °C |
| `temp_cooling` | r0A | °C |
| `temp_supply_shunt` | r0B | °C |
| `temp_supply_target` | r0E | °C |
| `temp_supply_shunt_target` | r0F | °C |
| `temp_hot_gas` | r17 | °C (compressor discharge) |
| `temp_hotwater_supply` | r18 | °C |
| `current` | r0C | A |
| `integral` | r19 | °C·min (degree-minutes) |
| `supply_pump_speed` / `brine_pump_speed` | r1E / r1F | % |
| `sw_version` | r1D | diagnostic |
| `runtime_compressor` | r68 | h |
| `runtime_aux_3kw` | r6A | h |
| `runtime_hotwater` | r6C | h |
| `runtime_aux_6kw` | r72 | h |

### Binary sensors

| Key | Register & bit |
|---|---|
| `brine_pump`, `compressor`, `supply_pump`, `hotwater_production`, `aux2_heating`, `aux1_heating` | r10: `0x01`, `0x02`, `0x04`, `0x08`, `0x10`, `0x80` |
| `aux_3kw`, `aux_6kw` | r0D: `0x01`, `0x02` |
| `alarm` | r11: `0x40` |
| `alarm_high_pressure`, `alarm_low_pressure`, `alarm_motor_breaker`, `alarm_brine_flow`, `alarm_brine_temperature` | r13: `0x01`…`0x10` |
| `alarm_outdoor_sensor`, `alarm_supply_sensor`, `alarm_return_sensor`, `alarm_hotwater_sensor`, `alarm_indoor_sensor`, `alarm_phase_order`, `alarm_overheating` | r14: `0x01`…`0x40` |

### Writable settings

Each of these accepts the usual `number` options; `min_value` / `max_value` can be narrowed in YAML.

| Key | Register | Type | Default range | Verified on a real pump |
|---|---|---|---|---|
| `setting_indoor_temp` | r32 | number, °C | 10–30 | ✅ tracked changes made on the pump's own display |
| `setting_main_mode` | r33 | select | Off / Auto / Heatpump only / Heater only / Hot water only | ✅ |
| `setting_curve_slope` | r34 | number | 22–56 | range from the pump menu |
| `setting_curve_min` / `setting_curve_max` | r35 / r36 | number, °C | 0–200 | ❌ |
| `setting_curve_offset_p5` / `_0` / `_n5` | r37 / r38 / r39 | number, °C | −5–5 | ❌ |
| `setting_room_factor` | r3C | number | 0–4 (room temperature influence, needs a physical room sensor) | ❌ |
| `setting_heating_stop_temp` | r3A | number, °C | 0–200 | ❌ |
| `setting_hotwater_start_temp` / `setting_hotwater_stop_temp` | r44 / r54 | number, °C | 0–100 | ❌ |
| `setting_max_electric_steps` | r51 | number | 0–3 | ❌ |
| `setting_max_current` | r52 | number, A | 0–100 | ❌ |

❌ = register meaning taken from the ThermIQ map, not yet confirmed on real hardware. Wide default ranges are raw
register limits – **set sensible `min_value` / `max_value` for your installation** (see `example.yaml`).

## Writing settings

When the pump reads a byte from us, instead of a register number we may answer `[register | 0x80, lo, hi]` –
"write this value to that register". There is **no protocol-level acknowledgement**: we only know the pump
received the three bytes. That is why every writable entity also **polls its own register**: the state shown in
Home Assistant is what the pump actually reports, so if the pump ignores or clamps a value, the entity corrects
itself on the next read. Until the pump reports the register again after a write, the entity keeps the value you
set rather than falling back to the reading from before the write. The pump may answer some registers (e.g. r32)
only every few minutes. Such a register is marked with `~` in the `regs:` debug dump.

Not everything is writable: writing r01 (indoor temperature) to emulate a room sensor is **ignored** by the pump.
The physical "Room sensor" port is not a plain resistive input either (it carries ~27 V DC and a digital protocol).

Factory reset (r59) and counter reset (r5A) are deliberately not exposed as named entities.

## How it works

The EXT port is an I2C bus where **the heat pump is the master** (~20 kHz clock) and the accessory is a slave
at 7-bit address **0x2E**. The ESP8266 has no hardware I2C slave, so this component implements one in software
(the ESP32 runs the same engine, see [ESP8266 vs ESP32](#esp8266-vs-esp32)):

1. A GPIO interrupt on the falling edge of SDA (START) enters `on_sda_fall()`, which then polls both lines with a
   cycle counter until STOP. Everything on that path lives in IRAM.
2. The pump periodically writes a **ping** (`0xFE`, `0xFD` after its own boot).
3. The pump then **reads one byte** from us: `0xFF` = "nothing", `0x00`–`0x7F` = "send me register N",
   `[reg|0x80, lo, hi]` = "write this value".
4. If we asked for a register, the pump writes back a **data frame** `[reg, lo, hi]` (little-endian int16).

Example of reading r06 (return temperature, 24 °C):

```
pump → us   S 5C FE P        ping
pump ← us   S 5D [06] P      we ask for r06
pump → us   S 5C 06 18 00 P  r06 = 0x0018 = 24 °C
```

Only registers used in your YAML are polled, one per ping, round-robin; a pending write has priority over reads.
Safety nets: per-edge (1.5 ms) and per-transaction (8 ms) timeouts, a circuit breaker after 50 consecutive failed
transactions, a plausibility window for temperatures (−60…200 °C), and a **bus gate** – the interrupt is attached
only after WiFi has been connected for 5 s and detached immediately when it drops, so bus traffic never starves
the WiFi association on the single ESP8266 core.

### ESP8266 vs ESP32

The protocol engine (`thermia_slave.h`) is shared; only `thermia_hw.h` differs, each backend using the fastest
access its chip offers:

| | ESP8266 | ESP32 (all variants) |
|---|---|---|
| Sampling both lines | one read of `GPI` (GPIO0–15) | one read of `GPIO_IN` (GPIO0–31) |
| Driving SDA | output-enable toggle with latch at 0 (`GPES`/`GPEC`) | hardware open-drain pad, latch `W1TC`/`W1TS` |
| Time base | `ccount` register | `esp_cpu_get_cycle_count()` (Xtensa and RISC-V) |
| Where the ISR busy-polls | the only core, shared with WiFi | core 1 (ESPHome loop), WiFi on core 0 – single-core C3/C6/S2 behave like the ESP8266 |

The ESP32's hardware I2C slave is deliberately not used: the byte we return on the pump's read depends on the ping
received just before it and would have to be in the peripheral's TX FIFO already; the sniffer and self-test also
need raw pin access. The bus gate is kept on ESP32 as well – harmless on dual-core, needed on single-core variants.

| File | Role |
|---|---|
| `thermia_slave.h`, `thermia_slave.cpp` | I2C slave engine (ISR bit-bang), protocol, sniffer, SDA probe – no ESPHome dependencies besides `IRAM_ATTR` |
| `thermia_hw.h` | Per-platform GPIO/timer access (ESP8266, ESP32); replaced by a simulator in host tests |
| `thermia_bus_gate.h` | Decides when the bus may be touched (network-stability gate) |
| `thermia.h`, `thermia.cpp` | ESPHome component: entities, publishing, logging, diagnostics |
| `thermia_number.h`, `thermia_select.h` | Writable `number` / `select` entities |
| `__init__.py` | YAML schema, driven by the register tables |

## Diagnostics

With `debug_frames: true` a healthy start-up looks like:

```
[thermia] pump -> us : FE            <- ping (our ACK works)
[thermia] us -> pump : 00            <- we ask for r00
[thermia] pump -> us : 00 03 00      <- r00 = 3
[thermia] link=up pings=57 data=57 reads=57 ok=114 err=0 foreign=.. max_isr=480us
[thermia] regs: r00=0003 r05=0018 r06=0016 ...
```

| Symptom | Meaning |
|---|---|
| `pings=0`, `isr=0` | Lines not connected (wiring, level shifter, ground) |
| `isr` grows, `pings=0`, `err`/`foreign` grow | SDA and SCL swapped, or SDA not reaching the ESP |
| `pings` grows, `data=0` | Pump pings but doesn't send registers – check the frame log |
| `err` grows together with `pings` | Interrupt too slow for the pump's clock |
| `unanswered` grows | The pump doesn't answer some register (normal for some, e.g. r10 while the compressor is idle) |
| `foreign` grows | Expected: the pump also polls addresses 0x32 and 0x37 |

**Hardware debugging** – `sniff: true` records both lines once a second (and disables the protocol). Decode with:

```sh
esphome logs thermia.yaml > logs.txt
python3 tools/decode_sniff.py logs.txt
```

`selftest: true` briefly pulls SDA low three times at boot, logs whether the line follows, then logs every SDA level
change. It never touches SCL.

Sporadic exceptions/resets: see [docs/crash-analysis.md](docs/crash-analysis.md) for what was ruled out, what was
fixed and what to collect.

## Tests

The slave engine is tested on the host against a simulated I2C master (ping → read → data frame, repeated START,
foreign addresses, stuck bus, never-ending frames, late interrupts, writes, bus gate, sniffer):

```sh
g++ -std=c++17 -DTHERMIA_HOST_TEST -I test -I components/thermia test/test_slave.cpp components/thermia/thermia_slave.cpp -o test/test_slave && test/test_slave
```

CI runs these tests and compiles `example.yaml` (ESP8266) and `example-esp32.yaml` (ESP32, ESP32-C3) with ESPHome
on every push.

## Limitations

- ESP8266 and ESP32 only. On ESP32 both pins must be GPIO0–31, and `inverted:` pins are not supported.
- ESP32 support is compile-tested; the bus timing was verified on real hardware with the ESP8266 only.
- Temperatures have 1 °C resolution – that's what the pump reports.
- Register meanings come from the ThermIQ map; not all are confirmed on every model.

## Credits

- [rainisto/arduino_i2c_orja](https://github.com/rainisto/arduino_i2c_orja) – Arduino slave for the same port; the
  protocol logic follows it.
- [Danfoss-lämpöpumpun salaisuudet](https://omakotikotitalomme.blogspot.com/2015/03/danfoss-lampopumpun-salaisuudet.html)
  – the original reverse-engineering blog post (pinout, address, ping, frame format).
- [ThermIQ/thermiq_mqtt-ha](https://github.com/ThermIQ/thermiq_mqtt-ha) – register map and mode labels.
- Repository layout inspired by [thegroove/esphome-custom-component-examples](https://github.com/thegroove/esphome-custom-component-examples).

Thermia and Danfoss are trademarks of their respective owners; this project is not affiliated with them.

## License

[MIT](LICENSE)
