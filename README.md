# BLE → RS-232 Serial Mouse Bridge

> ESP32 firmware that acts as a **Microsoft Serial Mouse emulator** — connects to any BLE HID mouse and forwards movement, buttons and scroll wheel to a PC via the RS-232 serial port.

Ideal for retro PCs, Socket 7 / 486 / 386 machines or any device with a DB9 serial COM port that you want to use with a modern wireless mouse.

---

## Block Diagram

![Block diagram](doc/block_diagram.svg)

---

## Features

- **BLE HID host** — connects to any Bluetooth LE mouse (NimBLE-Arduino 2.x)
- **RS-232 serial mouse emulator** — bit-bang TX with precise timing via `esp_timer_get_time()`
- **High-DPI scaling** — configurable divisor for modern mice (400–3200 DPI)
- **Scroll wheel** — forwarded to PC via IntelliMouse `MZ` protocol (requires ctmouse ≥ 3.4)
- **Sub-pixel accumulator** — motion remainder preserved between packets for smooth movement
- **RTS / DTR identification** — responds to either signal for maximum driver compatibility
- **Scan-before-connect** — waits for mouse to start advertising before connecting
- **Persistent pairing** — `connect` retries up to 20 times automatically; keep mouse in pairing mode until connected
- **Auto-reconnect** — BLE daemon task detects disconnection and reconnects automatically
- **Battery level** — reads mouse battery percentage if supported by device
- **NVS storage** — paired mouse and all settings remembered across reboots
- **Serial console** — full command interface at 115200 baud
- **WiFi disabled** — WiFi stack is deinitialised at startup, saving ~20 mA

---

## Hardware

### Components

| Component | Description |
|-----------|-------------|
| ESP32-WROOM-32 | Main microcontroller |
| BSS138 bidirectional level shifter | 3.3 V ↔ 5 V logic translation |
| MAX232CPE | TTL ↔ RS-232 level converter |
| 4× 1 µF electrolytic capacitor | Charge pump capacitors for MAX232 |
| DB9 female connector | Serial port — connects to PC COM port |

### Connectors

#### DB9 Female (RS-232 COM port)

<img src="doc/db9_female.png" width="400" alt="DB9 male and female connectors"/>

| Pin | Signal | Direction | Description |
|-----|--------|-----------|-------------|
| 1 | DCD | — | Not connected |
| 2 | RD | ← ESP32 | Serial data from bridge to PC (mouse data) |
| 3 | TD | — | Not connected |
| 4 | DTR | → ESP32 | PC asserts to reset/identify mouse (GPIO5) |
| 5 | GND | — | Signal ground |
| 6 | DSR | — | Not connected |
| 7 | RTS | → ESP32 | PC asserts to reset/identify mouse (GPIO23) |
| 8 | CTS | — | Not connected |
| 9 | RI | — | Not connected |

> ⚠️ **Note:** Only 3 signal wires are required: **pin 2** (RD), **pin 5** (GND), and at least one of **pin 4** (DTR) or **pin 7** (RTS). Most DOS mouse drivers use DTR, RTS or both for identification.

> ⚠️ **MAX232 polarity:** The MAX232 receiver inverts RS-232 levels to TTL. RS-232 asserted (+12V) becomes GPIO **LOW**. The firmware detects the **FALLING** edge as the identification trigger.

### Level Shifter

<img src="doc/level_shifter.png" width="220" alt="BSS138 IIC I2C 5V to 3.3V bidirectional level shifter"/>

The BSS138-based bidirectional level shifter translates between ESP32's 3.3 V GPIO and the 5 V TTL side of the MAX232. It has built-in pull-up resistors on both sides — **no external resistors needed**. The LV GND and HV GND are connected internally on the module.

The module has two sides:
- **LV side** — connect to ESP32 (3.3 V logic)
- **HV side** — connect to MAX232 TTL pins (5 V logic)

### MAX232CPE

The MAX232CPE converts between 5 V TTL (from the level shifter) and RS-232 voltage levels (±12 V) required by the serial port.

It requires **4 external 1 µF electrolytic capacitors** connected between its charge pump pins (C1+/C1−, C2+/C2−, VS+, VS−). Refer to the MAX232 datasheet for exact placement.

