using System;
using System.Collections;
using System.Diagnostics;
using System.IO;
using System.Threading;
using Beefy;
using Beefy.widgets;
using Beefy.theme.dark;
using IDE.Compiler;
using IDE.util;

namespace IDE.ui
{
	/// A disposable, in-memory Beef program with Run/Stop controls. Scratch buffers are never
	/// saved as part of a workspace; on Run the buffer is written to a private temp project and
	/// compiled/run through BeefBuild so it is completely isolated from the open workspace.
	public class ScratchPanel : SourceViewPanel
	{
		public String mScratchTitle ~ delete _;
		public String mScratchDir ~ delete _;

		SpawnedProcess mProcess;
		Thread mOutThread;
		Thread mErrThread;
		Monitor mOutputMonitor = new .() ~ delete _;
		List<String> mPendingOutput = new .() ~ DeleteContainerAndItems!(_);
		bool mRunning;
		bool mCleanedUp;

		Project mScratchResolveProject;
		ProjectSource mScratchResolveSource;
		List<String> mDependencies = new .() ~ DeleteContainerAndItems!(_);

		public this()
		{

		}

		public override bool HasUnsavedChanges()
		{
			return false;
		}

		public override bool FileNameMatches(String fileName)
		{
			if ((mFilePath == null) || (fileName == null))
				return false;

			String thisPath = scope String(mFilePath);
			IDEUtils.FixFilePath(thisPath);
			String otherPath = scope String(fileName);
			IDEUtils.FixFilePath(otherPath);
			return Path.Equals(thisPath, otherPath);
		}

		public void Init(StringView title)
		{
			mScratchTitle = new String(title);
			mScratchDir = new String();
			if (Path.GetTempFileName(mScratchDir) case .Err)
			{
				mScratchDir.AppendF("beef_scratch_{0}", Internal.GetTickCountMicro());
			}
			else
			{
				File.Delete(mScratchDir).IgnoreError();
			}
			mScratchDir.Append("_scratch");

			String srcDir = scope String()..Append(mScratchDir, "/src");
			Directory.CreateDirectory(srcDir).IgnoreError();

			// Set up a normal (untitled) source editor, then mark it as Beef so token
			// classification/coloring works without needing a workspace project.
			Show(null, false, null);
			mIsScratch = true;
			mIsSourceCode = true;
			mIsBeefSource = true;
			mFilePath = new String();
			mFilePath.Append(srcDir);
			mFilePath.Append("/Program.bf");

			mEditWidget.mEditWidgetContent.AppendText(
				"""
				using System;

				// Scratch programs use Scratch.Program as the entry point.
				namespace Scratch
				{
					class Program
					{
						static void Main()
						{
							Console.WriteLine("Hello, Beef!");
						}
					}
				}
				""");

			WriteScratchSourceFile();

			mPanelHeader = new PanelHeader();
			mPanelHeader.Label = mScratchTitle;

			var runButton = mPanelHeader.AddButton("Run");
			runButton.mOnMouseClick.Add(new (evt) => RunScratch());

			var stopButton = mPanelHeader.AddButton("Stop");
			stopButton.mOnMouseClick.Add(new (evt) => StopScratch());

			var depsButton = mPanelHeader.AddButton("Deps");
			depsButton.mOnMouseClick.Add(new (evt) => ShowDependencyMenu(depsButton));

			AddWidget(mPanelHeader);
			ResizeComponents();

			DoFastClassify();
			InitResolveProject();
			QueueFullRefresh(true);
		}

		public override void Update()
		{
			base.Update();

			FlushOutput();
			CheckProcessDone();
		}

		public override void Dispose()
		{
			if (mCleanedUp)
			{
				base.Dispose();
				return;
			}
			mCleanedUp = true;

			KillProcess();
			RemoveResolveProject();
			base.Dispose();

			if (mScratchDir != null)
				Directory.DelTree(mScratchDir).IgnoreError();
		}

		void SetStatus(StringView status)
		{
			if (mPanelHeader == null)
				return;

			if (status.IsEmpty)
				mPanelHeader.mLabel.Set(mScratchTitle);
			else
				mPanelHeader.mLabel.Set(scope String()..AppendF("{0}  [{1}]", mScratchTitle, status));
		}

		bool IsDependencySelected(StringView projectName)
		{
			for (var dep in mDependencies)
			{
				if (dep == projectName)
					return true;
			}
			return false;
		}

