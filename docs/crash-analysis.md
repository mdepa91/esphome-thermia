# Sporadic exceptions on single-core chips – analysis notes

Findings from investigating occasional exceptions/resets on the ESP8266 (the same reasoning applies to the
single-core ESP32 variants: C3, C6, S2). Root cause **not yet confirmed** – a crash log is still needed, see
[What to collect next](#what-to-collect-next).

## Ruled out: ISR code in flash

The classic cause of *sporadic* ESP8266 crashes is an interrupt handler that calls or reads something located in
flash. It only fails when the interrupt fires while the flash cache is disabled (preferences write, OTA), hence
"sporadic".

Checked by disassembling the firmware (`objdump` of `firmware.elf`, resolving every `call0` target and every
`l32r` literal on the ISR path):

| Build | ISR path in IRAM | References to flash from the ISR path |
|---|---|---|
| `db6c97e` (original, header-only engine) | yes | 0 |
| current (ISR code in `thermia_slave.cpp`) | yes | 0 |

The path checked: Arduino core `interrupt_handler` → `ThermiaComponent::isr_` → every `ThermiaSlave` function
reachable from `on_sda_fall()`. Only `probe_sda()` lives in flash, which is intended (main context, before the ISR
is attached).

## Found and fixed: unbounded ISR duration

The per-transaction budget (`max_transaction_`, 8 ms) was only checked at the start of a transaction and in the
write-receive loop. `skip_to_end()` (up to 512 bits) and the read-response loop were bounded only by the per-edge
timeout (1.5 ms). A noisy bus – e.g. interference when the compressor starts – with SCL edges arriving slower than
~50 µs but faster than 1.5 ms and no STOP could keep one ISR call running far past 8 ms.

With interrupts masked for that long on a single core, WiFi's timer interrupts are starved – a plausible source of
SDK exceptions and watchdog resets.

Reproduced by the host test `test_isr_budget` (foreign address followed by 600 slow clock bits, no STOP):

| Engine | Longest single ISR call |
|---|---|
| original | **312 ms** |
| fixed | < 10 ms (budget + at most one edge timeout) |

Fix: `ThermiaSlave::expired()` is checked in every loop that can follow the bus for more than a few bits. A frame
cut off by the budget counts as a bus error (`err` in the status line).

## Memory

- The 1 KB sniffer capture buffer is now allocated only with `sniff: true`. Static RAM on the ESP8266 went from
  44.6 % to 43.4 % (~1 KB more heap).
- Still possible if memory turns out to be the problem: keep the per-register tables (`slot_`, `rx_ms_`,
  `seen_seq_` – 1.25 KB for all 128 registers) only for polled registers, saving ~0.7 KB.
- The bigger consumers on the ESP8266 are ESPHome itself: 40+ entities plus the encrypted API (noise handshake
  allocations on every Home Assistant reconnect). Heap fragmentation there is a common cause of sporadic crashes.

## Headroom for further single-core optimization

Small. The busy-polling bit-bang has to be present for every bit it sends or receives: roughly 90 bits per
~110 ms polling cycle (ping, read, data frame, the pump's polls of foreign addresses 0x32/0x37), about 4–5 ms of
CPU at ~20 kHz (estimate, not measured). An interrupt-per-edge design would use less CPU, but WiFi interrupt
jitter on the ESP8266 would then corrupt bits – this is why polling was chosen in the first place.

## What to collect next

1. **Crash log over USB** – ESPHome decodes ESP8266 stack traces automatically:

   ```sh
   esphome logs thermia.yaml --device /dev/ttyUSB0
   ```

   The exception number (`Exception (N)`) and the backtrace point to the culprit, e.g. 0 = illegal instruction,
   3 = load/store error, 28/29 = null pointer, `Soft WDT reset` = the loop did not yield.

2. **Heap and reset reason** from the `debug` component:

   ```yaml
   debug:
     update_interval: 30s
   sensor:
     - platform: debug
       free: {name: "Heap free"}
       block: {name: "Heap max block"}
       fragmentation: {name: "Heap fragmentation"}
       loop_time: {name: "Loop time"}
   text_sensor:
     - platform: debug
       reset_reason: {name: "Reset reason"}
   ```

   If "Heap max block" drops to around 4–6 KB before the crashes, memory is the cause – first step: remove the
   entities you don't use from the YAML.

3. **The `err` counter** in the `thermia` status line (`debug_frames` not required). If it jumps around the time of
   a crash – especially while the compressor starts – bus noise and long ISR calls are the likely cause, which the
   ISR budget fix above addresses.
