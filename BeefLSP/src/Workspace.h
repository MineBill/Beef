#pragma once

#include <string>
#include <vector>

namespace beflsp
{

struct ProjectInfo
{
	std::string mName;
	std::string mDir;
	std::string mStartupObject;
	// BfTargetType; defaults to BeefLib like the IDE does when a project has no
	// StartupObject (see Project.GetDefaultTargetType).
	int mTargetType = 2;
	bool mDisabled = false;
	std::vector<std::string> mSources;      // normalized absolute paths
	std::vector<std::string> mDependencies; // project names
};

// Reads the workspace description (BeefSpace.toml + BeefProj.toml files) from
// disk and enumerates the sources that make up the compilation.
class Workspace
{
public:
	bool Discover(const std::string& rootPath, std::string& outError);

	const std::string& GetRootPath() const { return mRootPath; }
	const std::string& GetSpaceFile() const { return mSpaceFile; }
	const std::vector<ProjectInfo>& GetProjects() const { return mProjects; }

	const ProjectInfo* FindProject(const std::string& name) const;
	const ProjectInfo* FindProjectForFile(const std::string& filePath) const;

private:
	bool LoadWorkspaceFile(const std::string& spaceFile, std::string& outError);
	bool ScanForProjectFiles(const std::string& dir);
	bool LoadProject(const std::string& name, const std::string& dir, const std::string& spaceDir);
	ProjectInfo* FindOrAddProject(const std::string& name);
	void ResolveDependencyProject(const std::string& name, const std::string& spaceDir);
	std::string FindWorkspaceFile(const std::string& rootPath) const;
	std::string FindDependencyDir(const std::string& name) const;

	std::string mRootPath;
	std::string mSpaceFile;
	std::vector<ProjectInfo> mProjects;
};

}
