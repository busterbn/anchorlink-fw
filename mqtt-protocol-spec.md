# MQTT Protocol Specification — Boat Monitor

## Broker

- **Host:** TBD (e.g. HiveMQ Cloud endpoint)
- **Port:** 8883 (TLS)
- **Auth:** Username/password per device and per web client

## Topic Structure

All topics are prefixed with the device IMEI number.

| Topic | Direction | QoS | Retained | Description |
|---|---|---|---|---|
| `{imei}/state` | Device → Server | 0 | Yes | Periodic telemetry |
| `{imei}/cmd` | Server → Device | 1 | No | Commands to the device |
| `{imei}/status` | Device → Broker (LWT) | 1 | Yes | Online/offline status |

---

## 1. Device State — `{imei}/state`

Published by the device every **30 seconds** and on every state change (e.g. after a relay toggle).

**Retained: Yes** — ensures new subscribers immediately receive the latest state.

### Payload (JSON)

```json
{
  "voltage": 12.41,
  "power_w": 47.2,
  "relays": [true, false, true, false, false],
  "ts": 1710423600
}
```

| Field | Type | Description |
|---|---|---|
| `voltage` | float | Battery voltage in volts |
| `power_w` | float | Current power consumption in watts |
| `relays` | bool[5] | State of relay 1–5 (true = ON) |
| `ts` | int | Unix timestamp (UTC) of the measurement |

---

## 2. Commands — `{imei}/cmd`

Published by the web server when the user toggles a relay.

**QoS 1** — guarantees delivery to the device.

### Payload (JSON)

```json
{
  "action": "set_relay",
  "relay": 2,
  "state": true
}
```

| Field | Type | Description |
|---|---|---|
| `action` | string | Command type. Currently only `"set_relay"` |
| `relay` | int | Relay number (1–5) |
| `state` | bool | Desired state (true = ON, false = OFF) |

### Acknowledgement

The device acknowledges by publishing a new `{imei}/state` with the updated relay state. There is no separate ACK topic. If the UI does not see an updated state within **5 seconds**, it should display a timeout error.

---

## 3. Online/Offline Status — `{imei}/status`

The device sets an **MQTT Last Will and Testament (LWT)** on connect:

- **Will topic:** `{imei}/status`
- **Will payload:** `"offline"`
- **Will retain:** Yes
- **Will QoS:** 1

Immediately after connecting, the device publishes:

```
Topic: {imei}/status
Payload: "online"
Retained: true
```

The broker automatically publishes `"offline"` if the device loses connection.

---

## Sequence Diagrams

### Normal Operation

```
Device                    Broker                    Web UI
  |--- CONNECT (LWT set) -->|                         |
  |--- PUB "online" ------->|                         |
  |                          |<--- SUB {imei}/state ---|
  |                          |<--- SUB {imei}/status --|
  |                          |--- retained state ----->|
  |                          |--- "online" ----------->|
  |                          |                         |
  |--- PUB state (30s) ---->|--- forward state ------->|
  |--- PUB state (30s) ---->|--- forward state ------->|
```

### Relay Toggle

```
Web UI                    Broker                    Device
  |--- PUB cmd ------------>|--- forward cmd --------->|
  |                          |                  (toggle relay)
  |                          |<--- PUB state (updated)--|
  |<--- forward state ------|                          |
```

### Device Disconnect

```
Device                    Broker                    Web UI
  |--- connection lost      |                         |
  |                         |--- PUB "offline" ------>|
  |                         |   (LWT, retained)       |
```

---

## Notes

- **Timestamp:** The device uses modem time (AT+CCLK) which is synced via the LTE network.
- **Relay numbering:** 1-indexed everywhere. `"relay": 2` in a command corresponds to `relays[1]` in the state array (0-indexed array, but representing relay 2).
- **Extensibility:** New command types can be added via the `action` field without breaking existing functionality.
- **Client ID:** The device uses its IMEI as the MQTT client ID. Web clients use an arbitrary unique ID.
