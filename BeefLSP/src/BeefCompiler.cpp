#include "BeefCompiler.h"

#include "BeefApi.h"
#include "Log.h"
#include "LspText.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

using namespace Beefy;

namespace beflsp
{

using Clock = std::chrono::steady_clock;

static int ElapsedMs(Clock::time_point start)
{
	return (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

static const int MAX_DIAGNOSTICS = 500;

// Reads the errors/warnings the compiler recorded for one pass. 'onlyFile'
// limits the result to a single path ("" keeps everything); 'texts' is used to
// translate byte offsets into LSP positions when the source is known.
static void CollectPassDiagnostics(BfPassInstance* passInstance, const std::string& onlyFile,
	const std::map<std::string, std::string>& texts, std::vector<Diagnostic>& outDiagnostics)
{
	int errorCount = BfPassInstance_GetErrorCount(passInstance);
	for (int idx = 0; idx < errorCount; idx++)
	{
		if ((int)outDiagnostics.size() >= MAX_DIAGNOSTICS)
			break;

		int code = 0;
		bool isWarning = false;
		bool isAfter = false;
		bool isDeferred = false;
		bool isPersistent = false;
		BfWhileSpecializingFlags isWhileSpecializing = BfWhileSpecializingFlag_None;
		char* projectName = NULL;
		char* fileName = NULL;
		int srcStart = -1;
		int srcEnd = -1;
		int line = -1;
		int column = -1;
		int moreInfoCount = 0;

		const char* message = BfPassInstance_GetErrorData(passInstance, idx, code, isWarning, isAfter, isDeferred,
			isWhileSpecializing, isPersistent, projectName, fileName, srcStart, srcEnd, &line, &column, moreInfoCount);

		if ((message == NULL) || (fileName == NULL))
			continue;

		std::string file = fileName;
		if (!onlyFile.empty() && (file != onlyFile))
			continue;

		Diagnostic diagnostic;
		diagnostic.mFile = file;
		diagnostic.mMessage = message;
		diagnostic.mIsWarning = isWarning;
		diagnostic.mCode = code;

		auto textItr = texts.find(file);
		const std::string* text = (textItr != texts.end()) ? &textItr->second : NULL;
		if ((text != NULL) && (srcStart >= 0) && (srcStart <= (int)text->size()))
		{
			int endOffset = (srcEnd > srcStart) ? srcEnd : (srcStart + 1);
			if (endOffset > (int)text->size())
				endOffset = (int)text->size();
			PositionFromOffset(*text, srcStart, diagnostic.mLine, diagnostic.mColumn);
			PositionFromOffset(*text, endOffset, diagnostic.mEndLine, diagnostic.mEndColumn);
		}
		else if ((line >= 0) && (column >= 0))
		{
			// Without the source we can only trust the compiler's line/byte
			// column, which is exact for ASCII.
			diagnostic.mLine = line;
			diagnostic.mColumn = column;
			diagnostic.mEndLine = line;
			diagnostic.mEndColumn = column + 1;
		}
		else
		{
			continue;
		}

		outDiagnostics.push_back(std::move(diagnostic));
	}
}

static bool ReadTextFile(const std::string& path, std::string& outText){
	std::ifstream stream(path, std::ios::in | std::ios::binary);
	if (!stream.is_open())
		return false;

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	outText = buffer.str();

	// The Beef parser tolerates a missing trailing newline, but it makes error
	// recovery (and therefore autocomplete at the end of a file) more reliable.
	if (outText.empty() || (outText.back() != '\n'))
		outText += '\n';

	return true;
}

static const char* GetTargetTriple()
{
#ifdef _WIN32
	return "x86_64-pc-windows-msvc";
#else
	return "x86_64-unknown-linux-gnu";
#endif
}

// The macros the IDE always defines for the target (see
// IDEApp.GetBeefPreprocessorMacros). Without them, platform-specific `#if`
// blocks are skipped and their types appear to be missing.
static std::string GetPreprocessorMacros()
{
	std::string macros = "BF_MACHINE_X64";
#ifdef _WIN32
	macros += "\nBF_PLATFORM_WINDOWS";
#else
	macros += "\nBF_PLATFORM_LINUX";
#endif
	return macros;
}

// The compiler reports definition locations as (line, byte column). Converts
// that to a byte offset in 'text'.
static int ByteColumnToOffset(const std::string& text, int line, int byteColumn)
{
	size_t pos = 0;
	for (int idx = 0; idx < line; idx++)
	{
		size_t newLine = text.find('\n', pos);
		if (newLine == std::string::npos)
			return (int)text.size();
		pos = newLine + 1;
	}
	if (byteColumn < 0)
		byteColumn = 0;
	size_t offset = pos + (size_t)byteColumn;
	if (offset > text.size())
		offset = text.size();
	return (int)offset;
}

// Parses the `defLoc\tfile\tline\tchar` lines produced for
// BfResolveType_GoToDefinition.
static void ParseDefinitionLocations(const std::string& info, const std::map<std::string, std::string>& texts,
	Beefy::BfCompiler* compiler, std::vector<DefinitionLocation>& outLocations)
{
	size_t start = 0;
	while (start < info.size())
	{
		size_t end = info.find('\n', start);
		if (end == std::string::npos)
			end = info.size();
		std::string line = info.substr(start, end - start);
		start = end + 1;

		if (line.compare(0, 7, "defLoc\t") != 0)
			continue;

		size_t firstTab = line.find('\t', 7);
		size_t secondTab = (firstTab == std::string::npos) ? std::string::npos : line.find('\t', firstTab + 1);
		if (secondTab == std::string::npos)
			continue;

		DefinitionLocation location;
		location.mFile = line.substr(7, firstTab - 7);
		int lineNum = atoi(line.substr(firstTab + 1, secondTab - firstTab - 1).c_str());
		int byteColumn = atoi(line.substr(secondTab + 1).c_str());

		bool isEmitFile = (location.mFile.compare(0, 6, "$Emit$") == 0);
		location.mIsEmit = isEmitFile;

		std::string emitText;
		auto textItr = texts.find(location.mFile);
		const std::string* text = (textItr != texts.end()) ? &textItr->second : NULL;
		if ((text == NULL) && isEmitFile && (compiler != NULL))
		{
			// Emitted sources only exist inside the compiler.
			const char* source = BfCompiler_GetEmitSource(compiler, location.mFile.c_str());
			if ((source != NULL) && (source[0] != 0))
			{
				emitText = source;
				text = &emitText;
			}
		}

		if (text != NULL)
		{
			int startOffset = ByteColumnToOffset(*text, lineNum, byteColumn);
			int endOffset = startOffset + 1;
			if (endOffset > (int)text->size())
				endOffset = (int)text->size();
			PositionFromOffset(*text, startOffset, location.mLine, location.mColumn);
			PositionFromOffset(*text, endOffset, location.mEndLine, location.mEndColumn);
		}
		else
		{
			location.mLine = lineNum;
			location.mColumn = byteColumn;
			location.mEndLine = lineNum;
			location.mEndColumn = byteColumn + 1;
		}

		bool duplicate = false;
		for (const auto& existing : outLocations)
		{
			if ((existing.mFile == location.mFile) && (existing.mLine == location.mLine) && (existing.mColumn == location.mColumn))
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			outLocations.push_back(std::move(location));
	}
}

// Parses the emit regions out of the BfCompiler_GetCollapseRegions payload:
//   +<project:TypeName>                          registers an embed id
//   t<id>,<charIdx>,<startLine>,<endLine>,<rev>  generated type body
//   m...                                         generated method body
static void ParseEmitRegions(const std::string& regions, const std::string& text, std::vector<EmitRegion>& outRegions)
{
	std::vector<std::string> typeNames;
	size_t start = 0;
	while (start < regions.size())
	{
		size_t end = regions.find('\n', start);
		if (end == std::string::npos)
			end = regions.size();
		std::string line = regions.substr(start, end - start);
		start = end + 1;

		if (line.empty())
			continue;

		if (line[0] == '+')
		{
			typeNames.push_back(line.substr(1));
			continue;
		}

		if ((line[0] != 't') && (line[0] != 'm'))
			continue;

		int embedId = 0;
		int charIdx = 0;
		int startLine = 0;
		int endLine = 0;
		int textVersion = 0;
		if (sscanf(line.c_str() + 1, "%d,%d,%d,%d,%d", &embedId, &charIdx, &startLine, &endLine, &textVersion) != 5)
			continue;
		if ((embedId < 0) || (embedId >= (int)typeNames.size()))
			continue;

		EmitRegion region;
		region.mTypeName = typeNames[embedId];
		int column = 0;
		if ((charIdx >= 0) && (charIdx <= (int)text.size()))
			PositionFromOffset(text, charIdx, region.mLine, column);
		else
			continue;
		region.mEmitStartLine = startLine;
		region.mEmitEndLine = endLine;

		bool duplicate = false;
		for (const auto& existing : outRegions)
		{
			if ((existing.mTypeName == region.mTypeName) && (existing.mLine == region.mLine) && (existing.mEmitStartLine == region.mEmitStartLine))
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			outRegions.push_back(std::move(region));
	}

	// Keep the regions ordered by source line.
	std::sort(outRegions.begin(), outRegions.end(), [](const EmitRegion& lhs, const EmitRegion& rhs)
		{
			return lhs.mLine < rhs.mLine;
		});
}

BeefCompilerHost::~BeefCompilerHost()
{
	// The compiler holds a pointer back into the system, so it has to go first.	if (mCompiler != NULL)
	{
		BfCompiler_Delete(mCompiler);
		mCompiler = NULL;
	}
	if (mSystem != NULL)
	{
		BfSystem_Delete(mSystem);
		mSystem = NULL;
	}
}

Beefy::BfProject* BeefCompilerHost::GetOrCreateProject(const ProjectInfo& projectInfo)
{
	auto itr = mBfProjects.find(projectInfo.mName);
	if (itr != mBfProjects.end())
		return itr->second;

	BfProject* bfProject = BfSystem_CreateProject(mSystem, projectInfo.mName.c_str(), projectInfo.mDir.c_str());
	BfProject_SetOptions(bfProject, projectInfo.mTargetType, projectInfo.mStartupObject.c_str(), GetPreprocessorMacros().c_str(),
		0 /*optLevel*/, 0 /*ltoType*/, 0 /*relocType*/, 0 /*picLevel*/, BfProjectFlags_None);

	mBfProjects[projectInfo.mName] = bfProject;
	mProjectByDir[projectInfo.mDir] = &projectInfo;
	mProjectDirsByLength.push_back(projectInfo.mDir);
	std::sort(mProjectDirsByLength.begin(), mProjectDirsByLength.end(),
		[](const std::string& lhs, const std::string& rhs) { return lhs.size() > rhs.size(); });

	return bfProject;
}

bool BeefCompilerHost::LoadSource(BfProject* bfProject, const std::string& path, const std::string& text)
{
	BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);

	auto prevItr = mFileParsers.find(path);
	BfParser* parser = BfSystem_CreateParser(mSystem, bfProject);
	if (prevItr != mFileParsers.end())
		BfParser_SetNextRevision(prevItr->second, parser);

	BfParser_SetSource(parser, text.c_str(), (int)text.size(), path.c_str(), -1);

	bool worked = BfParser_Parse(parser, passInstance, false);
	worked = BfParser_Reduce(parser, passInstance) && worked;

	BfSystem_Lock(mSystem, 0);
	// Passing a NULL resolve pass data makes the def builder publish the file's
	// type definitions into the system (this is the project-load pass; the
	// autocomplete pass below does the opposite).
	worked = BfParser_BuildDefs(parser, passInstance, NULL, false) && worked;
	BfSystem_Unlock(mSystem);

	BfPassInstance_Delete(passInstance);

	mFileParsers[path] = parser;
	mResolvedTexts[path] = text;
	mRegionStrings.erase(path);

	if (!worked)
		Log("Parse/defs reported problems in %s (continuing)", path.c_str());

	return true;
}

bool BeefCompilerHost::CompileAll(std::string& outError)
{
	(void)outError;

	auto start = Clock::now();
	BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);

	// A resolve-only compiler expects a resolve pass data to be active even for
	// a whole-workspace resolve (BfModule::ResolveTypeResult dereferences it).
	// This mirrors the IDE's ResolveAllCommand, which classifies with
	// BfResolveType_Classify and lets BfCompiler_ClassifySource run the compile.
	BfResolvePassData* resolvePassData = BfParser_CreateResolvePassData(NULL, BfResolveType_Classify, false);

	BfSystem_Lock(mSystem, 0);
	BfCompiler_ClassifySource(mCompiler, passInstance, resolvePassData);
	BfSystem_Unlock(mSystem);

	mCompileDiagnostics.clear();
	CollectPassDiagnostics(passInstance, "", mResolvedTexts, mCompileDiagnostics);

	BfResolvePassData_Delete(resolvePassData);
	BfPassInstance_Delete(passInstance);

	BfSystem_RemoveOldParsers(mSystem);
	BfSystem_RemoveOldData(mSystem);

	Log("Workspace resolve took %d ms (%d diagnostic(s))", ElapsedMs(start), (int)mCompileDiagnostics.size());

	return true;
}

bool BeefCompilerHost::Initialize(const Workspace& workspace, std::string& outError)
{
	// A resolve without System.Object dereferences core types inside the
	// compiler, so refuse to compile when corlib is missing instead of
	// crashing the server. BeefBuild always adds corlib implicitly, and so does
	// Workspace::Discover.
	const ProjectInfo* corlibProject = workspace.FindProject("corlib");
	if ((corlibProject == NULL) || corlibProject->mSources.empty())
	{
		outError = "corlib was not found, so the workspace cannot be resolved. Add it to "
			"BeefSpace.toml (corlib = {Path = \"<Beef>/BeefLibs/corlib\"}) or set BEEF_PATH "
			"to the Beef installation directory.";
		Log("%s", outError.c_str());
		return false;
	}

	// Registers the LLVM targets/process-wide compiler state. The IDE does this
	// in IDEApp's constructor, before any BfSystem exists.
	IDEHelper_ProgramStart();

	mSystem = BfSystem_Create();
	if (mSystem == NULL)
	{
		outError = "Failed to create the Beef compiler system";
		return false;
	}

	mSystem->mPtrSize = 8;

	// Create the projects first so dependencies can be wired by name.
	for (const auto& projectInfo : workspace.GetProjects())
	{
		if (projectInfo.mDir.empty())
			continue;
		GetOrCreateProject(projectInfo);
	}

	for (const auto& projectInfo : workspace.GetProjects())
	{
		auto itr = mBfProjects.find(projectInfo.mName);
		if (itr == mBfProjects.end())
			continue;

		for (const auto& depName : projectInfo.mDependencies)
		{
			auto depItr = mBfProjects.find(depName);
			if (depItr == mBfProjects.end())
				continue;
			BfProject_AddDependency(itr->second, depItr->second);
		}
	}

	mCompiler = BfSystem_CreateCompiler(mSystem, true /*isResolveOnly*/);

	int workerCount = (int)std::thread::hardware_concurrency();
	if (workerCount < 2)
		workerCount = 2;
	if (workerCount > 8)
		workerCount = 8;

	BfCompiler_SetOptions(mCompiler, NULL, 0, GetTargetTriple(), "", 0 /*toolsetType*/, 0 /*simdSetting*/,
		0 /*allocStackCount*/, workerCount, (BfCompilerOptionFlags)0, NULL, NULL);

	int loadedCount = 0;
	for (const auto& projectInfo : workspace.GetProjects())
	{
		auto itr = mBfProjects.find(projectInfo.mName);
		if (itr == mBfProjects.end())
			continue;

		for (const auto& sourcePath : projectInfo.mSources)
		{
			std::string text;
			if (!ReadTextFile(sourcePath, text))
			{
				Log("Could not read %s", sourcePath.c_str());
				continue;
			}

			LoadSource(itr->second, sourcePath, text);
			loadedCount++;
		}
	}

	Log("Loaded %d sources, resolving...", loadedCount);

	std::string compileMessage;
	CompileAll(compileMessage);

	mCompiled = true;

	if (loadedCount == 0)
	{
		outError = "No Beef sources were loaded from the workspace";
		return false;
	}

	return true;
}

const ProjectInfo* BeefCompilerHost::GetProjectForFile(const std::string& filePath) const
{
	for (const auto& dir : mProjectDirsByLength)
	{
		if (filePath.size() < dir.size())
			continue;
		if (filePath.compare(0, dir.size(), dir) != 0)
			continue;
		if ((filePath.size() > dir.size()) && (filePath[dir.size()] != '/'))
			continue;
		auto itr = mProjectByDir.find(dir);
		if (itr != mProjectByDir.end())
			return itr->second;
	}
	return NULL;
}

// Re-parses 'filePath' with the current text and publishes its updated type
// definitions, then resolves the workspace again. This is what makes
// completion and diagnostics see types/members that were added since the last
// resolve - the IDE gets the same effect from its background Classify passes.
void BeefCompilerHost::RefreshFile(const std::string& filePath, const std::string& text)
{
	const ProjectInfo* projectInfo = GetProjectForFile(filePath);
	BfProject* bfProject = NULL;
	if (projectInfo != NULL)
	{
		auto itr = mBfProjects.find(projectInfo->mName);
		if (itr != mBfProjects.end())
			bfProject = itr->second;
	}

	Log("Refreshing type graph for %s", filePath.c_str());
	auto start = Clock::now();
	LoadSource(bfProject, filePath, text);

	std::string compileError;
	CompileAll(compileError);

	Log("Refresh of %s took %d ms", GetFileName(filePath).c_str(), ElapsedMs(start));
}

bool BeefCompilerHost::RunAutocompletePass(const std::string& filePath, const std::string& text, int cursorOffset, std::string& outInfo)
{
	outInfo.clear();
	auto start = Clock::now();

	const ProjectInfo* projectInfo = GetProjectForFile(filePath);
	BfProject* bfProject = NULL;
	if (projectInfo != NULL)
	{
		auto itr = mBfProjects.find(projectInfo->mName);
		if (itr != mBfProjects.end())
			bfProject = itr->second;
	}

	BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);
	BfParser* parser = BfSystem_CreateParser(mSystem, bfProject);

	// Same ordering as SourceViewPanel.DoClassify: the classifying flag and the
	// source have to be set before the autocomplete cursor is applied.
	BfParser_SetIsClassifying(parser);
	BfParser_SetSource(parser, text.c_str(), (int)text.size(), filePath.c_str(), -1);
	BfParser_SetAutocomplete(parser, cursorOffset);

	BfResolvePassData* resolvePassData = BfParser_CreateResolvePassData(parser, BfResolveType_Autocomplete, false);

	BfParser_Parse(parser, passInstance, false);
	BfParser_Reduce(parser, passInstance);

	BfSystem_Lock(mSystem, 0);
	BfParser_BuildDefs(parser, passInstance, resolvePassData, false);
	BfParser_CreateClassifier(parser, passInstance, resolvePassData, NULL);
	bool success = BfCompiler_ClassifySource(mCompiler, passInstance, resolvePassData);
	BfParser_FinishClassifier(parser, resolvePassData);
	BfSystem_Unlock(mSystem);

	const char* info = BfCompiler_GetAutocompleteInfo(mCompiler);
	if (info != NULL)
		outInfo = info;

	BfResolvePassData_Delete(resolvePassData);
	BfPassInstance_Delete(passInstance);

	BfSystem_DeleteParser(mSystem, parser);
	BfSystem_RemoveOldParsers(mSystem);

	if (!success)
		Log("Autocomplete classify did not complete for %s", filePath.c_str());

	int elapsed = ElapsedMs(start);
	if (elapsed > 100)
		Log("Autocomplete pass for %s took %d ms", GetFileName(filePath).c_str(), elapsed);

	return true;
}

bool BeefCompilerHost::Autocomplete(const std::string& filePath, const std::string& text, int cursorOffset,
	std::string& outInfo, std::string& outError)
{
	if (!mCompiled || (mCompiler == NULL))
	{
		outError = "The workspace has not been resolved yet";
		return false;
	}

	RunAutocompletePass(filePath, text, cursorOffset, outInfo);

	// Nothing found can simply mean "no members", but it can also mean the
	// current text declares types/members that the resolved workspace does not
	// know about yet. In that case rebuild the file's definitions and retry.
	if (outInfo.empty() && !mRefreshing)
	{
		auto itr = mResolvedTexts.find(filePath);
		if ((itr == mResolvedTexts.end()) || (itr->second != text))
		{
			mRefreshing = true;
			RefreshFile(filePath, text);
			mRefreshing = false;

			RunAutocompletePass(filePath, text, cursorOffset, outInfo);
		}
	}

	return true;
}

bool BeefCompilerHost::GoToDefinition(const std::string& filePath, const std::string& text, int cursorOffset,
	std::vector<DefinitionLocation>& outLocations, std::string& outError)
{
	outLocations.clear();
	if (!mCompiled || (mCompiler == NULL))
	{
		outError = "The workspace has not been resolved yet";
		return false;
	}

	auto runPass = [&](std::vector<DefinitionLocation>& locations) -> bool
	{
		const ProjectInfo* projectInfo = GetProjectForFile(filePath);
		BfProject* bfProject = NULL;
		if (projectInfo != NULL)
		{
			auto itr = mBfProjects.find(projectInfo->mName);
			if (itr != mBfProjects.end())
				bfProject = itr->second;
		}

		BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);
		BfParser* parser = BfSystem_CreateParser(mSystem, bfProject);

		BfParser_SetIsClassifying(parser);
		BfParser_SetSource(parser, text.c_str(), (int)text.size(), filePath.c_str(), -1);
		BfParser_SetAutocomplete(parser, cursorOffset);

		BfResolvePassData* resolvePassData = BfParser_CreateResolvePassData(parser, BfResolveType_GoToDefinition, false);

		BfParser_Parse(parser, passInstance, false);
		BfParser_Reduce(parser, passInstance);

		BfSystem_Lock(mSystem, 0);
		BfParser_BuildDefs(parser, passInstance, resolvePassData, false);
		BfParser_CreateClassifier(parser, passInstance, resolvePassData, NULL);
		bool success = BfCompiler_ClassifySource(mCompiler, passInstance, resolvePassData);
		BfParser_FinishClassifier(parser, resolvePassData);
		BfSystem_Unlock(mSystem);

		const char* info = BfCompiler_GetAutocompleteInfo(mCompiler);
		if (info != NULL)
			ParseDefinitionLocations(info, mResolvedTexts, mCompiler, locations);

		BfResolvePassData_Delete(resolvePassData);
		BfPassInstance_Delete(passInstance);
		BfSystem_DeleteParser(mSystem, parser);
		BfSystem_RemoveOldParsers(mSystem);
		return success;
	};

	bool success = runPass(outLocations);

	// Same reasoning as completion: the document may declare types the resolved
	// workspace does not know about yet.
	if (outLocations.empty() && !mRefreshing)
	{
		auto itr = mResolvedTexts.find(filePath);
		if ((itr == mResolvedTexts.end()) || (itr->second != text))
		{
			mRefreshing = true;
			RefreshFile(filePath, text);
			mRefreshing = false;
			runPass(outLocations);
		}
	}

	return success;
}

static std::string Trimmed(const std::string& value)
{
	size_t start = 0;
	size_t end = value.size();
	while ((start < end) && isspace((unsigned char)value[start]))
		start++;
	while ((end > start) && isspace((unsigned char)value[end - 1]))
		end--;
	return value.substr(start, end - start);
}

// Turns a doc comment into plain markdown: drops the `///` prefixes and the
// structural tags, keeps the prose.
static std::string CleanDocComment(const std::string& docs)
{
	std::string result;
	size_t start = 0;
	bool pendingBlank = false;
	while (start <= docs.size())
	{
		size_t end = docs.find('\n', start);
		if (end == std::string::npos)
			end = docs.size();
		std::string line = docs.substr(start, end - start);
		start = end + 1;

		std::string trimmed = Trimmed(line);
		if (trimmed.compare(0, 3, "///") == 0)
			trimmed = Trimmed(trimmed.substr(3));
		else if (!trimmed.empty() && (trimmed[0] == '*'))
			trimmed = Trimmed(trimmed.substr(1));

		if (trimmed.empty())
		{
			pendingBlank = !result.empty();
			continue;
		}

		if ((trimmed.front() == '<') && (trimmed.back() == '>'))
			continue;

		if (!result.empty())
			result += pendingBlank ? "\n\n" : "\n";
		pendingBlank = false;
		result += trimmed;
	}
	return result;
}

bool BeefCompilerHost::Hover(const std::string& filePath, const std::string& text, int cursorOffset, std::string& outMarkdown)
{
	outMarkdown.clear();
	if (!mCompiled || (mCompiler == NULL))
		return false;

	const ProjectInfo* projectInfo = GetProjectForFile(filePath);
	BfProject* bfProject = NULL;
	if (projectInfo != NULL)
	{
		auto itr = mBfProjects.find(projectInfo->mName);
		if (itr != mBfProjects.end())
			bfProject = itr->second;
	}

	BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);
	BfParser* parser = BfSystem_CreateParser(mSystem, bfProject);