---

### Wiring Diagram

![Wiring diagram](doc/wiring_diagram.svg)

#### Step-by-step connections

**ESP32 → Level Shifter (LV side)**

| ESP32 pin | Level Shifter LV | Signal |
|-----------|-----------------|--------|
| 3V3 | LV | 3.3 V reference |
| GND | GND | Common ground |
| GPIO16 | A1 | TX (data out to MAX232) |
| GPIO23 | A2 | RTS input (from MAX232) |
| GPIO5 | A3 | DTR input (from MAX232) |

**Level Shifter (HV side) → MAX232**

| Level Shifter HV | MAX232 pin | Signal |
|-----------------|-----------|--------|
| HV | VCC | 5 V power |
| GND | GND | Common ground |
| B1 | T1IN | TX data → RS-232 transmitter |
| B2 | R1OUT | RTS TTL output from RS-232 receiver |
| B3 | R2OUT | DTR TTL output from RS-232 receiver |

**MAX232 → DB9 Female**

| MAX232 pin | DB9 pin | Signal |
|-----------|---------|--------|
| T1OUT | pin 2 (RD) | Serial data to PC |
| R1IN | pin 7 (RTS) | RTS line from PC |
| R2IN | pin 4 (DTR) | DTR line from PC |
| GND | pin 5 (GND) | Signal ground |

**Power**

| Source | Destination | Note |
|--------|------------|------|
| External 5 V | ESP32 VIN | Powers the ESP32 and the HV side |
| ESP32 VIN | Level Shifter HV | 5 V rail |
| ESP32 VIN | MAX232 VCC | 5 V rail |
| ESP32 3V3 | Level Shifter LV | 3.3 V reference |

> 💡 **Why a level shifter?** The MAX232 TTL side operates at 5 V. ESP32 GPIO is 3.3 V tolerant max. Connecting directly risks damaging the ESP32. The BSS138 level shifter safely translates between the two voltage levels bidirectionally.

> 💡 **Bit-bang timing:** The firmware uses `taskENTER_CRITICAL` + `esp_timer_get_time()` for precise 833 µs bit timing, blocking FreeRTOS/BLE interrupts for the duration of each byte to prevent framing errors on the PC UART.

---

## Software

### Requirements

| Tool | Version |
|------|---------|
| Arduino IDE | 2.x |
| ESP32 Arduino core | ≥ 3.x |
| NimBLE-Arduino | ≥ 1.4.2 |

### Installation

1. Install **ESP32 Arduino core** via Boards Manager
2. Install **NimBLE-Arduino** via Library Manager
3. Open `ble_serial_mouse_bridge.ino` in Arduino IDE
4. Select board: **ESP32 Dev Module**
5. Upload

### Configuration

Pin assignments at the top of the sketch:

```cpp
#define TX_PIN   16   // bit-bang TX → MAX232 T1IN → DB9 pin 2
#define RTS_PIN  23   // input: DB9 pin 7 → MAX232 R1OUT → GPIO23
#define DTR_PIN   5   // input: DB9 pin 4 → MAX232 R2OUT → GPIO5
```

Set `DTR_PIN` to `-1` if DTR is not wired.

---

## Usage

Connect via **Serial monitor at 115200 baud**.

### First-time pairing

```
scan                          # scan BLE HID devices for 10 seconds
  #1   db:81:f4:bb:6b:5d  Mouse   -71 dBm  MX Master 3

connect db:81:f4:bb:6b:5d    # connect and save
```

`connect` retries automatically (up to 20 attempts with 500 ms between each). **Keep the mouse in pairing mode** until you see `[NVS] Saved`. Press any key in the Serial Monitor to abort.

### Commands

