import * as path from 'path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from 'vscode-languageclient/node';
import { BeefDebugAdapterFactory, BeefDebugConfigurationProvider } from './beefDebug';
import { findServer } from './beefProject';
import { BeefTaskProvider, BEEF_TASK_TYPE } from './beefTasks';

const CLIENT_ID = 'beeflsp';
const CLIENT_NAME = 'Beef Language Server';
const EMIT_SCHEME = 'beef-emit';
const DEBUG_TYPE = 'beef';

interface EmitSourceResult {
    text: string;
    version: number;
}

let client: LanguageClient | undefined;
let outputChannel: vscode.OutputChannel | undefined;
let statusBar: vscode.StatusBarItem | undefined;

/**
 * Serves the generated ("emitted") sources that the server only keeps in
 * memory, so go-to-definition and the "Show generated code" lens can display
 * them (and peek them) like ordinary documents.
 */
class EmitContentProvider implements vscode.TextDocumentContentProvider {
    async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
        if (!client) {
            return '// The Beef language server is not running.';
        }

        const typeName = emitTypeNameFromUri(uri);
        try {
            const result = await client.sendRequest<EmitSourceResult | null>('beef/emitSource', { typeName });
            if (result && typeof result.text === 'string') {
                return result.text;
            }
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            outputChannel?.appendLine(`Failed to load generated source for '${typeName}': ${message}`);
        }

        return '// The generated source is no longer available.';
    }
}

function emitRange(startLine: number, endLine: number): vscode.Range {
    const start = Math.max(0, startLine);
    const end = Math.max(start, endLine);
    return new vscode.Range(start, 0, end, 0);
}

/** "beef-emit:/<encoded type name>.bf" -> "<type name>" */
function emitTypeNameFromUri(uri: vscode.Uri): string {
    let path = uri.path.replace(/^\/+/, '');
    if (path.toLowerCase().endsWith('.bf')) {
        path = path.slice(0, -3);
    }
    return decodeURIComponent(path);
}

/** Shows 'locations' in VS Code's peek widget. */async function peekLocations(
    document: vscode.TextDocument,
    position: vscode.Position,
    locations: vscode.Location[],
): Promise<void> {
    try {
        await vscode.commands.executeCommand(
            'editor.action.peekLocations', document.uri, position, locations, 'peek');
        return;
    } catch {
        // Older clients without that command: open the first target beside.
    }

    const document_ = await vscode.workspace.openTextDocument(locations[0].uri);
    await vscode.window.showTextDocument(document_, {
        preview: true,
        viewColumn: vscode.ViewColumn.Beside,
        selection: locations[0].range,
    });
}

/** Peeks the generated source at 'uri' instead of opening it in an editor. */
async function showEmit(emitUri: string, startLine: number, endLine: number): Promise<void> {
    const uri = vscode.Uri.parse(emitUri);
    const range = emitRange(startLine, endLine);
    const editor = vscode.window.activeTextEditor;

    if (editor) {
        await peekLocations(editor.document, editor.selection.active, [new vscode.Location(uri, range)]);
        return;
    }

    const document = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(document, {
        preview: true,
        viewColumn: vscode.ViewColumn.Beside,
        selection: range,
    });
}

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

function expandUserPath(value: string): string {
    const trimmed = value.trim();
    if (trimmed === '~' || trimmed.startsWith('~/') || trimmed.startsWith('~\\')) {
        const home = process.env.HOME ?? process.env.USERPROFILE ?? '';
        return path.join(home, trimmed.slice(1));
    }
    return trimmed;
}

/** `beef.sdk.path` as a usable path, or undefined when it is not configured. */
function configuredSdkPath(): string | undefined {
    const value = vscode.workspace.getConfiguration('beef').get<string>('sdk.path') ?? '';
    const trimmed = value.trim().replace(/^["']|["']$/g, '');
    return trimmed ? expandUserPath(trimmed) : undefined;
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
    const sdkPath = configuredSdkPath();

    const serverOptions: ServerOptions = {
        command: lookup.serverPath,
        args: extraArgs,
        options: {
            cwd: workspaceFolder?.uri.fsPath,
            // The server locates corlib and other dependencies through
            // BEEF_PATH when the workspace does not pin them itself.
            env: sdkPath ? { ...process.env, BEEF_PATH: sdkPath } : undefined,
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
    context.subscriptions.push(vscode.workspace.registerTextDocumentContentProvider(EMIT_SCHEME, new EmitContentProvider()));
    context.subscriptions.push(vscode.commands.registerCommand('beef.showEmit', showEmit));
    context.subscriptions.push(vscode.commands.registerCommand('beef.restartServer', restartClient));
    context.subscriptions.push(vscode.tasks.registerTaskProvider(BEEF_TASK_TYPE, new BeefTaskProvider()));
    context.subscriptions.push(vscode.debug.registerDebugConfigurationProvider(DEBUG_TYPE, new BeefDebugConfigurationProvider()));
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory(DEBUG_TYPE, new BeefDebugAdapterFactory()));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration('beef.server') || event.affectsConfiguration('beef.sdk.path')) {
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
