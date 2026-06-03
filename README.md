# zenoh-esp-now

**[日本語版 README](README_ja.md)**

Zenoh pub/sub over ESP-NOW for ESP32-S3 — routerless wireless P2P communication using [zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) as the middleware and ESP-NOW as the physical transport layer.

## Overview

This project implements a custom zenoh-pico transport backend that replaces UDP multicast with ESP-NOW broadcast.  It requires no IP stack, no Wi-Fi association, and no router for node-to-node communication, while remaining fully interoperable with the standard Zenoh ecosystem through a gateway node.

```
[ESP-NOW network]                    [Wi-Fi / Zenoh network]
  Node A (pub/sub)                     zenohd Router (PC/SBC)
  Node B (pub/sub)  <──  Gateway  ──>  zenoh Client (PC)
  Node C (pub/sub)    ESP32-S3         zenoh-pico (Wi-Fi ESP32)
                      WIFI_AP_STA
```

### Key design decisions

| Item | Decision | Reason |
|---|---|---|
| Transport mode | Multicast peer | Single broadcast delivers to all nodes, no peer table management |
| TX path | ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`) | O(1) send regardless of node count |
| Discovery | zenoh-pico JOIN messages | Periodic broadcast JOIN → automatic peer registration, zero-config |
| Reliability | Best-effort (no ACK) | pub/sub data loss is tolerable |
| Encryption | Disabled (Phase 1) | Broadcast and CCMP are mutually exclusive |
| Channel | Fixed (Kconfig) | All nodes must share the same 2.4 GHz channel |

## Requirements

### Hardware

- ESP32-S3 (or ESP32 / ESP32-C series — change `IDF_TARGET` in `CMakeLists.txt`)
- 8 MB flash recommended

### Software

- [ESP-IDF v5.5.x](https://github.com/espressif/esp-idf) with the Xtensa toolchain
- Git (submodule support)

## Getting Started

### 1. Clone with submodules

```bash
git clone --recurse-submodules <repo-url>
cd zenoh-espnow
```

### 2. Apply zenoh-pico patches

The patches add an override hook in `network.c` and a `#ifndef` guard in `config.h` so `zenoh_espnow` can replace the UDP multicast PAL functions with ESP-NOW equivalents.

```bash
scripts/apply_patches.sh
```

### 3. Source the ESP-IDF environment

```bash
source $IDF_PATH/export.sh
```

### 4. Build and flash an example

```bash
cd examples/espnow_node
idf.py menuconfig   # set ESPNOW_CHANNEL to match your network
idf.py build flash monitor
```

## Repository Structure

```
zenoh-espnow/
├── components/
│   ├── zenoh_pico_idf/          ESP-IDF component wrapper for zenoh-pico
│   │   └── CMakeLists.txt
│   └── zenoh_espnow/            ESP-NOW custom transport (this library)
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── zenoh_espnow.h   Public API (diagnostics)
│       ├── src/
│       │   └── zenoh_espnow_link.c  PAL override: open/close/read/write
│       └── zenoh_espnow_patch/
│           └── 0001-*.patch     Minimal patches to zenoh-pico
├── examples/
│   ├── espnow_broadcast/        Phase 1: bare ESP-NOW broadcast smoke test
│   ├── zenoh_pubsub/            Phase 1: zenoh-pico pub/sub over Wi-Fi UDP
│   ├── espnow_node/             Phase 2/3: zenoh pub/sub via ESP-NOW (no AP)
│   └── gateway/                 Phase 4: ESP-NOW <-> Wi-Fi/zenohd bridge
├── third_party/
│   └── zenoh-pico/              Submodule — zenoh-pico v1.9.0
├── docs/
│   ├── spec.md                  Detailed specification
│   └── Milestone.md             Phase plan and progress
└── scripts/
    └── apply_patches.sh         Apply zenoh-pico patches
```

## Examples

### `espnow_broadcast`