| Command | Description |
|---------|-------------|
| `scan` | Scan BLE HID devices for 10 s, show MAC / type / RSSI / name |
| `connect <mac>` | Connect to MAC address, retry until success (max 20 attempts), save to NVS |
| `forget` | Erase saved mouse and ALL settings, reset to defaults |
| `proto <M\|M3\|MZ>` | Set serial mouse protocol (saved to NVS) |
| `scale <1-64>` | Set movement divisor for DPI scaling (saved to NVS) |
| `flipy` | Toggle Y-axis inversion (saved to NVS) |
| `flipw` | Toggle scroll wheel inversion (saved to NVS) |
| `reportid <0-255>` | Set BLE HID Report ID filter (saved to NVS) |
| `testm` | Manually send identification sequence |
| `status` | Show connection state and all settings |
| `help` | Show command reference |

### Protocol selection

| Protocol | Ident | Bytes | Frame | Features | Recommended for |
|----------|-------|-------|-------|----------|-----------------|
| `M` | `'M'` | 3 | 7N2 | Left + Right button | Maximum compatibility |
| `M3` | `'M3'` | 4 | 7N2 | + Middle button | Logitech-aware drivers |
| `MZ` | `'MZ'` | 4 | 7N2 | + Scroll wheel + Middle | **ctmouse ≥ 3.4** (default) |

After changing the protocol, reload the mouse driver on the PC. For `ctmouse`:
```
CTMOUSE /U    (unload)
CTMOUSE       (reload)
```

### Scale setting

| Mouse DPI | Recommended scale |
|-----------|-------------------|
| 400 DPI | 1–2 |
| 800 DPI | 2–3 |
| 1600 DPI | 4–6 |
| 3200 DPI | 8–12 |

### NVS storage

All settings survive a power cycle. The following values are stored:

| Key | Default | Command |
|-----|---------|---------|
| `mac` | — | `connect` |
| `type` | 1 | `connect` |
| `proto` | MZ (2) | `proto` |
| `scale` | 4 | `scale` |
| `flipy` | false | `flipy` |
| `flipw` | false | `flipw` |
| `reportid` | 0 | `reportid` |

`forget` clears all keys and resets every value to its default.

### Boot status output

On every boot the firmware prints the current NVS configuration before the help text:

```
[NVS] Saved mouse: db:81:f4:bb:6b:5d
[NVS] proto=MZ  scale=1/4  flipy=off  flipw=off  reportid=0
```

### Status output

```
── Status ─────────────────────────────
  BLE:      CONNECTED
  MAC:      db:81:f4:bb:6b:5e
  Battery:  100%
  Proto:    MZ (wheel)
  Scale:    1/4
  FlipY:    no
  FlipW:    no
  ReportID: 0 (auto)
───────────────────────────────────────
```

### Automatic reconnect

When the mouse disconnects, the firmware starts a BLE scan and waits for the mouse to begin advertising, then connects immediately. A BLE daemon FreeRTOS task monitors the connection independently of the main loop.

---

## Serial Mouse Protocol Implementation

### Identification sequence

When the PC driver initialises the serial port it asserts **DTR** and/or **RTS** (RS-232 +12V → GPIO LOW after MAX232 inversion). The firmware detects the **falling edge** on GPIO and after a 14 ms debounce responds with the identification byte(s):

| Protocol | Response | Frame |
|----------|----------|-------|
| M | `'M'` (0x4D) | 1200 baud 7N2 |
| M3 | `'M'` + `'3'` | 1200 baud 7N2 |
| MZ | `'M'` + `'Z'` | 1200 baud 7N2 |

A **200 ms blackout** after each identification suppresses repeated edge triggers from the driver's RTS/DTR toggle sequence and prevents packet desynchronisation.

### Packet formats

**Microsoft (M) — 3 bytes, 7N2:**

```
Byte 0:  1  LB  RB  Y7  Y6  X7  X6    (bit 6 = sync marker)
Byte 1:  0  X5  X4  X3  X2  X1  X0
Byte 2:  0  Y5  Y4  Y3  Y2  Y1  Y0
```

**Logitech M3 — 4 bytes, 7N2:**

```
Bytes 0–2: same as Microsoft
Byte 3:  0  0  MB  0  0  0  0  0      (bit 5 = middle button)
```

**IntelliMouse MZ — 4 bytes, 7N2:**

```
Bytes 0–2: same as Microsoft
Byte 3:  0  0  MB  0  W3  W2  W1  W0  (bit 5 = MB, bits 3:0 = wheel ±8)
```

### Bit-bang timing