	BfParser_SetIsClassifying(parser);
	BfParser_SetSource(parser, text.c_str(), (int)text.size(), filePath.c_str(), -1);
	BfParser_SetAutocomplete(parser, cursorOffset);

	// GetResultString is what the IDE's mouseover uses: ":" + signature,
	// optionally followed by "\x03" + documentation.
	BfResolvePassData* resolvePassData = BfParser_CreateResolvePassData(parser, BfResolveType_GetResultString, false);

	BfParser_Parse(parser, passInstance, false);
	BfParser_Reduce(parser, passInstance);

	BfSystem_Lock(mSystem, 0);
	BfParser_BuildDefs(parser, passInstance, resolvePassData, false);
	BfParser_CreateClassifier(parser, passInstance, resolvePassData, NULL);
	BfCompiler_ClassifySource(mCompiler, passInstance, resolvePassData);
	BfParser_FinishClassifier(parser, resolvePassData);
	BfSystem_Unlock(mSystem);

	const char* info = BfCompiler_GetAutocompleteInfo(mCompiler);
	std::string resultInfo = (info != NULL) ? info : "";

	BfResolvePassData_Delete(resolvePassData);
	BfPassInstance_Delete(passInstance);
	BfSystem_DeleteParser(mSystem, parser);
	BfSystem_RemoveOldParsers(mSystem);

