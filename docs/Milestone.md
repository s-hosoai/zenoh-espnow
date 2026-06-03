# Milestone：zenoh-esp-now 実装計画

## フェーズ概要

```
Phase 1  ESP-NOW基礎確認                ✅ 完了 (2026-06-03)
Phase 2  Zenoh-pico カスタムリンク実装  ✅ 完了 (2026-06-03)
Phase 3  Discovery / ゼロコンフィグ     ✅ 完了 (2026-06-03)
Phase 4  ゲートウェイ（Wi-Fi中継）      ← 現在
Phase 5  堅牢化・最適化
```

---

## Phase 1：ESP-NOW 基礎確認と環境構築 ✅

**目標**: ESP-NOW単体でブロードキャスト・ユニキャスト通信を安定動作させる。
Zenoh-picoを乗せる前の土台を固める。

**完了条件**: 2台以上のESP32間でブロードキャスト送受信が安定動作し、チャンネル管理を把握している。

**完了日**: 2026-06-03

### TODO

- [x] ESP-IDF v5.5.4 開発環境セットアップ（toolchain, idf.py）
- [x] ESP-NOW ブロードキャスト送受信サンプル実装・動作確認
  - [x] `esp_now_init()`, `esp_now_register_recv_cb()`, `esp_now_send(FF:FF:FF:FF:FF:FF)` の基本動作
  - [x] 送信コールバック（ACKなし確認）
- [ ] ESP-NOW ユニキャスト送受信サンプル実装（Phase 2以降で必要になれば実施）
  - [ ] `esp_now_add_peer()` / `esp_now_del_peer()` の動的管理確認
  - [ ] 満杯（20台）→ del → add のスロット再利用確認
- [ ] チャンネル管理の確認（Phase 2以降で実施）
  - [ ] STAのみ（ch=1固定）vs WIFI_AP_STA（APチャンネル追従）の動作差異確認
  - [ ] `esp_wifi_get_channel()` でチャンネル取得・ログ確認
- [ ] ペイロード上限確認（250B境界でのパケット送受信）
- [x] 複数ノード（2台）でのブロードキャスト疎通確認

**備考**:
- ESP-IDF v5.5.x では `esp_now_send_cb_t` のシグネチャが変更（`const esp_now_send_info_t *tx_info`）
- `MACSTR`/`MAC2STR` は `esp_mac.h` の明示的インクルードが必要
- zenoh-pico（v1.9.0）の動作確認も同時実施（`examples/zenoh_pubsub`）
  - [x] Wi-Fi STA + zenoh-pico peer モード（UDP multicast）で pub/sub 疎通確認

---

## Phase 2：Zenoh-pico カスタムリンク実装 ✅

**目標**: Zenoh-picoのPAL層にESP-NOWを差し込み、Zenoh wire formatをESP-NOW上で運ぶ。

**完了条件**: 2台のESP32間でZenoh pub/subが動作し、`z_put()` → `z_declare_subscriber()` コールバックが呼ばれる。

**完了日**: 2026-06-03

### TODO

- [x] zenoh-pico を ESP-IDF コンポーネントとして追加（`components/zenoh_pico_idf/`）
- [x] ビルドフラグ設定
  - [x] `Z_FEATURE_MULTICAST_TRANSPORT=1`（config.h デフォルトで有効）
  - [x] `Z_BATCH_MULTICAST_SIZE=250`（ESP-NOW v1.0制約、zenoh_espnow が注入）
- [x] カスタムリンク実装（`components/zenoh_espnow/`）
  - [x] `_z_open_udp_multicast()`: ESP-NOW初期化、recv cb登録、broadcastピア追加
  - [x] `_z_close_udp_multicast()`: ESP-NOW deinit、FreeRTOSキュー削除
  - [x] `_z_send_udp_multicast()`: `esp_now_send(FF:FF:FF:FF:FF:FF, buf, len)`
  - [x] `_z_read_udp_multicast()`: FreeRTOSキュー経由でrecvコールバックから受信（タイムアウト対応）
- [x] zenoh-pico multicast peer セッション確立確認
- [x] `z_put()` / `z_declare_subscriber()` 疎通テスト（2台、`examples/espnow_node`）

**備考**:
- UDP multicast PAL関数をオーバーライドする方式を採用（zenoh-picoコアへの変更最小化）
- zenoh-picoへのパッチは2箇所のみ: `network.c` ガード + `config.h` の `#ifndef` 化
- 実装ソースを `zenoh_pico_idf` に注入することで静的ライブラリのリンク順問題を回避
- `ZENOH_ESPNOW_LINK_OVERRIDE` フラグで元のUDP multicast実装を条件コンパイルで無効化
- AP不要（Wi-Fi STA起動のみ）、チャンネルはKconfigで設定

---

## Phase 3：Discovery / ゼロコンフィグ（Scouting）✅

**目標**: 事前にIPもMACも設定せず、電源を入れるだけで近隣ノードを発見・接続できるようにする。

**完了条件**: 3台以上のESP32を同時起動し、設定なしで全台のpub/subが相互に動作する。

**完了日**: 2026-06-03

### TODO

- [x] Zenoh Scouting メッセージのESP-NOWブロードキャスト対応確認
  - [x] multicast transport の JOIN パケットが ESP-NOW broadcast で送信されることを確認
  - [x] 受信側が JOIN を受けてピアを登録しセッションを確立することを確認
- [x] 動的ピア発見の実装
  - [x] `_z_read_udp_multicast` の `addr` パラメータ経由で送信元MACをzenoh-picoへ渡す
  - [x] zenoh-picoが ZID + addr からピアを自動登録（`ztm->_peers`）
- [x] ノード追加・離脱テスト
  - [x] 実行中に新ノードを追加 → 既存ノードが自動発見することを確認
