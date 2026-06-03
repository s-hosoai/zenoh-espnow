# zenoh-esp-now

**[English README](README.md)**

ESP-NOW を物理トランスポートとして使い、[zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) を介して ESP32-S3 間でルータ不要の Zenoh pub/sub 通信を実現するプロジェクトです。

## 概要

zenoh-pico の UDP マルチキャスト PAL 関数を ESP-NOW ブロードキャストで置き換えるカスタムトランスポートを実装しています。IPスタック・Wi-Fi 接続・ルータなしでノード間通信が可能であり、ゲートウェイ経由で標準的な Zenoh エコシステムとも相互通信できます。

```
[ESP-NOWネットワーク]                  [Wi-Fi / Zenohネットワーク]
  Node A (pub/sub)                       zenohd Router (PC/SBC)
  Node B (pub/sub)  <── Gateway ──>      zenoh Client (PC)
  Node C (pub/sub)    ESP32-S3           zenoh-pico (Wi-Fi ESP32)
                      WIFI_AP_STA
```

### 主要な設計方針

| 項目 | 決定 | 理由 |
|---|---|---|
| トランスポートモード | multicast peer | ブロードキャスト1回で全台配信、ピアテーブル管理不要 |
| 送信方式 | ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`) | ノード数に依存しない O(1) 送信 |
| Discovery | zenoh-pico JOIN メッセージ | 定期ブロードキャスト JOIN → 自動ピア登録、ゼロコンフィグ |
| 信頼性 | ベストエフォート（ACKなし） | pub/sub はデータ欠損許容 |
| 暗号化 | 無効（Phase 1） | ブロードキャストと CCMP は共存不可 |
| チャンネル | 固定（Kconfig） | 全ノードが同一の 2.4 GHz チャンネルを使用する必要あり |

## 必要環境

### ハードウェア

- ESP32-S3（ESP32 / ESP32-C シリーズも可 — `CMakeLists.txt` の `IDF_TARGET` を変更）
- フラッシュ 8 MB 推奨

### ソフトウェア

- [ESP-IDF v5.5.x](https://github.com/espressif/esp-idf)（Xtensa ツールチェーン付き）
- Git（サブモジュール対応）

## クイックスタート

### 1. サブモジュールごとクローン

```bash
git clone --recurse-submodules <repo-url>
cd zenoh-espnow
```

### 2. zenoh-pico へパッチを適用

`network.c` にオーバーライドフックを追加し、`config.h` に `#ifndef` ガードを入れることで `zenoh_espnow` が UDP マルチキャスト PAL 関数を ESP-NOW に置き換えられるようにします。

```bash
scripts/apply_patches.sh
```

### 3. ESP-IDF 環境を読み込む

```bash
source $IDF_PATH/export.sh
```

### 4. サンプルをビルド・書き込み

```bash
cd examples/espnow_node
idf.py menuconfig   # ESPNOW_CHANNEL をネットワークに合わせて設定
idf.py build flash monitor
```

## リポジトリ構成

```
zenoh-espnow/
├── components/
│   ├── zenoh_pico_idf/          zenoh-pico の ESP-IDF コンポーネントラッパー
│   │   └── CMakeLists.txt
│   └── zenoh_espnow/            ESP-NOW カスタムトランスポート（本ライブラリ）
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── zenoh_espnow.h   公開 API（診断情報）
│       ├── src/
│       │   └── zenoh_espnow_link.c  PAL オーバーライド: open/close/read/write
│       └── zenoh_espnow_patch/
│           └── 0001-*.patch     zenoh-pico への最小限のパッチ
├── examples/
│   ├── espnow_broadcast/        Phase 1: 生 ESP-NOW ブロードキャスト動作確認
│   ├── zenoh_pubsub/            Phase 1: Wi-Fi UDP での zenoh-pico pub/sub 動作確認
│   ├── espnow_node/             Phase 2/3: ESP-NOW 経由 zenoh pub/sub（AP 不要）
│   └── gateway/                 Phase 4: ESP-NOW ↔ Wi-Fi/zenohd ブリッジ
├── third_party/
│   └── zenoh-pico/              サブモジュール — zenoh-pico v1.9.0
├── docs/
│   ├── spec.md                  詳細仕様書
│   └── Milestone.md             フェーズ計画と進捗
└── scripts/
    └── apply_patches.sh         zenoh-pico パッチ適用スクリプト
```

## サンプル一覧

### `espnow_broadcast`

