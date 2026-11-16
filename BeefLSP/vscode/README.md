# Beef for VS Code

Language support for [Beef](https://www.beeflang.org) backed by `beeflsp` (in the
parent directory), which drives the same C++ compiler frontend as the Beef IDE.

## Features

* `.bf` file association, syntax highlighting and comment/bracket/indent rules.
* Completion (including member completion from `corlib` and project
  dependencies) with documentation on resolve.
* Go to definition for types, members and locals.
* Hover with the symbol signature and its documentation.
* Signature help for calls (all overloads, active parameter tracked as you type).
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
* Build, run, test and clean tasks for every configuration/platform pair in the
  workspace, and a `beef` launch configuration that builds and debugs the
  startup project with the Beef debugger (`beeflsp --debug-adapter`).
* Status bar entry and a `Beef: Restart Language Server` command.

Hover, references, rename and formatting are not implemented in the server yet.

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

`beeflsp` also hosts the debug adapter (`beeflsp --debug-adapter`), so the same
binary provides the language server and debugging.

## Building and debugging

The extension registers `beef` tasks, so `Terminal → Run Task…` lists a build,
run, test and clean task for every `[Configs.<Config>.<Platform>]` pair declared
by the workspace (Debug and Release on the host platform when the file declares
no configurations, e.g. `beef: build Debug/Win64`). They invoke
`BeefBuild -config=… -platform=…` in the workspace folder.

BeefBuild is looked up in `beef.buildTool.path` when set, otherwise in the Beef
SDK (`beef.sdk.path`, or the SDK discovered next to the workspace and its
parents, from the `corlib` path the workspace pins, or on `PATH`) as
`<sdk>/IDE/dist/BeefBuild[.exe]`. `beef.sdk.path` is also handed to the language
server as `BEEF_PATH`, so corlib is found for workspaces that do not pin it.

To pin a task in `tasks.json`:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "type": "beef",
      "command": "build",
      "config": "Debug",
      "platform": "Win64",
      "label": "build",
      "group": { "kind": "build", "isDefault": true }
    }
  ]
}
```

A launch configuration of type `beef` starts the Beef debugger through
`beeflsp --debug-adapter` — the same debugger core the Beef IDE uses — so
breakpoints, stepping, variables, watches, evaluate and the call stack work with
Beef semantics (nested structs, `System.String` contents, arrays and collections
come from the SDK's `BeefDbgVis.toml`). No other extension is required.

Breakpoints bind against the debug information in the binary, so build with
`EmitDebugInfo` enabled (the Debug configuration is; Release usually is not).
The debugger stops at the process entry point only when `stopOnEntry` is set;
otherwise it runs to the first breakpoint. Attach is not implemented yet.

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "beef",
      "request": "launch",
      "name": "Beef: Debug",
      "config": "Debug",
      "platform": "Win64"
    }
  ]
}
```

`buildBeforeLaunch` (default `true`) runs `beef: build <Config>/<Platform>`
first; set it to `false` to launch an existing binary. `project`, `program`,
`args`, `cwd` and `stopOnEntry` are also available. The default `program` is
`build/<Config>_<Platform>/<Project>/<Project>[.exe]`; workspaces that override
`TargetDirectory` or `TargetName` in `BeefProj.toml` should set `program`
explicitly. The SDK path for the debug visualizers comes from `beef.sdk.path`
(or the discovered SDK), so nothing extra is needed in `launch.json`.

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
| `beef.buildTool.path` | `""` | Absolute path to `BeefBuild`/`BeefBuild.exe`. Takes precedence over `beef.sdk.path`. |
| `beef.sdk.path` | `""` | Absolute path to the Beef SDK (the folder containing `IDE/dist/BeefBuild` and `BeefLibs/corlib`). Used for the build tasks and passed to the server as `BEEF_PATH`. Empty means auto-detect. |

Changing a `beef.server` setting restarts the server. The output channel
`Beef Language Server` contains the server's stderr log, which is where
workspace resolution progress is reported.
