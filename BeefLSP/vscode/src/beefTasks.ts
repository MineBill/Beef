import * as vscode from 'vscode';
import {
    BeefCombination,
    BeefWorkspace,
    findBeefWorkspace,
    findBuildTool,
    pickCombination,
} from './beefProject';

export const BEEF_TASK_TYPE = 'beef';

export type BeefTaskVerb = 'build' | 'run' | 'test' | 'clean';

export interface BeefTaskDefinition extends vscode.TaskDefinition {
    /** What BeefBuild does: build, run (-run), test (-test) or clean (-clean). */
    command: BeefTaskVerb;
    config?: string;
    platform?: string;
}

/** Task label, also used as the default `preLaunchTask` of a launch configuration. */
export function beefTaskName(verb: BeefTaskVerb, combination: BeefCombination): string {
    return `beef: ${verb} ${combination.config}/${combination.platform}`;
}

function buildArguments(verb: BeefTaskVerb, combination: BeefCombination): string[] {
    const args = [`-config=${combination.config}`, `-platform=${combination.platform}`];
    switch (verb) {
        case 'run':
            args.push('-run');
            break;
        case 'test':
            args.push('-test');
            break;
        case 'clean':
            args.push('-clean');
            break;
        default:
            break;
    }
    return args;
}

function createTask(
    workspace: BeefWorkspace,
    buildTool: string,
    verb: BeefTaskVerb,
    combination: BeefCombination,
): vscode.Task {
    const definition: BeefTaskDefinition = {
        type: BEEF_TASK_TYPE,
        command: verb,
        config: combination.config,
        platform: combination.platform,
    };

    const execution = new vscode.ShellExecution(buildTool, buildArguments(verb, combination), {
        cwd: workspace.folder.uri.fsPath,
    });
    const task = new vscode.Task(
        definition,
        workspace.folder,
        beefTaskName(verb, combination),
        BEEF_TASK_TYPE,
        execution,
        []);

    switch (verb) {
        case 'build':
            task.group = vscode.TaskGroup.Build;
            break;
        case 'test':
            task.group = vscode.TaskGroup.Test;
            break;
        case 'clean':
            task.group = vscode.TaskGroup.Clean;
            break;
        default:
            break;
    }

    if (verb === 'run') {
        task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated };
    }
    return task;
}

/**
 * Registers a build/run/test/clean task for every configuration and platform
 * the workspace declares, so "Tasks: Run Task" lists them for a Beef folder.
 */
export class BeefTaskProvider implements vscode.TaskProvider {
    async provideTasks(): Promise<vscode.Task[]> {
        const tasks: vscode.Task[] = [];
        for (const folder of vscode.workspace.workspaceFolders ?? []) {
            const workspace = await findBeefWorkspace(folder);
            if (!workspace) {
                continue;
            }
            const buildTool = findBuildTool(workspace);
            if (!buildTool) {
                continue;
            }
            for (const combination of workspace.combinations) {
                for (const verb of ['build', 'run', 'test', 'clean'] as BeefTaskVerb[]) {
                    tasks.push(createTask(workspace, buildTool, verb, combination));
                }
            }
        }
        return tasks;
    }

    async resolveTask(task: vscode.Task): Promise<vscode.Task | undefined> {
        const definition = task.definition as BeefTaskDefinition;
        if (definition.type !== BEEF_TASK_TYPE || !definition.command) {
            return undefined;
        }

        const folder = (typeof task.scope === 'object')
            ? task.scope
            : vscode.workspace.workspaceFolders?.[0];
        if (!folder) {
            return undefined;
        }

        const workspace = await findBeefWorkspace(folder);
        if (!workspace) {
            return undefined;
        }
        const buildTool = findBuildTool(workspace);
        if (!buildTool) {
            return undefined;
        }

        const combination = pickCombination(workspace, definition.config, definition.platform);
        const resolved = createTask(workspace, buildTool, definition.command, combination);
        if (task.name && task.name !== beefTaskName(definition.command, combination)) {
            resolved.name = task.name;
        }
        return resolved;
    }
}
