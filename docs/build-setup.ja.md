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

## 検証済みの範囲（このマシン）

- Release x64: コンパイル → リンク → WDKTestCert テスト署名 → InfVerif
  （Universal 判定・警告0）→ Inf2Cat カタログ生成まで成功。
- `scripts/sign-dev.ps1`: 自己署名証明書作成 → dll/cat 署名 → .cer 出力まで成功。
- `scripts/install.ps1` 以降（証明書信頼・pnputil・devnode・実機 plug）は未実施。
  docs/design.ja.md のチェックリスト参照。
