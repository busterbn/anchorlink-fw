# MQTT Protocol Specification — Boat Monitor

## Broker

- **Host:** TBD (e.g. HiveMQ Cloud endpoint)
- **Port:** 8883 (TLS)
- **Auth:** Username/password per device and per web client

## Topic Structure

All topics are prefixed with the device IMEI number. There are **two relays**
(`relay1`, `relay2`) and **two batteries**, reported together on a single `bat`
topic.

| Topic                 | Direction        | QoS | Retained | Payload (JSON)                                   |
|-----------------------|------------------|-----|----------|--------------------------------------------------|
| `{imei}/relay1`       | Device → Server  | 1   | Yes      | `{"state":0}` or `{"state":1}`                   |
| `{imei}/relay2`       | Device → Server  | 1   | Yes      | `{"state":0}` or `{"state":1}`                   |
| `{imei}/bat`          | Device → Server  | 0   | No       | `{"bat1":14.45,"bat2":12.83}`                    |
| `{imei}/gps`          | Device → Server  | 0   | No       | `{"lat":55.123456,"lon":12.345678}`              |
| `{imei}/anchor-alarm` | Device → Server  | 1   | No       | `{"distance":42}`                                |
| `{imei}/pair`         | Device → Server  | 1   | No       | `{"status":"ready"}` (BTN0 long press)           |
| `{imei}/status`       | Device → Broker  | 1   | Yes      | `{"status":"online"}` / `{"status":"offline"}` (LWT) |
| `{imei}/fw`           | Device → Server  | 1   | Yes      | `{"version":"2.1.1"}` (on connect)               |
| `{imei}/fota`         | Device → Server  | 1   | No       | `{"status":"updating"}` (on FOTA start)          |
| `{imei}/cmd/#`        | Server → Device  | 1   | No       | `{"cmd":"..."}` — see §8                          |

**All payloads are JSON** (UTF-8). Numeric values are JSON **numbers**, not
strings — e.g. voltage is `14.45`, relay state is `1`, distance is `42`.

---

## 1. Relay State — `{imei}/relay1`, `{imei}/relay2`

Published by the device whenever the relay state changes (from a button press
or from a `cmd` message). Also published once for each relay immediately after
(re)connecting to the broker, so that any newly-subscribing client gets the
current state.

- **Payload:** `{"state":1}` (ON) or `{"state":0}` (OFF)
- **Retained:** Yes — new subscribers immediately receive the latest state
- **QoS:** 1

Example:

```
Topic:    862345678901234/relay1
Payload:  {"state":1}
Retained: true
```

---

## 2. Battery Voltage — `{imei}/bat`

The device only reports battery voltage when explicitly polled by the cloud
via a `bat` command (see §8). It does **not** sample periodically and does
**not** auto-publish on connect — the cloud is in charge of when to ask.

Both batteries are reported together in a single message.

All charging detection / state-of-charge logic lives in the cloud — the
device only reports voltage.

- **Payload:** `{"bat1":<v>,"bat2":<v>}` — voltages as JSON numbers with 2
  decimals, e.g. `{"bat1":14.45,"bat2":12.83}`
- **Retained:** No
- **QoS:** 0

Example:

```
Topic:    862345678901234/bat
Payload:  {"bat1":14.45,"bat2":12.83}
Retained: false
```

---

## 3. Online/Offline Status — `{imei}/status`

The device sets an **MQTT Last Will and Testament (LWT)** on connect:

- **Will topic:** `{imei}/status`
- **Will payload:** `{"status":"offline"}`
- **Will retain:** Yes
- **Will QoS:** 1

Immediately after connecting, the device publishes:

```
Topic:    {imei}/status
Payload:  {"status":"online"}
Retained: true
QoS:      1
```

The broker automatically publishes `{"status":"offline"}` if the device loses
connection.

---

## 4. GPS — `{imei}/gps`

Published when the device gets a GPS fix in response to a `gps` command, after
setting an anchor (the origin), or together with an anchor alarm.

