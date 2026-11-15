#include "BeefCompiler.h"

#include "BeefApi.h"
#include "Log.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace Beefy;

namespace beflsp
{

static bool ReadTextFile(const std::string& path, std::string& outText)
{
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

BeefCompilerHost::~BeefCompilerHost()
{
	// The compiler holds a pointer back into the system, so it has to go first.
	if (mCompiler != NULL)
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
	BfProject_SetOptions(bfProject, projectInfo.mTargetType, projectInfo.mStartupObject.c_str(), "",
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

	if (!worked)
		Log("Parse/defs reported problems in %s (continuing)", path.c_str());

	return true;
}

bool BeefCompilerHost::CompileAll(std::string& outError)
{
	(void)outError;

	BfPassInstance* passInstance = BfSystem_CreatePassInstance(mSystem);

	// A resolve-only compiler expects a resolve pass data to be active even for
	// a whole-workspace resolve (BfModule::ResolveTypeResult dereferences it).
	// This mirrors the IDE's ResolveAllCommand, which classifies with
	// BfResolveType_Classify and lets BfCompiler_ClassifySource run the compile.
	BfResolvePassData* resolvePassData = BfParser_CreateResolvePassData(NULL, BfResolveType_Classify, false);

	BfSystem_Lock(mSystem, 0);
	BfCompiler_ClassifySource(mCompiler, passInstance, resolvePassData);
	BfSystem_Unlock(mSystem);

	BfResolvePassData_Delete(resolvePassData);
	BfPassInstance_Delete(passInstance);

	BfSystem_RemoveOldParsers(mSystem);
	BfSystem_RemoveOldData(mSystem);

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
	BfCompiler_SetOptions(mCompiler, NULL, 0, GetTargetTriple(), "", 0 /*toolsetType*/, 0 /*simdSetting*/,
		0 /*allocStackCount*/, 1 /*maxWorkerThreads*/, (BfCompilerOptionFlags)0, NULL, NULL);

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
// completion see types/members that were added since the last resolve - the
// IDE gets the same effect from its background Classify passes.
bool BeefCompilerHost::RefreshFile(const std::string& filePath, const std::string& text)
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
	LoadSource(bfProject, filePath, text);

	std::string compileError;
	CompileAll(compileError);

	return true;
}

bool BeefCompilerHost::RunAutocompletePass(const std::string& filePath, const std::string& text, int cursorOffset, std::string& outInfo)
{
	outInfo.clear();

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

}
