# zenoh-esp-now

**[English README](README.md)**

**ESP-NOW 上で Zenoh pub/sub — ルータ不要、IPスタック不要、設定不要。**

複数の ESP32-S3 デバイスが [zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) を使い、ESP-NOW ブロードキャストで直接 pub/sub データを交換できます。インフラ不要で、電源を入れるだけで自動的に互いを発見して通信を開始します。

```
  Node A          Node B          Node C
 (pub/sub)       (pub/sub)       (pub/sub)
    │                │                │
    └────────────────┴────────────────┘
           ESP-NOW ブロードキャスト
        （ルータなし・AP なし・IP なし）
```

## このプロジェクトが解決すること

標準的な Zenoh は TCP/UDP 上で動作し、ネットワークインフラが必要です。本プロジェクトはそのトランスポート層を ESP-NOW に置き換えます。ESP-NOW は IEEE 802.11 Vendor-specific Action Frame を使った低レイテンシプロトコルで、**IPスタックも Wi-Fi 接続も不要**です。

| | 標準 Zenoh | zenoh-esp-now |
|---|---|---|
| トランスポート | TCP / UDP | ESP-NOW ブロードキャスト |
| Router 必要 | 必要（zenohd） | **不要** |
| IP アドレス必要 | 必要 | **不要** |
| ノード発見 | 手動 or zenohd | **自動（JOIN）** |
| レイテンシ | 〜ms（Wi-Fi） | 〜ms（ESP-NOW） |
| 最大ペイロード | 無制限（フラグメント） | 250 B / フレーム |

## クイックスタート

### 1. クローンとセットアップ

```bash
git clone <repo-url>
cd zenoh-espnow
bash scripts/setup.sh   # サブモジュール初期化
```

### 2. 2台以上の ESP32-S3 に `espnow_node` を書き込む

```bash
cd examples/espnow_node
idf.py menuconfig   # ESPNOW_CHANNEL を設定（全ボードで同じ値）
idf.py build flash monitor
```

以上です。各ノードは隣接ノードを自動発見し、追加設定なしで pub/sub を開始します。

期待されるログ出力：

```
I zenoh_espnow: Ready  MAC=24:58:7c:xx:xx:xx  ch=1
I zenoh_espnow: Opening Zenoh session (ESP-NOW transport)...
I espnow_node:  Subscribed to 'sensor/**'
I espnow_node:  TX: {"seq":0,"mac":"24:58:7c:xx:xx:xx"}
I espnow_node:  RX  'sensor/24:58:7c:yy:yy:yy/data'  '{"seq":3,...}'
```

## 仕組み

zenoh-pico の UDP マルチキャスト PAL 関数を、コンパイル時に `ZENOH_ESPNOW_LINK_OVERRIDE` フラグで ESP-NOW 版に差し替えています。ロケータ文字列 `udp/224.0.0.225:7447` はそのまま使うことで、zenoh-pico の multicast peer セッションロジック（JOIN・ピア発見・pub/sub ルーティング）を変更なしに利用します。変わるのは物理的な送受信パスのみです。

```
zenoh-pico  peer mode
  └─ _z_send_udp_multicast()  →  esp_now_send(FF:FF:FF:FF:FF:FF, ...)
  └─ _z_read_udp_multicast()  →  FreeRTOS キュー ← ESP-NOW 受信コールバック
  └─ _z_open_udp_multicast()  →  esp_now_init()
```

ピア発見は zenoh-pico 組み込みの **JOIN メッセージ機構**を使います。各ノードが定期的に JOIN フレームをブロードキャストし、受信したノードが送信元を Zenoh ピアとして登録します。手動ペアリングやアドレス設定は一切不要です。

## 主要な設計方針

| 項目 | 決定 | 理由 |
|---|---|---|
| 送信方式 | ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`) | ノード数に依存しない O(1) 送信 |
| ピア発見 | zenoh-pico JOIN ブロードキャスト | ゼロコンフィグ自動検出 |
| 信頼性 | ベストエフォート | pub/sub はデータ欠損許容 |
| 暗号化 | 無効 | ブロードキャストと CCMP は共存不可 |
| チャンネル | 固定（Kconfig） | 全ノードが同一の 2.4 GHz チャンネルを使用 |
| ペイロード上限 | 250 B / フレーム | ESP-NOW v1.0；zenoh-pico フラグメンテーションで透過的に対応 |

## 必要環境

- **ハードウェア**: ESP32-S3（ESP32 / ESP32-C も可 — `CMakeLists.txt` の `IDF_TARGET` を変更）
- **ソフトウェア**: [ESP-IDF v5.5.x](https://github.com/espressif/esp-idf)、Git

## 既存の ESP-IDF プロジェクトへの組み込み

プロジェクトの `components/` ディレクトリにクローンするだけで使用できます：

```bash
cd your_project/components
git clone <repo-url> zenoh-espnow
cd zenoh-espnow
bash scripts/setup.sh   # サブモジュール初期化
```

プロジェクトの `CMakeLists.txt` に追加：

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components/zenoh-espnow/components")
```

