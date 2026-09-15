# atoms3-voice-control

iPhone の Safari で音声認識 (Web Speech API) した結果を、`wss://` で AtomS3 に送る実験用ファームウェア。

```
[iPhone Safari] ── https / wss ──> [AtomS3]  https://192-168-1-50.local-ip.sh/
  webkitSpeechRecognition             ページ配信 + WebSocket 受信 + LCD 表示 + Serial 出力
```

ブラウザの音声認識と WebSocket を HTTPS ページから使うには、AtomS3 側も信頼された証明書で HTTPS を話す必要がある。
ここでは [local-ip.sh](https://local-ip.sh/) を使う。

- `192-168-1-50.local-ip.sh` は `192.168.1.50` に解決される
- `*.local-ip.sh` の Let's Encrypt ワイルドカード証明書と秘密鍵が公開されている
- AtomS3 は起動時にそれをダウンロードして HTTPS サーバに載せるので、iPhone 側の設定は不要

## 使い方

```sh
cp include/secrets.example.h include/secrets.h   # WIFI_SSID / WIFI_PASSWORD を書く
pio run -t upload
pio device monitor
```

1. LCD に `WiFi` → `NTP` → `TLS cert` と進み、QR コードが出る
2. iPhone のカメラで QR を読むとページが開く。
   QR の中身は `http://<IP>/` で、証明書が有効なら `https://<IP をダッシュ区切り>.local-ip.sh/` にリダイレクトされる
3. 上の表で `Secure context: true` / `SpeechRecognition: available` / `WebSocket: open` と証明書の残り日数を確認
4. ボタンを押している間に話す。認識途中の文字は灰色、確定した文字は白で LCD に出る
5. 音声を使わず「文字を送る」欄から送ると、音声認識と通信のどちらが問題かを切り分けられる

AtomS3 のボタンで QR 表示と認識結果表示を切り替える。

### Serial 出力

認識途中・確定の両方が 1 行 1 JSON で出る。

```
{"text":"前に進んで","final":false}
{"text":"前に進んで","final":true}
```

ロボットの制御を足すなら `src/main.cpp` の `handleSpeech()` に書く。
これは httpd タスクから呼ばれるので、重い処理や描画は `loop()` 側に回すこと。

## 前提と制限

- **iPhone と AtomS3 が同じ LAN にいて、インターネットにも出られること。**
  `*.local-ip.sh` の名前解決は公開 DNS 経由、証明書のダウンロードと有効期限チェックの NTP もネットが要る。
- ゲスト Wi-Fi など端末間通信が隔離されたネットワークでは届かない。
- **DNS リバインディング保護**のあるルーター・学内 DNS は、プライベート IP を返す応答を捨てる。
  `http://<IP>/` は開けるのに `https://…local-ip.sh/` が開けない場合はこれを疑う。
- TLS セッションは RAM の都合で同時 2 本まで。同じページを複数タブで開かない。
- 秘密鍵は世界中に公開されているので、通信内容は保護されない。同じ LAN の誰でも `/ws` に接続できる。

## 証明書の期限

local-ip.sh の証明書は Let's Encrypt の 90 日証明書で、期限の 1 ヶ月ほど前に差し替わる。

- **自動更新**: AtomS3 は 1 日 1 回ダウンロードし直し、中身が変わっていれば HTTPS サーバを再起動して差し替える。
  再起動の瞬間に WebSocket は一度切れ、ページが自動で再接続する。失敗したら 10 分ごとに再試行。
- **起動時にダウンロードできなかったら**: LittleFS に保存済みの有効期限内の証明書で起動する。
  QR の下に `C` が付き、ページにも「保存済みのものを使っています」と出る。
- **残り日数の表示**:
  - LCD: QR の下に `192.168.1.50 43d` (残り 2 日を切ると `12h`)。緑 = 正常、橙 = 残り 14 日未満
  - ページ: 「証明書: あと43日 (2026-11-17 23:51まで)」。残り 14 日未満なら黄色で「自動更新できていません」と最後のエラー
  - 残り 14 日未満は、1 日 1 回の更新が 2 週間以上失敗し続けているという意味
- **期限が切れたら**: HTTPS サーバを止める。期限切れの証明書では Safari がページを表示しないので、
  説明は平文 HTTP 側に出す。QR (`http://<IP>/`) を読むと、リダイレクトの代わりに
  「証明書の期限が切れています」のページ (期限・最後のエラー・再試行間隔) が出る。
  このページは 30 秒ごとに再読み込みし、証明書が戻れば音声認識のページに切り替わる。LCD は `EXP` (赤)。
- 起動時にダウンロードもキャッシュも使えなかったときも同じページ (「有効な証明書がありません」、LCD は `NO`) を出し、
  10 分ごとに再試行する。
- 開いたままのページで接続が 3 回続けて失敗すると、`http://<IP>/` を開くよう案内が出る。

未確認: iOS Safari が `http://<IP>/` を勝手に HTTPS に書き換えると、説明ページが出ない。実機で確かめること。

## LCD のエラー

| 表示 | 原因 |
| --- | --- |
| `WiFi connect failed` | SSID / パスワード、2.4GHz かどうか |
| `NTP sync failed` | ネットに出られていない (証明書の期限確認に時刻が要る) |
| QR の下が `NO` / `EXP` (赤) | 証明書がない / 期限切れ。QR を読むと理由のページが出る。原因は Serial の `[tls]` 行にも出る |
| QR の下が橙 | 残り 14 日未満。自動更新が失敗し続けている |
| `HTTPS server failed to start` | ヒープ不足など。Serial のログを見る |

証明書の取得エラーの例:

- `GET https://local-ip.sh/... -> connection refused` など: local-ip.sh に届かない
- `downloaded key does not match certificate`: 証明書と鍵を取る間に差し替わった。次の再試行で直る

## iOS Safari の音声認識の癖

- `continuous = true` でも無音が続くとセッションが切れることがある ([WebKit Bug 288963](https://lists.webkit.org/pipermail/webkit-unassigned/2025-March/1212357.html))。
  ページ側で、ユーザーが聞き取りを続けたい間は `onend` で再開している。
- 操縦の非常停止は音声に頼らず、別の経路 (物理ボタン、コマンドが途切れたら止まるタイムアウト) を用意すること。

## 構成

| パス | 内容 |
| --- | --- |
| `src/main.cpp` | Wi-Fi / NTP / HTTPS + WebSocket サーバ / LCD 表示 |
| `src/cert_store.cpp` | local-ip.sh の証明書取得・検証 (パース、鍵の対応、期限)・キャッシュ |
| `src/web/index.html` | iPhone 用ページ (`board_build.embed_txtfiles` でファームに埋め込み) |
| `src/web/unavailable.html` | 証明書がない・期限切れのときに平文 HTTP で返す説明ページ |
| `certs/letsencrypt_roots.pem` | local-ip.sh からのダウンロードを検証する ISRG Root YR + ISRG Root X1 |
| `include/config.h` | URL、NTP サーバ、タイムアウトなどの定数 |

`platform = espressif32@6.10.0` (arduino-esp32 2.0.17 / mbedTLS 2.28) に固定している。
`cert_store.cpp` が mbedTLS 2.x の API を使っているため。

## Sources

- local-ip.sh (名前の形式、証明書と鍵の公開、更新時期): https://local-ip.sh/
- PsychicHttp (HTTPS + WebSocket、SSL の同時接続数の制約): https://github.com/hoeken/PsychicHttp
- Let's Encrypt のルート証明書: https://letsencrypt.org/certificates/
- ESP-IDF HTTPS server: https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/protocols/esp_https_server.html
- WebKit Bug 288963 (iOS Safari の continuous の挙動): https://lists.webkit.org/pipermail/webkit-unassigned/2025-March/1212357.html
