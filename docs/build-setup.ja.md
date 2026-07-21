# ビルド環境の構築メモ（2026-07 時点の実録）

Z390 機（VS2026 Community 18.8 / SDK 26100）で最初にビルドを通すまでに
必要だった手順。次のマシンでの再現用。

## 必要物

1. **Visual Studio 2026**（C++ デスクトップ開発）
2. **Windows SDK**（26100 系。VS インストーラ経由で入っていれば OK）
3. **WDK**: `winget install Microsoft.WindowsWDK.10.0.26100`
   - iddcx ヘッダは `Include\10.0.26100.0\um\iddcx\<minor>\`（1.10 まで）。
     vcxproj の `IDDCX_VERSION_MINOR` がディレクトリ選択に使われる。
4. **VS2026 への WDK 統合は VSIX ではなく VS コンポーネント**:
   ```
   setup.exe modify --installPath "C:\Program Files\Microsoft Visual Studio\18\Community" ^
     --add Component.Microsoft.Windows.DriverKit --quiet --norestart
   ```
   （`--quiet` は**最初から昇格したプロセス**で実行しないと exit 5007。）
   これで `MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\` に
   `WindowsUserModeDriver10.0` が入る。

## ハマりどころ

- **WDK 26100 の wdksetup は VS2026 に何も統合しない**（Dev17=VS2022 向け
  コンテンツのみ）。統合は上記 VS コンポーネントが正。
- **`Microsoft.DriverKit.Build.Tasks.18.0.dll` が見つからない**:
  kit `build\10.0.26100.0\bin` には 17.0 しか無く、VS18 の msbuild は 18.0 を
  要求する。暫定対応として WDK 28000（`winget install
  Microsoft.WindowsWDK.10.0.28000`）を入れ、
  `build\10.0.28000.0\bin\Microsoft.DriverKit.Build.Tasks*.18.0.dll` を
  `build\10.0.26100.0\bin\` へコピー（要昇格）。
  ※26100 用の更新版 WDK が 18.0 タスクを同梱したらコピーは不要になる。
- ビルド成果物は**ソリューション直下** `x64\Release\`（パッケージは
  `x64\Release\nyanvdd\`）に出る。`scripts/build.ps1` はそこから `out\` へ
  集める。
- **MSBuild は 64bit 版（`Bin\amd64\MSBuild.exe`）を使うこと**: WDK ビルド
  タスクは自プロセスのアーキテクチャに合わせて `infverif.dll` をロードする
  が、kit の `build\<ver>\bin` には x64/arm64 しか無い。32bit MSBuild だと
  「DLL 'x86\InfVerif.dll' を読み込めません (0x8007007E)」で **INF 検証だけ
  が黙って落ちる**（ビルド自体は成功扱いになるのが罠。attestation 提出は
  InfVerif /h 必須なので見逃さないこと）。build.ps1 は amd64 優先で解決済み。

## CI（GitHub Actions）

`.github/workflows/ci.yml` が push ごとに2ジョブを回す。

- **Unit tests**: `tests/` と `cli/` をビルドしてテストを実行。WDK 不要
  （モードと EDID のロジックを OS 非依存に切り出してあるため）。
- **Driver package (unsigned)**: NuGet の WDK でドライバをビルドし
  `infverif /h` まで通す。**未署名**。

windows-2025 ランナーは[プリインストール WDK を落とした](https://github.com/actions/runner-images/issues/13071)ので、
[NuGet 経由](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget)で入れる
（`packages.config` + `Directory.Build.props`。後者は `Exists()` 条件付き
なので、WDK インストール済みのローカルでは何もしない）。

CI 構築で踏んだ罠（順に潰した）:

1. **プロジェクト単体ビルドと .sln 経由で出力先が変わる**。単体だと
   `<proj>\x64\Release\`。CI では `/p:OutDir=` で明示固定。
2. **NuGet WDK の `WDKContentRoot` に末尾セパレータが無い**。WDK 側は
   `$(WDKContentRoot)bin\...` と連結するので `\cbin\` になる →
   `Directory.Build.props` で `EnsureTrailingSlash` して補正。
3. **`WindowsTargetPlatformVersion` の既定はインストール済み SDK の最新版**。
   NuGet パッケージの bin は自分のバージョン（28000）しか持たないので
   `/p:WindowsTargetPlatformVersion=10.0.28000.0` で固定。
4. **32bit MSBuild だと `InfToolPath` が空になる**。WDK はビルドプロセス
   自身のアーキでツールパスを決めるが、`stampinf.exe` / `infverif.exe` は
   x64 にしか無い → `setup-msbuild` に `msbuild-architecture: x64`。
   （ローカルで `InfVerif.dll` を掴めなかったのと同じ根本原因）

`Directory.Build.targets` に `/p:NyanShowWdkPaths=true` で有効化する診断
ターゲットがある。キット構成が変わって「Failed to locate: xxx.exe」が出たら
まずこれで解決済みパスを見る。

**署名は CI でやらない**（秘密鍵を持ち込まない）。配布用の署名済み
パッケージは `scripts/package.ps1` がローカルで作る。

## 検証済みの範囲（このマシン）

- Release x64: コンパイル → リンク → WDKTestCert テスト署名 → InfVerif
  （Universal 判定・警告0）→ Inf2Cat カタログ生成まで成功。
- `scripts/sign-dev.ps1`: 自己署名証明書作成 → dll/cat 署名 → .cer 出力まで成功。
- `scripts/install.ps1` 以降（証明書信頼・pnputil・devnode・実機 plug）は未実施。
  docs/design.ja.md のチェックリスト参照。
