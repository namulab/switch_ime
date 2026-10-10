# switch_ime

**IMEをトグルせず、好きなキーで日本語入力と英数字入力を直接切り替えます。**

`switch_ime` は、日本語入力のONとOFFにそれぞれ別のキーを割り当てられる、Windows向けの軽量な常駐ユーティリティです。

従来のトグル操作と異なり、**現在の入力モードを確認し、切り替えが必要な場合だけ**IMEに指示を送ります。日本語入力がすでにONの場合は再度切り替え指示を送らないため、変換未確定の文字列への不要な干渉を避けられます。

タスクトレイに常駐し、IME独自のキーカスタマイズ機能ではなくWindows APIを利用します。

## 特長

### 日本語入力と英数字入力を別々のキーに割り当て

次の2つの操作に、それぞれ好きなキーを割り当てられます。

- **IME ON** — 日本語（ひらがな）入力へ切り替える。
- **IME OFF** — 英数字の直接入力へ切り替える。

例えば、「ひらがな」キーをIME ON、「変換」キーをIME OFFに設定できます。割り当てるキーは自由に変更できます。

### 必要なときだけ切り替える

`switch_ime` は、無条件に入力モードをトグルしません。

| 押したキー | 現在 `A` | 現在 `あ` |
| --- | --- | --- |
| IME ONキー | `あ` へ切り替え | 何もしない |
| IME OFFキー | 何もしない | `A` へ切り替え |

指定したモードになっている場合は、IMEへの切り替え指示を一切送りません。

特に、日本語の変換未確定状態でIME ONキーを再度押しても、不要なモード切り替え指示によって編集中の文字列に影響することを避けられます。

入力モードが取得できない場合も、切り替え指示は送りません。

### キーを自由に設定

シンプルな設定ダイアログで、割り当てたいキーを実際に押して登録します。

設定内容はEXEと同じフォルダの `switch_ime.ini` に保存され、変更後はアプリの再起動なしで反映されます。

### Windowsへのサインイン時に自動起動

設定画面の「Windowsにサインインしたときに起動する」をチェックしてOKを押すと、現在のユーザーのスタートアップに登録します。チェックを外してOKを押すと解除します。キャンセルした場合は変更しません。

自動起動の登録先はWindowsのユーザー別 `Run` レジストリで、INIには保存しません。EXEを移動した場合は、移動先で設定を開いて、チェックした状態でOKを押し、登録先を更新してください。

### タスクトレイへの入力モード表示

常駐中は、検出した入力モードをタスクトレイのアイコンに表示します。

| 表示 | 意味 |
| --- | --- |
| `あ` | 日本語入力 |
| `A` | 英数字入力 |
| 空白 | 入力モードを取得できない状態 |

アイコンの右クリックメニューから **「設定」** または **「終了」** を選択できます。

### 軽量・ポータブル

C++とWin32 APIで実装しています。追加のアプリケーションフレームワーク、キーボードフック用DLL、インストーラーは不要です。

## 動作環境

- Windows 11
- 本アプリで使用するWindowsのIME制御・入力機構に対応した日本語IME

IMEやアプリケーションによって動作が異なる場合があります。すべてのIMEでの動作を保証するものではありません。

## インストールと使い方

1. リリースパッケージが公開されている場合はそこから `switch_ime.exe` を入手するか、ソースコードからビルドします。
2. `switch_ime.ini` を作成・更新できるフォルダにEXEを配置します。
3. `switch_ime.exe` を起動します。
4. 初回起動時に設定ダイアログが表示されるので、**IME ON** と **IME OFF** に割り当てるキーを指定して **「OK」** を押します。
5. アプリがタスクトレイに常駐します。設定を変更する場合はアイコンを右クリックして **「設定」** を選択します。

DLL登録やインストーラーは必要ありません。

## 動作の仕組み

```text
設定したキーを押す
        |
        v
現在のIME入力モードを取得
        |
        +-- すでに目的のモード --> 切り替え指示なし
        |
        +-- 状態取得不可 -------> 切り替え指示なし
        |
        +-- 異なるモード
                 |
                 v
        対応するIME ON/OFFキーを送信
```

設定したキーの入力は低レベルキーボードフックで検出します。設定対象のキーだけを処理し、それ以外のキーボード入力には干渉しません。

## 技術資料：使用するWindows API

### 現在のIMEモードの取得

IME状態の取得には、WindowsのInput Method Manager（IMM）とIME制御メッセージを利用します。

- `ImmGetDefaultIMEWnd` — 入力先に対応するデフォルトのIMEウィンドウを取得する。
- `SendMessageTimeoutW` — IMEウィンドウへの問い合わせで無期限に待機しないようにする。
- `WM_IME_CONTROL` / `IMC_GETOPENSTATUS` — IMEのON/OFFを調べる。
- `WM_IME_CONTROL` / `IMC_GETCONVERSIONMODE` — 日本語入力か英数字入力かを区別するため、変換モードを取得する。

`IMC_GETOPENSTATUS` と `IMC_GETCONVERSIONMODE` は従来から使われている互換性重視の仕組みであり、WindowsやIMEの実装によって結果が異なる場合があります。

### 入力モードの切り替え

Win32 APIの `SendInput` で、次の仮想キーを送信します。

| 操作 | 仮想キー | 値 |
| --- | --- | --- |
| 日本語入力ON | `VK_IME_ON` | `0x16` |
| 英数字入力 / IME OFF | `VK_IME_OFF` | `0x1A` |

切り替え時にはKEYDOWNとKEYUPを送ります。

アプリが送信先のウィンドウを選んでIME変更メッセージを直接送るのではなく、Windowsの通常のキーボード入力経路を使用します。

なお、`SendInput` に成功してもIMEのモード変更まで保証されるわけではありません。また、管理者権限など、より高い整合性レベルで実行されるアプリには入力を送信できない場合があります。

### キーボードフックとトレイアイコン

- `SetWindowsHookExW(WH_KEYBOARD_LL)` — 他プロセスへのフックDLL注入なしで、設定されたキーを検出する。
- `Shell_NotifyIconW` — タスクトレイへの表示とアイコン更新を行う。

### Microsoftの参考資料

- [ImmGetDefaultIMEWnd](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetdefaultimewnd)
- [WM_IME_CONTROL](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-control)
- [SendMessageTimeoutW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw)
- [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
- [Virtual-Key Codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)
- [SetWindowsHookExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw)

## ソースコードからのビルド

- Visual Studio 2026（Visual C++デスクトップ開発ツール）
- Windows SDK

Visual Studioでソリューションまたはプロジェクトを開き、必要な構成（例：`Release | x64`）を選択してビルドしてください。

## テスト

ソリューションにC++の独立したテストプロジェクトを含めています。ビルドと実行方法は [テスト手順](tests/README.md) を参照してください。

## リリース

タグによる自動ビルド・公開については [リリース手順](docs/releases.md) を参照してください。

## ライセンス

MIT Licenseです。[LICENSE](LICENSE) を参照してください。