- **Payload:** `{"lat":<deg>,"lon":<deg>}` — both JSON numbers with 6 decimal
  places, e.g. `{"lat":55.123456,"lon":12.345678}`. Southern/western values are
  negative.
- **Retained:** No
- **QoS:** 0

---

## 5. Anchor Alarm — `{imei}/anchor-alarm`

Published once when the device drifts outside the configured anchor radius.
The accompanying GPS location is also published on `{imei}/gps`. After firing,
monitoring stops automatically.

- **Payload:** `{"distance":<m>}` — distance from the anchor origin in meters as
  a JSON integer, e.g. `{"distance":42}`.
- **Retained:** No
- **QoS:** 1

---

## 6. Firmware Version — `{imei}/fw`

Published once by the device immediately after (re)connecting to the broker so
that any subscribing client knows which firmware version is currently running.
Retained, so newly-subscribing clients receive the value without waiting.

- **Payload:** `{"version":"<X.Y.Z>"}`, e.g. `{"version":"2.1.1"}` (sourced from
  `CONFIG_MEMFAULT_NCS_FW_VERSION`)
- **Retained:** Yes
- **QoS:** 1

Example:

```
Topic:    862345678901234/fw
Payload:  {"version":"2.1.1"}
Retained: true
```

---

## 7. Pairing — `{imei}/pair`

Published by the device when **BTN0 is held for 3 seconds**. The intended use is
to let a web/app client put itself into pairing mode and bind to the device.

- **Payload:** `{"status":"ready"}`
- **Retained:** No
- **QoS:** 1

Example:

```
Topic:    862345678901234/pair
Payload:  {"status":"ready"}
Retained: false
```

---

## 8. FOTA Status — `{imei}/fota`

Published by the device when it starts an OTA update (in response to a
`fota_update` command) to acknowledge that the update is beginning. The device
reboots into the new image if the download succeeds.

- **Payload:** `{"status":"updating"}`
- **Retained:** No
- **QoS:** 1

---

## 9. Commands — `{imei}/cmd/#`

The device subscribes to `{imei}/cmd/#` (wildcard — any subtopic under `cmd`).
The exact subtopic is not significant; the device acts on the **payload** only.

Each command is a JSON object with a `"cmd"` string. `anchor-alarm` additionally
takes an integer `"radius"` (meters).

| Payload                                  | Action                                                                  |
|------------------------------------------|-------------------------------------------------------------------------|
| `{"cmd":"rel1"}`                         | Toggle relay 1 (and publish new state on `/relay1`)                     |
| `{"cmd":"rel2"}`                         | Toggle relay 2 (and publish new state on `/relay2`)                     |
| `{"cmd":"bat"}`                          | Read both batteries and publish on `/bat`                               |
| `{"cmd":"gps"}`                          | Acquire a GPS fix and publish on `/gps`                                 |
| `{"cmd":"anchor-alarm","radius":<m>}`    | Set anchor: acquire fix, save as origin, then monitor with radius `<m>` |
| `{"cmd":"anchor-alarm","radius":0}`      | Cancel any active anchor monitoring                                     |
| `{"cmd":"fota_update"}`                  | Check for an OTA update now and install if newer (device reboots)       |
| `{"cmd":"reboot"}`                       | Reboot the device                                                       |

- **QoS:** 1 (recommended, to guarantee delivery)
- **Retained:** No
- The parser recognises only the `"cmd"` and `"radius"` keys. Send **only**
  these — unexpected keys, malformed JSON, or an unknown `"cmd"` value are
  ignored. `"radius"` must be a non-negative JSON integer.

Example — toggle relay 1:

```
Topic:    862345678901234/cmd/foo
Payload:  {"cmd":"rel1"}
```

Example — start anchor alarm with 25 m radius:

```
Topic:    862345678901234/cmd/foo
Payload:  {"cmd":"anchor-alarm","radius":25}
```

### Anchor alarm flow