- [x] 3台以上でのpub/sub疎通テスト（ゼロコンフィグ）

**備考**:
- Scout/Hello ではなく zenoh-pico multicast peer モードの **JOIN メッセージ**が Discovery を担う
- JOIN は `_zp_multicast_send_join_task_fn` により `Z_JOIN_INTERVAL` ごとに定期送信
- 既存の ESP-NOW ブロードキャスト実装がそのまま動作するため追加実装なし
- `Z_FEATURE_MULTICAST_DECLARATIONS` は現在 0（デフォルト）、Phase 5 で必要に応じて検討

---

## Phase 4：ゲートウェイ実装（Wi-Fi/Zenohネットワーク中継）

**目標**: ゲートウェイESP32がESP-NOWネットワークとWi-Fi/Zenohネットワークを双方向に中継する。

**完了条件**: ESP-NOWノードのpubが、Wi-Fi LAN上のZenoh clientのsubに届く。逆方向も動作する。

### TODO

- [ ] ゲートウェイ用Wi-Fi設定
  - [ ] `WIFI_AP_STA` モード設定
  - [ ] STA: Wi-Fi APへの接続
  - [ ] チャンネル確認・ESP-NOWノードへのチャンネル周知方法決定（ZenohメタデータかOOB）
- [ ] zenoh-picoデュアルセッション実装
  - [ ] `session_a`: ESP-NOWリンク（multicast peer）起動
  - [ ] `session_b`: TCP unicast でzenohd Routerへ接続（client モード）
  - [ ] 2セッションの独立した送受信スレッド管理（FreeRTOSタスク）
- [ ] 双方向転送ロジック実装
  - [ ] `sub(session_a, "**") → pub(session_b)`: ESP-NOW → Wi-Fi 転送
  - [ ] `sub(session_b, "**") → pub(session_a)`: Wi-Fi → ESP-NOW 転送
  - [ ] ループ防止（転送元セッションへの再送信を防ぐフラグ管理）
- [ ] Wi-Fi側 zenohd Router の準備（PC/SBC上）
  - [ ] `zenohd -l tcp/0.0.0.0:7447` 起動確認
  - [ ] Zenoh clientからの疎通確認
- [ ] 統合テスト
  - [ ] ESP-NOWノード pub → Wi-Fi client sub の疎通
  - [ ] Wi-Fi client pub → ESP-NOWノード sub の疎通
  - [ ] ゲートウェイ再起動後の自動再接続確認

---

## Phase 5：堅牢化・最適化

**目標**: 実用レベルの安定性・性能を確保する。

**完了条件**: 長時間（24時間以上）連続動作で通信断なし。スループット・レイテンシが要件を満たす。

### TODO

- [ ] エラーハンドリング強化
  - [ ] ESP-NOW送信失敗時のリトライロジック（必要な箇所のみ）
  - [ ] Wi-Fi切断時のzenohd再接続（自動再接続、Zenoh-pico v1.3.3以降でサポート済み）
  - [ ] FreeRTOSタスクウォッチドッグ設定
- [ ] パフォーマンス測定
  - [ ] pub → sub エンドツーエンドレイテンシ計測
  - [ ] ブロードキャスト条件下のスループット計測
  - [ ] メモリ使用量プロファイリング（heaptrack相当）
- [ ] ペイロードサイズ最適化
  - [ ] Zenoh wire format のオーバーヘッド計測（実測5〜7B確認）
  - [ ] v2.0（1470B）ペイロード対応の検討
- [ ] セキュリティ（暗号化対応、オプション）
  - [ ] PMK共有方式の設計（OOB provisioning）
  - [ ] ブロードキャストからユニキャスト暗号化への移行設計
  - [ ] `CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM` 設定
- [ ] スケールテスト
  - [ ] 10台同時接続での安定動作確認
  - [ ] ノード頻繁追加・離脱（チャーン）テスト
- [ ] ドキュメント整備
  - [ ] API リファレンス
  - [ ] 使い方ガイド（Getting Started）
  - [ ] アーキテクチャ解説（architecture.md）

---

## マイルストーン一覧

| マイルストーン | フェーズ | 完了条件 |
|---|---|---|
| M1: ESP-NOW基礎確認完了 | Phase 1 | 3台でブロードキャスト疎通 |
| M2: Zenoh on ESP-NOW（2台） | Phase 2 | z_put → subscriber callback 動作 |
| M3: ゼロコンフィグ発見 | Phase 3 | 設定なし3台起動で全台pub/sub成功 |
| M4: ゲートウェイ中継動作 | Phase 4 | ESP-NOW → Wi-Fi Zenoh client へデータ到達 |
| M5: 本番品質 | Phase 5 | 24時間連続動作・スループット要件達成 |

## 依存関係

```
M1 ──► M2 ──► M3 ──► M4 ──► M5
                 └──────────────► (M4と並行でセキュリティ設計可)
```

## 参考リソース

- [ESP-NOW IDF ドキュメント](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
- [ESP-IDF v5.5.x リリース](https://github.com/espressif/esp-idf/releases/tag/v5.5.3)（採用バージョン）
- [zenoh-pico GitHub](https://github.com/eclipse-zenoh/zenoh-pico)
- [zenoh-pico P2P改善ブログ (2025-07)](https://zenoh.io/blog/2025-07-11-zenoh-pico-peer-to-peer-unicast/)
- [Zenoh over Serial（カスタムリンク参考実装）](https://zenoh.io/blog/2022-08-12-zenoh-serial/)
- [Zenoh Deployment ガイド](https://zenoh.io/docs/getting-started/deployment/)
- [ESP-NOW with WiFi Coexistence](https://circuitlabs.net/esp-now-with-wifi-coexistence/)
