import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from 'vscode-languageclient/node';

const CLIENT_ID = 'beeflsp';
const CLIENT_NAME = 'Beef Language Server';

let client: LanguageClient | undefined;
let outputChannel: vscode.OutputChannel | undefined;
let statusBar: vscode.StatusBarItem | undefined;

function getOutputChannel(): vscode.OutputChannel {
    outputChannel ??= vscode.window.createOutputChannel(CLIENT_NAME);
    return outputChannel;
}

function setStatus(text: string, tooltip: string): void {
    if (!statusBar) {
        return;
    }
    statusBar.text = text;
    statusBar.tooltip = tooltip;
}

function serverExecutableName(): string {
    return process.platform === 'win32' ? 'beeflsp.exe' : 'beeflsp';
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

function findOnPath(): string | undefined {
    const executable = serverExecutableName();
    for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
        if (!dir) {
            continue;
        }
        const candidate = path.join(dir, executable);
        if (isFile(candidate)) {
            return candidate;
        }
    }
    return undefined;
}

interface ServerLookup {
    serverPath?: string;
    problem?: string;
}

function findServer(): ServerLookup {
    const configuration = vscode.workspace.getConfiguration('beef');
    const configured = expandUserPath(configuration.get<string>('server.path') ?? '');
    if (configured) {
        if (isFile(configured)) {
            return { serverPath: configured };
        }
        return { problem: `"beef.server.path" points at '${configured}', which does not exist.` };
    }

    const executable = serverExecutableName();
    const relativeDirectories = ['build-lsp/bin', 'build/bin', 'BeefLSP/build/bin', 'IDE/dist'];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        for (const relativeDirectory of relativeDirectories) {
            const candidate = path.join(folder.uri.fsPath, relativeDirectory, executable);
            if (isFile(candidate)) {
                return { serverPath: candidate };
            }
        }
    }

    const fromPath = findOnPath();
    if (fromPath) {
        return { serverPath: fromPath };
    }

    return {
        problem: `Could not find '${executable}'. Set "beef.server.path" or build it with: ` +
            'cmake -S BeefLSP -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release',
    };
}

async function startClient(): Promise<void> {
    const lookup = findServer();
    if (!lookup.serverPath) {
        setStatus('$(warning) Beef', 'Beef language server not found');
        getOutputChannel().appendLine(`Server not found: ${lookup.problem}`);
        const action = await vscode.window.showErrorMessage(`Beef: ${lookup.problem}`, 'Open Settings');
        if (action === 'Open Settings') {
            await vscode.commands.executeCommand('workbench.action.openSettings', 'beef.server');
        }
        return;
    }

    const configuration = vscode.workspace.getConfiguration('beef');
    const extraArgs = configuration.get<string[]>('server.extraArgs') ?? [];
    const workspaceFolder = vscode.workspace.workspaceFolders?.[0];

    const serverOptions: ServerOptions = {
        command: lookup.serverPath,
        args: extraArgs,
        options: {
            cwd: workspaceFolder?.uri.fsPath,
        },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'beef' }],
        outputChannel: getOutputChannel(),
        initializationOptions: {
            diagnosticsDelayMs: configuration.get<number>('server.diagnosticsDelayMs') ?? 300,
        },
        synchronize: {
            configurationSection: 'beef',
        },
    };

    client = new LanguageClient(CLIENT_ID, CLIENT_NAME, serverOptions, clientOptions);
    setStatus('$(sync~spin) Beef', 'Starting Beef language server...');

    try {
        await client.start();
        setStatus('$(check) Beef', `Beef language server running (${lookup.serverPath})`);
        getOutputChannel().appendLine(`Started ${lookup.serverPath}`);
    } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        client = undefined;
        setStatus('$(error) Beef', 'Beef language server failed to start');
        getOutputChannel().appendLine(`Failed to start ${lookup.serverPath}: ${message}`);
        getOutputChannel().show(true);
    }
}

async function stopClient(): Promise<void> {
    const running = client;
    client = undefined;
    if (running) {
        await running.stop();
    }
}

async function restartClient(): Promise<void> {
    setStatus('$(sync~spin) Beef', 'Restarting Beef language server...');
    await stopClient();
    await startClient();
}

export function activate(context: vscode.ExtensionContext): void {
    outputChannel = vscode.window.createOutputChannel(CLIENT_NAME);
    statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    statusBar.command = 'beef.restartServer';
    statusBar.text = '$(sync~spin) Beef';
    statusBar.tooltip = 'Beef language server';
    statusBar.show();

    context.subscriptions.push(outputChannel, statusBar);
    context.subscriptions.push(vscode.commands.registerCommand('beef.restartServer', restartClient));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration('beef.server')) {
            void restartClient();
        }
    }));

    void startClient();
}

export async function deactivate(): Promise<void> {
    await stopClient();
    statusBar?.dispose();
    statusBar = undefined;
    outputChannel?.dispose();
    outputChannel = undefined;
}