```
Web UI                                    Broker                        Device
  |-- PUB cmd {"cmd":"anchor-alarm","radius":25} ->|--------------------->|
  |                                          |                   (acquire GPS fix)
  |                                          |<-- PUB gps {"lat":55.123456,"lon":12.345678}
  |<-- forward gps --------------------------|
  |                                                (origin saved; periodic
  |                                                 GPS checks against 25 m)
  |                                          ...
  |                                          |<-- PUB anchor-alarm {"distance":42}  (drifted)
  |                                          |<-- PUB gps {"lat":55.123890,"lon":12.345111}
  |<-- forward anchor-alarm + gps -----------|
                                                          (monitoring stopped)
```

To cancel before any breach:

```
Topic:    862345678901234/cmd/foo
Payload:  {"cmd":"anchor-alarm","radius":0}
```

---

## Sequence Diagrams

### Device Boots / Reconnects

```
Device                                                Broker                Web UI
  |--- CONNECT (LWT: {"status":"offline"}) ------------->|                       |
  |--- SUB {imei}/cmd/# ---------------------------------|                       |
  |--- PUB {imei}/status  {"status":"online"} retain --->|--- forward --------->|
  |--- PUB {imei}/fw      {"version":"X.Y.Z"} retain --->|--- forward --------->|
  |--- PUB {imei}/relay1  {"state":0|1}       retain --->|--- forward --------->|
  |--- PUB {imei}/relay2  {"state":0|1}       retain --->|--- forward --------->|
```

Note: battery voltages are **not** auto-published on connect; the cloud must
poll with a `bat` command when it wants a fresh reading.

### UI Subscribes (any time)

```
Web UI                              Broker
  |--- SUB {imei}/relay1 ------------>|
  |--- SUB {imei}/relay2 ------------>|
  |--- SUB {imei}/bat    ------------>|
  |--- SUB {imei}/status ------------>|
  |<-- retained relay1, relay2, status |
```

### Toggle Relay from UI

```
Web UI                                   Broker                          Device
  |--- PUB {imei}/cmd {"cmd":"rel1"} ------>|--- forward --------------->|
  |                                         |                     (toggle relay 1)
  |                                         |<-- PUB relay1 {"state":1}  retain=true
  |<--- forward relay1 {"state":1} ---------|
```

### Toggle Relay from Button

```
                                     Broker                          Device
                                       |                     (BTN1 press → toggle)
                                       |<-- PUB relay1 {"state":1}  retain=true
  Web UI <--- forward relay1 ----------|
```

### Request Battery

```
Web UI                                  Broker                          Device
  |--- PUB {imei}/cmd {"cmd":"bat"} ------->|--- forward --------------->|
  |                                         |                  (read both ADCs)
  |                                         |<-- PUB bat {"bat1":14.45,"bat2":12.83}
  |<--- forward bat ------------------------|
```

### Device Disconnect

```
Device                              Broker                          Web UI
  |--- connection lost                  |                              |
  |                                     |--- PUB status {"status":"offline"} -->|
  |                                     |    (LWT, retained)           |
```

---

## Buttons (device side, FYI)

| Button         | Action                                                       |
|----------------|--------------------------------------------------------------|
| BTN0 (short)   | Logs `"BTN0 pressed"` (no MQTT side effect)                  |
| BTN0 (3 s hold)| Publishes `{"status":"ready"}` on `{imei}/pair`             |
| BTN1           | Toggles relay 1 → publishes new state on `{imei}/relay1`     |
| BTN2           | Toggles relay 2 → publishes new state on `{imei}/relay2`     |

---

## Notes

- **Client ID:** The device uses its IMEI as the MQTT client ID. Web clients
  use an arbitrary unique ID.
- **JSON everywhere:** Every payload — telemetry, status, and inbound commands —
  is a JSON object. Numeric fields are JSON numbers, not quoted strings.
- **No streaming mode:** The device does not publish a periodic state bundle.
  Relay state is published on change (and once on connect); battery voltages
  are published only on demand in response to a `bat` command.
- **Extensibility:** New commands can be added by extending the `"cmd"` values in
  the table above (and the parser). New telemetry can be added by adding new
  topics under `{imei}/...`.