The firmware does not use the hardware UART for RS-232 output. Instead it bit-bangs GPIO16 with precise timing:

```
1200 baud → 1 bit = 833 µs
Frame: START(0) | D0 D1 D2 D3 D4 D5 D6 | STOP(1) STOP(1)
```

`taskENTER_CRITICAL` blocks all FreeRTOS and BLE interrupts during each byte to prevent timing corruption. `esp_timer_get_time()` provides hardware-accurate µs timestamps independent of the scheduler.

### BLE HID report parsing

The firmware auto-detects the BLE mouse report format by payload length:

| Length | Format | Notes |
|--------|--------|-------|
| 3 B | `[btn][dx8][dy8]` | Standard 3-byte |
| 4 B | `[btn][dx8][dy8][wheel]` | Standard 4-byte |
| 5 B | `[btn][dx8][dy8][wheel][hwheel]` | Reference HID format |
| 7 B | `[btn][extra][X_lo][XY_mid][Y_hi][wheel][hwheel]` | **Logitech 12-bit packed** (MX Master 2/3, G502, M650) |

Logitech 12-bit X/Y extraction:
```
X = d[2] | ((d[3] & 0x0F) << 8)   → sign-extend to int16
Y = (d[3] >> 4) | (d[4] << 4)     → sign-extend to int16
```

The Report Reference descriptor (UUID 0x2908) is read for each HID characteristic at connect time. Only `Input` type reports (type byte = 1) are subscribed. `ReportID = 0` (boot protocol) is skipped.

### FreeRTOS architecture

```
Core 0                          Core 1 (Arduino loop)
────────────────────────        ───────────────────────────────
BLE stack (NimBLE)              loop()
  └─ notifyCallback()             ├─ handleSerial()
       └─ portENTER_CRITICAL      ├─ RTS/DTR ident handler (14 ms debounce)
            g_accX += dx          ├─ BLE reconnect logic
            g_accY += dy          ├─ keepalive battery read
            g_accW += wheel       └─ processMouseMovement()
                                       └─ sendSerialPacket()
bleDaemonTask (priority 1)                  └─ bbSendByte()
  check every 3 s                                └─ taskENTER_CRITICAL
  → scan + reconnect                              → bit-bang GPIO16
```

---

## Tested Drivers

| Driver | Protocol | Result |
|--------|----------|--------|
| ctmouse 3.4+ | MZ | ✅ Movement, buttons, scroll wheel |
| MS MOUSE.COM 8.20 | M, MZ | ✅ Movement, buttons |

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| *Mouse not found* | Ident not received in time | Check RTS/DTR wiring; try `testm` while driver is loading |
| *Mouse found* but cursor doesn't move | Wrong protocol | Try `proto M` then reload driver |
| Cursor moves erratically | Packet desync — wrong baud/framing | Verify MAX232 capacitors; check VCC 5 V on MAX232 |
| Movement reversed | Y axis inverted | Use `flipy` |
| Scroll reversed | Wheel direction | Use `flipw` |
| Cursor moves but very slowly | Scale too high | Try `scale 2` or `scale 1` |
| BLE mouse not found in scan | Mouse not in pairing mode | Put mouse in pairing/discoverable mode first |
| `connect` fails many times | Mouse exits pairing mode too quickly | Keep mouse in pairing mode; firmware retries up to 20 times |
| Connects but no movement data | Wrong Report ID | Check serial log for `[BLE]` lines; set `reportid 17` for MX Master |
| Mouse disconnects frequently | Mouse enters BLE sleep | Firmware reads battery every 5 s as keepalive — reduce `KEEPALIVE_MS` if needed |
| Device names missing in scan | — | Fixed: scan now uses `onResult` (fires after full advertisement received) |

---

## Credits & References

- Serial mouse protocol: [Tomi Engdahl — PC Mouse Information](https://courses.cs.washington.edu/courses/cse477/00sp/projectwebs/groupb/PS2-mouse/mouse.html)
- Serial mouse protocol reference: [roborooter.com/post/serial-mice](https://roborooter.com/post/serial-mice/)
- NimBLE-Arduino: [h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)

---

## License

MIT
