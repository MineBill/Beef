using System;
using System.Collections;
using System.IO;
using System.Threading;
using Beefy.widgets;

namespace IDE.ui
{
	// The generic Find dialog. It uses the command palette UI over a filtered list of every file in the
	// workspace. Prefixing the search with '@' switches to searching types instead, and Enter jumps to
	// the type's declaration. Opened with Alt+Shift+L ("Find").
	class FindDialog : PaletteWidget
	{
		class FileInfo
		{
			public ProjectSource mProjectSource;
			public String mRelPath ~ delete _; // Workspace-relative path, also used for matching
			public String mName ~ delete _; // File name, shown as the row label
			public String mDirectory ~ delete _; // Directory portion, shown on the right
		}

		class TypeInfo
		{
			public String mFullName ~ delete _; // Eg "System.Collections.List<>"
			public String mName ~ delete _; // Last segment, shown as the row label
			public String mNamespace ~ delete _; // Everything before the last segment, shown on the right
			public String mFilePath ~ delete _;
			public int32 mLine;
			public int32 mColumn;
		}

		class PendingTypes
		{
			public String mResult ~ delete _;
			public WaitEvent mDoneEvent = new .() ~ delete _;
		}

		class FileEntry : Entry
		{
			public ProjectSource mProjectSource;
		}

		class ClassEntry : Entry
		{
			public String mFilePath ~ delete _;
			public int32 mLine;
			public int32 mColumn;
		}

		static String sLastSearchString = new String() ~ delete _;

		List<FileInfo> mFiles = new .() ~ DeleteContainerAndItems!(_);
		List<TypeInfo> mTypes = new .() ~ DeleteContainerAndItems!(_);
		PendingTypes mPendingTypes ~ delete _;
		bool mSearchingClasses;
		int32 mPendingRevision;
		int32 mLoadedRevision = -1;

		public static void Show()
		{
			if (gApp.mFindDialog != null)
			{
				// Toggle off if it's already open.
				gApp.mFindDialog.Close();
				return;
			}

			var dialog = new FindDialog();
			dialog.Init();
		}

		void Init()
		{
			gApp.mFindDialog = this;
			// The owning window deletes us on close; remember the search and drop the app reference then.
			mOnRemovedFromParent.Add(new (widget, prevParent, widgetWindow) =>
				{
					sLastSearchString.Clear();
					if (mEditWidget != null)
						mEditWidget.GetText(sLastSearchString);
					if (gApp.mFindDialog == this)
						gApp.mFindDialog = null;
				});

			RefreshFiles();
			InitPalette("Find", sLastSearchString);
		}

		public ~this()
		{
			// Don't free mPendingTypes while the compiler thread is still filling it in.
			if (mPendingTypes != null)
				mPendingTypes.mDoneEvent.WaitFor();
		}

		public override void Update()
		{
			base.Update();

			if (mPendingTypes != null)
			{
				if (mPendingTypes.mDoneEvent.WaitFor(0))
				{
					ParseTypeList(mPendingTypes.mResult);
					DeleteAndNullify!(mPendingTypes);
					mLoadedRevision = mPendingRevision;
					Rebuild();
				}
				return;
			}

			// The type list is only needed for '@' searches. Load it once per compile, then filter locally.
			if (!mSearchingClasses)
				return;

			var compiler = gApp.mBfResolveCompiler;
			if ((compiler == null) || (compiler.IsPerformingBackgroundOperation()))
				return;

			int32 revision = compiler.GetCompileRevision();
			if (revision == mLoadedRevision)
				return;

			mPendingRevision = revision;
			mPendingTypes = new .();
			compiler.DoBackground(new => BkgGetTypeList);
		}

		// ---- Files ----

		void RefreshFiles()
		{
			ClearAndDeleteItems(mFiles);

			for (var project in gApp.mWorkspace.mProjects)
				AddFilesFromFolder(project.mRootFolder);
		}

		void AddFilesFromFolder(ProjectFolder folder)
		{
			for (var projectEntry in folder.mChildItems)
			{
				if (var childFolder = projectEntry as ProjectFolder)
				{
					AddFilesFromFolder(childFolder);
					continue;
				}

				var projectSource = projectEntry as ProjectSource;
				if (projectSource == null)
					continue;

				var fileInfo = new FileInfo();
				fileInfo.mProjectSource = projectSource;

				String fullPath = scope String();
				projectSource.GetFullImportPath(fullPath);

				fileInfo.mRelPath = new String();
				gApp.mWorkspace.GetWorkspaceRelativePath(fullPath, fileInfo.mRelPath);

				// Files from dependency projects sit outside the workspace dir, so show those relative
				// to their project instead of as an absolute path.
				if (Path.IsPathRooted(fileInfo.mRelPath))
				{
					String projectRelPath = scope String();
					Path.GetRelativePath(fullPath, projectSource.mProject.mProjectDir, projectRelPath);
					fileInfo.mRelPath.Set(projectSource.mProject.mProjectName);
					fileInfo.mRelPath.Append(Path.DirectorySeparatorChar);
					fileInfo.mRelPath.Append(projectRelPath);
				}

				fileInfo.mName = new String();
				Path.GetFileName(fileInfo.mRelPath, fileInfo.mName);

				fileInfo.mDirectory = new String();
				int slashPos = fileInfo.mRelPath.LastIndexOf(IDEUtils.cNativeSlash);
				if (slashPos != -1)
					fileInfo.mDirectory.Append(fileInfo.mRelPath, 0, slashPos);

				mFiles.Add(fileInfo);
			}
		}

