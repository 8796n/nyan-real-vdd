# nyan Real VDD

nyan Real / Spatial Wall（XRグラス向け 3DoF 空間ウォール）のための
仮想ディスプレイドライバー（Windows IddCx indirect display driver）。
スクリプタブルな仮想モニターが必要な用途なら単体でも使えます。

English: [README.md](README.md)

## なぜ自作するのか

Spatial Wall は仮想ディスプレイのキャプチャでウォールを描いています。汎用
VDD でも動きますが、ゴーストモニター・インデックス衝突・トポロジ変更の
ストール時にモニターが数秒消える、といった正しさの穴が残ります。本ドライバー
は設計でこれらを潰します:

- **cookie 相関。** モニターはクライアントが選んだ 32bit cookie 付きで
  plug され、cookie は EDID シリアル番号に焼き込まれます（vendor `NYN` /
  product `0x3D0F` / serial = cookie）。OS のディスプレイと自分の要求の対応
  付けが常に取れ、コネクタ番号を同一性として使いません。
- **強制 keepalive なし。** モニターは明示的に unplug するまで存続します。
  プロセスのストールやトポロジ変更の詰まりで勝手に消えることは構造的に
  ありません。クライアントは起動時にリコンサイル（list して知らない cookie
  を unplug）します。孤児掃除が欲しい場合のみ任意のウォッチドッグ
  （10秒以上・任意の制御呼び出しでリフレッシュ）を使えます。
- **対応 OS は Windows 11 24H2 以上（IddCx 1.10 フロア）。** それより古い
  Windows は全て EOL のため、未検証の荷物として抱えません。precise present
  regions とフレーム経路の realtime GPU priority は常時有効、1.10 超の機能
  （IddCx 1.11 の D3D12 等）はランタイム検出です。

## 構成

| パス | 内容 |
|---|---|
| `driver/` | UMDF/IddCx ドライバー本体（`nyanvdd.dll` + INF） |
| `include/nyanvdd_protocol.h` | 公開制御プロトコル（IOCTL）— クライアントへコピーして使う |
| `cli/` | `nyanvddctl` — リファレンスクライアント兼 devnode インストーラ |
| `scripts/` | build / sign / install / uninstall（PowerShell） |
| `docs/` | 設計・署名メモ（日本語） |

## ビルド

必要: Visual Studio (C++)、Windows SDK、
[WDK](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk)
（VS 拡張込み）。

```powershell
scripts\build.ps1              # -> out\package, out\nyanvddctl.exe（ユニットテストも実行）
scripts\sign-dev.ps1           # 自己署名（開発・配布）
```

モードと EDID のロジックは意図的に Windows 非依存にしてあり、`tests\` は
ただのコンソールプログラムとして実行できる（WDK もインストールも再起動も
不要）。`scripts\build.ps1 -SkipTests` で省略可。

## 別のマシンへ入れる

```powershell
scripts\package.ps1
```

で両方できる（名前は `nyan-real-vdd-<version>+g<commit>`）:

- **ポータブル ZIP** — 展開して管理者 PowerShell で `.\install.ps1`
- **インストーラ .exe**（Inno Setup がある場合）— 英語・日本語対応、
  プログラムと機能に登録、Windows 11 24H2 未満と ARM64 は拒否

どちらも対象マシンにリポジトリ・WDK・ビルドを要求しない。
CI はビルドとテストを検証するが成果物は**未署名**（署名鍵はローカルから出さない）。

> **そのマシンに証明書を信頼させる点に注意。** ドライバーは自己署名の開発用
> 証明書で署名されており、インストールするとその証明書がマシンの「信頼された
> ルート証明機関」と「信頼された発行元」に入る。以降そのマシンは、このドライバー
> だけでなく**その鍵で署名されたあらゆるバイナリ**を受け入れる。インストーラは
> これを表示して確認を取るが、サイレント（`/VERYSILENT`）では確認が省かれる
> のでここを読んでおくこと。アンインストールすれば証明書も消える。
> 恒久的に不要にする道筋は [docs/signing.ja.md](docs/signing.ja.md) の
> EV + attestation。

配布向けの終了コード: 成功=**0**、ドライバー登録失敗=**7**（何も残さず中止）、
再起動が要る場合は `/RESTARTEXITCODE` に従う。

## インストールと動作確認

```powershell
# 管理者 PowerShell
scripts\install.ps1            # 証明書信頼 + pnputil + devnode 作成

out\nyanvddctl.exe status
out\nyanvddctl.exe plug 1920x1080@120
out\nyanvddctl.exe list
out\nyanvddctl.exe resolve      # cookie -> \\.\DISPLAYn・モード・位置
out\nyanvddctl.exe unplug all

scripts\uninstall.ps1          # 全部戻す
```

本ドライバーはユーザーモードのみ（カーネルコードなし）。自己署名フローでは
インストーラが発行元証明書をマシンの Root / TrustedPublisher に追加します —
だからこそ本リポジトリは履歴込みで公開しています（信頼するものを監査できる
ように）。ストア的にクリーンな配布は EV + Microsoft attestation 署名に署名
工程だけ差し替えれば成立します（`docs/signing.ja.md`）。

## ライセンス

MIT。一部は Microsoft
[Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples)
IndirectDisplay サンプル（MIT）由来。詳細は [LICENSE](LICENSE)。