コンポーネントの `CMakeLists.txt` に追加：

```cmake
idf_component_register(
    ...
    REQUIRES zenoh_pico_idf zenoh_espnow
)
```

## リポジトリ構成

```
zenoh-espnow/
├── components/
│   ├── zenoh_espnow/            ESP-NOW トランスポートコンポーネント
│   │   ├── include/zenoh_espnow.h
│   │   └── src/zenoh_espnow_link.c
│   └── zenoh_pico_idf/          zenoh-pico の ESP-IDF コンポーネントラッパー
├── zenoh-pico/                  サブモジュール — zenoh-pico（upstream main）
├── examples/
│   ├── espnow_node/             コアサンプル: ESP-NOW 上の Zenoh pub/sub
│   ├── gateway/                 応用: Wi-Fi / zenohd へのブリッジ
│   ├── espnow_broadcast/        ユーティリティ: 生 ESP-NOW チャンネル確認
│   └── zenoh_pubsub/            ユーティリティ: 標準 Wi-Fi 上の zenoh-pico
├── docs/                        spec.md, Milestone.md
└── scripts/setup.sh             サブモジュール初期化
```

## サンプル一覧

### `espnow_node` — コアサンプル

AP もルータも不要な ESP-NOW 上の Zenoh pub/sub。

- `sensor/<MAC>/data` を 2 秒ごとにパブリッシュ
- `sensor/**` をサブスクライブ
- 同じチャンネルの 2 台以上で動作

```bash
cd examples/espnow_node && idf.py menuconfig && idf.py build flash monitor
```

### `espnow_broadcast` — チャンネル確認

Zenoh を乗せない生 ESP-NOW ブロードキャスト。`espnow_node` を書き込む前のチャンネル設定確認に使います。

---

## 応用: Wi-Fi / Zenoh ネットワークへのゲートウェイ

ゲートウェイサンプルは、ESP-NOW ネットワークを Wi-Fi 上の標準的な Zenoh ネットワークに接続します。

```
[ESP-NOWネットワーク]                  [Wi-Fi / Zenohネットワーク]
  espnow_node A                          zenohd Router (PC/SBC)
  espnow_node B  <── Gateway ──>         zenoh Client (PC)
  espnow_node C    ESP32-S3
```

ゲートウェイは **2つの独立した zenoh-pico セッション**を実行します：

| セッション | モード | トランスポート |
|---|---|---|
| `session_a` | peer | ESP-NOW（本ライブラリ） |
| `session_b` | client | TCP → zenohd Router |

転送はキースペースを分離してループを防止します：

```
sub(session_a, "sensor/**")  →  pub(session_b)   ESP-NOW → Wi-Fi
sub(session_b, "cmd/**")     →  pub(session_a)   Wi-Fi  → ESP-NOW
```

`ZENOH_ESPNOW_LINK_OVERRIDE` は UDP マルチキャスト関数のみを置換し、TCP 関数は `network.c` に残るため、両トランスポートが同一バイナリで共存できます。

### ゲートウェイのセットアップ

```bash
cd examples/gateway
idf.py menuconfig   # Wi-Fi SSID/パスワードと zenohd の IP を設定
idf.py build flash monitor
```

PC 側：

```bash
zenohd -l tcp/0.0.0.0:7447
z_sub --key 'sensor/**'
```

ゲートウェイのログ `ESP-NOW ch=X` を確認し、全 `espnow_node` デバイスの `ESPNOW_CHANNEL` を同じ値に設定してください。

### ゲートウェイの主な機能

- Wi-Fi 切断時に zenohd へ自動再接続（指数バックオフ）
- Wi-Fi 障害中も `session_a`（ESP-NOW）は継続動作
- `zenoh_espnow_get_rx_dropped()` / `zenoh_espnow_get_tx_failed()` で診断情報を取得可能

---

## コンポーネント API

```c
#include "zenoh_espnow.h"

uint8_t  zenoh_espnow_get_channel(void);     // 現在の Wi-Fi チャンネル
uint32_t zenoh_espnow_get_rx_dropped(void);  // RX キュー溢れ回数
uint32_t zenoh_espnow_get_tx_failed(void);   // TX 失敗回数（リトライ後）
```

## 既知の制約

| 制約 | 備考 |
|---|---|
| ペイロード ≤ 250 B / フレーム | ESP-NOW v1.0；zenoh-pico フラグメンテーションで透過的に対応 |
| 起動時にチャンネル固定 | 全ノードが同一の 2.4 GHz チャンネルを使用する必要あり |
| 暗号化なし | ブロードキャストと CCMP は共存不可 |
| ベストエフォートのみ | ACK なし；確実な配信には zenoh Advanced Pub/Sub を検討 |
| ESP-NOW セッションは 1 バイナリに 1 つ | グローバルシングルトン |
| Gateway TCP 接続不可（既知の不具合） | `gateway` サンプルが zenohd への接続に失敗する。原因調査中。 |

## ライセンス

Apache License 2.0  
zenoh-pico: EPL-2.0 OR Apache-2.0
