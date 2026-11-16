import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

export interface BeefCombination {
    config: string;
    platform: string;
}

export interface BeefWorkspace {
    /** Folder the Beef workspace or project file lives in. */
    folder: vscode.WorkspaceFolder;
    /** `BeefSpace.toml` for a workspace, `BeefProj.toml` for a single project. */
    projectFile: vscode.Uri;
    isWorkspace: boolean;
    /** Startup project declared by `[Workspace]`, when there is one. */
    startupProject?: string;
    /** Absolute path of the `corlib` project, when the file pins one. */
    corlibPath?: string;
    /** `[Configs.<Config>.<Platform>]` pairs, in file order, or defaults. */
    combinations: BeefCombination[];
}

const WORKSPACE_FILE = 'BeefSpace.toml';
const PROJECT_FILE = 'BeefProj.toml';

export function hostPlatform(): string {
    if (process.platform === 'win32') {
        return process.arch === 'ia32' ? 'Win32' : 'Win64';
    }
    if (process.platform === 'darwin') {
        return 'macOS';
    }
    return 'Linux64';
}

function isFile(candidate: string): boolean {
    try {
        return fs.statSync(candidate).isFile();
    } catch {
        return false;
    }
}

function expandUserPath(value: string): string {
    const trimmed = value.trim();
    if (trimmed === '~' || trimmed.startsWith('~/') || trimmed.startsWith('~\\')) {
        const home = process.env.HOME ?? process.env.USERPROFILE ?? '';
        return path.join(home, trimmed.slice(1));
    }
    return trimmed;
}

async function readText(uri: vscode.Uri): Promise<string | undefined> {
    // Prefer the editor buffer so unsaved edits are honoured.
    const open = vscode.workspace.textDocuments.find((document) => document.uri.fsPath === uri.fsPath);
    if (open) {
        return open.getText();
    }
    try {
        const bytes = await vscode.workspace.fs.readFile(uri);
        return Buffer.from(bytes).toString('utf8');
    } catch {
        return undefined;
    }
}

/** `[Configs.Debug.Win64]` and `[[Configs.Debug.Win64.DistinctOptions]]` headers. */
const CONFIG_SECTION = /^\s*\[\[?\s*Configs\s*\.\s*([^.\]\s]+)\s*\.\s*([^.\]\s]+)/;

function parseCombinations(text: string): BeefCombination[] {
    const combinations: BeefCombination[] = [];
    const seen = new Set<string>();
    for (const line of text.split(/\r?\n/)) {
        const match = CONFIG_SECTION.exec(line);
        if (!match) {
            continue;
        }
        const key = `${match[1]}/${match[2]}`;
        if (seen.has(key)) {
            continue;
        }
        seen.add(key);
        combinations.push({ config: match[1], platform: match[2] });
    }
    return combinations;
}

/** The value of 'Name' inside the `[Project]` table. */
function parseProjectName(text: string): string | undefined {
    let inProjectTable = false;
    for (const line of text.split(/\r?\n/)) {
        if (/^\s*\[/.test(line)) {
            inProjectTable = /^\s*\[\s*Project\s*\]/.test(line);
            continue;
        }
        if (!inProjectTable) {
            continue;
        }
        const match = /^\s*Name\s*=\s*"([^"]*)"/.exec(line);
        if (match) {
            return match[1];
        }
    }
    return undefined;
}

async function locateProjectFile(folder: vscode.WorkspaceFolder): Promise<vscode.Uri | undefined> {
    for (const name of [WORKSPACE_FILE, PROJECT_FILE]) {
        const candidate = vscode.Uri.joinPath(folder.uri, name);
        try {
            await vscode.workspace.fs.stat(candidate);
            return candidate;
        } catch {
            // Not at the workspace root; look a little deeper below.
        }
    }

    for (const name of [WORKSPACE_FILE, PROJECT_FILE]) {
        const found = await vscode.workspace.findFiles(
            new vscode.RelativePattern(folder, `**/${name}`),
            '**/{node_modules,.git,build,.vs}/**',
            1);
        if (found.length > 0) {
            return found[0];
        }
    }
    return undefined;
}