		void RebuildFileEntries(StringView queryView)
		{
			String query = scope String(queryView);

			for (var fileInfo in mFiles)
			{
				int32 score = 0;
				if (!query.IsEmpty)
				{
					if (!FuzzyMatch(query, fileInfo.mRelPath, out score))
						continue;
				}

				var entry = new FileEntry();
				entry.mName = new String(fileInfo.mName);
				entry.mDetail = new String(fileInfo.mDirectory);
				entry.mScore = score;
				entry.mProjectSource = fileInfo.mProjectSource;
				mEntries.Add(entry);
			}
		}

		// ---- Classes ----

		void BkgGetTypeList()
		{
			var bfSystem = gApp.mBfResolveSystem;
			var bfCompiler = gApp.mBfResolveCompiler;

			bfSystem.Lock(0);
			String outStr = scope String();
			bfCompiler.GetTypeDefList(outStr, true);
			bfSystem.Unlock();

			mPendingTypes.mResult = new String(outStr);
			mPendingTypes.mDoneEvent.Set();
		}

		// Each line is a type kind char followed by the internal type name and its source location:
		//   iNamespace.Interface\tfile\tline\tcolumn
		//   cNamespace.Class<,>\tfile\tline\tcolumn
		//   vNamespace.ValueType\tfile\tline\tcolumn
		// Projects ('+', '='), globals ('g') and member lines are skipped.
		void ParseTypeList(StringView typeList)
		{
			ClearAndDeleteItems(mTypes);

			for (var line in typeList.Split('\n'))
			{
				if (line.Length == 0)
					continue;

				var fields = line.Split('\t');
				StringView nameField = fields.GetNext().Get();
				char8 kind = nameField[0];
				if ((kind != 'i') && (kind != 'c') && (kind != 'v'))
					continue;

				var typeInfo = new TypeInfo();
				typeInfo.mFullName = new String(nameField.Substring(1));
				// Internal names use '+' between outer and inner types.
				typeInfo.mFullName.Replace('+', '.');

				if (fields.GetNext() case .Ok(let filePath))
				{
					typeInfo.mFilePath = new String(filePath);
					if ((fields.GetNext() case .Ok(let lineStr)) && (lineStr.Length != 0))
						typeInfo.mLine = int32.Parse(lineStr).Get();
					if (fields.GetNext() case .Ok(let columnStr))
						typeInfo.mColumn = int32.Parse(columnStr).Get();
				}

				int dotPos = typeInfo.mFullName.LastIndexOf('.');
				typeInfo.mName = new String();
				typeInfo.mNamespace = new String();
				if (dotPos == -1)
					typeInfo.mName.Append(typeInfo.mFullName);
				else
				{
					typeInfo.mName.Append(typeInfo.mFullName, dotPos + 1);
					typeInfo.mNamespace.Append(typeInfo.mFullName.Substring(0, dotPos));
				}

				mTypes.Add(typeInfo);
			}
		}

		void RebuildClassEntries(StringView queryView)
		{
			String query = scope String(queryView);
			query.Trim();

			for (var typeInfo in mTypes)
			{
				int32 score = 0;
				if (!query.IsEmpty)
				{
					if (!FuzzyMatch(query, typeInfo.mFullName, out score))
						continue;
				}

				var entry = new ClassEntry();
				entry.mName = new String(typeInfo.mName);
				entry.mDetail = new String(typeInfo.mNamespace);
				entry.mScore = score;
				if (typeInfo.mFilePath != null)
					entry.mFilePath = new String(typeInfo.mFilePath);
				entry.mLine = typeInfo.mLine;
				entry.mColumn = typeInfo.mColumn;
				mEntries.Add(entry);
			}
		}

		// ---- Results ----

		protected override void RebuildEntries(StringView queryView)
		{
			if ((queryView.Length > 0) && (queryView[0] == '@'))
			{
				mSearchingClasses = true;
				RebuildClassEntries(queryView.Substring(1));
			}
			else
			{
				mSearchingClasses = false;
				RebuildFileEntries(queryView);
			}
		}

		protected override void RunEntry(Entry entry)
		{
			if (var fileEntry = entry as FileEntry)
			{
				if (fileEntry.mProjectSource != null)
				{
					gApp.RecordHistoryLocation(true);
					gApp.ShowProjectItem(fileEntry.mProjectSource, false);
				}
				return;
			}

			if (var classEntry = entry as ClassEntry)
			{
				if (classEntry.mFilePath == null)
					return;

				gApp.RecordHistoryLocation(true);
				gApp.ShowSourceFileLocation(classEntry.mFilePath, -1, -1, classEntry.mLine, classEntry.mColumn, .Always);
			}
		}
	}
}
