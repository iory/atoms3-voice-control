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
2. iPhone のカメラで QR を読むとページが開く (IP を `http://` で直打ちしても HTTPS 側にリダイレクトされる)
3. 上の表で `Secure context: true` / `SpeechRecognition: available` / `WebSocket: open` を確認
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
- ダウンロードに失敗しても LittleFS に有効期限内の証明書キャッシュがあれば起動する。
  その場合は QR の下の IP の横に赤い `C` が出て、Serial にも `(from cache: download failed)` と出る。
  キャッシュも期限切れならエラーで止まる。

## LCD のエラー

| 表示 | 原因 |
| --- | --- |
| `WiFi connect failed` | SSID / パスワード、2.4GHz かどうか |
| `NTP sync failed` | ネットに出られていない (証明書の期限確認に時刻が要る) |
| `no certificate: GET https://local-ip.sh/... -> ...` | local-ip.sh に届かない、かつキャッシュもない |
| `... key does not match certificate` | ダウンロードした証明書と鍵の組が食い違っている (更新の瞬間など)。再起動して再取得 |
| `HTTPS server failed to start` | ヒープ不足など。Serial のログを見る |

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
