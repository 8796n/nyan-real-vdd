# リポジトリ・ガイド（エージェント / コントリビュータ向け）

nyan Real VDD — Spatial Wall 用の IddCx 仮想ディスプレイドライバー。
**GitHub 公開リポジトリ（履歴込み）**。obs-3dof とは独立に回す。

> このファイルは [`AGENTS.md`](AGENTS.md) と同じ内容を保つ。片方を編集したら
> もう片方も更新すること。

## いちばん大事な規約

- **`include/nyanvdd_protocol.h` が唯一の契約。** レイアウト・意味を変えたら
  `NYANVDD_PROTOCOL_VERSION` を必ずバンプし、consuming 側（obs-3dof の
  Windows VDD 制御層）へのコピー更新を README/PR に明記する。
- **UMDF 純正構成を守る（カーネルコード禁止）。** カーネル成分を足した瞬間、
  自己署名配布（docs/signing.ja.md ①）が成立しなくなる。
- モニターの同一性は **cookie のみ**（EDID シリアル焼き込み）。コネクタ番号を
  同一性として使う API・実装を追加しない。
- 強制 keepalive を導入しない。孤児掃除はクライアントのリコンサイル+
  オプトインのウォッチドッグ（下限10秒）で行う。

## ビルド

- `scripts/build.ps1`（VS + SDK + WDK 必須）→ `scripts/sign-dev.ps1` →
  `scripts/install.ps1`（管理者）。手動 msbuild 直叩きはしない。
- IddCx バージョンは vcxproj の `NyanIddCxMinor`（既定10）/
  `NyanIddCxMinimumRequired`（既定5）で制御。根拠は docs/design.ja.md。

## 言語

- コード・コミットメッセージ・`docs/` は日本語…ではなく、**このリポジトリは
  公開前提のため: コード内コメント・README = 英語、`docs/` = 日本語、
  コミットメッセージ = 日本語**（作者の標準運用）。
- 配布物の文言（CLI 出力等）は英語。

## 実機検証

未消化チェックリストは docs/design.ja.md 末尾。変更したら該当項目を必ず
再消化してからリリースする。
