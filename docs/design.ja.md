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
  - ~~1.8+: `IDDCX_ADAPTER_FLAGS_PREFER_PRECISE_PRESENT_REGIONS`~~ —
    2026-07-23 実測で WGC のダーティ精度に効果なしと判明し**要求を廃止**
    （「差分キャプチャの実測」参照。`NYANVDD_CAP_PRECISE_DIRTY` は以後
    点灯しない）
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
| keepalive 途切れ→ドライバ自動撤去（メガネ挿抜のトポロジストールで5〜6秒消える） | 強制 keepalive なし。既定は明示 unplug まで存続。ウォッチドッグはオプトイン・下限10秒・全 IOCTL でリフレッシュ。時計は unbiased（スリープ中は進まず、復帰時 D0Entry でフルリセット）— GetTickCount64 のままだと armed でスリープ→復帰の瞬間に全滅する |
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

## リモートデスクトップ中は仮想ディスプレイが「見えない」

**症状**: RDP 接続中に plug すると、**PLUG は成功を返し LIST にも出るのに
ディスプレイが現れない**。以前は理由が一切分からなかった。

**機序**（2026-07-22 実機で確認）: 本ドライバのモニターは**コンソール
セッションのデスクトップ**に属する。RDP 接続中はコンソールセッションが
`Conn` に落ち、アクティブは rdp-tcp セッションになる。このとき:

- `IddCxMonitorArrival` は成功する
- **`EvtIddCxAdapterCommitModes2` はパスをアクティブとしてコミットする**
  （`CommitModes2: 1 path(s), 1 active`）= OS は実際にモニターを駆動している
- しかし RDP セッションの `QueryDisplayConfig` / `EnumDisplayDevices` には
  一切現れない

つまり**「OS が駆動しているか」と「呼び出し元セッションから見えるか」は
別の問い**。前者だけを見て "active" と報告すると嘘になる（実装当初これを
やって矛盾した出力になった）。

**現在の表示**:
- `plug`: コンソールが非アクティブなら即座に警告して exit 3
- `list`: モニター毎に「driven by the OS / not attached to a desktop」を出し、
  さらにコンソールが非アクティブなら「このセッションからは見えない」と明示
- `resolve`: 自セッションのトポロジで判定（これが「見えるか」の正解）+ 理由

セッション判定は**クライアント側**（`nyanvddctl.cpp` の
`IsConsoleSessionActive`）に置いた。ドライバはセッション0で動くので
wtsapi32 依存を持ち込む必要がある一方、呼び出し元は自分で答えられるため。

**未確認**: RDP 切断後にコンソールへ戻ったとき、モニターがそのまま現れるか
（CommitModes がアクティブのままなので現れるはずだが、実機で要確認）。

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

## 差分キャプチャの実測（dirty-probe）

「Spatial Wall の差分更新を最小エリアで」が本当に成立するかを
`tools/dirty-probe` で実測した。WGC `DirtyRegionMode = ReportOnly`（24H2
API）でダーティ矩形メタデータだけを読む計測ツールで、対象は
`nyanvddctl resolve` が返す GDI 名で指定する — 物理モニターや他社 VDD とも
同条件で比較できる。

2026-07-23 実測（Z390 / 24H2 / IddCx 1.11 = 0x1B01、VDD 1080p@60、物理 4K）:

| シナリオ | fps | ダーティ/フレーム（中央値） |
|---|---|---|
| VDD 静止 | 0.9 | 2,073,600 px（全画面） |
| VDD 64×64@10Hz | 9.3 | 4,096 px（= 64×64、矩形1個） |
| VDD 64×64@10Hz + `DisableAdapterFlags=0x20` | 9.3 | 4,096 px（完全同一） |
| VDD/物理 × カーソル旋回 × 捕捉あり/なし（全4通り） | 34〜42 | 573〜593 px |

わかったこと:

1. **差分最小化は end-to-end で成立する。** 64×64 の更新は WGC に
   ちょうど 4096 px・矩形1個で届く。ただし担い手はクライアントの
   `DirtyRegions` 消費であって、テクスチャ自体は ReportOnly では
   毎フレーム全画面分レンダリングされる。矩形は「どこを読めばよいか」の
   メタデータで、コピー/エンコード/転送を絞るのはクライアントの仕事。
