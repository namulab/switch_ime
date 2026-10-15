# Releases / リリース

The current local build version is **1.0.2** in `version.h`. Windows numeric file/product versions use four fields (`1.0.2.0`); the displayed version strings use three (`1.0.2`).

現在のローカルビルドの版番号は `version.h` の **1.0.2** です。Windowsの数値形式は4桁（`1.0.2.0`）、表示文字列は3桁（`1.0.2`）です。

After pushing this repository to GitHub, commit the workflow and create/push a tag:

GitHubへリポジトリを登録し、ワークフローを含むコミットに以下のようなタグを作成・送信してください。

```text
git tag v1.0.2
git push origin v1.0.2
```

Creating the same tag through GitHub's interface also triggers the workflow. Tags must be `vMAJOR.MINOR.PATCH` with no leading zeroes except zero itself. Each component must fit Windows' 16-bit version fields (0–65535). Prerelease suffixes are not accepted by this workflow.

GitHubの画面から同形式の新しいタグを作成しても実行されます。タグは `vMAJOR.MINOR.PATCH` 形式で、0以外の先頭ゼロは使えません。各値の範囲はWindowsの16ビット版番号に合わせて0～65535です。このワークフローはプレリリースの接尾辞に対応しません。

The workflow uses `windows-2025-vs2026`, writes the tag version to `version.h` in its disposable checkout, builds Release/x64, and runs the C++ tests. Tests read the built product EXE's numeric and string version fields. The generated header is not committed back to the repository. Node.js and `cmd` run the automation; no PowerShell scripts are used.

ワークフローは `windows-2025-vs2026` 上でタグの版番号を一時チェックアウトの `version.h` に設定し、Release/x64をビルドしてC++テストを実行します。テストでは製品EXEの数値・文字列のバージョンを読み取って確認します。生成したヘッダーをリポジトリへ書き戻すコミットは行いません。自動化はNode.jsと `cmd` を使用し、PowerShellスクリプトは使いません。

On success, it creates a GitHub Release with generated notes and attaches `switch_ime-VERSION-windows-x64.zip` containing:

成功後、変更履歴を自動生成したGitHub Releaseを作成し、以下を含む `switch_ime-VERSION-windows-x64.zip` を添付します。

```text
switch_ime.exe
LICENSE
README.md
```

The app creates `switch_ime.ini` on first setup; no personal INI, tests, PDBs, or DLLs are distributed. The workflow uses the repository's `GITHUB_TOKEN` with `contents: write`; no personal access token is required. GitHub Actions must be enabled and allowed to use that permission in the repository/organization. If GitHub's New Release UI creates the tag and Release together, the workflow attaches the ZIP to that Release and keeps its notes. An existing draft is published after upload. Existing assets are not overwritten; rerunning after the ZIP has already been uploaded fails. Use a new version tag for a new release.

`switch_ime.ini` は初回設定でアプリが作成します。個人設定、テストEXE、PDB、DLLは配布物に含めません。リポジトリの `GITHUB_TOKEN` に `contents: write` を付けて公開するため、個人アクセストークンの登録は不要です。リポジトリ／組織でGitHub Actionsと該当権限が許可されている必要があります。GitHubのNew Release画面でタグとReleaseを同時に作成した場合は、そのReleaseへZIPを添付し、既存の説明は維持します。下書きの場合は添付後に公開します。公開済みファイルは置換せず、同名ZIPが既に添付されている場合は失敗します。新しいリリースには新しい版番号のタグを使用してください。

The desktop/real-IME tests remain manual; GitHub's build runner is not a validation of actual Japanese IME behavior.

通常デスクトップ／実IMEの動作は別途手動確認します。GitHubでビルドに成功したことは実IMEの互換性確認にはなりません。

References: [GitHub runner images](https://github.com/actions/runner-images), [GitHub CLI release create](https://cli.github.com/manual/gh_release_create).