	if (resultInfo.empty())
		return false;

	size_t docsPos = resultInfo.find('\x03');
	std::string signaturePart = (docsPos == std::string::npos) ? resultInfo : resultInfo.substr(0, docsPos);
	std::string docsPart = (docsPos == std::string::npos) ? "" : resultInfo.substr(docsPos + 1);
	for (char& c : docsPart)
	{
		if (c == '\x03')
			c = '\n';
	}

	std::string signature;
	std::string typeKind;
	size_t start = 0;
	while (start < signaturePart.size())
	{
		size_t end = signaturePart.find('\n', start);
		if (end == std::string::npos)
			end = signaturePart.size();
		std::string line = signaturePart.substr(start, end - start);
		start = end + 1;

		if (line.empty())
			continue;
		if (line[0] == ':')
			line = line.substr(1);
		if (line.compare(0, 5, "type\t") == 0)
		{
			typeKind = line.substr(5);
			continue;
		}
		if (!signature.empty())
			signature += '\n';
		signature += line;
	}

	std::string markdown;
	if (!signature.empty())
	{
		markdown += "```beef\n";
		markdown += signature;
		markdown += "\n```";
	}
	if (!typeKind.empty())
	{
		if (!markdown.empty())
			markdown += "\n\n";
		markdown += "*" + typeKind + "*";
	}
	std::string docs = CleanDocComment(docsPart);
	if (!docs.empty())
	{
		if (!markdown.empty())
			markdown += "\n\n";
		markdown += docs;
	}