/**
 * Reads the Beef workspace (or single project) of 'folder': its project file,
 * startup project and the configuration/platform pairs it declares.
 */
export async function findBeefWorkspace(folder: vscode.WorkspaceFolder): Promise<BeefWorkspace | undefined> {
    const projectFile = await locateProjectFile(folder);
    if (!projectFile) {
        return undefined;
    }

    const text = (await readText(projectFile)) ?? '';
    const isWorkspace = path.basename(projectFile.fsPath).toLowerCase() === WORKSPACE_FILE.toLowerCase();
    const workspace: BeefWorkspace = {
        folder,
        projectFile,
        isWorkspace,
        startupProject: /^\s*StartupProject\s*=\s*"([^"]*)"/m.exec(text)?.[1],
        combinations: parseCombinations(text),
    };

    const corlib = /corlib\s*=\s*\{\s*Path\s*=\s*"([^"]+)"/i.exec(text)?.[1];
    if (corlib) {
        workspace.corlibPath = path.resolve(path.dirname(projectFile.fsPath), corlib.replace(/\//g, path.sep));
    }

    if (workspace.combinations.length === 0) {
        const platform = hostPlatform();
        workspace.combinations = [
            { config: 'Debug', platform },
            { config: 'Release', platform },
        ];
    }

    if (!isWorkspace && workspace.startupProject === undefined) {
        workspace.startupProject = parseProjectName(text);
    }

    return workspace;
}

/** Name of the produced binary's project: the startup project, else the folder's project. */
export function projectName(workspace: BeefWorkspace): string {
    if (workspace.startupProject) {
        return workspace.startupProject;
    }
    return path.basename(path.dirname(workspace.projectFile.fsPath));
}

export function defaultCombination(workspace: BeefWorkspace): BeefCombination {
    const host = hostPlatform();
    return workspace.combinations.find((combination) => combination.platform === host)
        ?? workspace.combinations[0]
        ?? { config: 'Debug', platform: host };
}

/** Picks the requested config/platform, falling back to the workspace's default pair. */
export function pickCombination(workspace: BeefWorkspace, config?: string, platform?: string): BeefCombination {
    const fallback = defaultCombination(workspace);
    const configName = config ?? fallback.config;
    if (platform) {
        return { config: configName, platform };
    }
    const candidates = workspace.combinations.filter((combination) => combination.config === configName);
    const match = candidates.find((combination) => combination.platform === fallback.platform) ?? candidates[0];
    return { config: configName, platform: match?.platform ?? fallback.platform };
}

/** A setting value as a usable path: `~` expanded, surrounding quotes removed. */
function configuredPath(key: string): string {
    const value = vscode.workspace.getConfiguration('beef').get<string>(key) ?? '';
    return expandUserPath(value.trim().replace(/^["']|["']$/g, ''));
}

let sdkWarningShown = false;

/** BeefBuild inside a Beef SDK folder: `<sdk>/IDE/dist/BeefBuild[.exe]`. */
function buildToolInSdk(sdkPath: string, executable: string): string | undefined {
    for (const relative of [path.join('IDE', 'dist', executable), executable, path.join('bin', executable)]) {
        const candidate = path.join(sdkPath, relative);
        if (isFile(candidate)) {
            return candidate;
        }
    }
    return undefined;
}

/**
 * The Beef SDK (installation) folder: `beef.sdk.path` when set, otherwise the
 * first ancestor of the workspace or of the pinned corlib path that contains
 * `IDE/dist/BeefBuild`.
 */
export function findSdkPath(workspace: BeefWorkspace): string | undefined {
    const executable = process.platform === 'win32' ? 'BeefBuild.exe' : 'BeefBuild';

    const configured = configuredPath('sdk.path');
    if (configured) {
        if (buildToolInSdk(configured, executable)) {
            return configured;
        }
        if (!sdkWarningShown) {
            sdkWarningShown = true;
            void vscode.window.showWarningMessage(
                `Beef: "beef.sdk.path" points at '${configured}', which does not contain ` +
                `IDE/dist/${executable}. Fix the setting or clear it to enable auto-detection.`);
        }
        return undefined;
    }

    const searchAncestors = (start: string): string | undefined => {
        let directory = start;
        for (let depth = 0; depth < 6; depth++) {
            if (buildToolInSdk(directory, executable)) {
                return directory;
            }
            const parent = path.dirname(directory);
            if (parent === directory) {
                break;
            }
            directory = parent;
        }
        return undefined;
    };

    const fromWorkspace = searchAncestors(workspace.folder.uri.fsPath);
    if (fromWorkspace) {
        return fromWorkspace;
    }

    // A workspace outside the Beef tree still points at corlib inside it
    // (`<Beef>/BeefLibs/corlib`), which leads to `<Beef>/IDE/dist`.
    if (workspace.corlibPath) {
        const fromCorlib = searchAncestors(path.dirname(workspace.corlibPath));
        if (fromCorlib) {
            return fromCorlib;
        }
    }
    return undefined;
}

/**
 * Finds BeefBuild: `beef.buildTool.path` when set, then `beef.sdk.path` or the
 * discovered Beef SDK (`<sdk>/IDE/dist/BeefBuild[.exe]`), then `PATH`.
 */
export function findBuildTool(workspace: BeefWorkspace): string | undefined {
    const executable = process.platform === 'win32' ? 'BeefBuild.exe' : 'BeefBuild';

    const configuredTool = configuredPath('buildTool.path');
    if (configuredTool) {
        return isFile(configuredTool) ? configuredTool : undefined;
    }

    const sdkPath = findSdkPath(workspace);
    if (sdkPath) {
        return buildToolInSdk(sdkPath, executable);
    }

    for (const entry of (process.env.PATH ?? '').split(path.delimiter)) {
        if (!entry) {
            continue;
        }
        const candidate = path.join(entry, executable);
        if (isFile(candidate)) {
            return candidate;
        }
    }
    return undefined;
}

/** `<workspace>/build/<Config>_<Platform>`, the default `$(BuildDir)`. */
export function buildDirectory(workspace: BeefWorkspace, combination: BeefCombination): string {
    return vscode.Uri.joinPath(
        workspace.folder.uri, 'build', `${combination.config}_${combination.platform}`).fsPath;
}

/** Default build output of 'project' for a configuration and platform. */
export function targetPath(workspace: BeefWorkspace, project: string, combination: BeefCombination): string {
    const executable = process.platform === 'win32' ? `${project}.exe` : project;
    return path.join(buildDirectory(workspace, combination), project, executable);
}

export interface ServerLookup {
    serverPath?: string;
    problem?: string;
}

/** Locates `beeflsp` for the language server and the debug adapter. */
export function findServer(): ServerLookup {
    const configuration = vscode.workspace.getConfiguration('beef');
    const configured = expandUserPath(configuration.get<string>('server.path') ?? '');
    if (configured) {
        if (isFile(configured)) {
            return { serverPath: configured };
        }
        return { problem: `"beef.server.path" points at '${configured}', which does not exist.` };
    }

    const executable = process.platform === 'win32' ? 'beeflsp.exe' : 'beeflsp';
    const relativeDirectories = ['build-lsp/bin', 'build/bin', 'BeefLSP/build/bin', 'IDE/dist'];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        for (const relativeDirectory of relativeDirectories) {
            const candidate = path.join(folder.uri.fsPath, relativeDirectory, executable);
            if (isFile(candidate)) {
                return { serverPath: candidate };
            }
        }
    }

    for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
        if (!dir) {
            continue;
        }
        const candidate = path.join(dir, executable);
        if (isFile(candidate)) {
            return { serverPath: candidate };
        }
    }

    return {
        problem: `Could not find '${executable}'. Set "beef.server.path" or build it with: ` +
            'cmake -S BeefLSP -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release',
    };
}
