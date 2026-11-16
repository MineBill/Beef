# beeflsp

A Language Server Protocol server for Beef, built directly on the C++ compiler
frontend that powers the Beef IDE (`IDEHelper/Compiler`). It is not a
reimplementation: the same `BfSystem` / `BfParser` / `BfCompiler` /
`BfAutoComplete` pipeline that the IDE drives through its extern calls is driven
here from C++.

Current feature set: **completion** (with `completionItem/resolve`
documentation and trigger on `.`), **go-to-definition** (including locals and
method overloads resolved through `BfResolveType_GoToDefinition`),
**diagnostics** (errors and warnings from the resolve pass, plus live syntax
errors for the file being edited), and **generated-code support** (definitions
that land in comptime-emitted code point at a `beef-emit:` document, and a code
lens on the lines that emitted code peeks it). Everything else is intentionally
absent for now; see "Known limitations" below.

## How it works

```
initialize / didOpen / didChange          (LSP, stdio)
   └─ Server                      src/Server.cpp
        ├─ Workspace              src/Workspace.cpp   BeefSpace.toml + BeefProj.toml -> project/source list
        └─ BeefCompilerHost       src/BeefCompiler.cpp
             ├─ BfSystem_Create / BfSystem_CreateProject / BfProject_SetOptions
             ├─ per source: BfParser_SetSource -> Parse -> Reduce -> BuildDefs
             ├─ BfCompiler_ClassifySource      <- resolves the whole workspace once
             │    └─ BfPassInstance_GetErrorData  -> publishDiagnostics
             └─ per request: SetAutocomplete(cursor) -> Parse -> Reduce
                             -> BuildDefs -> CreateClassifier
                             -> BfCompiler_ClassifySource     (autocomplete pass)
                             -> BfCompiler_GetAutocompleteInfo
                  └─ CompletionParser  src/CompletionParser.cpp  (line protocol -> entries)
                       └─ CompletionItem[] with textEdit over the compiler's insertRange
```

`didOpen`/`didChange` additionally run a parse-only pass for the edited document
(`BeefCompilerHost::AnalyzeFile`) so syntax errors show up while typing; the full
resolve diagnostics replace them after the next workspace resolve.

Go-to-definition reuses the same single-file pipeline with
`BfResolveType_GoToDefinition` and reads the `defLoc` lines from the result.
Targets inside comptime-generated code (`$Emit$<project:TypeName>`) are exposed
through the `beef-emit:` URI scheme; the client asks for the text with the
`beef/emitSource` request.

## Generated code

Comptime attributes (`[Bitfield]`, `[FlexibleArray]`, ...) can emit source for
the type they are applied to. The server reads the same region list the IDE
builds its "Emit" gutter buttons from (`BfCompiler_GetCollapseRegions`) and
returns a `textDocument/codeLens` entry on every line that emitted code. The
lens command is `beef.showEmit` with the emit type and the generated line
range; the extension peeks the generated document there.

`beef-emit:` documents are served by a `TextDocumentContentProvider` that calls
`beef/emitSource`, so go-to-definition into generated code and the lens both
show read-only generated source rather than a missing file.

The first `initialized` (or the first completion request) triggers a one-time
resolve-only compile of every project in the workspace, including `corlib` and
any dependencies listed in `BeefSpace.toml`. That is what makes member
completion on library types (`Console.`, `String.`, ...) work. Subsequent
requests only run the autocomplete resolve pass, which does not reshape the
project type graph.

## Building

Requirements: CMake 3.16+, Ninja and a C++17 compiler.