		void ShowDependencyMenu(Widget anchor)
		{
			Menu menu = new Menu();
			bool hadProjects = false;

			for (var project in gApp.mWorkspace.mProjects)
			{
				if (project == mScratchResolveProject)
					continue;

				hadProjects = true;
				bool selected = IsDependencySelected(project.mProjectName);

				String label = scope String();
				label.Append(selected ? "[x] " : "[ ] ");
				label.Append(project.mProjectName);

				Project depProject = project;
				var item = menu.AddItem(label);
				item.mOnMenuItemSelected.Add(new (evt) => ToggleDependency(depProject.mProjectName));
			}

			if (!hadProjects)
			{
				var item = menu.AddItem("No workspace projects");
				item.SetDisabled(true);
			}

			var menuWidget = DarkTheme.sDarkTheme.CreateMenuWidget(menu);
			menuWidget.Init(anchor, 0, anchor.mHeight);
		}

		void ToggleDependency(StringView projectName)
		{
			for (int i = 0; i < mDependencies.Count; i++)
			{
				if (mDependencies[i] == projectName)
				{
					delete mDependencies[i];
					mDependencies.RemoveAt(i);
					UpdateResolveDependencies();
					return;
				}
			}

			mDependencies.Add(new String(projectName));
			UpdateResolveDependencies();
		}

		void UpdateResolveDependencies()
		{
			if (mScratchResolveProject == null)
				return;

			var bfSystem = IDEApp.sApp.mBfResolveSystem;
			var bfCompiler = IDEApp.sApp.mBfResolveCompiler;
			if ((bfSystem == null) || (bfCompiler == null))
				return;

			if (!bfSystem.mProjectMap.TryGetValue(mScratchResolveProject, var bfProject))
				return;

			// Dependency changes can't run concurrently with a resolve operation.
			bfCompiler.WaitForBackground();

			bfProject.ClearDependencies();
			AddResolveDependency(bfSystem, bfProject, "corlib");
			for (var dep in mDependencies)
				AddResolveDependency(bfSystem, bfProject, dep);

			QueueFullRefresh(true);
		}

		void AddResolveDependency(BfSystem bfSystem, BfProject bfProject, StringView projectName)
		{
			var project = gApp.mWorkspace.FindProject(projectName);
			if (project == null)
				return;

			if (bfSystem.mProjectMap.TryGetValue(project, var depBfProject))
				bfProject.AddDependency(depBfProject);
		}

		void AddBuildProject(List<Project> projects, Project project)
		{
			if (project == null)
				return;
			if (project.mProjectName == "corlib")
				return;

			for (var checkProject in projects)
			{
				if (checkProject == project)
					return;
			}

			projects.Add(project);
			for (var dep in project.mDependencies)
				AddBuildProject(projects, gApp.mWorkspace.FindProject(dep.mProjectName));
		}

		public void RunScratch()
		{
			if ((mProcess != null) && (mProcess.HasExited))
				CheckProcessDone();

			if (mRunning)
				return;

			if (!WriteScratchProject())
				return;

			String beefBuildPath = scope String();
			GetBeefBuildPath(beefBuildPath);
			if (!File.Exists(beefBuildPath))
			{
				gApp.OutputErrorLine("Unable to locate BeefBuild executable at '{0}'", beefBuildPath);
				return;
			}

			String args = scope String()..Append("-workspace=\"", mScratchDir, "\" -run -config=Debug");

			ProcessStartInfo startInfo = scope ProcessStartInfo();
			startInfo.UseShellExecute = false;
			startInfo.RedirectStandardOutput = true;
			startInfo.RedirectStandardError = true;
			startInfo.CreateNoWindow = true;
			startInfo.SetFileName(beefBuildPath);
			startInfo.SetArguments(args);
			startInfo.SetWorkingDirectory(mScratchDir);

			mProcess = new SpawnedProcess();
			if (mProcess.Start(startInfo) case .Err)
			{
				gApp.OutputErrorLine("Failed to start '{0}'", beefBuildPath);
				delete mProcess;
				mProcess = null;
				return;
			}

			mRunning = true;
			SetStatus("running");

			mOutThread = new Thread(new => ReadOutThread);
			mOutThread.Start(this, false);
			mErrThread = new Thread(new => ReadErrThread);
			mErrThread.Start(this, false);
		}

		public void StopScratch()
		{
			if (mProcess == null)
				return;

			SetStatus("stopping");
			if (!mProcess.HasExited)
				mProcess.Kill(0, .KillChildren);
		}

		void KillProcess()
		{
			if (mProcess != null)
			{
				if (!mProcess.HasExited)
					mProcess.Kill(0, .KillChildren);
				mProcess.WaitFor(-1);
			}

			if (mOutThread != null)
			{
				mOutThread.Join();
				delete mOutThread;
				mOutThread = null;
			}
			if (mErrThread != null)
			{
				mErrThread.Join();
				delete mErrThread;
				mErrThread = null;
			}

			if (mProcess != null)
			{
				mProcess.Close();
				delete mProcess;
				mProcess = null;
			}
			mRunning = false;
		}

