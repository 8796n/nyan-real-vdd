# 設計メモ

## 全体像

- 1アダプタ（root列挙のSWDデバイス）+ 動的モニター最大4枚。起動時はモニター0。
- 制御はデバイスインターフェース `{C9AC49E6-0024-4979-96C7-A3E4B911CFFC}` への
  IOCTL（`include/nyanvdd_protocol.h` が唯一の契約。consuming 側へは
  ヘッダをコピーして持つ — submodule にはしない）。
- IddCx の DeviceIoControl は内部キューへリダイレクトされるため、カスタム
  IOCTL は `EvtIddCxDeviceIoControl` で受ける（WDF の既定キューには来ない）。
- devnode は `SwDeviceCreate`（HWID `NyanVDD`）+ `SwDeviceSetLifetime
  (ParentPresent)` で恒久化。削除は lifetime を Handle に戻して close。

## IddCx バージョン戦略

- **ビルド**: 1.10 ヘッダ（`NyanIddCxMinor=10`）+ `IDDCX_MINIMUM_VERSION_REQUIRED=10`。
- **対応 OS: Windows 11 24H2 (26100) 以上**（2026-07-21 決定）。理由 =
  Windows 10 は 2025-10 で EOL、Win11 23H2 以前もコンシューマ向けは EOL 済みで、
  古いフロアは「未検証の負債」にしかならない。
- 施行点は**2箇所**: INF の decoration `10.0...26100`（バインドを制限）と
  `IDDCX_MINIMUM_VERSION_REQUIRED=10`（**フレームワークのロード時ゲート**）。
  ※24H2 未満を支えたくなったら**両方**下げる必要がある。decoration だけでは
  ドライバがロードできない。
- *1 系コールバックはフロア 1.10 では呼ばれないが、共通実装の薄いラッパー
  なので登録したまま残す（フロアを下げる時の保険 + 登録必須検証への安全策）。
- 実行時に `IddCxGetVersion` で判定して段階的に有効化:
  - 1.8+ (`0x1800`): `IDDCX_ADAPTER_FLAGS_PREFER_PRECISE_PRESENT_REGIONS`
    （キャプチャ側のダーティ精度向上）
  - 1.9+ (`0x1900`): `IddCxSetRealtimeGPUPriority`（スワップチェーンごと、
    SetDevice 直後に呼ぶ）
  - 1.10+ (`0x1A00`): *2 系 DDI（`IDDCX_MONITOR_MODE2` / `IDDCX_TARGET_MODE2`、
    `IDDCX_WIRE_BITS_PER_COMPONENT`）で bpc を報告
- 1.11（D3D12 / DisplayID / atomic I2C）は現状不要。必要になったら
  `NyanIddCxMinor` を上げるだけ（msbuild プロパティ）。

## ParsecVDD で困った4欠陥への対応表

| ParsecVDD の欠陥（2026-07 調査） | 本ドライバーの対応 |
|---|---|
| kill→即再起動でゴースト採用・index 衝突 | cookie が EDID シリアルに焼かれる。同一性は cookie のみ。plug は重複 cookie を `ERROR_ALREADY_EXISTS` で拒否 |
| 削除が実物を巻き添え | unplug は cookie 指定。コネクタ番号での削除 API を持たない |
| keepalive 途切れ→ドライバ自動撤去（メガネ挿抜のトポロジストールで5〜6秒消える） | 強制 keepalive なし。既定は明示 unplug まで存続。ウォッチドッグはオプトイン・下限10秒・全 IOCTL でリフレッシュ |
| IOCTL タイムアウト無し | 制御 IOCTL は同期完結（monitor arrival/departure は IddCx 呼び出しのみ）。クライアント側は通常のタイムアウト付き呼び出しで良い |

孤児掃除はクライアント起動時のリコンサイルが本線:
`LIST → 自分の管理表にない cookie を UNPLUG`。

## cookie → OS ディスプレイの逆引き（設計の売りの実装）

cookie 相関は**モニターの ContainerId に cookie を埋める**ことで成立している
（`{408B3FE4-8AC2-4E97-83D8-BE29xxxxxxxx}` の下位4バイト、リトルエンディアン）。
EDID を読み直す必要はなく、非管理者で引ける。手順とヘルパは
`include/nyanvdd_protocol.h` に公開契約として置き、**動く参照実装が
`nyanvddctl resolve`**（QueryDisplayConfig → TARGET_DEVICE_NAME →
CM_Get_DevNode_PropertyW(ContainerId) → SOURCE_DEVICE_NAME → GDI名）。