	outMarkdown = markdown;
	return !outMarkdown.empty();
}

bool BeefCompilerHost::GetRegionString(const std::string& filePath, std::string& outRegions)
{
	auto cachedItr = mRegionStrings.find(filePath);
	if (cachedItr != mRegionStrings.end())
	{
		outRegions = cachedItr->second;
		return true;
	}

	if (!mCompiled || (mCompiler == NULL))
		return false;

	auto parserItr = mFileParsers.find(filePath);
	if (parserItr == mFileParsers.end())
		return false;

	BfResolvePassData* resolvePassData = BfParser_CreateResolvePassData(parserItr->second, BfResolveType_Classify, false);
	char explicitTypeNames[1] = { 0 };

	BfSystem_Lock(mSystem, 0);
	const char* regionString = BfCompiler_GetCollapseRegions(mCompiler, parserItr->second, resolvePassData, explicitTypeNames);
	outRegions = (regionString != NULL) ? regionString : "";
	BfSystem_Unlock(mSystem);

	BfResolvePassData_Delete(resolvePassData);

	mRegionStrings[filePath] = outRegions;
	return true;
}

bool BeefCompilerHost::GetEmitRegions(const std::string& filePath, std::vector<EmitRegion>& outRegions)
{
	outRegions.clear();

	std::string regions;
	if (!GetRegionString(filePath, regions))
		return false;

	auto textItr = mResolvedTexts.find(filePath);
	if (textItr == mResolvedTexts.end())
		return false;

	ParseEmitRegions(regions, textItr->second, outRegions);
	return true;
}