		void CheckProcessDone()
		{
			if ((mProcess == null) || (!mProcess.HasExited))
				return;

			if (mOutThread != null)
			{
				mOutThread.Join();
				delete mOutThread;
				mOutThread = null;
			}
			if (mErrThread != null)
			{
				mErrThread.Join();
				delete mErrThread;
				mErrThread = null;
			}

			mProcess.WaitFor(-1);
			mProcess.Close();
			delete mProcess;
			mProcess = null;
			mRunning = false;

			FlushOutput();
			SetStatus("finished");
		}

		static void ReadOutThread(Object obj)
		{
			((ScratchPanel)obj).ReadStream(true);
		}

		static void ReadErrThread(Object obj)
		{
			((ScratchPanel)obj).ReadStream(false);
		}

		void ReadStream(bool isStdOut)
		{
			var process = mProcess;
			if (process == null)
				return;

			FileStream fileStream = scope FileStream();
			if (isStdOut)
			{
				if (process.AttachStandardOutput(fileStream) case .Err)
					return;
			}
			else
			{
				if (process.AttachStandardError(fileStream) case .Err)
					return;
			}

			StreamReader streamReader = scope StreamReader(fileStream, null, false, 4096);
			while (true)
			{
				var line = scope String();
				if (streamReader.ReadLine(line) case .Err)
					break;
				using (mOutputMonitor.Enter())
					mPendingOutput.Add(new String(line));
			}
		}

		void FlushOutput()
		{
			List<String> toOutput = scope List<String>();
			using (mOutputMonitor.Enter())
			{
				for (var line in mPendingOutput)
					toOutput.Add(line);
				mPendingOutput.Clear();
			}

			for (var line in toOutput)
			{
				if (line.IsEmpty)
					gApp.OutputLine("[{0}]", mScratchTitle);
				else
					gApp.OutputLine("[{0}] {1}", mScratchTitle, line);
				delete line;
			}
		}

		void InitResolveProject()
		{
			var bfSystem = IDEApp.sApp.mBfResolveSystem;
			var bfCompiler = IDEApp.sApp.mBfResolveCompiler;
			if ((bfSystem == null) || (bfCompiler == null))
				return;

			bfCompiler.WaitForBackground();

			var project = new Project();
			project.mIsScratch = true;
			project.mProjectName.Set(mScratchTitle);
			project.mProjectName.Replace(" ", "");
			project.mProjectName.Append("Resolve");
			project.mProjectDir.Set(mScratchDir);
			// Use a non-empty project path so this isn't treated as a composite "single file"
			// project (which would require a CompositeFile to be active).
			project.mProjectPath.Set(scope String()..Append(mScratchDir, "/BeefProj.toml"));
			project.mRootFolder.mPath = new String("src");
			project.mGeneralOptions.mTargetType = .BeefConsoleApplication;
			project.mBeefGlobalOptions.mStartupObject.Set("Scratch.Program");
			project.GetOptions(IDEApp.sApp.mConfigName, IDEApp.sApp.mPlatformName, true);

			var projectSource = new ProjectSource();
			projectSource.mIncludeKind = .Manual;
			projectSource.mProject = project;
			projectSource.mParentFolder = project.mRootFolder;
			projectSource.mPath = new String("src/Program.bf");
			projectSource.mName.Set("Program.bf");
			project.mRootFolder.AddChild(projectSource);

			bfSystem.AddProject(project);
			var bfProject = bfSystem.GetBfProject(project);
			bfProject.SetDisabled(false);

			var macros = scope DefinesSet();
			if (IDEApp.sApp.GetCurWorkspaceOptions() != null)
				IDEApp.sApp.GetBeefPreprocessorMacros(macros);

			bfProject.SetOptions(.BeefConsoleApplication, "Scratch.Program", macros.mDefines,
				.O0, .None, .NotSet, .NotSet, false, false, false, false);

			// The resolve system already has the workspace projects registered.
			AddResolveDependency(bfSystem, bfProject, "corlib");
			for (var dep in mDependencies)
				AddResolveDependency(bfSystem, bfProject, dep);

			mScratchResolveProject = project;
			mScratchResolveSource = projectSource;
			mProjectSource = projectSource;

			// Use the canonical path the compiler/project system reports for this source,
			// otherwise go-to-definition/history paths won't match and will open a duplicate tab.
			String fullSourcePath = scope String();
			projectSource.GetFullImportPath(fullSourcePath);
			mFilePath.Set(fullSourcePath);

			// Share our live editor buffer with the resolve edit data so tab switches and
			// classifications keep using this widget instead of creating an empty replacement.
			var editData = IDEApp.sApp.GetEditData(projectSource, false);
			if (editData != null)
			{
				if (editData.mEditWidget == null)
				{
					editData.mEditWidget = mEditWidget;
					editData.mOwnsEditWidget = false;
				}
				mEditData = editData;
			}

			// Give the resolve compiler a parser now. Subsequent classifications parse the
			// live editor buffer directly.
			bfCompiler.QueueProjectSource(projectSource, .None, false);
		}

