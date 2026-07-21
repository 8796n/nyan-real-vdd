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

- **ビルド**: 1.10 ヘッダ（`NyanIddCxMinor=10`）+ `IDDCX_MINIMUM_VERSION_REQUIRED=5`。
- **実行時フロア**: 1.5 = Windows 10 2004 (19041)。Spatial Wall 本体の下限に一致。
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

未消化:

- [ ] watchdog の発火と自動解除
- [ ] 非管理者での制御チャネル open（今回の一巡は昇格済みシェル併用）
- [ ] plug→即 kill→再起動→リコンサイルでゴーストが出ない
- [ ] メガネ挿抜のトポロジストール中にモニターが消えない（ParsecVDD 比較）
- [ ] 選択中モードが preferred (@120Hz) になっているかの目視
- [ ] `EnableFp16=1` + gamma ramp 実装後に「HDR を使用する」が出るか
- [ ] Win10 19041 実機（または VM）で 1.5 フロア動作
- [ ] スリープ/復帰でモニター構成が保持されるか