2026-07-22 実機確認（3枚同時）:
```
cookie 0xAAAA0001 -> \\.\DISPLAY257  1920x1200@60 at (3840,0)
cookie 0xBBBB0002 -> \\.\DISPLAY258  2560x1440@90 at (5760,0)
cookie 0xCCCC0003 -> \\.\DISPLAY259  1280x720@60  at (8320,0)
```

**注意**: PLUG の成功は monitor arrival の受理までで、OS のトポロジ適用は
非同期。plug 直後の列挙は空振りしうる（実測では即時でも引けたが保証はない）。
クライアントは WM_DISPLAYCHANGE か CM_Register_Notification を先に張るか、
タイムアウト付きでポーリングすること。ヘッダにも明記した。

## アダプタ初期化の失敗（恒久死の回避）

`IddCxAdapterInitAsync` は宣言したケイパビリティ次第で失敗する（実例:
IddCx 1.11 で `CAN_PROCESS_FP16`）。以前は失敗すると `m_AdapterReady` が
false のまま放置され、デバイスは「正常動作中」に見えるのに以後すべての PLUG が
NOT_READY を返す**恒久死**になっていた（FP16 デバッグで実際に踏んだ）。

現在は**オプション機能を段階的に落として再試行**する。実機ログ:
```
attempt 0: 0xC000000D (caps flags 0x60)   ← FP16込みで拒否
attempt 1: 0x00000000 (caps flags 0x20)   ← FP16を落として成功
Adapter came up with reduced capabilities (0x60 requested, 0x20 accepted)
```
成功した組み合わせに合わせて `CapFlags` も落とすので、status は実態を映す。
全滅した場合はデバイスを生かしたまま `AdapterState = FAILED` にする
（制御インターフェースを残してクライアントが理由を読めるようにするため。
デバイスを落とすと Device Manager に黄色ビックリが出るだけで理由が残らない）。

## HDR10 の現状（準備あり・既定 SDR・FP16 はオプトイン）

- a01+ / RayNeo Air 4 Pro など HDR10 パネル対応が動機。
- **2026-07-21 実機知見（IddCx 1.11 = 0x1B01, Z390）**:
  - `IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16` を宣言すると
    `IddCxAdapterInitAsync` が `STATUS_INVALID_PARAMETER` で拒否される。
    *2 コールバック一式 + `EvtIddCxAdapterQueryTargetInfo` +
    `EvtIddCxMonitorSetDefaultHdrMetaData` を登録しても不足
    （本命疑い = ガンマランプ / 3x4 色空間変換サポート）。
  - **bpc 報告は FP16 宣言と厳密整合が必要**: FP16 未宣言のまま
    `IDDCX_TARGET_MODE2` に 10bpc を報告すると `IddCxMonitorArrival` が
    `STATUS_INVALID_PARAMETER` で落ちる（発見に時間を要した罠）。
- 現状の既定: 常時 8bpc（SDR）。FP16 実験は
  `HKLM\SOFTWARE\nyan-real-vdd\EnableFp16 = 1`（DWORD）でオプトイン。
  汎用の切り分けノブとして `DisableAdapterFlags`（bitmask）もある。
- 次の一手: `EvtIddCxMonitorSetGammaRamp`（3x4 colorspace transform）実装 →
  FP16 既定有効化 → `--hdr` plug で「HDR を使用する」トグル確認。

## モード報告の不変条件（重要）

**モードを報告するコールバックは必ず `ResolveModeList()` を通すこと。**
OS はモニターモードとターゲットモードの**積**しか実現しない。片方だけに載った
モードは選択されず、しかも PLUG は成功を返すので「成功したのに別の解像度で
出る」という最悪の壊れ方をする（2026-07-22 に実機で確認: `plug 1920x1200@60`
→ 実際は 1920x1080、LIST は 1920x1200 と報告）。

同じ理由で **PLUG は表現できないモードを受理しない**。受理範囲は
`IsSupportedMode()` が単一の判定源で、EDID 1.4 が記述できる上限に一致する
（実効ピクセル ≤ 4095、ライン周波数 ≤ 510 kHz = レンジ記述子の 255+255、
ピクセルクロック ≤ 2550 MHz）。範囲は `include/nyanvdd_protocol.h` にも明記。

