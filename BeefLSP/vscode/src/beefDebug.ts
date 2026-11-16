import * as path from 'path';
import * as vscode from 'vscode';
import {
    findBeefWorkspace,
    findBuildTool,
    findSdkPath,
    findServer,
    hostPlatform,
    pickCombination,
    projectName,
    targetPath,
} from './beefProject';
import { beefTaskName } from './beefTasks';

/**
 * Starts `beeflsp --debug-adapter` for a session. VS Code spawns one process per
 * session, so the debugger's process-wide state never leaks between sessions or
 * into the language server.
 */
export class BeefDebugAdapterFactory implements vscode.DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        _session: vscode.DebugSession,
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        const lookup = findServer();
        if (!lookup.serverPath) {
            void vscode.window.showErrorMessage(
                `Beef: the debug adapter needs beeflsp. ${lookup.problem ?? ''}`);
            return undefined;
        }
        return new vscode.DebugAdapterExecutable(lookup.serverPath, ['--debug-adapter']);
    }
}

/**
 * Resolves a `beef` launch configuration into what the adapter needs: the
 * startup project's binary, its working directory, the SDK (for the debug
 * visualizers) and the build task that produces the binary.
 */
export class BeefDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
    async provideDebugConfigurations(
        folder: vscode.WorkspaceFolder | undefined,
    ): Promise<vscode.DebugConfiguration[]> {
        const workspaceFolder = folder ?? vscode.workspace.workspaceFolders?.[0];
        const workspace = workspaceFolder ? await findBeefWorkspace(workspaceFolder) : undefined;
        const combination = workspace
            ? pickCombination(workspace)
            : { config: 'Debug', platform: hostPlatform() };

        return [{
            type: 'beef',
            request: 'launch',
            name: `Beef: Debug (${combination.config}/${combination.platform})`,
            config: combination.config,
            platform: combination.platform,
            buildBeforeLaunch: true,
        }];
    }

    async resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        configuration: vscode.DebugConfiguration,
    ): Promise<vscode.DebugConfiguration | undefined> {
        const workspaceFolder = folder ?? vscode.workspace.workspaceFolders?.[0];
        if (!workspaceFolder) {
            void vscode.window.showErrorMessage(
                'Beef: open the folder that contains BeefSpace.toml (or BeefProj.toml) before launching.');
            return undefined;
        }
        if (configuration.request && configuration.request !== 'launch') {
            void vscode.window.showErrorMessage('Beef: only "launch" debug configurations are supported.');
            return undefined;
        }

        const workspace = await findBeefWorkspace(workspaceFolder);
        if (!workspace) {
            void vscode.window.showErrorMessage(
                `Beef: no BeefSpace.toml or BeefProj.toml found in '${workspaceFolder.name}'.`);
            return undefined;
        }

        const combination = pickCombination(workspace, configuration.config, configuration.platform);
        const project = configuration.project ?? projectName(workspace);
        const program = configuration.program ?? targetPath(workspace, project, combination);
        const cwd = configuration.cwd ?? path.dirname(program);

        const configuredSdk = (vscode.workspace.getConfiguration('beef').get<string>('sdk.path') ?? '')
            .trim().replace(/^["']|["']$/g, '');
        const sdkPath = configuredSdk || findSdkPath(workspace) || '';

        const resolved: vscode.DebugConfiguration = {
            type: 'beef',
            request: 'launch',
            name: configuration.name ?? `Beef: ${project} (${combination.config}/${combination.platform})`,
            program,
            args: configuration.args ?? [],
            cwd,
            stopOnEntry: configuration.stopOnEntry ?? false,
            config: combination.config,
            platform: combination.platform,
            sdkPath,
        };

        // Keep an explicit preLaunchTask; otherwise build the target first when
        // BeefBuild is available.
        const preLaunchTask = configuration.preLaunchTask
            ?? (configuration.buildBeforeLaunch === false
                ? undefined
                : (findBuildTool(workspace) ? beefTaskName('build', combination) : undefined));
        if (preLaunchTask) {
            resolved.preLaunchTask = preLaunchTask;
        }

        return resolved;
    }
}
