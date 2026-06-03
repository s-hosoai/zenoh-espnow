# zenoh-esp-now

**[日本語版 README](README_ja.md)**

**Zenoh pub/sub over ESP-NOW — no router, no IP stack, no configuration.**

This project lets multiple ESP32-S3 devices run [zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) and exchange pub/sub data directly over ESP-NOW broadcast, with zero infrastructure required.  Just power them on — they discover each other automatically and start communicating.

```
  Node A          Node B          Node C
 (pub/sub)       (pub/sub)       (pub/sub)
    │                │                │
    └────────────────┴────────────────┘
           ESP-NOW broadcast
        (no router, no AP, no IP)
```

## Why this matters

Standard Zenoh runs over TCP/UDP and requires a network infrastructure.  This project replaces that transport layer with ESP-NOW, a low-latency IEEE 802.11 Vendor-specific Action Frame protocol that works **without any IP stack or Wi-Fi association**.

| | Standard Zenoh | zenoh-esp-now |
|---|---|---|
| Transport | TCP / UDP | ESP-NOW broadcast |
| Router required | Yes (zenohd) | **No** |
| IP address required | Yes | **No** |
| Node discovery | Manual or zenohd | **Automatic (JOIN)** |
| Latency | ~ms (Wi-Fi) | ~ms (ESP-NOW) |
| Max payload | Unlimited (fragmented) | 250 B / frame |

## Quick Start

### 1. Clone

```bash
git clone --recurse-submodules <repo-url>
cd zenoh-espnow
```

### 2. Apply zenoh-pico patches

```bash
scripts/apply_patches.sh
```

### 3. Flash `espnow_node` to two or more ESP32-S3 boards

```bash
cd examples/espnow_node
idf.py menuconfig   # set ESPNOW_CHANNEL (all boards must use the same channel)
idf.py build flash monitor
```

That is all.  Each node automatically discovers its neighbours, then publishes and subscribes without any further configuration.

Expected output:

```
I zenoh_espnow: Ready  MAC=24:58:7c:xx:xx:xx  ch=1
I zenoh_espnow: Opening Zenoh session (ESP-NOW transport)...
I espnow_node:  Subscribed to 'sensor/**'
I espnow_node:  TX: {"seq":0,"mac":"24:58:7c:xx:xx:xx"}
I espnow_node:  RX  'sensor/24:58:7c:yy:yy:yy/data'  '{"seq":3,...}'
```

## How It Works

zenoh-pico's UDP multicast PAL functions are replaced at compile time by ESP-NOW equivalents via `ZENOH_ESPNOW_LINK_OVERRIDE`.  The locator string `udp/224.0.0.225:7447` is kept so zenoh-pico's multicast peer session logic (JOIN, peer discovery, pub/sub routing) works unchanged — only the physical send/receive path is swapped.

```
zenoh-pico  peer mode
  └─ _z_send_udp_multicast()  →  esp_now_send(FF:FF:FF:FF:FF:FF, ...)
  └─ _z_read_udp_multicast()  →  FreeRTOS queue ← ESP-NOW recv callback
  └─ _z_open_udp_multicast()  →  esp_now_init()
```

Peer discovery uses zenoh-pico's built-in **JOIN message mechanism**: each node periodically broadcasts a JOIN frame; on reception, the remote node registers the sender as a Zenoh peer.  No manual pairing or address configuration is needed.

## Key Design Decisions