bool BeefCompilerHost::GetFoldingRanges(const std::string& filePath, std::vector<FoldingRange>& outRanges)
{
	outRanges.clear();

	std::string regions;
	if (!GetRegionString(filePath, regions))
		return false;

	auto textItr = mResolvedTexts.find(filePath);
	if (textItr == mResolvedTexts.end())
		return false;
	const std::string& text = textItr->second;

	size_t start = 0;
	while (start < regions.size())
	{
		size_t end = regions.find('\n', start);
		if (end == std::string::npos)
			end = regions.size();
		std::string line = regions.substr(start, end - start);
		start = end + 1;

		if (line.empty())
			continue;

		char kind = line[0];
		if ((kind == '+') || (kind == '~') || (kind == 't') || (kind == 'm'))
			continue;

		int startIdx = 0;
		int endIdx = 0;
		if (sscanf(line.c_str() + 1, "%d,%d", &startIdx, &endIdx) != 2)
			continue;
		if ((startIdx < 0) || (endIdx <= startIdx) || (endIdx > (int)text.size()))
			continue;

		FoldingRange range;
		int column = 0;
		PositionFromOffset(text, startIdx, range.mStartLine, column);
		PositionFromOffset(text, endIdx, range.mEndLine, column);
		if (range.mEndLine <= range.mStartLine)
			continue;

		switch (kind)
		{
		case 'C': range.mKind = "comment"; break;
		case 'R': range.mKind = "region"; break;
		case 'N': range.mKind = "region"; break;
		case 'U': range.mKind = "imports"; break;
		default: break;
		}

		outRanges.push_back(std::move(range));
	}

	std::sort(outRanges.begin(), outRanges.end(), [](const FoldingRange& lhs, const FoldingRange& rhs)
		{
			if (lhs.mStartLine != rhs.mStartLine)
				return lhs.mStartLine < rhs.mStartLine;
			return lhs.mEndLine < rhs.mEndLine;
		});
	outRanges.erase(std::unique(outRanges.begin(), outRanges.end(), [](const FoldingRange& lhs, const FoldingRange& rhs)
		{
			return (lhs.mStartLine == rhs.mStartLine) && (lhs.mEndLine == rhs.mEndLine);
		}), outRanges.end());

	return true;
}

