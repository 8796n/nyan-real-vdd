# GPU共有キャプチャの比較試作（2026-10-08）

WGCの取得・配送を省いた場合の効果を測るため、IddCx画像をドライバー所有の
3-slot BGRA8共有texture ringへGPUコピーし、別D3D11 deviceから読む診断経路を追加した。
通常経路はWGCのまま。診断接続がない画面はtextureを確保・コピーしない。

## 範囲と契約

- プロトコルv4。既存のv3制御レイアウトは不変で、新CLIと更新したSpatial Wallはv3/v4を扱う。
  consumerの `src/vendor/nyan-real-vdd/nyanvdd_protocol.h` を本リポジトリの正本から同期する。
- `OPEN_CAPTURE(cookie)` は**コンソールセッションの昇格済み管理者専用**。
  診断には専用device handleを使い、閉じると配送を停止する。1 cookieにつき1 consumer。
  WDFによる呼出元の識別と、共有mapping/textureのSYSTEM・LocalService・Administrators限定ACLで制限する。
  ユーザー入力のPID・共有名・connector indexを認証や画像の識別に使わない。
- `Global\NyanVddCapture-{GUID}-meta` は読み取り専用で開く。`-0`〜`-2` はkeyed mutex付きtexture。
  producerはkey 0、consumerはkey 1を待ち時間0で取得する。空きがなければ配送を見送る。
  consumerは古いREADY slotを返却し、最新slotをGPU読み取りの間保持する。
- IddCx callbackにGPU初期化・consumer待ち・GPU objectの破棄を持ち込まない。
  画像処理とGPU objectの所有は既存swap-chain worker内。共有用deviceの追加作成も不要。
  file cleanupはCPU状態だけ停止し、workerがGPU objectを後片付けする。
- mode/device変更、swap-chain停止、共有失敗、hardware protected surfaceで世代を終了する。
  静止時に接続した場合は次の画像到着までWAITING。初期画面を取得する製品用の回復経路は未実装。
- 非管理者アプリへの採用には、対話ユーザーの許可・secure desktop・セッション切替への対応、
  WGCのカーソル非合成や取得除外ウィンドウとの同等性を別途検証する必要がある。
  現段階をWGCの置き換えや製品リリースとは扱わない。

## 実行できる検査

```powershell
scripts/build.ps1
x64/Release/dirty_probe.exe --share-self-test
```

958件の既存・プロトコル検査が成功。GPU検査はドライバー本体と同じ `FramePublisher` を
2つのD3D11 deviceで動かし、保持画像の全画素不変、満杯時のスキップ、
30更新の全画素一致、停止後に配送しないことをRTX 3060で確認した。
この検査だけではSession 0からの実際の共有・file cleanup・IddCxとの連携は証明しない。

比較ベンチ（WGCとsharedで同じ刺激・poll・画素検査を使う）:

```powershell
out/nyanvddctl.exe resolve
x64/Release/dirty_probe.exe --capture-bench wgc --monitor DISPLAYn --seconds 10 --stimulus 64@60
# 以下は昇格済みコンソールで実行する。cookieはresolveで得た対象のみ。
x64/Release/dirty_probe.exe --capture-bench shared --cookie 0x12345678 --monitor DISPLAYn --seconds 10 --stimulus 64@60
```

`--stimulus` を省くと静止、画面幅以上を指定すると全面更新。
最初の1秒を除外し、poll CPU時間、画像内markerを書き換えてから1画素をGPU→CPUへ読み戻すまでの時間、
consumerと同一セッションのDWMのプロセスCPU時間を記録する。sharedはUMDF hostのPIDとCPU時間も報告する。
WGCにも同じPIDを `--driver-pid` で渡すとhost CPUを比較できる。GetProcessTimesは粗い粒度なので
短時間の0msを「無負荷」と解釈しない。publisherのsubmit時間はGPU実行時間ではない。
1画素の同期読み戻しは両条件共通の検査用処理で、実アプリの発光遅延やGPU描画全体の測定ではない。

生ログ・既存packageの退避先はローカルの `out/diagnostics/capture-20261008/`。
2026-10-08にUAC承認後、署名済みv4を導入し、RTX 3060上でSession 0のドライバーから
昇格済み診断プロセスへFHD画像を共有できた。初回2秒の検査は99フレーム・marker不一致0。
通常権限からの接続は `E_ACCESSDENIED (0x80070005)` で拒否された。
診断プロセスを通常終了してからの再接続も成功し、静止定常時の追加配送は0フレームだった。
元surfaceは `BindFlags=0x28, MiscFlags=0x802`。共有可能なフラグはあるが、
元surfaceのハンドル輸出と保持期間の制御は試しておらず、全面ゼロコピーが成立する証明ではない。
ドライバー更新前に、更新したSpatial Wallでv3のFHD60画面2枚が復元すること、
アプリの42テストとレイヤ検査が通ることも確認した。
比較測定の集計、異常終了・mode変更の検証は別途記録する。

昇格済みPowerShellで、Spatial Wallを通常終了してから次を実行する。
スクリプトは既存仮想画面があると中断し、専用cookieの試験画面だけを追加・削除する。
更新には `out/package` の事前署名が必要。再起動要求時にWindowsを自動再起動しない。

```powershell
tools/dirty-probe/run-capture-trial.ps1 -UpdateDriver
```

FHD/4K × 静止/64×64/全面 × WGC/sharedを各10秒、3反復する。反復ごとに取得経路の順を反転する。
各試験で画像markerの一致を検査し、失敗時は測定を止める。通常終了のたびに専用device handleを閉じ、
次のshared接続でも成功することを確認する。測定ログは `out/diagnostics/capture-日時/` に保存する。
試験後のアプリ再起動は実行者が行う。スクリプトはドライバーを自動ロールバックしない。
