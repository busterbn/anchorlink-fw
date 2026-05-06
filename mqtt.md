# MQTT Protocol Specification — Boat Monitor

## Broker

- **Host:** TBD (e.g. HiveMQ Cloud endpoint)
- **Port:** 8883 (TLS)
- **Auth:** Username/password per device and per web client

## Topic Structure

All topics are prefixed with the device IMEI number. There are **two relays**
(`relay1`, `relay2`) and **two batteries** (`bat1`, `bat2`).

| Topic                 | Direction        | QoS | Retained | Description                                  |
|-----------------------|------------------|-----|----------|----------------------------------------------|
| `{imei}/relay1`       | Device → Server  | 1   | Yes      | Relay 1 state — `"0"` or `"1"`               |
| `{imei}/relay2`       | Device → Server  | 1   | Yes      | Relay 2 state — `"0"` or `"1"`               |
| `{imei}/bat1`         | Device → Server  | 0   | No       | Battery 1 voltage, e.g. `"14.45"`            |
| `{imei}/bat2`         | Device → Server  | 0   | No       | Battery 2 voltage, e.g. `"12.83"`            |
| `{imei}/gps`          | Device → Server  | 0   | No       | GPS fix `"lat,lon"` (6 decimals)             |
| `{imei}/anchor-alarm` | Device → Server  | 1   | No       | Anchor breach distance in meters             |
| `{imei}/cmd/#`        | Server → Device  | 1   | No       | Commands to the device (any subtopic)        |
| `{imei}/pair`         | Device → Server  | 1   | No       | Pairing request — `"ready"` (BTN0 long press) |
| `{imei}/relay1/current_h` | Device → Server | 1 | No       | Hourly relay 1 current — `"avg,latest"` (A)  |
| `{imei}/relay2/current_h` | Device → Server | 1 | No       | Hourly relay 2 current — `"avg,latest"` (A)  |
| `{imei}/status`       | Device → Broker  | 1   | Yes      | `"online"` / `"offline"` (LWT)               |
| `{imei}/fw`           | Device → Server  | 1   | Yes      | Firmware version, e.g. `"0.1.1"` (on connect) |

All payloads are plain UTF-8 strings — no JSON.

---

## 1. Relay State — `{imei}/relay1`, `{imei}/relay2`

Published by the device whenever the relay state changes (from a button press
or from a `cmd` message). Also published once for each relay immediately after
(re)connecting to the broker, so that any newly-subscribing client gets the
current state.

- **Payload:** `"1"` (ON) or `"0"` (OFF)
- **Retained:** Yes — new subscribers immediately receive the latest state
- **QoS:** 1

Example:

```
Topic:    862345678901234/relay1
Payload:  1
Retained: true
```

---

## 2. Battery Voltage — `{imei}/bat1`, `{imei}/bat2`

The device samples both batteries **once a minute** and publishes whenever
either voltage has moved by **≥ 0.10 V** since the last published reading.
Both batteries are always published together (so the cloud can assume that
between messages the voltage stays within ±0.10 V of the latest payload).

The device also publishes:

- **Immediately on (re)connect to the broker**, so newly-subscribing clients
  see a current value without waiting for the next change.
- **On demand** in response to a `bat` command (see §8).

All charging detection / state-of-charge logic lives in the cloud — the
device only reports voltage.

- **Payload:** Voltage as a decimal string with 2 decimals, e.g. `"14.45"`
- **Retained:** No
- **QoS:** 0

Example:

```
Topic:    862345678901234/bat1
Payload:  14.45
Retained: false
```

---

## 3. Online/Offline Status — `{imei}/status`

The device sets an **MQTT Last Will and Testament (LWT)** on connect:

- **Will topic:** `{imei}/status`
- **Will payload:** `"offline"`
- **Will retain:** Yes
- **Will QoS:** 1

Immediately after connecting, the device publishes:

```
Topic:    {imei}/status
Payload:  online
Retained: true
QoS:      1
```