Bare ESP-NOW broadcast sender/receiver.  Used to verify channel configuration and hardware before adding zenoh.

```bash
cd examples/espnow_broadcast && idf.py build flash monitor
```

### `zenoh_pubsub`

zenoh-pico pub/sub over standard Wi-Fi UDP multicast.  Used to verify zenoh-pico works before replacing the transport.

```bash
cd examples/zenoh_pubsub
idf.py menuconfig   # set Wi-Fi SSID/password and Zenoh mode
idf.py build flash monitor
```

### `espnow_node`

A fully functional zenoh peer node using ESP-NOW as transport.  No AP connection required.  Each node:

- Publishes `sensor/<MAC>/data` every 2 seconds
- Subscribes to `sensor/**`

```bash
cd examples/espnow_node
idf.py menuconfig   # set ESPNOW_CHANNEL
idf.py build flash monitor
```

All nodes must be configured to the **same channel**.

### `gateway`

Bridges the ESP-NOW network with a zenohd Router over Wi-Fi/TCP.

```
sub(session_a, "sensor/**") → pub(session_b)   ESP-NOW → zenohd
sub(session_b, "cmd/**")    → pub(session_a)   zenohd  → ESP-NOW
```

```bash
cd examples/gateway
idf.py menuconfig   # set Wi-Fi SSID/password and zenohd IP
idf.py build flash monitor
```

On the PC side:

```bash
zenohd -l tcp/0.0.0.0:7447   # start the router
z_sub --key 'sensor/**'       # subscribe to ESP-NOW node data
```

After flashing the gateway, check its log for the line:

```
I gateway: IP: 192.168.x.x  ESP-NOW ch=6
```

Set `ESPNOW_CHANNEL=6` (or whatever channel is shown) on all `espnow_node` devices.

## Component API

```c
#include "zenoh_espnow.h"

uint8_t  zenoh_espnow_get_channel(void);     // current Wi-Fi channel
uint32_t zenoh_espnow_get_rx_dropped(void);  // RX queue overflow count
uint32_t zenoh_espnow_get_tx_failed(void);   // TX failure count
```

## How the Transport Override Works

zenoh-pico is built with `ZENOH_ESPNOW_LINK_OVERRIDE=1` (injected by `zenoh_espnow`).  This compile flag suppresses the original UDP multicast functions in `network.c`.  `zenoh_espnow_link.c` provides replacements with the same signatures that use ESP-NOW internally.

```
zenoh-pico (peer mode, "udp/224.0.0.225:7447")
  └─ _z_f_link_*_udp_multicast()
       └─ [PAL calls — redirected by ZENOH_ESPNOW_LINK_OVERRIDE]
            ├─ _z_send_udp_multicast()  →  esp_now_send(FF:FF:...:FF)
            ├─ _z_read_udp_multicast()  →  xQueueReceive(rx_queue)
            ├─ _z_open_udp_multicast()  →  esp_now_init()
            └─ _z_close_udp_multicast() →  esp_now_deinit()
```

TCP functions (`_z_open_tcp`, etc.) are in a separate block in `network.c` and are **not** affected by the override, allowing `session_a` (ESP-NOW) and `session_b` (TCP to zenohd) to coexist in the same gateway binary.

## Known Limitations

| Limitation | Notes |
|---|---|
| Payload ≤ 250 bytes | ESP-NOW v1.0; zenoh-pico fragmentation handles larger messages |
| Channel fixed at boot | All nodes must share the same 2.4 GHz channel |
| No encryption | Broadcast and CCMP are mutually exclusive in ESP-NOW |
| Best-effort only | No ACK; use zenoh Advanced Pub/Sub for reliable delivery |
| Single ESP-NOW session | Global singleton; cannot open two ESP-NOW sessions in one binary |

## License

This project is licensed under the Apache License 2.0.  
zenoh-pico is licensed under the EPL-2.0 OR Apache-2.0.
