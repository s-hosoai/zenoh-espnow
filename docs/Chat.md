# 設計議論ログ：zenoh-esp-now

> 記録日: 2026-06-03  
> 目的: ESP-NOWの上にZenoh-picoを載せた，ルータ不要Wi-Fi P2P通信システムの設計検討

---

## 1. ESP-NOWプロトコルスタックの確認

**Q: ESP-NOWのプロトコルスタックを教えてください（espressif/esp-nowリポジトリ参照）**

### ESP-NOWとOSIモデルの関係

従来のWi-Fi（TCP/IP）スタックはOSI 7層をすべて使うのに対し、ESP-NOWはL3〜L7を単一層に集約している。これによりIPスタックを持たないコネクションレス通信が可能になり、ミリ秒オーダーの応答速度を実現している。

### フレームフォーマット（Vendor-specific Action Frame）

```
| MAC Header(24B) | Category(1B) | Org ID(3B) | Random(4B) | Vendor Specific Content | FCS(4B) |
```

Vendor Specific Element内部:
```
| Element ID(1B)=221 | Length(1B) | Org ID(3B)=0x18fe34 | Type(1B)=4 | Ver(1B) | Body(0-250B) |
```

### バージョン差異

| バージョン | 最大ペイロード | 備考 |
|---|---|---|
| v1.0 | 250 bytes | 標準 |
| v2.0 | 1470 bytes | v2.0デバイス同士のみ |

### セキュリティ

CCMPで暗号化（IEEE Std. 802.11-2012準拠）。PMK（16B）でLMK（16B）をAES-128暗号化し，LMKがVendor-specific Action Frameを暗号化する。

---

## 2. Zenoh-picoのアーキテクチャ確認

**Q: Zenoh-picoのレイヤも教えてください**

### Zenoh-picoの7層アーキテクチャ

| 層 | 名称 | 役割 |
|---|---|---|
| 7 | Application layer | ユーザーコード（pub/sub, query/reply） |
| 6 | Public API layer | C API, 所有権管理（z_owned_*, z_loaned_*） |
| 5 | Session management | セッション管理・サブスクリプション・ルーティング |
| 4 | Transport layer | フラグメンテーション・シーケンス番号・信頼性 |
| 3 | Protocol layer | メッセージシリアライズ/デシリアライズ（Zenoh wire format） |
| 2 | Platform abstraction (PAL) | OS非依存インターフェース（src/system/espidf/） |
| 1 | Link / transport backend | 実際の通信媒体（**カスタム実装ポイント**） |

### サポートトランスポート

TCP/IP，UDP/IP，QUIC，Serial（UART/USB），BLE，OpenThreadX，Unix socket，Shared memory

### パフォーマンス特性

- 最小オーバーヘッド：**5〜7 bytes / メッセージ**
- ユニキャストレイテンシ：〜45µs
- マルチキャストレイテンシ：〜15µs
- フットプリント：&lt;50KB（最小〜15KB）

### ESP-NOWとの統合イメージ

PAL層のLink backendにESP-NOWを差し込む。送信側は `esp_now_send()` ラッパー，受信側は `esp_now_register_recv_cb()` ラッパーを実装する。

---

## 3. Zenoh-pico P2Pユニキャストとピア発見

**Q: Zenoh-pico P2Pユニキャストについて。Peer同士が予めIPかMACを知っておく必要がありますか？事前登録なしでの通信を実現したいです。**

### 3モードの比較

| モード | Discovery方式 | IP/MAC事前知識 | 特徴 |
|---|---|---|---|
| Multicast peer（従来） | UDP multicast Scout（224.0.0.224:7446） | 不要 | 自動発見。ただしIP環境前提 |
| Unicast peer（2025年7月追加） | 固定endpoint指定（-l/-e） | **必要** | TCP P2P，ルータ不要，信頼性あり |
| Client mode | zenohd Router経由 | Routerアドレスのみ | ルータ必須，スケーラブル |

### ESP-NOW上でのゼロコンフィグ実現戦略

**問題**: Zenoh-picoのMulticast scoutingはUDP multicastを前提としており，ESP-NOWには直接使えない。

**解決策A: ブロードキャストScouting**
1. 起動時にZenoh ScoutメッセージをESP-NOWブロードキャスト（`FF:FF:FF:FF:FF:FF`）で送信
2. 近隣ESP32がScout受信 → Hello返答（src MACでPeerを認識）
3. `esp_now_add_peer(MAC)` で動的ペアリング登録
4. Zenoh Sessionを確立

**解決策B: Gossip Scouting**
- 1台だけ知っているPeerへ接続 → そのPeerが知っているPeer情報を転送
- 連鎖的にメッシュ化，新参加ノードも自動発見

**推奨: A＋B 組み合わせ（完全ゼロコンフィグ）**

```
起動時 → Scout BROADCAST → 近隣発見（src MAC認識）→ Session確立 → Gossip拡散
```

- **Scouting phaseのみBROADCAST**（`FF:FF:FF:FF:FF:FF`），Session確立後はユニキャスト
- Zenoh ZID（128-bit）= ESP32 MACアドレスベースで生成すると管理が簡潔

