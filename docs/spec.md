# 仕様書：zenoh-esp-now

## 1. プロトコルスタック

### 1.1 ESP-NOWレイヤ

ESP-NOWはIEEE 802.11の **Vendor-specific Action Frame** を用いたコネクションレス通信プロトコル。
OSIモデルのL3〜L7を単一レイヤに集約しており、IPスタックを持たない。

#### フレーム構造

```
| MAC Header | Category(1B) | Org ID(3B) | Random(4B) | Vendor Specific Content | FCS(4B) |
                                                          └─ Element: ID(1B) + Len(1B) + Org(3B)
                                                                     + Type(1B=4) + Ver(1B) + Body(0-250B)
```

#### 主要パラメータ

| 項目 | 値 |
|---|---|
| 最大ペイロード | 250 bytes (v1.0) / 1470 bytes (v2.0) |
| デフォルトビットレート | 1 Mbps |
| 暗号化方式 | CCMP / AES-128 (PMK 16B + LMK 16B) |
| ピアテーブル上限 | 20エントリ（暗号化ありは最大17、デフォルト7） |
| チャンネル | Wi-Fi STA接続中はAPチャンネルに固定 |

#### ピア管理の性質

- **送信**: `esp_now_add_peer()` で事前登録必須（上限20）
- **受信**: 登録不要。ブロードキャスト・暗号化なしユニキャストは任意デバイスから受信可能
- **動的管理**: `esp_now_add_peer()` / `esp_now_del_peer()` はランタイムで何度でも呼び出し可能
- → 「N台が抜け入れ替わる」シナリオは完全に対応可能

---

### 1.2 Zenoh-picoレイヤ

#### 7層アーキテクチャ

| 層 | 名称 | 主要ファイル |
|---|---|---|
| 7 | Application layer | ユーザーコード |
| 6 | Public API layer | `src/api/api.c` |
| 5 | Session management | `src/net/session.c` |
| 4 | Transport layer | `src/transport/` |
| 3 | Protocol layer | `src/protocol/codec/` |
| 2 | Platform abstraction (PAL) | `src/system/espidf/` |
| 1 | Link / transport backend | **カスタム実装ポイント** |

#### 動作モード

| モード | 説明 | 本プロジェクトでの使用 |
|---|---|---|
| multicast peer | UDPマルチキャストでScouting・通信 | ESP-NOWネットワーク側（ブロードキャスト代替） |
| unicast peer | TCP P2P、ルータなし、2025年7月追加 | 将来のreply/query強化に検討 |
| client | zenohd Router経由 | ゲートウェイのWi-Fi側セッション |

#### Zenoh wire format オーバーヘッド

- 最小オーバーヘッド: **5〜7 bytes / メッセージ**
- フットプリント: &lt;50KB（最小 〜15KB）
- ユニキャスト遅延: 〜45µs、マルチキャスト: 〜15µs

---

### 1.3 カスタムトランスポート（実装コア）

Zenoh-picoのPAL層（Link backend）にESP-NOWを差し込む。

#### 送受信の対応関係

| Zenoh操作 | ESP-NOW送信方式 | 理由 |
|---|---|---|
| Scout / Hello | ブロードキャスト (`FF:FF:FF:FF:FF:FF`) | ピア登録不要、全台発見 |
| pub (data) | ブロードキャスト | O(1)送信、key matchingは受信側 |
| query | ブロードキャスト | queryable所在が事前不明 |
| reply (query応答) | ユニキャスト (MAC指定) | 返信先MACが既知 |

#### Discovery フロー（ゼロコンフィグ）

```
1. 起動時: Zenoh Scout メッセージをESP-NOWブロードキャストで送信
2. 近隣ESP32が受信 → Hello返答（src MACでピアを認識）
3. 受信コールバック内でZenoh Session相手として登録
4. Gossip scouting: 既知ピア情報を新参加ノードへ伝搬
```

#### ZIDとMACの対応

- Zenoh ZID (128-bit) の下位48bitにESP32 MACアドレスを格納することで管理を一元化

---

## 2. ネットワーク構成仕様

### 2.1 ESP-NOWネットワーク（Southリージョン）

- 全ノードが同一Wi-Fiチャンネルを使用（ゲートウェイAPチャンネルに合わせる）
- Zenoh multicast peer モード
- pub/sub通信はESP-NOWブロードキャスト
- 信頼性: ベストエフォート（ACKなし）、データ欠損許容
- 同時接続ノード数: 理論上無制限（ブロードキャスト受信はピアテーブル不要）
- 送信元として管理するアクティブピア数: 最大20（ピアテーブル制約）