2. **`PREFER_PRECISE_PRESENT_REGIONS` は WGC のダーティ精度に無関係。**
   フラグを落としても結果は完全同一だった。WGC のダーティは DWM 自身の
   damage 追跡由来で、IddCx swap-chain への present region（本ドライバは
   捨てる側）とは別経路。「キャプチャ側のダーティ精度向上」という当初の
   採用理由は誤り。MS ドキュメント上このフラグは DWM の合成コストを
   増やしうるため、同日**要求を廃止した**（`NYANVDD_CAP_PRECISE_DIRTY`
   定数は旧ビルドの status 解読用に残置。以後 status に precise-dirty は
   点灯しない）。
3. **ハードウェアカーソル DDI は実装しない（設計判断）。** HW カーソルの
   物理と SW カーソルの VDD、`IsCursorCaptureEnabled` のオンオフ、全4通り
   でカーソル移動の damage は同一（〜575 px・入力レート 34〜42fps）。
   damage 追跡とカーソル合成は別機構であり、DDI を実装しても WGC ベースの
   クライアントから見える挙動は変わらない。面積は全画面換算 0.01fps で
   誤差レベル。フレーム到着率だけが影響で、必要なら
   `GraphicsCaptureSession.MinUpdateInterval`（24H2）で絞れる。
4. **完全静止のモニターには 〜1Hz で全画面ダーティが届く。** 今回最大の
   帯域項目（全画面換算 0.8fps）。実 damage が流れると消えることから、
   damage ゼロのモニターに対する WGC の保守的リフレッシュと推定
   （物理は常時微小 damage があるため観測されない）。静止した壁では
   「矩形1個＝全画面」フレームの特別扱い（変化なし検出やスロットル）を
   クライアント側に推奨。原因の深掘りは未了。

クライアント実装への含意: テキストカーソル（caret）の点滅も〜600 px で
届くため、矩形サイズでの間引きは不可。キャプチャの GPU コスト自体を
下げたければ `ReportAndRender`（バッファの dirty 部分だけ更新）が次の
選択肢で、dirty-probe のモードを替えれば同条件で測れる。

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

- [x] RDP 中の plug で理由が明示される（plug/list/resolve の3経路）

2026-07-23 消化分（dirty-probe による差分キャプチャ実測 — 詳細は
「差分キャプチャの実測」節）:

- [x] 部分更新のダーティ矩形が更新領域まで絞れる（64×64 → 正確に 4096 px）
- [x] `PREFER_PRECISE_PRESENT_REGIONS` の WGC への効果（無し）
- [x] カーソル移動の damage 量（HW/SW・捕捉設定に依らず 〜575 px @ 入力レート
      → HW カーソル DDI は実装しないと決定）
- [x] 静止画面のキャプチャ挙動（完全停止ではなく 〜1Hz 全画面 refresh）
- [x] watchdog の発火と自動解除（10s arm → 13s 放置 → 全 unplug + 自動解除。
      同日、時計を unbiased 化しスリープ誤発火を修正）

未消化:

- [ ] RDP 切断 → コンソール復帰でモニターがそのまま見えるか
- [ ] S3/S4 復帰で二重初期化しないこと（再入ガードは入れたが実機未確認）
- [ ] watchdog armed のままスリープ → 復帰で誤発火しないこと
      （unbiased time + D0Entry リフレッシュ実装済み、実機未確認）
- [ ] plug→即 kill→再起動→リコンサイルでゴーストが出ない
- [ ] メガネ挿抜のトポロジストール中にモニターが消えない（ParsecVDD 比較）
- [ ] 選択中モードが preferred (@120Hz) になっているかの目視
- [ ] `EnableFp16=1` + gamma ramp 実装後に「HDR を使用する」が出るか
- [ ] 2台目マシン（N100）が 24H2 以上か確認して導入
- [ ] スリープ/復帰でモニター構成が保持されるか
- [ ] 静止時 〜1Hz 全画面 refresh の原因と対策（`MinUpdateInterval` /
      `ReportAndRender` での挙動確認）
- [ ] カーソルが捕捉画像のピクセルに実際に含まれるか（HW/SW、捕捉設定で
      違うか）の目視確認 — dirty-probe はメタデータしか読まないため未検証