| Item | Decision | Reason |
|---|---|---|
| TX path | ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`) | O(1) — no per-node registration |
| Discovery | zenoh-pico JOIN broadcast | Zero-config automatic peer detection |
| Reliability | Best-effort | pub/sub tolerates loss |
| Encryption | Disabled | Broadcast and CCMP are mutually exclusive |
| Channel | Fixed (Kconfig) | All nodes must share the same 2.4 GHz channel |
| Payload limit | 250 B / frame | ESP-NOW v1.0; zenoh-pico fragmentation handles larger payloads |

## Requirements

- **Hardware**: ESP32-S3 (or ESP32 / ESP32-C — set `IDF_TARGET` in `CMakeLists.txt`)
- **Software**: [ESP-IDF v5.5.x](https://github.com/espressif/esp-idf), Git

## Repository Structure

```
zenoh-espnow/
├── components/
│   ├── zenoh_pico_idf/          ESP-IDF component wrapper for zenoh-pico
│   └── zenoh_espnow/            ESP-NOW transport library
│       ├── include/zenoh_espnow.h
│       ├── src/zenoh_espnow_link.c
│       └── zenoh_espnow_patch/  Minimal patches to zenoh-pico
├── examples/
│   ├── espnow_node/             Core example: Zenoh pub/sub over ESP-NOW
│   ├── gateway/                 Advanced: bridge to Wi-Fi / zenohd
│   ├── espnow_broadcast/        Utility: bare ESP-NOW channel test
│   └── zenoh_pubsub/            Utility: zenoh-pico over standard Wi-Fi
├── third_party/zenoh-pico/      Submodule — zenoh-pico v1.9.0
├── docs/                        spec.md, Milestone.md
└── scripts/apply_patches.sh
```

## Examples

### `espnow_node` — Core example

Zenoh pub/sub over ESP-NOW with no AP or router.

- Publishes `sensor/<MAC>/data` every 2 s
- Subscribes to `sensor/**`
- Works with 2 or more boards on the same channel

```bash
cd examples/espnow_node && idf.py menuconfig && idf.py build flash monitor
```

### `espnow_broadcast` — Channel verification

Bare ESP-NOW broadcast, no Zenoh.  Use this to confirm channel alignment before flashing `espnow_node`.

---

## Advanced: Gateway to Wi-Fi / Zenoh Network

The gateway example bridges the ESP-NOW network to a standard Zenoh network running over Wi-Fi.

```
[ESP-NOW network]                    [Wi-Fi / Zenoh network]
  espnow_node A                        zenohd Router (PC/SBC)
  espnow_node B  <──  Gateway  ──>     zenoh Client (PC)
  espnow_node C    ESP32-S3
```

The gateway runs **two independent zenoh-pico sessions**:

| Session | Mode | Transport |
|---|---|---|
| `session_a` | peer | ESP-NOW (this library) |
| `session_b` | client | TCP → zenohd Router |

Forwarding uses non-overlapping key spaces to avoid loops:

```
sub(session_a, "sensor/**")  →  pub(session_b)   ESP-NOW → Wi-Fi
sub(session_b, "cmd/**")     →  pub(session_a)   Wi-Fi  → ESP-NOW
```

Both transports coexist in one binary because `ZENOH_ESPNOW_LINK_OVERRIDE` only replaces UDP multicast functions; TCP functions in `network.c` are untouched.

### Gateway setup

```bash
cd examples/gateway
idf.py menuconfig   # set Wi-Fi SSID/password and zenohd IP
idf.py build flash monitor
```

On the PC:

```bash
zenohd -l tcp/0.0.0.0:7447
z_sub --key 'sensor/**'
```

Check the gateway log for `ESP-NOW ch=X` and set the same channel on all `espnow_node` devices.

### Gateway features

- Reconnects to zenohd automatically on Wi-Fi disconnect (exponential backoff)
- `session_a` (ESP-NOW) continues uninterrupted during Wi-Fi outage
- Diagnostics via `zenoh_espnow_get_rx_dropped()` / `zenoh_espnow_get_tx_failed()`

---

## Component API

```c
#include "zenoh_espnow.h"

uint8_t  zenoh_espnow_get_channel(void);     // current Wi-Fi channel
uint32_t zenoh_espnow_get_rx_dropped(void);  // RX queue overflow count
uint32_t zenoh_espnow_get_tx_failed(void);   // TX failure count (with retry)
```

## Known Limitations

| Limitation | Notes |
|---|---|
| Payload ≤ 250 B / frame | ESP-NOW v1.0; zenoh-pico fragmentation is transparent |
| Channel fixed at boot | All nodes must share the same 2.4 GHz channel |
| No encryption | Broadcast and CCMP are mutually exclusive in ESP-NOW |
| Best-effort only | No ACK; use zenoh Advanced Pub/Sub for reliability |
| Single ESP-NOW session | Global singleton per binary |

## License

Apache License 2.0.  
zenoh-pico: EPL-2.0 OR Apache-2.0.