## 物理サイズとスケーリング（実測メモ）

**「96 DPI にすれば 100% になる」は Windows では成り立たない。**
Windows は物理サイズから視聴距離を推定し、大きいパネルをテレビとみなして
10フィートUI を当てる。24H2 実測（いずれも 96 DPI 相当の物理サイズ）:

| 高さ | 解像度 | 既定スケーリング |
|---|---|---|
| 29 cm | 1920x1080 | 100% |
| 38 cm | 2560x1440 / 3440x1440 | 100% |
| 48 cm | 3200x1800 | 250% |
| 57 cm | 3840x2160 | **300%** |

そのため `PhysicalSizeMm()` は 96 DPI を基準にしつつ **高さを 380 mm で頭打ち**
にする（`kMaxPanelHeightMm`）。結果、1080p/1200p/1440p は 100%、4K は 150%
（= 旧来の固定 600x340mm と同等）に落ち着く。**4K で 100% は EDID だけでは
到達不能**: 96 DPI にするとテレビ扱いになり 300% まで悪化する。
38〜48 cm の間は未探索なので、4K を 125% に持ち込める高さがあるかは不明。

## スワップチェーン処理

フレームは acquire → 即 release。本ドライバーのモニターは DWM に合成させ、
アプリ（Windows.Graphics.Capture）に拾わせるための存在で、ピクセル輸送は
しない。GPU コストは実質ゼロ。MMCSS "Distribution" + （1.9+）realtime GPU
priority で遅延源にならないようにする。

## 実機検証チェックリスト

2026-07-21 消化分（Z390 / Win11 IddCx 1.11 = 0x1B01）:

- [x] インストール（証明書信頼 + pnputil + devnode）→ `status` 応答
- [x] plug/unplug/list/status の一巡（1920x1080@120, cookie 0xC0FFEE01）
- [x] EDID 相関: PnP `DISPLAY\NYN3D0F\...`、WMI で mfr=NYN /
      name="nyan Wall" / serial=`NW-C0FFEE01` を確認
- [x] `rt-gpu-priority` 点灯（スワップチェーン割当後に
      `IddCxSetRealtimeGPUPriority` 成功）+ `precise-dirty` 点灯
- [x] unplug all で PnP からも即時消滅
- [x] 非管理者での制御チャネル open（plug/unplug/list/status の一巡は
      非昇格シェルから実行 = INF の SDDL が機能。昇格が要るのは
      ドライバ更新と devnode 作成/削除のみ → scripts/dev-update.ps1）

2026-07-22 消化分（多エージェント監査で発見した3件の修正を実機検証）:

- [x] テーブル外解像度が実現する（`plug 1920x1200@60` → 実機で 1920x1200）
- [x] 4K がテレビ扱いにならない（300% → 150%、アスペクト比も解像度に一致）
- [x] `rt-gpu-priority` が unplug で消える（2枚 plug 中は点灯、0枚で消灯）
- [x] 表現できないモードの拒否（4K@240 / 8K@60 が ERROR_INVALID_PARAMETER）
- [x] ユニットテスト 914 件（`scripts/build.ps1` が毎回実行）

- [x] cookie → OS ディスプレイ逆引き（`nyanvddctl resolve` が3枚とも解決）
- [x] アダプタ初期化失敗からの回復（FP16 拒否 → 機能を落として起動 → plug 成功）
- [x] PLUG 直後の resolve（実測では即時でも引けた。保証はないので契約に明記）

未消化:

- [ ] S3/S4 復帰で二重初期化しないこと（再入ガードは入れたが実機未確認）
- [ ] watchdog の発火と自動解除
- [ ] plug→即 kill→再起動→リコンサイルでゴーストが出ない
- [ ] メガネ挿抜のトポロジストール中にモニターが消えない（ParsecVDD 比較）
- [ ] 選択中モードが preferred (@120Hz) になっているかの目視
- [ ] `EnableFp16=1` + gamma ramp 実装後に「HDR を使用する」が出るか
- [ ] 2台目マシン（N100）が 24H2 以上か確認して導入
- [ ] スリープ/復帰でモニター構成が保持されるか
