# Tests / テスト

The test executables use C++17 and Windows APIs. No scripting runtime or external testing framework is required.

テストはC++17とWindows APIで実装しています。スクリプト環境や外部テストフレームワークは不要です。

## Build / ビルド

Open `switch_ime.sln` in Visual Studio 2026 and build `Debug | x64` or `Release | x64`. Keep `switch_ime` as the startup project for normal application development. The tests are separate console applications and are not shipped with the app.

Visual Studio 2026で `switch_ime.sln` を開き、`Debug | x64` または `Release | x64` をビルドしてください。通常の開発時は `switch_ime` をスタートアッププロジェクトにします。テストは別のコンソールアプリで、製品EXEには含まれません。

## Run / 実行

Run these executables directly, or select the corresponding startup project in Visual Studio and use **Start Without Debugging**:

以下のEXEを直接実行するか、対応するプロジェクトをスタートアッププロジェクトにしてVisual Studioの「デバッグなしで開始」で実行します。

```text
bin\x64\Release\tests\switch_ime_tests.exe
bin\x64\Release\tests\switch_ime_smoke.exe
```

The desktop smoke test uses its own INI with the administrator option off. It also launches a second copy under a different EXE name and checks that only the original stays resident. Unit tests simulate elevation requests and never display UAC or start an elevated process.

デスクトップテストは独立したINIで管理者起動をOFFにして実行します。別のEXE名のコピーも起動し、先に起動したアプリだけが常駐し続けることを確認します。単体テストは昇格要求を模擬し、UAC表示や昇格プロセスの起動は行いません。

- `switch_ime_tests`: key filtering, repeat suppression, injected-input exclusion, IME decision table, INI round trips, settings save/cancel, and modal tray-menu handling. It creates `test-output` beside its own EXE. It does not install a global hook or send real keyboard input. Dialogs appear briefly and close automatically. An optional argument specifies a different existing parent location for test output.
- `switch_ime_smoke`: hidden startup, tray registration, restoration after icon deletion plus a `TaskbarCreated` notification, and clean exit. Run on a normal Windows desktop; a sandbox may not have access to Explorer's tray. It launches an isolated copy of the app with F23/F24 settings, then closes it and removes the test copy. It neither changes the user's INI nor restarts Explorer, and it does not inject key input. An optional argument specifies the app EXE; the default is `..\switch_ime.exe` relative to the test executable directory.

- `switch_ime_tests`：キー判定、リピート抑止、注入入力の除外、IME判定、INI読み書き、設定の保存・キャンセル、設定中のトレイ操作を確認します。テストEXEの横に `test-output` を作成します。実キー送信やグローバルフック設置は行いません。設定画面は一時表示して自動で閉じます。引数で出力先フォルダを指定することもできます。
- `switch_ime_smoke`：通常起動時の非表示、トレイ登録、アイコン削除＋`TaskbarCreated` 通知による復帰、正常終了を確認します。通常のWindowsデスクトップで実行してください。F23/F24の独立設定でアプリのコピーを起動し、終了後にコピーを削除します。ユーザーのINI変更、Explorer再起動、キー送信は行いません。引数で製品EXEのパスを指定でき、省略時はテストEXEのフォルダを基準とする `..\switch_ime.exe` を使います。

Both programs return zero on success and a nonzero exit code on failure. A successful build alone does not verify real IME compatibility; confirm the configured keys with your IME on the normal desktop.

`switch_ime_tests` also checks the built product EXE's numeric and string version information against `version.h`. The release automation's tag parser has additional tests using Node.js's built-in test module: `node tests/release.test.mjs`. The release workflow runs them automatically; Node.js is not needed for the app or C++ tests.

成功時は終了コード0、失敗時は非0を返します。実IMEとの互換性はビルドだけでは確認できないため、通常デスクトップで設定したキーの動作を別途確認してください。

`switch_ime_tests` では製品EXEの数値・文字列のバージョンを `version.h` と照合します。リリース自動化のタグ判定はNode.jsの標準テスト機能を使用し、`node tests/release.test.mjs` で実行できます。リリースワークフローでは自動実行します。製品アプリとC++テストにNode.jsは不要です。
