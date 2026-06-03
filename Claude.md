# zenoh-esp-now

## プロジェクト概要

ESP-NOW上にZenoh-picoのトランスポート層を実装し、ルータ不要のWi-Fi P2P分散通信ネットワークを構築するプロジェクト。
最終的にはESP-NOWネットワークとWi-Fi/Zenohネットワークを中継ゲートウェイで接続し、標準的なZenohエコシステムと相互通信できる複合ネットワークを実現する。

## 技術スタック

| レイヤ | 技術 |
|---|---|
| アプリケーション | Zenoh pub/sub, query/reply |
| ミドルウェア | zenoh-pico (C, ESP-IDF) |
| カスタムトランスポート | ESP-NOW (Espressif) |
| 物理/MAC | 802.11 Vendor-specific Action Frame, 2.4GHz |
| MCU | ESP32 (ESP-IDF v5.x) |

## ターゲット環境

- **MCU**: ESP32シリーズ（ESP32, ESP32-S, ESP32-C）
- **フレームワーク**: ESP-IDF v5.5.x + FreeRTOS
- **zenoh-pico**: v1.x（最新安定版、git submodule）
- **ネットワーク規模**: 同時接続 〜20ノード（ESP-NOWピアテーブル上限内）

## アーキテクチャ概要

```
[ESP-NOWネットワーク]          [Wi-Fi/Zenohネットワーク]
  Node A (pub/sub)              zenohd Router (PC/SBC)
  Node B (pub/sub)  <──────>    Zenoh Client (PC)
  Node C (pub/sub)  Gateway     Zenoh-pico (Wi-Fi ESP32)
  Node D (pub/sub)  ESP32
                    WIFI_AP_STA
```

### ESP-NOWネットワーク側
- Zenoh-picoを **multicast peer モード** で動作
- トランスポートバックエンドとして **ESP-NOWブロードキャスト** を使用
- Scout/Discovery もブロードキャストで実施（事前ペア登録不要）
- key matchingは受信側Zenoh-picoが実施

### Wi-Fi/Zenohネットワーク側
- 標準的なTCP/UDP上のZenoh（zenoh-pico またはzenoh-rust）
- zenohd Routerを介してクライアントが接続

### ゲートウェイノード
- `WIFI_AP_STA` モードでESP-NOWとWi-Fiを同時動作
- zenoh-picoを2セッションで起動
  - `session_A`: ESP-NOWリンク（multicast peer）
  - `session_B`: Wi-FiリンクでzenohD Routerへ接続（client）
- アプリ層で双方向転送: `sub(A)→pub(B)`, `sub(B)→pub(A)`

## 設計上の主要な決定事項

| 項目 | 決定 | 理由 |
|---|---|---|
| トランスポートモード | multicast peer | pub/subをbroadcast 1回で全台配信、ピアテーブル不要 |
| pub送信方式 | ESP-NOWブロードキャスト | O(1)送信、台数制限なし |
| reply/query送信方式 | ESP-NOWユニキャスト | 返信先MACが明確 |
| Discovery方式 | ブロードキャストScouting | 事前登録なし・ゼロコンフィグ |
| 信頼性 | ベストエフォート（ACKなし） | pub/subはデータ欠損許容 |
| 暗号化 | Phase 1は無効 | ブロードキャスト送信と両立しないため |
| チャンネル | AP固定チャンネルに全台統一 | ESP-NOW + Wi-Fi共存制約 |

## リポジトリ構成（予定）

```
zenoh-esp-now/
├── Claude.md               # このファイル（プロジェクト概要）
├── spec.md                 # 詳細仕様
├── Milestone.md            # フェーズ・TODO管理
├── Chat.md                 # 設計議論ログ
├── components/
│   ├── zenoh-pico/         # git submodule（公式リポジトリ固定バージョン）
│   └── zenoh_espnow/       # カスタムトランスポートコンポーネント
│       ├── include/
│       │   └── zenoh_espnow.h
│       ├── zenoh_espnow_link.c     # PAL実装（send/recv）
│       ├── zenoh_espnow_scout.c    # Scouting実装
│       ├── zenoh_espnow_patch/     # zenoh-picoへのパッチ群
│       │   ├── 0001-add-espnow-feature-flag.patch
│       │   └── 0002-add-espnow-link-files.patch
│       └── CMakeLists.txt
├── examples/
│   ├── node/               # 通常ノード（pub/sub）
│   └── gateway/            # ゲートウェイノード（WIFI_AP_STA）
├── scripts/
│   └── apply_patches.sh    # submoduleにパッチを当てるスクリプト
└── docs/
    └── architecture.md
```