On Windows the server links the compiler that the normal Beef build produces,
so run `bin/build.bat` once first (it creates `IDE/dist/IDEHelper64{,_d}.lib`
and `BeefySysLib64_Static.lib`). Set `-DBEEFLSP_USE_PREBUILT=OFF` to compile the
compiler frontend from source instead.

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S BeefLSP -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-lsp
```

On Linux the LSP builds IDEHelper and BeefySysLib through CMake and needs the
LLVM 22.1 development files (CI installs `llvm-22-dev`):

```bash
cmake -S BeefLSP -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-lsp
```

Both can also be driven from the root project:

```bash
cmake -S . -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release -DBF_LSP=ON
cmake --build build-lsp --target beeflsp
```

The executable is written to `<build>/bin/beeflsp` (`beeflsp.exe` on Windows).

### Smoke test

The server can run a single autocomplete request without LSP framing, which is
handy to check that the workspace resolves:

```bash
./build-lsp/bin/beeflsp --completion <workspaceRoot> <file.bf> <line> <character>
```

## Editor integration

`vscode/` contains a VS Code extension that starts `beeflsp`, registers `.bf`
files and ships syntax highlighting. See `vscode/README.md` for building the
extension and locating the server binary.

## Running

```bash
./build-lsp/bin/beeflsp
```

It speaks LSP over stdin/stdout and logs to stderr. The workspace root is taken
from `rootUri` (or `rootPath` / `workspaceFolders[0].uri`) during
`initialize`.

Neovim example:

```lua
vim.lsp.start {
  name = 'beeflsp',
  cmd = { 'C:/path/to/beeflsp.exe' },
  root_dir = vim.fs.dirname(vim.fs.find({ 'BeefSpace.toml', 'BeefProj.toml' }, { upward = true })[1]),
  capabilities = vim.lsp.protocol.make_client_capabilities(),
}
```

VS Code: point a generic LSP client extension at the executable and open the
folder that contains `BeefSpace.toml`.

## Workspace discovery

1. Walk up from the workspace root looking for `BeefSpace.toml`.
2. Every entry under `Projects = { Name = { Path = "..." } }` becomes a
   `BfProject`; `Dependencies` from each `BeefProj.toml` become
   `BfProject_AddDependency` links.
3. `corlib` is added as an implicit dependency of every project, because
   BeefBuild always compiles against it and the CLI project templates do not
   mention it. It is located next to the workspace, next to the `beeflsp`
   executable (`<Beef>/BeefLibs/corlib` for this repository), in `BEEF_PATH`,
   or in `C:/Beef`. If it cannot be found the server reports that instead of
   resolving (and crashing).
4. Sources are the `.bf` files under `<project>/src` (the IDE's default
   auto-included folder), plus any `[[ProjectFolder.Items]] Type = "AutoFolder"`
   directories and explicit `Type = "Source"` files. Files elsewhere in the
   project directory are not compiled, just like in the IDE.
5. C/C++ and custom-build projects (`IDEHelper`, `BeefySysLib`, `Debugger64`,
   ...) are ignored: they contribute no Beef type definitions.

Without a `BeefSpace.toml` the server falls back to scanning the root and its
immediate subdirectories for `BeefProj.toml`.

## Resolution model

All compiler work runs on a dedicated worker thread; the message loop only reads
and writes JSON-RPC. The reader, worker and main thread communicate through
queues, so a long resolve never stops the server from answering.

The first `initialized` (or the first completion request) queues a one-time
resolve-only compile of every Beef project in the workspace. That is what makes
member completion on library types (`Console.`, `String.`, ...) work. It takes
roughly 3.5 s for the IDE workspace with the compiler's worker threads enabled.

After that, each completion request runs only the single-file autocomplete
resolve pass, which is fast (single-digit milliseconds) and never reshapes the
project type graph. If a request comes back empty *and* the document text has
diverged from what was last resolved, the server assumes new type definitions
were introduced (for example a renamed class), re-parses that file, re-resolves
the workspace and retries the request once. Incremental re-resolves cost
~100-250 ms on the IDE workspace.

## Diagnostics

The resolve publishes the errors and warnings it produced, with `source: "beef"`,
error/warning severity and warning codes (e.g. `CS0168`). No request is needed:
after a document has been quiet for 150 ms a parse-only pass refreshes its syntax
errors, and after 300 ms (or immediately on save) the file's definitions are
re-parsed and the workspace is re-resolved incrementally, which refreshes the
semantic errors as well. On the IDE workspace that turns an edit into updated
diagnostics in roughly 400-500 ms. The delay is configurable through the
`diagnosticsDelayMs` initialization option (`beef.server.diagnosticsDelayMs` in
the VS Code extension). Files that are no longer mentioned are cleared.

## Known limitations

* Full document sync (`textDocumentSync.change = 1`). Fine for typical files,
  but every keystroke re-sends the document.
* No hover, references, rename, signature help, formatting or semantic tokens
  yet. The compiler endpoints for those (`BfResolveType_*`) are already
  reachable from `BeefCompilerHost`.
* Only the macros the IDE always defines are passed (`BF_MACHINE_X64`,
  `BF_PLATFORM_WINDOWS`/`BF_PLATFORM_LINUX`); configuration-specific defines
  (`[Configs.*]`, `DistinctOptions`, project `PreprocessorMacros`) are not.
* Requests are queued but `$/cancelRequest` is ignored, so a superseded
  completion still runs.
* On Windows the server links `IDE/dist/IDEHelper64{,_d}.lib`, so the checkout
  has to be built once with `bin/build.bat`.