		void RemoveResolveProject()
		{
			var project = mScratchResolveProject;
			if (project == null)
				return;

			var bfSystem = IDEApp.sApp.mBfResolveSystem;
			var bfCompiler = IDEApp.sApp.mBfResolveCompiler;
			if ((bfSystem != null) && (bfCompiler != null))
			{
				if (mScratchResolveSource != null)
					bfCompiler.QueueProjectSourceRemoved(mScratchResolveSource);
				if (bfSystem.mProjectMap.TryGetValue(project, var bfProject))
				{
					bfProject.SetDisabled(true);
					bfSystem.RemoveBfProject(project);
					bfCompiler.QueueDeleteBfProject(bfProject);
				}
			}

			if (mProjectSource == mScratchResolveSource)
				mProjectSource = null;

			mScratchResolveSource = null;
			mScratchResolveProject = null;
			IDEApp.sApp.IdleDeferDelete(project);
		}

		bool WriteScratchSourceFile()
		{
			String srcDir = scope String()..Append(mScratchDir, "/src");
			if (Directory.CreateDirectory(srcDir) case .Err)
			{
				gApp.OutputErrorLine("Unable to create scratch directory '{0}'", srcDir);
				return false;
			}

			String sourceText = scope String();
			mEditWidget.GetText(sourceText);

			String sourcePath = scope String()..Append(srcDir, "/Program.bf");
			if (File.WriteAllText(sourcePath, sourceText) case .Err)
			{
				gApp.OutputErrorLine("Unable to write scratch source '{0}'", sourcePath);
				return false;
			}
			return true;
		}

		bool WriteScratchProject()
		{
			if (!WriteScratchSourceFile())
				return false;

			String projectToml = scope String();
			projectToml.Append(
				"""
				FileVersion = 1

				[Project]
				Name = "BeefScratch"
				StartupObject = "Scratch.Program"

				[Dependencies]
				corlib = "*"
				""");
			projectToml.Append("\n");

			for (var dep in mDependencies)
			{
				var depProject = gApp.mWorkspace.FindProject(dep);
				if (depProject != null)
					projectToml.AppendF("\"{}\" = \"*\"\n", depProject.mProjectName);
			}

			if (File.WriteAllText(scope String()..Append(mScratchDir, "/BeefProj.toml"), projectToml) case .Err)
			{
				gApp.OutputErrorLine("Unable to write scratch project file");
				return false;
			}

			List<Project> buildProjects = scope List<Project>();
			for (var dep in mDependencies)
				AddBuildProject(buildProjects, gApp.mWorkspace.FindProject(dep));

			String spaceToml = scope String();
			spaceToml.Append("FileVersion = 1\n");
			spaceToml.Append("Projects = {BeefScratch = {Path = \".\"}");

			for (var project in buildProjects)
			{
				String projectDir = scope String(project.mProjectDir);
				projectDir.Replace('\\', '/');
				spaceToml.AppendF(", \"{}\" = {{Path = \"{}\"}}", project.mProjectName, projectDir);
			}

			spaceToml.Append("}\n\n[Workspace]\nStartupProject = \"BeefScratch\"\n");

			if (File.WriteAllText(scope String()..Append(mScratchDir, "/BeefSpace.toml"), spaceToml) case .Err)
			{
				gApp.OutputErrorLine("Unable to write scratch workspace file");
				return false;
			}

			return true;
		}

		static void GetBeefBuildPath(String outPath)
		{
			outPath.Append(gApp.mInstallDir, "BeefBuild");
#if DEBUG
			outPath.Append("_d");
#endif
#if BF_PLATFORM_WINDOWS
			outPath.Append(".exe");
#endif
			if (!File.Exists(outPath))
			{
#if DEBUG
				outPath.Clear();
				outPath.Append(gApp.mInstallDir, "BeefBuild");
#if BF_PLATFORM_WINDOWS
				outPath.Append(".exe");
#endif
#endif
			}
		}
	}
}
