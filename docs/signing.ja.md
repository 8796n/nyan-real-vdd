# 署名の段取り

IddCx ドライバーは UMDF（ユーザーモードのみ）なので、カーネルドライバーの
「Microsoft 署名必須」は適用されない。配布は2段構え。

## ① 自己署名（現行・¥0）

- `scripts/sign-dev.ps1` が `CN=nyan Real Driver Publisher` の自己署名証明書を
  作成し、`nyanvdd.dll` と `nyanvdd.cat`（Inf2Cat 再生成）に署名。
- インストール側（`scripts/install.ps1`）が `.cer` を **Root と
  TrustedPublisher の両方**に追加 → Secure Boot 有効の素の Win10/11 x64 で
  テストモード無しでインストール可能。
- ユーザーのルートストアに触る行儀の悪さが欠点。だからリポジトリは履歴込み
  公開・ソース監査可能を担保する。鍵は `Cert:\CurrentUser\My` に残る
  （エクスポート可設定）。同一証明書を使い続けること（更新時の混乱防止）。
- タイムスタンプは既定で digicert の TSA を打つ（オフラインなら
  `-NoTimestamp`。自己署名10年なので実害は小さい）。

## ② EV + Microsoft Attestation 署名（製品配布時）

1. EV コードサイニング証明書を購入（年5〜10万円。SSL.com / Certum / Sectigo
   等。2023年以降は HSM トークン/クラウド保管必須）。
2. Partner Center ハードウェアプログラム登録（無料・EV 証明書が身元確認に必須。
   Azure Trusted Signing では代替不可 — 2026年時点で確認済み）。
3. パッケージ（inf + dll + cat）を EV 署名した CAB で提出 → Microsoft が
   attestation 署名（無料）。
4. 返ってきた署名済みパッケージをそのまま配布。`install.ps1 -SkipCert` で
   証明書工程を飛ばす以外、パイプラインは不変。

制約: attestation 署名は Windows 10/11 クライアント専用（Server 非対応）。
本用途では無関係。Windows Update 配布や Server 対応が要る場合のみ WHQL
（HLK テスト）へ。

## 注意

- カーネルモード成分を追加したら①は即死する（Secure Boot の CI が適用され
  Microsoft 署名必須になる）。IddCx 純正構成（UMDF のみ）を守ること。
- 2025年以降、attestation 提出には InfVerif /h パスが必須。CI に
  `infverif /h` を足しておくと移行が楽。