The broker automatically publishes `"offline"` if the device loses connection.

---

## 4. GPS — `{imei}/gps`

Published when the device gets a GPS fix in response to a `gps` command, after
setting an anchor (the origin), or together with an anchor alarm.

- **Payload:** `"lat,lon"` — both with 6 decimal places, e.g.
  `"55.123456,12.345678"`. Negative values are prefixed with `-`.
- **Retained:** No
- **QoS:** 0

---

## 5. Anchor Alarm — `{imei}/anchor-alarm`

Published once when the device drifts outside the configured anchor radius.
The accompanying GPS location is also published on `{imei}/gps`. After firing,
monitoring stops automatically.

- **Payload:** Distance from the anchor origin in meters as a decimal integer,
  e.g. `"42"`.
- **Retained:** No
- **QoS:** 1

---

## 6. Relay Current — `{imei}/relay1/current_h`, `{imei}/relay2/current_h`

When a relay is ON the device samples its current draw every 10 s through a
10 mΩ shunt sitting between battery 1 and the relay output:

```
I = (V_bat1 - V_relayX) / 0.010
```

Once per hour, on the hour (UTC), the device publishes:

- **`avg`** — mean current across all 10 s samples taken during the past hour
  while the relay was on
- **`latest`** — the most recent 10 s sample

If a relay was off for the full hour, no message is sent for that relay.

- **Payload:** `"avg,latest"` — both amps, 2 decimals, e.g. `"3.45,3.21"`
- **Retained:** No
- **QoS:** 1

Example:

```
Topic:    862345678901234/relay1/current_h
Payload:  3.45,3.21
```

---

## 7. Firmware Version — `{imei}/fw`

Published once by the device immediately after (re)connecting to the broker so
that any subscribing client knows which firmware version is currently running.
Retained, so newly-subscribing clients receive the value without waiting.

- **Payload:** Version string, e.g. `"0.1.1"` (sourced from
  `CONFIG_MEMFAULT_NCS_FW_VERSION`)
- **Retained:** Yes
- **QoS:** 1

Example:

```
Topic:    862345678901234/fw
Payload:  0.1.1
Retained: true
```

---

## 8. Pairing — `{imei}/pair`

Published by the device when **BTN0 is held for 3 seconds**. The intended use is
to let a web/app client put itself into pairing mode and bind to the device.

- **Payload:** `"ready"`
- **Retained:** No
- **QoS:** 1

Example:

```
Topic:    862345678901234/pair
Payload:  ready
Retained: false
```

---

## 9. Commands — `{imei}/cmd/#`

The device subscribes to `{imei}/cmd/#` (wildcard — any subtopic under `cmd`).
The exact subtopic is not significant; the device acts on the **payload** only.

| Payload             | Action                                                                  |
|---------------------|-------------------------------------------------------------------------|
| `rel1`              | Toggle relay 1 (and publish new state on `/relay1`)                     |
| `rel2`              | Toggle relay 2 (and publish new state on `/relay2`)                     |
| `bat`               | Read both batteries and publish on `/bat1`, `/bat2`                     |
| `gps`               | Acquire a GPS fix and publish on `/gps`                                 |
| `anchor-alarm <m>`  | Set anchor: acquire fix, save as origin, then monitor with radius `<m>` |
| `anchor-alarm 0`    | Cancel any active anchor monitoring                                     |
| `fota_update`       | Check for an OTA update now and install if newer (device reboots)       |

- **QoS:** 1 (recommended, to guarantee delivery)
- **Retained:** No
- Unknown payloads are ignored.

Example — toggle relay 1:

```
Topic:    862345678901234/cmd/foo
Payload:  rel1
```

Example — start anchor alarm with 25 m radius:

```
Topic:    862345678901234/cmd/foo
Payload:  anchor-alarm 25
```

### Anchor alarm flow