bool BeefCompilerHost::GetEmitSource(const std::string& typeName, std::string& outText, int& outVersion)
{
	outText.clear();
	outVersion = -1;

	if (!mCompiled || (mCompiler == NULL))
		return false;

	std::string fileName = "$Emit$";
	fileName += typeName;

	int version = BfCompiler_GetEmitSourceVersion(mCompiler, fileName.c_str());
	const char* text = BfCompiler_GetEmitSource(mCompiler, fileName.c_str());
	if ((text == NULL) || (text[0] == 0))
	{
		Log("No generated source for '%s' (version %d)", fileName.c_str(), version);
		return false;
	}

	outText = text;
	outVersion = version;
	return true;
}

bool BeefCompilerHost::GetEmitOrigin(const std::string& typeName, int emitLine, DefinitionLocation& outLocation)
{
	outLocation = DefinitionLocation();

	if (!mCompiled || (mCompiler == NULL))
		return false;

	int embedLine = 0;
	int embedLineChar = 0;
	unsigned long long hash = 0;
	const char* originFile = BfCompiler_GetEmitLocation(mCompiler, typeName.c_str(), emitLine, embedLine, embedLineChar, hash);
	if ((originFile == NULL) || (originFile[0] == 0))
		return false;

	outLocation.mFile = originFile;

	auto textItr = mResolvedTexts.find(outLocation.mFile);
	if (textItr != mResolvedTexts.end())
	{
		const std::string& text = textItr->second;
		int startOffset = ByteColumnToOffset(text, embedLine, embedLineChar);
		int endOffset = startOffset + 1;
		if (endOffset > (int)text.size())
			endOffset = (int)text.size();
		PositionFromOffset(text, startOffset, outLocation.mLine, outLocation.mColumn);
		PositionFromOffset(text, endOffset, outLocation.mEndLine, outLocation.mEndColumn);
	}
	else
	{
		outLocation.mLine = embedLine;
		outLocation.mColumn = embedLineChar;
		outLocation.mEndLine = embedLine;
		outLocation.mEndColumn = embedLineChar + 1;
	}

	return true;
}

