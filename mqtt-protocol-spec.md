# MQTT Protocol Specification — Boat Monitor

## Broker

- **Host:** TBD (e.g. HiveMQ Cloud endpoint)
- **Port:** 8883 (TLS)
- **Auth:** Username/password per device and per web client

## Topic Structure

All topics are prefixed with the device IMEI number.

| Topic | Direction | QoS | Retained | Description |
|---|---|---|---|---|
| `{imei}/state` | Device → Server | 0 | Yes | Telemetry snapshot |
| `{imei}/cmd` | Server → Device | 1 | No | Commands to the device |
| `{imei}/status` | Device → Broker (LWT) | 1 | Yes | Online/offline status |

---

## 1. Device State — `{imei}/state`

Published by the device in two modes:

### Idle Mode (default)
The device only publishes state when something changes (e.g. after a relay toggle). This minimizes data usage when no one is watching.

### Streaming Mode
When the web UI is open, it sends a `start_stream` command. The device then publishes state every **30 seconds** in addition to on-change publishes. Streaming automatically stops after **5 minutes** unless renewed. This timeout protects against missed `stop_stream` commands (browser crash, network loss, etc.).

**Retained: Yes** — ensures new subscribers immediately receive the latest state without requesting it.

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

Published by the web server. **QoS 1** — guarantees delivery to the device.

### 2.1 Set Relay

```json
{
  "action": "set_relay",
  "relay": 2,
  "state": true
}
```

| Field | Type | Description |
|---|---|---|
| `action` | string | `"set_relay"` |
| `relay` | int | Relay number (1–5) |
| `state` | bool | Desired state (true = ON, false = OFF) |

**Acknowledgement:** The device publishes a new `{imei}/state` with the updated relay state. There is no separate ACK topic. If the UI does not see an updated state within **5 seconds**, it should display a timeout error.

### 2.2 Start Stream

Sent when the web UI opens or becomes active. Tells the device to begin periodic state publishing.

```json
{
  "action": "start_stream"
}
```

The device begins publishing `{imei}/state` every **30 seconds**. The stream timeout resets to **5 minutes** on each `start_stream` received. The web UI should re-send `start_stream` every **4 minutes** to keep the stream alive.

### 2.3 Stop Stream

Sent when the web UI closes or becomes inactive.

```json
{
  "action": "stop_stream"
}
```

The device returns to idle mode (publish on change only).

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

### UI Opens — Start Streaming

```
Web UI                    Broker                    Device
  |--- SUB {imei}/state --->|                         |
  |--- SUB {imei}/status -->|                         |
  |<-- retained state ------|                         |
  |<-- "online" ------------|                         |
  |                          |                         |
  |--- PUB start_stream --->|--- forward cmd --------->|
  |                          |                  (start 30s timer)
  |                          |<--- PUB state (30s) ----|
  |<--- forward state ------|                          |
  |                          |<--- PUB state (30s) ----|
  |<--- forward state ------|                          |
```

### UI Sends Keep-Alive (every 4 min)

```
Web UI                    Broker                    Device
  |--- PUB start_stream --->|--- forward cmd --------->|
  |                          |                  (reset 5min timeout)
```

### UI Closes — Stop Streaming

```
Web UI                    Broker                    Device
  |--- PUB stop_stream ---->|--- forward cmd --------->|
  |                          |                  (back to idle)
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

### Stream Timeout (no keep-alive received)

```
Device
  |--- 5 min since last start_stream
  |--- (back to idle, publish on change only)
```

---

## Notes

- **Timestamp:** The device uses modem time (AT+CCLK) which is synced via the LTE network.
- **Relay numbering:** 1-indexed everywhere. `"relay": 2` in a command corresponds to `relays[1]` in the state array (0-indexed array, but representing relay 2).
- **Extensibility:** New command types can be added via the `action` field without breaking existing functionality.
- **Client ID:** The device uses its IMEI as the MQTT client ID. Web clients use an arbitrary unique ID.
- **Data usage in idle:** Minimal — only MQTT keep-alive pings (~5.5 KB/day at 60s interval) plus occasional on-change publishes.
