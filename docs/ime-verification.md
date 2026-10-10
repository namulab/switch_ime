# IME検証メモ

2026-10-10までに実施したサンプルアプリでの検証記録です。正式アプリでは以下の状態取得経路と `VK_IME_ON` を採用しています。`VK_IME_OFF / 0x1A` は正式アプリへの実装後に実機確認する対象で、下記の比較結果には含みません。

## 状態取得に使用した経路

1. `GetForegroundWindow` で前面ウィンドウを取得する。
2. `GetWindowThreadProcessId` と `GetGUIThreadInfo` で入力フォーカス先を特定する。編集用の子ウィンドウにも注意する。
3. `ImmGetDefaultIMEWnd` でフォーカス先の既定IMEウィンドウを取得する。
4. `SendMessageTimeoutW` で `WM_IME_CONTROL` / `IMC_GETOPENSTATUS`（`0x0005`）を問い合わせる。
5. IME ONの場合は `IMC_GETCONVERSIONMODE`（`0x0001`）も問い合わせる。

送信APIの戻り値は送信成功／失敗、出力引数の値はIMEの応答として別々に扱った。送信失敗やタイムアウトをIME OFFに変換しない。正常なopen応答0はOFF、1はONとして扱い、ON時は変換モードの `IME_CMODE_NATIVE` が0なら英数字、1なら日本語を判定した。`IME_CMODE_ROMAN` はローマ字入力方式であり、英数字モードの判定には使わない。全角英数字も英数字として扱う。日本語判定には `GetKeyboardLayout` の言語IDも確認し、他言語のNATIVEを日本語と判定しない。

サンプルの取得間隔は100ms、各送信のタイムアウトは20ms、フラグは `SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT`。前面・フォーカス・IMEウィンドウ・配列を問い合わせ後に再確認し、対象変更時は取得失敗扱いとした。取得不能時に前回状態や配列から推測で補完しなかった。

`IMC_GETOPENSTATUS` / `IMC_GETCONVERSIONMODE` は現行のデスクトップ向け `WM_IME_CONTROL` の公式コマンド一覧には掲載されておらず、使用したWindows SDK 10.0.26100.0の `imm.h` / `immdev.h` にも定義がなかった。サンプルでは従来の値を明示した。すべてのWindows 11アプリ・IMEで正しく応答する保証はない。未対応のIMEが送信成功時に0を返す場合、正常なOFF／英数字の0応答と区別できない。2つの問い合わせも原子的ではない。この限界は正式実装時にも考慮する。

## 切り替え方式の比較結果

同じ前面の入力欄に対して、以下を順番に各1回実施した。各方式はKEYDOWNとKEYUPを2要素の `INPUT` 配列にし、1回の `SendInput` で送信した。

| 方式 | ユーザーが確認した結果 |
| --- | --- |
| `SendInput(VK_IME_ON / 0x16)` | OK |
| `SendInput(VK_DBE_HIRAGANA / 0xF2)` | OK |

2イベントの挿入成功だけでなく、送信後1秒以内に既存の取得処理で実際の日本語モードを観測したことをOK条件とした。ユーザーは2回の「A → あ」と、両方式がOKと表示された結果ダイアログを確認した。入力欄のアプリ名・IMEの製品名や設定は記録されていないため、この結果をすべての環境に一般化しない。

`VK_KANA`、切り替え用の `SendMessage` / `PostMessage`、他プロセスのIMEコンテキストの直接変更は使わなかった。既存の状態取得に限り、読み取り専用の `SendMessageTimeoutW` を維持した。

フォーカスの変化直後を避けるため、同じ対象が300ms以上安定し、英数字状態を連続取得した後、送信直前にも再取得した。日本語モード時は送信しなかった。観察中の取得失敗・対象変更はNGではなく判定不可とした。フォーカス確認と `SendInput` は原子的ではなく、確認直後の対象変更を完全には防げない。`SendInput` にはUIPIによる権限制約もある。

## 実行環境の注意

Visual Studio Community 2026 18.10 / MSVC v145 / Windows SDK 10.0.26100.0で、旧サンプルのDebug・Release / x64ビルドを確認した。模擬IME応答・タイムアウト・判定遷移のテストも成功した。

サンドボックス内ではExplorerへのトレイ登録が失敗した。通常デスクトップでトレイ登録・削除を確認し、サンプルの起動および実IME比較はサンドボックス外で行った。今後もタスクトレイや実入力を検証する場合は、通常のWindowsデスクトップで実行する。

## 調査に使用した公式資料

- [GetGUIThreadInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getguithreadinfo)
- [ImmGetDefaultIMEWnd](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetdefaultimewnd)
- [SendMessageTimeoutW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw)
- [WM_IME_CONTROL（現行デスクトップ資料）](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-control)
- [WM_IME_CONTROL（旧Windows CE資料）](https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms906448(v=msdn.10))
- [IME Conversion Mode Values](https://learn.microsoft.com/en-us/windows/win32/intl/ime-conversion-mode-values)
- [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