```
Web UI                          Broker                              Device
  |--- PUB cmd "anchor-alarm 25" ->|------------------------------->|
  |                                |                       (acquire GPS fix)
  |                                |<-- PUB gps="55.123456,12.345678"
  |<-- forward gps ----------------|
  |                                                  (origin saved; periodic
  |                                                   GPS checks against 25 m)
  |                                ...
  |                                |<-- PUB anchor-alarm="42"     (drifted)
  |                                |<-- PUB gps="55.123890,12.345111"
  |<-- forward anchor-alarm + gps -|
                                                       (monitoring stopped)
```

To cancel before any breach:

```
Topic:    862345678901234/cmd/foo
Payload:  anchor-alarm 0
```

---

## Sequence Diagrams

### Device Boots / Reconnects

```
Device                                           Broker                Web UI
  |--- CONNECT (LWT: status=offline) -------------->|                       |
  |--- SUB {imei}/cmd/# ----------------------------|                       |
  |--- PUB {imei}/status="online"  retain=true ---->|--- forward --------->|
  |--- PUB {imei}/fw="X.Y.Z"       retain=true ---->|--- forward --------->|
  |--- PUB {imei}/relay1="0|1"     retain=true ---->|--- forward --------->|
  |--- PUB {imei}/relay2="0|1"     retain=true ---->|--- forward --------->|
  |--- PUB {imei}/bat1="X.XX" ---------------------->|--- forward --------->|
  |--- PUB {imei}/bat2="X.XX" ---------------------->|--- forward --------->|
```

### UI Subscribes (any time)

```
Web UI                              Broker
  |--- SUB {imei}/relay1 ------------>|
  |--- SUB {imei}/relay2 ------------>|
  |--- SUB {imei}/bat1   ------------>|
  |--- SUB {imei}/bat2   ------------>|
  |--- SUB {imei}/status ------------>|
  |<-- retained relay1, relay2, status |
```

### Toggle Relay from UI

```
Web UI                              Broker                          Device
  |--- PUB {imei}/cmd "rel1" ---------->|--- forward --------------->|
  |                                     |                     (toggle relay 1)
  |                                     |<-- PUB relay1="1"  retain=true
  |<--- forward relay1="1" -------------|
```

### Toggle Relay from Button

```
                                     Broker                          Device
                                       |                     (BTN1 press → toggle)
                                       |<-- PUB relay1="1"  retain=true
  Web UI <--- forward relay1="1" ------|
```

### Request Battery

```
Web UI                              Broker                          Device
  |--- PUB {imei}/cmd "bat" ----------->|--- forward --------------->|
  |                                     |                  (read both ADCs)
  |                                     |<-- PUB bat1="14.45"
  |                                     |<-- PUB bat2="12.83"
  |<--- forward bat1, bat2 -------------|
```

### Device Disconnect

```
Device                              Broker                          Web UI
  |--- connection lost                  |                              |
  |                                     |--- PUB status="offline" ---->|
  |                                     |    (LWT, retained)           |
```

---

## Buttons (device side, FYI)

| Button         | Action                                                       |
|----------------|--------------------------------------------------------------|
| BTN0 (short)   | Logs `"BTN0 pressed"` (no MQTT side effect)                  |
| BTN0 (3 s hold)| Publishes `"ready"` on `{imei}/pair`                         |
| BTN1           | Toggles relay 1 → publishes new state on `{imei}/relay1`     |
| BTN2           | Toggles relay 2 → publishes new state on `{imei}/relay2`     |

---

## Notes

- **Client ID:** The device uses its IMEI as the MQTT client ID. Web clients
  use an arbitrary unique ID.
- **No streaming mode:** The device does not publish a periodic state bundle.
  Relay state is published on change (and once on connect); battery voltages
  are published on change (≥ 0.10 V), on connect, and on demand.
- **Extensibility:** New command payloads can be added by extending the
  payload-string table above. New telemetry can be added by adding new topics
  under `{imei}/...`.