生 ESP-NOW ブロードキャストの送受信サンプルです。zenoh を乗せる前にチャンネル設定やハードウェアを検証するために使います。

```bash
cd examples/espnow_broadcast && idf.py build flash monitor
```

### `zenoh_pubsub`

標準的な Wi-Fi UDP マルチキャスト上での zenoh-pico pub/sub サンプルです。トランスポートを置き換える前に zenoh-pico 単体の動作を確認します。

```bash
cd examples/zenoh_pubsub
idf.py menuconfig   # Wi-Fi SSID/パスワードと Zenoh モードを設定
idf.py build flash monitor
```

### `espnow_node`

ESP-NOW をトランスポートとして使う完全な zenoh ピアノードです。AP 接続不要。各ノードは：

- `sensor/<MAC>/data` を 2 秒ごとにパブリッシュ
- `sensor/**` をサブスクライブ

```bash
cd examples/espnow_node
idf.py menuconfig   # ESPNOW_CHANNEL を設定
idf.py build flash monitor
```

全ノードを**同じチャンネル**に設定してください。

### `gateway`

ESP-NOW ネットワークと zenohd Router を Wi-Fi/TCP で接続するブリッジです。

```
sub(session_a, "sensor/**") → pub(session_b)   ESP-NOW → zenohd
sub(session_b, "cmd/**")    → pub(session_a)   zenohd  → ESP-NOW
```

```bash
cd examples/gateway
idf.py menuconfig   # Wi-Fi SSID/パスワードと zenohd の IP を設定
idf.py build flash monitor
```

PC 側の準備：

```bash
zenohd -l tcp/0.0.0.0:7447   # Router 起動
z_sub --key 'sensor/**'       # ESP-NOW ノードのデータを受信
```

ゲートウェイ起動ログの以下の行でチャンネルを確認し、`espnow_node` の `ESPNOW_CHANNEL` と合わせてください：

```
I gateway: IP: 192.168.x.x  ESP-NOW ch=6
```

## コンポーネント API

```c
#include "zenoh_espnow.h"

uint8_t  zenoh_espnow_get_channel(void);     // 現在の Wi-Fi チャンネル
uint32_t zenoh_espnow_get_rx_dropped(void);  // RX キュー溢れ回数
uint32_t zenoh_espnow_get_tx_failed(void);   // TX 失敗回数
```

## トランスポートオーバーライドの仕組み

`zenoh_espnow` コンポーネントが `zenoh_pico_idf` に `ZENOH_ESPNOW_LINK_OVERRIDE=1` を注入します。このフラグで `network.c` の元の UDP マルチキャスト関数がコンパイルから除外され、`zenoh_espnow_link.c` が同じシグネチャの ESP-NOW 版を提供します。

```
zenoh-pico (peer mode, "udp/224.0.0.225:7447")
  └─ _z_f_link_*_udp_multicast()
       └─ [PAL 呼び出し — ZENOH_ESPNOW_LINK_OVERRIDE でリダイレクト]
            ├─ _z_send_udp_multicast()  →  esp_now_send(FF:FF:...:FF)
            ├─ _z_read_udp_multicast()  →  xQueueReceive(rx_queue)
            ├─ _z_open_udp_multicast()  →  esp_now_init()
            └─ _z_close_udp_multicast() →  esp_now_deinit()
```

TCP 関数（`_z_open_tcp` 等）は `network.c` の別ブロックにあり、オーバーライドの影響を受けません。これにより `session_a`（ESP-NOW）と `session_b`（TCP → zenohd）が同一のゲートウェイバイナリで共存できます。

## 既知の制約

| 制約 | 備考 |
|---|---|
| ペイロード ≤ 250 バイト | ESP-NOW v1.0 の制限；zenoh-pico のフラグメンテーションで大きなメッセージにも対応 |
| 起動時にチャンネル固定 | 全ノードが同一の 2.4 GHz チャンネルを使用する必要あり |
| 暗号化なし | ブロードキャストと CCMP は ESP-NOW で共存不可 |
| ベストエフォートのみ | ACK なし；確実な配信には zenoh Advanced Pub/Sub を検討 |
| ESP-NOW セッションは 1 つのみ | グローバルシングルトン；同一バイナリで 2 つの ESP-NOW セッションは不可 |

## ライセンス

本プロジェクトは Apache License 2.0 でライセンスされています。  
zenoh-pico は EPL-2.0 OR Apache-2.0 でライセンスされています。