---

## 4. ピア数制限の詳細

**Q: ピア数制限（20台）は同時接続の制約ですか？20台接続後に1台が抜け，新しい1台が入ってきた場合は通信可能ですか？**

### 結論：「抜けて入る」は完全に可能

20という制限の正体は**「同時に送信先として登録できるピアテーブルのサイズ」**であり，「通信可能な総デバイス数」ではない。

### 送信と受信の非対称性

| 操作 | ピアテーブル登録 | 制限 |
|---|---|---|
| 送信（ユニキャスト） | **必須** | 同時20台まで |
| 受信（ブロードキャスト） | **不要** | **制限なし** |
| 受信（暗号化なしユニキャスト） | **不要** | **制限なし** |

### 動的管理

- `esp_now_add_peer()` / `esp_now_del_peer()` はランタイムで何度でも実行可能
- 削除したスロットに別MACを追加可能
- → 「20台満杯→1台del→新1台add」は正常動作する

### 上限を超える台数との通信戦略

1. **ブロードキャスト専用**: ピアテーブル不要，台数制限なし，ただし暗号化不可
2. **動的ピア管理（LRU）**: 送信時にadd，送信後にdel → 事実上無制限
3. **ライブラリ委譲**: QuickESPNow等がピア管理を透過化，20台制限を隠蔽

---

## 5. pub/subの全台配信とESP-NOWの対応

**Q: pub通信は特定KeyをSubしているデバイスすべてに送信する必要がありますが，この場合でも大丈夫ですか？**

### Zenoh unicast peer の送信動作

Zenoh-picoのunicast peer モードでは：**「接続中の全peerへ送信，key matchingは受信側で実施」**

- Publisher が `z_put("sensor/temp", data)` → 接続中の全peerへ送信
- 各受信peerが自分のsubscriptionとkey matchingを実施
- key一致 → callbackを呼ぶ，不一致 → 破棄

### ESP-NOWブロードキャストで解決

pub/subにESP-NOWブロードキャストを使えば：

- **送信コスト O(1)**: 台数に関係なく1回のbroadcastで全台に届く
- **ピアテーブル不消費**: 受信側はadd_peer不要
- **key matchingはZenoh層**: ESP-NOW側は宛先を気にしなくてよい

### 操作種別ごとの送信方式

| Zenoh操作 | ESP-NOW方式 | 理由 |
|---|---|---|
| Scout/Hello | BROADCAST | 全台発見，登録不要 |
| pub（data） | BROADCAST | O(1)，key matchingは受信側 |
| query | BROADCAST | queryable所在が事前不明 |
| reply（応答） | UNICAST | 返信先MACが明確 |

### Zenoh multicast transport として実装

- Zenoh **multicast peer** モード + ESP-NOW broadcast → Zenohが想定する動作と一致
- `Z_FEATURE_MULTICAST_DECLARATIONS=1` を有効にすると帯域削減・write filtering（Sub不在時に送信しない）が使える

### 決定事項

- Phase 1はACKなしのブロードキャスト，データ欠損許容
- pub/subはbroadcast，query/replyのみunicast

---

## 6. Wi-FiネットワークとのGateway中継

**Q: ESP-NOW/Zenoh-picoネットワーク内の1台が通常のWi-Fiネットワークにも属し，Zenohネットワークへ中継したい。中継PeerまたはRouterで実現できますか？**

### 根本的な制約：単一ラジオ問題

ESP32は2.4GHz Wi-Fiラジオが1つだけ。

- ESP-NOW と Wi-Fi STA は**同じチャンネルしか使えない**
- Wi-Fi AP（ルータ）に接続したらそのAPのチャンネルに固定，変更不可

### 解決策：WIFI_AP_STA モード

```
WIFI_AP_STA モード:
  STA: Wi-Fi AP（ルータ）へ接続 → APチャンネルに固定
  AP : （形式上）同チャンネルでESP-NOWノード向けに公開
```

ESP-NOWネットワーク全台がゲートウェイのAPチャンネルに合わせることで共存可能。

### Zenoh ゲートウェイ実装

#### 案1（zenohd Router）

ESP32上でzenohd（Rust）を動かす必要があり，**現実的でない**。

#### 案2（zenoh-picoデュアルセッション）← 採用

```c
// セッションA: ESP-NOWリンク（multicast peer）
z_owned_session_t session_a;

// セッションB: TCP/IPでzenohd Routerへ接続（client）
z_owned_session_t session_b;

// 双方向転送
sub(session_a, "**") → pub(session_b);  // ESP-NOW → Wi-Fi
sub(session_b, "**") → pub(session_a);  // Wi-Fi → ESP-NOW
```

### 全体ネットワーク構成

```
[ESP-NOWネットワーク]              [Wi-Fi/Zenohネットワーク]
  Node A (multicast peer)
  Node B (multicast peer)  ←→   zenohd Router (PC/SBC)
  Node C (multicast peer)        Zenoh Client (PC)
  Node D (multicast peer)        Zenoh-pico (Wi-Fi ESP32)
          ↑↓ ESP-NOW broadcast
  Gateway ESP32
  (WIFI_AP_STA)
  session_a ←→ session_b
          ↑↓ TCP/IP
  Wi-Fi AP（ルータ）
```