### 2.2 Wi-Fi/Zenohネットワーク（Northリージョン）

- 標準的なTCP/UDP上のZenoh
- zenohd Router（PC/SBC）を中心としたstar/meshトポロジー
- Zenoh-pico / Zenoh-rust クライアントが接続

### 2.3 ゲートウェイノード仕様

#### ハードウェア要件

- ESP32（Wi-Fi + ESP-NOW両対応）
- RAM: 推奨520KB以上（ESP32標準で十分）

#### Wi-Fiモード

```
WIFI_AP_STA:
  STA: Wi-Fi APへ接続（zenohd Routerと同LAN）
  AP : ESP-NOWノード向け（実質チャンネル固定のため形式的）
```

#### zenoh-picoデュアルセッション

```c
// セッションA: ESP-NOWリンク（multicast peer）
z_owned_session_t session_a;  // ESP-NOW transport backend
z_owned_session_t session_b;  // TCP/IP transport (Wi-Fi → zenohd)

// 転送ロジック
// ESP-NOW → Wi-Fi
z_declare_subscriber(session_a, keyexpr_all, fwd_to_b, session_b);
// Wi-Fi → ESP-NOW
z_declare_subscriber(session_b, keyexpr_all, fwd_to_a, session_a);
```

#### チャンネル同期

- ゲートウェイ起動時: Wi-Fi STA接続後にチャンネルを取得
- ESP-NOWはそのチャンネルで自動動作（APSTA時は自動的に揃う）
- ESP-NOWノード側: ゲートウェイAPに合わせてチャンネル設定、またはpeer設定でch=0（自動）

---

## 3. インターフェース仕様

### 3.1 PAL実装インターフェース（カスタムトランスポート）

```c
// zenoh-pico PAL に実装する関数群
int8_t _z_espnow_open(void *arg);
int8_t _z_espnow_close(void *arg);
size_t _z_espnow_read(uint8_t *ptr, size_t len, void *arg);
size_t _z_espnow_write(const uint8_t *ptr, size_t len, void *arg);
int8_t _z_espnow_listen(void *arg);     // Scouting用ブロードキャスト受信

// 内部: Discovery
void _z_espnow_recv_cb(const uint8_t *mac, const uint8_t *data, int len);
// ESP-NOWコールバック → zenoh-picoの受信バッファへ転送
```

### 3.2 設定パラメータ

| パラメータ | デフォルト | 説明 |
|---|---|---|
| `ZENOH_ESPNOW_CHANNEL` | 0 (auto) | ESP-NOWチャンネル（0=APチャンネルに追従） |
| `ZENOH_ESPNOW_ENCRYPT` | false | 暗号化（Phase 1は無効） |
| `ZENOH_ESPNOW_MAX_ACTIVE_PEERS` | 20 | ピアテーブル上限 |
| `ZENOH_ESPNOW_SCOUT_INTERVAL_MS` | 1000 | Scouting送信間隔 |
| `ZENOH_ESPNOW_LEASE_MS` | 10000 | セッションkeep-aliveリース |
| `Z_FRAG_MAX_SIZE` | 1024 | フラグメンテーションバッファ（250B制約に合わせて設定） |
| `BATCH_MULTICAST_SIZE` | 250 | ESP-NOW v1.0の最大ペイロードに合わせる |

### 3.3 Zenoh key expression 規約（推奨）

```
sensor/<node_id>/<type>    # センサーデータ
cmd/<node_id>/<action>     # コマンド
status/<node_id>           # ノード状態
```

---

## 4. 制約・既知の限界

| 制約 | 内容 | 対策 |
|---|---|---|
| ペイロード上限 | ESP-NOW v1.0: 250B、v2.0: 1470B | Zenoh-picoのフラグメンテーション機能を活用 |
| チャンネル固定 | Wi-Fi接続後はAPチャンネルに固定 | WIFI_AP_STA + 全ノードをAPチャンネルに統一 |
| ブロードキャスト無暗号化 | Phase 1はデータ無暗号化 | Phase 2でPMK共有 + ユニキャスト暗号化へ移行 |
| 同時アクティブピア | ユニキャスト送信は最大20台 | ブロードキャスト中心設計で実質的に回避 |
| ACKなし | pub/subはベストエフォート | 欠損許容。確実性が必要ならZenoh Advanced Pub/Subを検討 |
| zenohd不可 | ESP32でRust zenohd は動作不可 | ゲートウェイはzenoh-picoデュアルセッションで対応 |
| 電波干渉 | 2.4GHz帯のみ | チャンネル選定で一般Wi-Fiと干渉を最小化 |
