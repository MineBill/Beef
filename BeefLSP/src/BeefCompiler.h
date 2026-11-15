#pragma once

#include <map>
#include <string>
#include <vector>

#include "Workspace.h"

namespace Beefy
{
class BfSystem;
class BfCompiler;
class BfProject;
class BfParser;
}

namespace beflsp
{

// Owns a BfSystem/BfCompiler pair and drives the same resolve passes the IDE
// runs: load every project source, resolve the whole workspace once, then run
// single-file autocomplete passes on top of that resolved state.
class BeefCompilerHost
{
public:
	~BeefCompilerHost();

	bool Initialize(const Workspace& workspace, std::string& outError);
	bool IsReady() const { return mCompiled; }

	// 'cursorOffset' is a byte offset into 'text'. On success 'outInfo' holds
	// the raw line protocol produced by BfCompiler::GenerateAutocompleteInfo.
	bool Autocomplete(const std::string& filePath, const std::string& text, int cursorOffset,
		std::string& outInfo, std::string& outError);

private:
	Beefy::BfProject* GetOrCreateProject(const ProjectInfo& projectInfo);
	bool LoadSource(Beefy::BfProject* bfProject, const std::string& path, const std::string& text);
	bool CompileAll(std::string& outError);
	bool RunAutocompletePass(const std::string& filePath, const std::string& text, int cursorOffset, std::string& outInfo);
	bool RefreshFile(const std::string& filePath, const std::string& text);
	const ProjectInfo* GetProjectForFile(const std::string& filePath) const;

	Beefy::BfSystem* mSystem = NULL;
	Beefy::BfCompiler* mCompiler = NULL;
	bool mCompiled = false;
	bool mRefreshing = false;

	std::map<std::string, Beefy::BfProject*> mBfProjects;
	std::map<std::string, const ProjectInfo*> mProjectByDir;
	std::vector<std::string> mProjectDirsByLength;

	// Last parser and source text that were fed into the type graph per file.
	std::map<std::string, Beefy::BfParser*> mFileParsers;
	std::map<std::string, std::string> mResolvedTexts;
};

}