### ノード構成まとめ

| 役割 | ノード | モード | トランスポート |
|---|---|---|---|
| ESP-NOWノード群 | ESP32 × N | multicast peer | ESP-NOW broadcast |
| 中継ゲートウェイ | ESP32 × 1 | WIFI_AP_STA | ESP-NOW + TCP/IP |
| Zenoh Router | PC / SBC | router | TCP :7447 |
| Wi-Fi側クライアント | PC / ESP32 | client / peer | TCP/UDP |

---

## 7. 設計決定事項サマリー

| 項目 | 決定 | 根拠・議論 |
|---|---|---|
| Zenoh動作モード | multicast peer | pubをbroadcast 1回で全台配信，ピアテーブル不要 |
| pub送信方式 | ESP-NOWブロードキャスト | O(1)送信，台数制限なし，key matchingは受信側 |
| query/reply送信方式 | ESP-NOWユニキャスト | 返信先MACが明確な場合はunicastが適切 |
| Discovery | ブロードキャストScouting + Gossip | ゼロコンフィグ，事前登録不要 |
| 信頼性 | ベストエフォート（ACKなし） | Phase 1はデータ欠損許容 |
| 暗号化 | Phase 1は無効 | ブロードキャストと暗号化は両立しない |
| ピア管理 | 動的add/del | 同時登録数=20だが抜け入れは自由 |
| ゲートウェイ | zenoh-picoデュアルセッション | zenohd（Rust）はESP32で動作不可 |
| チャンネル | APチャンネルに全台統一 | ESP-NOW + Wi-Fi共存制約への対応 |
| ZID管理 | MACアドレスベース | アドレス管理の一元化 |

---

## 8. 成果物（生成ドキュメント）

| ファイル | 内容 |
|---|---|
| `Claude.md` | プロジェクト概要，アーキテクチャ，リポジトリ構成 |
| `spec.md` | 詳細仕様（プロトコル，インターフェース，設定パラメータ，制約） |
| `Milestone.md` | フェーズ別TODO・マイルストーン（Phase 1〜5） |
| `Chat.md` | このファイル（設計議論の経緯ログ） |

---

## 9. プロジェクト設定の決定

### プロジェクト名

検討した候補：`zenow`，`znow`，`piconow`，`znp`，`zerolink`，`meshoh` など。
**決定：`zenoh-esp-now`**
理由：Zenohエコシステムからの逸脱を避け，内容が一目でわかる名称。

### リポジトリ構成方針

**フォークではなく，自プロジェクトリポジトリ + git submodule を採用。**

- zenoh-picoは `components/zenoh-pico/` に git submodule として固定バージョンで参照
- ESP-NOW固有の変更は `components/zenoh_espnow/` に集約し，`.patch` ファイルで管理
- upstream（zenoh-pico本家）の更新を `git submodule update` + パッチ再適用で追従

```
zenoh-esp-now/
├── components/
│   ├── zenoh-pico/        ← git submodule（公式）
│   └── zenoh_espnow/      ← 自作コンポーネント
├── examples/
│   ├── node/
│   └── gateway/
└── scripts/apply_patches.sh
```

### ESP-IDFバージョン

**決定：ESP-IDF v5.5.x（v5.5.3を起点）**

| 候補 | 判断 | 理由 |
|---|---|---|
| v5.5.x | **採用** | 現行安定版，zenoh-picoの実績帯，2028年1月までサポート |
| v6.0.x | 見送り | Picolibc移行・破壊的変更あり，zenoh-picoのv6.0対応未確認 |

v6.0はzenoh-picoのCIが対応を確認したタイミングで移行を検討する。

---

## 参考リソース（議論中に参照したURL）

- [ESP-NOW IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
- [ESP-NOW User Guide (espressif/esp-now)](https://github.com/espressif/esp-now/blob/master/User_Guide.md)
- [zenoh-pico GitHub](https://github.com/eclipse-zenoh/zenoh-pico)
- [zenoh-pico DeepWiki（アーキテクチャ詳細）](https://deepwiki.com/eclipse-zenoh/zenoh-pico)
- [Zenoh-Pico P2P Improvements (2025-07)](https://zenoh.io/blog/2025-07-11-zenoh-pico-peer-to-peer-unicast/)
- [Zenoh over Serial（カスタムリンク参考）](https://zenoh.io/blog/2022-08-12-zenoh-serial/)
- [Zenoh Deployment Guide](https://zenoh.io/docs/getting-started/deployment/)
- [ESP-NOW with WiFi Coexistence](https://circuitlabs.net/esp-now-with-wifi-coexistence/)
- [ESP-NOW FAQ (Espressif)](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/esp-now.html)
- [ESP-IDF v5.5.3 Release](https://github.com/espressif/esp-idf/releases/tag/v5.5.3)
- [ESP-IDF v6.0 Breaking Changes](https://github.com/espressif/esp-idf/releases/tag/v6.0)
