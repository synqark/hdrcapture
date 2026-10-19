# hdrrec

**フレーム単位で正確なタイミングで録画する、Windows 向け 4K / 60fps / HDR10 ゲーム録画ツール**

[English README](README.md)

ボーダーレスフルスクリーンのゲームを 3840x2160・60fps 固定・HDR10（BT.2020 PQ, 10bit）で、
NVIDIA NVENC（HEVC または AV1）により録画します。音声はシステム全体、または特定アプリの音だけを録音できます。

OBS や NVIDIA App の録画で起きる **interval stutter**（滑らかな 60fps のはずが、周期的にフレームが
重複・欠落する現象。[OBS フォーラムのスレッド](https://obsproject.com/forum/threads/interval-stutter-jitter-duplicated-frames-bug-workarounds.179196/)）
を解消するために作りました。ゲームが安定して 60fps で動いている状態の 20 秒テスト録画では、
1200 フレーム中 1200 フレームがすべて異なるゲームフレームになり、重複も欠落もありません。

## 動作環境

- Windows 11（動作確認済み）。最近の Windows 10 でも動く可能性はありますが未確認です。
- NVENC 対応の NVIDIA GPU、ドライバ 570 以降。AV1 は GeForce RTX 40 シリーズ以降が必要です。
- 録画するモニターで Windows の HDR がオン
- ゲームはボーダーレスフルスクリーンまたはウィンドウモード

## 使い方（GUI）

リリースの zip を展開し、`hdrrec_gui.exe` を起動します（`hdrrec.exe` は同じフォルダに置いておきます）。

- **Alt+F9**（変更可）、ウィンドウのボタン、通知領域（トレイ）のメニューで録画を開始・停止します。
  トレイアイコンは待機中が灰色、録画中が赤です。
- 最小化ボタンでトレイに格納、× ボタンで終了します（録画中なら停止してファイルを確定してから終了）。
  `hdrrec_gui.exe /tray` でウィンドウを出さずに起動します。
- 設定項目: ホットキー、保存先、モニター、エンコーダ（HEVC / AV1）、プリセット（p1〜p4）、画質（CQ またはビットレート）、
  音声（システム全体 / 録音しない / 特定アプリのみ）とゲイン、GPU 優先度。exe と同じフォルダの `hdrrec_gui.ini` に保存されます。
- 録画ごとに `logs\<録画名>.log` と `logs\<録画名>_times.csv`（フレームごとのタイミング。「録画の確認」参照）も保存します。

> 録画するモニター上に表示したウィンドウは動画にも映ります（Desktop Duplication は合成後の画面を取り込むため）。
> トレイに格納するか、別のモニターに置いてください。

## 使い方（コマンドライン）

```
hdrrec.exe [-o out.mkv] [-t 秒] [-monitor N] [-fps 60] [-codec hevc|av1]
           [-cq 18 | -bitrate Mbps] [-preset p4] [-maxrate Mbps]
           [-maxcll 1000] [-maxfall 400] [-mdl 1000]
           [-audio 0|1] [-acodec aac|pcm] [-abitrate 256] [-again dB]
           [-audio-process 名前.exe|PID] [-gpuprio 0..3]
           [-timelog times.csv] [-list-monitors]
```

| オプション | 意味 |
|---|---|
| `-o` | 出力ファイル（既定 `rec_YYYYmmdd_HHMMSS.mkv`） |
| `-t` | 指定秒数で停止（既定: Enter / Ctrl+C / 標準入力の終了まで） |
| `-monitor` | モニター番号（`-list-monitors` で一覧） |
| `-codec` | `hevc`（Main10）または `av1` |
| `-cq` / `-bitrate` | 画質指定（HEVC 1〜51、AV1 1〜63、小さいほど高画質）または平均ビットレート（Mbps） |
| `-preset` | NVENC プリセット `p1`〜`p7`。**p5 以上はゲームで GPU に負荷がかかった状態では 4K60 に追いつかない** |
| `-mdl`, `-maxcll`, `-maxfall` | HDR10 静的メタデータ（nit。既定値は 1000nit のディスプレイ向け） |
| `-audio-process` | 指定アプリ（とその子プロセス）の音声だけを録音 |
| `-again` | 音声ゲイン（dB。最大レベルで頭打ち） |
| `-gpuprio` | 録画処理の GPU 優先度: 0 変更なし（既定）、1 やや高、2 高、3 最高（リアルタイム） |
| `-timelog` | フレームごとの Present 時刻・遅延・配置判定を CSV に出力 |

終了時に `unique captures`、`repeated frames (source did not update / capture missed)`、`missed desktop frames`、
`encoder queue overflow` を表示します。

## 録画の確認

```
python tools/analyze_timelog.py logs/rec_..._times.csv   # Present 間隔、取りこぼしの位置
python tools/analyze_frames.py  rec_....mkv               # デコードして重複フレームを数える（ffmpeg, numpy が必要）
```

`scripts/test_record.bat [オプション]` は 5 秒後から 20 秒間 `tests/` に録画し、集計を表示します。

## ビルド

MSYS2 の **MINGW64** 環境を使います（UCRT64 は不可。同梱の FFmpeg ライブラリが msvcrt 向けのため）。

```
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf tar
bash scripts/build_hdrrec.sh          # -> build/hdrrec.exe, build/hdrrec_gui.exe
```

`third_party/` のビルド済み最小構成 FFmpeg を使います。FFmpeg 自体を作り直す（バージョンや機能を変える）場合は
`scripts/build_ffmpeg.sh` を参照してください。`scripts/package_release.sh VERSION` でリリース用 zip を作成し、
対応する FFmpeg のソースアーカイブも取得します。

## 制限事項

- NVIDIA GPU 専用（NVENC）。
- マウスカーソルは録画されません。
- 録画中の解像度や HDR モードの変更には対応していません（録画が止まるか、HDR がオフの間は映像が止まります）。
- 録画中に既定の音声デバイスが変わると、システム全体の音声はそれ以降無音になります（特定アプリの音声は影響を受けません）。
- ゲーム自体が 60fps を割るような GPU 負荷の高い場面では、少数のフレームを取りこぼすことがあります
  （長時間のテスト録画で約 0.07〜0.08%）。`-gpuprio` で改善する場合があるので、ゲームごとに試してください。

## ライセンス

hdrrec は [zlib ライセンス](LICENSE) です。

実行ファイルには LGPL 2.1 以降の FFmpeg などのサードパーティ製コンポーネントが静的リンクされています。
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) と [LICENSES/](LICENSES/) を参照してください。
