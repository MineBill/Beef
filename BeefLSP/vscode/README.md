# Beef for VS Code

Language support for [Beef](https://www.beeflang.org) backed by `beeflsp` (in the
parent directory), which drives the same C++ compiler frontend as the Beef IDE.

## Features

* `.bf` file association, syntax highlighting and comment/bracket/indent rules.
* Completion (including member completion from `corlib` and project
  dependencies) with documentation on resolve.
* Go to definition for types, members and locals.
* Hover with the symbol signature and its documentation.
* Semantic highlighting of resolved symbols (types, methods, properties, locals,
  parameters) from the compiler's classifier; keywords, comments and literals
  keep the grammar's colours.
* Folding ranges for types, methods, comments, `#region`s and `using` blocks.
* Diagnostics for errors and warnings. Semantic errors refresh a few hundred ms
  after you stop typing (immediately on save), syntax errors sooner.
* Generated code: a "Show generated code" code lens on the lines that comptime
  attributes emit from, and `beef-emit:` documents for definitions that resolve
  into generated source. Both peek the generated code inline (with syntax
  highlighting) instead of opening a tab.
* Status bar entry and a `Beef: Restart Language Server` command.

Hover, references, rename, signature help and formatting are not implemented in
the server yet.

## Requirements

The extension only starts a process; it does not contain the server. Build it
first from the repository root:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S BeefLSP -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-lsp
```

That produces `build-lsp/bin/beeflsp.exe` (or `bin/beeflsp` on Linux). The
extension finds it automatically when the workspace root contains
`build-lsp/bin`, `build/bin`, `BeefLSP/build/bin` or `IDE/dist`. Otherwise set
`beef.server.path`, or put `beeflsp` on `PATH`.

Open the folder that contains `BeefSpace.toml` (or `BeefProj.toml`) as your
workspace so the server can resolve the whole project.

## Developing the extension

```bash
cd BeefLSP/vscode
npm install
npm run build      # bundles to dist/extension.js
```

Press `F5` (Run Extension) to launch an Extension Development Host. The repo's
root `.gitignore` ignores `.vscode/`, so a fresh clone needs this launch
configuration added manually:

```json
{
  "type": "extensionHost",
  "request": "launch",
  "name": "Run Extension",
  "args": ["--extensionDevelopmentPath=${workspaceFolder}"],
  "outFiles": ["${workspaceFolder}/dist/**/*.js"],
  "preLaunchTask": "npm: build"
}
```

Other useful commands:

```bash
npm run typecheck  # tsc --noEmit
npm run package    # produces a .vsix via vsce
code --install-extension beeflsp-0.1.0.vsix
```

## Settings

| Setting | Default | Description |
| --- | --- | --- |
| `beef.server.path` | `""` | Absolute path to `beeflsp`. Empty means auto-detect. |
| `beef.server.extraArgs` | `[]` | Extra arguments passed to the server. |
| `beef.server.diagnosticsDelayMs` | `300` | Idle time before semantic errors are recomputed. Lower means snappier but more re-resolves; `0` refreshes as soon as you pause. |

Changing either setting restarts the server. The output channel
`Beef Language Server` contains the server's stderr log, which is where
workspace resolution progress is reported.
