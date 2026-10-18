# BLE プロトコル v1

ファームウェア（`firmware/src/ble.cpp`, `commands.cpp`）とアプリの共通仕様。
同じ JSON コマンドが USB シリアル（1 行 1 コマンド）でも使える。

## GATT

Service `6e4c0001-7b3a-4f2e-9a61-4c45442d4453`

| Characteristic | UUID | プロパティ | 用途 |
|---|---|---|---|
| Control | `6e4c0002-…` | Write（暗号化＋認証） | JSON コマンド（最大 512B） |
| Notify | `6e4c0003-…` | Notify | 応答とイベント（分割あり） |
| Bulk | `6e4c0004-…` | Write Without Response（暗号化＋認証） | ファイル転送のチャンク |

- どのキャラクタリスティックも認証済みのリンクが必要。初回に書き込むと OS のペアリングが始まり、**パネルに表示される 6 桁の PIN** を入力する（ボンディングされるので次回からは不要）
- MTU は 247 を要求する

### Notify の分割
各通知の 1 バイト目がフラグ：`0x01` = 続きがある、`0x00` = 最後。2 バイト目以降の UTF-8 をつなげると 1 つの JSON になる。

## コマンド

リクエスト `{"cmd": "...", "id": 123, ...}` → 応答 `{"id": 123, "ok": true, ...}`。失敗時は `{"ok": false, "error": "..."}`。

| cmd | 引数 | 応答 |
|---|---|---|
| `info` | | `info`: fw, proto, name, width, height, fsTotal, fsUsed, rtc, sound, time, effects, presets[] |
| `status` | | `status`: preset, demo, item, items, paused, standby, brightness, speed, mA, powerScale |
| `get_config` / `set_config` | `config`（部分更新可） | `reboot`: 再起動が必要だったか（必要なら自動で再起動する） |
| `brightness` | `value` 0-255, `save`（既定 true） | |
| `speed` | `value` 10-400(%), `save` | |
| `play` / `pause` / `next` / `prev` | | |
| `standby` | `on` | |
| `preset` | `index` 0-7 | |
| `get_preset` / `set_preset` / `delete_preset` | `index`, `preset` | |
| `reload` | `index` | ファイル転送で更新したプリセットを読み直す |
| `list_files` | `dir`（既定 `/assets`） | `files`: [{name, size}] |
| `delete_file` | `path` | |
| `file_begin` | `path`, `size`, `crc`（CRC-32/zlib） | |
| `file_data` | `offset`, `data`(base64) | シリアル用。BLE では Bulk を使う |
| `file_status` | | `active`, `expected` |
| `file_end` / `file_abort` | | `file_end` でサイズと CRC を検証し、確定する |
| `sync_time` | `epoch`(UTC 秒), `tzOffsetMin` | RTC にも書き込む |
| `sound` | `name`: tick/beep/chime/alarm（既定 chime） | 試し鳴らし。音量は設定の `volume` |
| `clear_bonds` / `reboot` | | |

### イベント（`{"evt": ...}`、id なし）
- `status`：本体ボタンで状態が変わった（`status` オブジェクト付き）
- `file_nack`：Bulk のオフセットが飛んだ。`expected` から送り直す

## ファイル転送（BLE）
1. `file_begin {path, size, crc}`。書ける場所は `/assets/*` と `/presets/*` だけ
2. Bulk に `[u32 LE offset][data]` を順に送る
3. 数チャンクごとに `file_status` で `expected` を確認する（フロー制御と抜けの検出）
4. `file_end` → `ok` で確定する（一時ファイルから rename するので、途中で電源が落ちても元のファイルは壊れない）

## データ形式

### LDB1 ビットマップ（`/assets/*.ldb`）
```
0  "LDB1"
4  u16 width, u16 height (LE)
8  u8 format: 0=RGB565(2B) 1=RGB565+A8(3B) 2=A8(1B)
9  u8[3] reserved
12 pixels（行優先、RGB565 は LE）
```
- テキストは format 1（色付き、アンチエイリアス）か 2（マスク。色はプリセットの `color` で指定し、`"rainbow"` で虹色アニメ）

### プリセット（`/presets/N.json`）
```json
{"name": "...", "transition": "fade|cut", "items": [ ... ]}
```
項目の共通フィールド：`type`、`duration`(ms、省略時は種別ごとの既定値または自然終了)、`bg`（`{"type":"color","color":"#000000"}` / `{"type":"effect","effect":"stars","params":{}}` / `{"type":"image","asset":"x.ldb"}`）

| type | フィールド |
|---|---|
| `text` | `asset` または `text`（ASCII の内蔵フォント、`scale`）、`mode`: scroll/fixed/blink、`dir`: left/right/up/down、`speed`(px/s)、`repeat`、`color`、`blinkMs`、`y` |
| `image` | `asset`（LDB1） |
| `gif` | `asset`（.gif、パネルサイズにリサイズ済み）、`loops` |
| `clock` | `format`: HM/HMS、`color`、`label`/`labelText`、`labelColor` |
| `countdown` | `target`(UTC 秒) / `seconds`（タイマー） / `mode: "stopwatch"`、`color`、`label`/`labelText`、`endSound`: none/beep/chime/alarm（0 になった瞬間に鳴らす）、`tickFrom`: 残りこの秒数から毎秒ピッと鳴らす（0 で無し）。タイマーは `duration` を省くと、終わって 5 秒後まで表示する |
| `effect` | `effect`: plasma/fire/matrix/life/stars/noise/rainbow、`params` |

エフェクトのパラメータ：plasma `palette`、fire `cooling` `sparking`、matrix `color`、life `stepMs`、stars `count`、noise `palette` `scale`、rainbow `density`。
パレット名：rainbow, heat, lava, cloud, ocean, forest, party。

### 設定（`set_config` の `config`）
`panel`: {w, h, cols, rows, startTop, startLeft, serpentine, rotation(0-3), flipH, flipV, driver, clkphase}、`brightness`、`brightLevels`[3]、`powerProfile`: battery/ac、`limitBatteryMa`、`limitAcMa`、`panelFullMa`、`name`、`speed`、`tzOffsetMin`、`volume`（0-100、0 で消音）
- 回転・反転のうち、キャンバスのサイズが変わらないもの（反転・180°）はその場で反映する。それ以外のパネル構成と名前の変更は再起動して反映する