void BeefCompilerHost::AnalyzeFile(const std::string& filePath, const std::string& text, std::vector<Diagnostic>& outDiagnostics)
{
	outDiagnostics.clear();
	if (mSystem == NULL)
		return;

	auto start = Clock::now();

	const ProjectInfo* projectInfo = GetProjectForFile(filePath);
	BfProject* bfProject = NULL;
	if (projectInfo != NULL)
	{
		auto itr = mBfProjects.find(projectInfo->mName);
		if (itr != mBfProjects.end())
			bfProject = itr->second;
	}

	BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);
	BfParser* parser = BfSystem_CreateParser(mSystem, bfProject);
	BfParser_SetSource(parser, text.c_str(), (int)text.size(), filePath.c_str(), -1);
	BfParser_Parse(parser, passInstance, false);
	BfParser_Reduce(parser, passInstance);

	std::map<std::string, std::string> texts;
	texts[filePath] = text;
	CollectPassDiagnostics(passInstance, filePath, texts, outDiagnostics);

	BfPassInstance_Delete(passInstance);
	BfSystem_DeleteParser(mSystem, parser);
	BfSystem_RemoveOldParsers(mSystem);

	int elapsed = ElapsedMs(start);
	if (elapsed > 40)
		Log("Analyze %s took %d ms (%d diagnostic(s))", GetFileName(filePath).c_str(), elapsed, (int)outDiagnostics.size());
}

}
