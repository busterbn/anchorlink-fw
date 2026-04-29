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
| `{imei}/relay1`       | Device → Server  | 0   | Yes      | Relay 1 state — `"0"` or `"1"`               |
| `{imei}/relay2`       | Device → Server  | 0   | Yes      | Relay 2 state — `"0"` or `"1"`               |
| `{imei}/bat1`         | Device → Server  | 0   | No       | Battery 1 voltage, e.g. `"14.45"`            |
| `{imei}/bat2`         | Device → Server  | 0   | No       | Battery 2 voltage, e.g. `"12.83"`            |
| `{imei}/bat1/charging`| Device → Server  | 0   | Yes      | Battery 1 charging state — `"0"` or `"1"`    |
| `{imei}/bat2/charging`| Device → Server  | 0   | Yes      | Battery 2 charging state — `"0"` or `"1"`    |
| `{imei}/gps`          | Device → Server  | 0   | No       | GPS fix `"lat,lon"` (6 decimals)             |
| `{imei}/anchor-alarm` | Device → Server  | 1   | No       | Anchor breach distance in meters             |
| `{imei}/cmd/#`        | Server → Device  | 1   | No       | Commands to the device (any subtopic)        |
| `{imei}/status`       | Device → Broker  | 1   | Yes      | `"online"` / `"offline"` (LWT)               |

All payloads are plain UTF-8 strings — no JSON.

---

## 1. Relay State — `{imei}/relay1`, `{imei}/relay2`

Published by the device whenever the relay state changes (from a button press
or from a `cmd` message). Also published once for each relay immediately after
(re)connecting to the broker, so that any newly-subscribing client gets the
current state.

- **Payload:** `"1"` (ON) or `"0"` (OFF)
- **Retained:** Yes — new subscribers immediately receive the latest state
- **QoS:** 0

Example:

```
Topic:    862345678901234/relay1
Payload:  1
Retained: true
```

---

## 2. Battery Voltage — `{imei}/bat1`, `{imei}/bat2`

Published only when the device receives a `bat` command (see §4). Both batteries
are published together.

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

## 4. Charging State — `{imei}/bat1/charging`, `{imei}/bat2/charging`

The device samples both battery voltages every 10 seconds and tracks whether
each battery is being charged (voltage above 13.0 V) or not (voltage below
12.8 V), with hysteresis in between. Whenever a battery's charging state
flips, the new state is published on its `charging` topic.

The current state is also republished for both batteries immediately after
(re)connecting to the broker, so newly-subscribing clients see it without
waiting for a transition.

- **Payload:** `"1"` (charging) or `"0"` (not charging)
- **Retained:** Yes
- **QoS:** 0

Example:

```
Topic:    862345678901234/bat1/charging
Payload:  1
Retained: true
```

---

## 5. GPS — `{imei}/gps`

Published when the device gets a GPS fix in response to a `gps` command, after
setting an anchor (the origin), or together with an anchor alarm.

- **Payload:** `"lat,lon"` — both with 6 decimal places, e.g.
  `"55.123456,12.345678"`. Negative values are prefixed with `-`.
- **Retained:** No
- **QoS:** 0

---

## 6. Anchor Alarm — `{imei}/anchor-alarm`

Published once when the device drifts outside the configured anchor radius.
The accompanying GPS location is also published on `{imei}/gps`. After firing,
monitoring stops automatically.

- **Payload:** Distance from the anchor origin in meters as a decimal integer,
  e.g. `"42"`.
- **Retained:** No
- **QoS:** 1

---

## 7. Commands — `{imei}/cmd/#`

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
  |--- PUB {imei}/relay1="0|1"     retain=true ---->|--- forward --------->|
  |--- PUB {imei}/relay2="0|1"     retain=true ---->|--- forward --------->|
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

| Button | Action                                                       |
|--------|--------------------------------------------------------------|
| BTN0   | Logs `"BTN0 pressed"` (no MQTT side effect)                  |
| BTN1   | Toggles relay 1 → publishes new state on `{imei}/relay1`     |
| BTN2   | Toggles relay 2 → publishes new state on `{imei}/relay2`     |

---

## Notes

- **Client ID:** The device uses its IMEI as the MQTT client ID. Web clients
  use an arbitrary unique ID.
- **No streaming mode:** The device no longer publishes a periodic state
  bundle. Relay state is published on change (and once on connect); battery
  voltages are published only on demand.
- **Extensibility:** New command payloads can be added by extending the
  payload-string table above. New telemetry can be added by adding new topics
  under `{imei}/...`.
