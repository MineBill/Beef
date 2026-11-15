#include "Workspace.h"
#include "Log.h"
#include "LspText.h"

#include "toml/toml.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace beflsp
{

using namespace std::filesystem;
using path_t = std::filesystem::path;

static bool EndsWith(const std::string& str, const std::string& suffix)
{
	return (str.size() >= suffix.size()) &&
		(str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
}

static int ParseTargetType(const std::string& targetType)
{
	// Mirrors BfTargetType in IDHelper/Compiler/BfSystem.h.
	if (targetType == "BeefConsoleApplication")
		return 0;
	if ((targetType == "BeefWindowsApplication") || (targetType == "BeefGUIApplication"))
		return 1;
	if ((targetType == "BeefLib") || (targetType == "BeefLib_StaticLib") || (targetType == "BeefLib_DynamicLib") ||
		(targetType == "BeefLib_Static") || (targetType == "BeefLib_Dynamic"))
		return 2;
	if (targetType == "CustomBuild")
		return 3;
	if (targetType == "BeefTest")
		return 4;
	if (targetType == "C_ConsoleApplication")
		return 5;
	if ((targetType == "C_WindowsApplication") || (targetType == "C_GUIApplication"))
		return 6;
	if (targetType == "BeefApplication_StaticLib")
		return 7;
	if (targetType == "BeefApplication_DynamicLib")
		return 8;
	return 2; // BeefLib
}

static bool IsSkippedDir(const std::string& name)
{
	if (name.empty())
		return false;
	if (name[0] == '.')
		return true;
	return (name == "obj") || (name == "build") || (name == "bin") || (name == "temp");
}

static bool IsPathPrefix(const std::string& dir, const std::string& path)
{
	if (path.size() <= dir.size())
		return false;
	if (path.compare(0, dir.size(), dir) != 0)
		return false;
	return path[dir.size()] == '/';
}

static void CollectSources(const std::string& dir, std::vector<std::string>& outSources)
{
	std::error_code ec;
	directory_iterator itr(path_t(dir), ec);
	if (ec)
		return;

	std::vector<std::string> subDirs;
	for (const auto& entry : itr)
	{
		std::string name = entry.path().filename().generic_string();
		if (entry.is_directory(ec))
		{
			if (!IsSkippedDir(name))
				subDirs.push_back(entry.path().generic_string());
		}
		else if (entry.is_regular_file(ec) && EndsWith(name, ".bf"))
		{
			outSources.push_back(NormalizePath(entry.path().generic_string()));
		}
	}

	for (const auto& subDir : subDirs)
		CollectSources(subDir, outSources);
}

std::string Workspace::FindWorkspaceFile(const std::string& rootPath) const
{
	std::string dir = rootPath;
	for (int depth = 0; depth < 8; depth++)
	{
		std::string candidate = JoinPath(dir, "BeefSpace.toml");
		if (PathExists(candidate))
			return candidate;

		std::string parent = GetDirectory(dir);
		if (parent.empty() || (parent == dir))
			break;
		dir = parent;
	}
	return "";
}

ProjectInfo* Workspace::FindOrAddProject(const std::string& name)
{
	for (auto& project : mProjects)
		if (project.mName == name)
			return &project;

	ProjectInfo info;
	info.mName = name;
	mProjects.push_back(info);
	return &mProjects.back();
}

bool Workspace::Discover(const std::string& rootPath, std::string& outError)
{
	mRootPath = NormalizePath(rootPath);
	mProjects.clear();
	mSpaceFile.clear();

	if (!IsDirectory(mRootPath))
	{
		outError = "Workspace root is not a directory: " + mRootPath;
		return false;
	}

	mSpaceFile = FindWorkspaceFile(mRootPath);
	if (!mSpaceFile.empty())
	{
		Log("Using workspace file %s", mSpaceFile.c_str());
		if (!LoadWorkspaceFile(mSpaceFile, outError))
			return false;
	}
	else
	{
		Log("No BeefSpace.toml found, scanning %s for BeefProj.toml", mRootPath.c_str());
		ScanForProjectFiles(mRootPath);
	}

	// Make sure every referenced dependency has a project entry, so that the
	// compiler can resolve names across projects (corlib in particular).
	std::string spaceDir = mSpaceFile.empty() ? mRootPath : GetDirectory(mSpaceFile);
	for (size_t i = 0; i < mProjects.size(); i++)
	{
		std::vector<std::string> deps = mProjects[i].mDependencies;
		for (const auto& depName : deps)
			ResolveDependencyProject(depName, spaceDir);
	}

	// BeefBuild always compiles against corlib, even when neither the workspace
	// nor the project file mentions it (the CLI project templates omit it), so
	// make it an implicit dependency of every project.
	std::string corlibDir = FindDependencyDir("corlib");
	if (!corlibDir.empty() && (FindProject("corlib") == NULL))
		LoadProject("corlib", corlibDir, spaceDir);
	for (auto& project : mProjects)
	{
		if (project.mName == "corlib")
			continue;
		if (std::find(project.mDependencies.begin(), project.mDependencies.end(), std::string("corlib")) == project.mDependencies.end())
			project.mDependencies.push_back("corlib");
	}

	// Collect sources now that all project directories are known.
	for (auto& project : mProjects)
	{
		if (project.mDir.empty() || !IsDirectory(project.mDir))
		{
			project.mDisabled = true;
			continue;
		}

		std::vector<std::string> sources;
		CollectSources(project.mDir, sources);
		for (const auto& extraSource : project.mSources)
		{
			if (std::find(sources.begin(), sources.end(), extraSource) == sources.end())
				sources.push_back(extraSource);
		}

		std::sort(sources.begin(), sources.end());
		sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
		project.mSources = std::move(sources);
	}

	// A workspace can nest project directories (or list a parent directory as a
	// project); make sure each source belongs to exactly one BfProject so the
	// compiler does not see two definitions of the same type.
	for (auto& project : mProjects)
	{
		std::vector<std::string> filtered;
		for (const auto& source : project.mSources)
		{
			bool ownedByOther = false;
			for (const auto& other : mProjects)
			{
				if (&other == &project)
					continue;
				if (IsPathPrefix(other.mDir, source))
				{
					ownedByOther = true;
					break;
				}
			}
			if (!ownedByOther)
				filtered.push_back(source);
		}
		project.mSources = std::move(filtered);
	}

	// Only Beef projects contribute type definitions. C/C++ and custom-build
	// projects (IDEHelper, BeefySysLib, Debugger64, ...) are part of the
	// workspace but must not be fed to the Beef compiler.
	mProjects.erase(std::remove_if(mProjects.begin(), mProjects.end(),
		[](const ProjectInfo& project)
		{
			return (project.mTargetType == 3) || (project.mTargetType >= 5);
		}), mProjects.end());

	size_t sourceCount = 0;
	for (const auto& project : mProjects)
	{
		sourceCount += project.mSources.size();
		Log("Project '%s': %d Beef sources, target type %d", project.mName.c_str(), (int)project.mSources.size(), project.mTargetType);
	}
	Log("Workspace: %d projects, %d sources", (int)mProjects.size(), (int)sourceCount);

	if (mProjects.empty())
	{
		outError = "No Beef projects found under " + mRootPath;
		return false;
	}

	return true;
}

bool Workspace::LoadWorkspaceFile(const std::string& spaceFile, std::string& outError)
{
	toml::ParseResult parseResult = toml::parseFile(spaceFile);
	if (!parseResult.valid())
	{
		outError = "Failed to parse " + spaceFile + ": " + parseResult.errorReason;
		return false;
	}

	const toml::Value& root = parseResult.value;
	std::string spaceDir = GetDirectory(spaceFile);

	const toml::Value* projects = root.find("Projects");
	if ((projects == NULL) || !projects->is<toml::Table>())
	{
		ScanForProjectFiles(spaceDir);
		return true;
	}

	for (const auto& kv : projects->as<toml::Table>())
	{
		std::string projectName = kv.first;
		std::string projectPath = ".";
		bool havePath = false;
		if (kv.second.is<toml::Table>())
		{
			const toml::Value* pathValue = kv.second.find("Path");
			if ((pathValue != NULL) && pathValue->is<std::string>())
			{
				projectPath = pathValue->as<std::string>();
				havePath = true;
			}
		}

		std::string projectDir;
		if (havePath)
		{
			projectDir = JoinPath(spaceDir, projectPath);
		}
		else
		{
			// Entries such as `Beefy2D = "*"` only carry a version; find the
			// project through the conventional layout instead.
			projectDir = FindDependencyDir(projectName);
		}

		if (projectDir.empty() || !PathExists(projectDir))
		{
			Log("Workspace project '%s' could not be located", projectName.c_str());
			continue;
		}

		LoadProject(projectName, projectDir, spaceDir);
	}

	return true;
}

bool Workspace::ScanForProjectFiles(const std::string& dir)
{
	// Look for BeefProj.toml in the root and one or two levels below it, which
	// covers the typical "src project somewhere under the opened folder" case.
	std::vector<std::string> candidates;
	std::error_code ec;

	if (PathExists(JoinPath(dir, "BeefProj.toml")))
		candidates.push_back(dir);

	directory_iterator itr(path_t(dir), ec);
	if (!ec)
	{
		for (const auto& entry : itr)
		{
			if (!entry.is_directory(ec))
				continue;
			std::string name = entry.path().filename().generic_string();
			if (IsSkippedDir(name))
				continue;
			if (PathExists(entry.path().generic_string() + "/BeefProj.toml"))
				candidates.push_back(NormalizePath(entry.path().generic_string()));
		}
	}

	if (candidates.empty() && PathExists(JoinPath(dir, "BeefProj.toml")))
		candidates.push_back(dir);

	std::string spaceDir = dir;
	for (const auto& projectDir : candidates)
	{
		std::string name = GetFileName(projectDir);
		LoadProject(name, projectDir, spaceDir);
	}

	return true;
}

bool Workspace::LoadProject(const std::string& name, const std::string& dir, const std::string& spaceDir)
{
	(void)spaceDir;

	std::string normalizedDir = NormalizePath(dir);
	ProjectInfo* existing = FindOrAddProject(name);
	if (!existing->mDir.empty())
		return true; // already loaded

	existing->mDir = normalizedDir;

	std::string projFile = JoinPath(normalizedDir, "BeefProj.toml");
	if (!PathExists(projFile))
	{
		Log("Project '%s' has no BeefProj.toml (%s)", name.c_str(), normalizedDir.c_str());
		return true;
	}

	toml::ParseResult parseResult = toml::parseFile(projFile);
	if (!parseResult.valid())
	{
		Log("Failed to parse %s: %s", projFile.c_str(), parseResult.errorReason.c_str());
		return true;
	}

	const toml::Value& root = parseResult.value;

	const toml::Value* project = root.find("Project");
	if ((project != NULL) && project->is<toml::Table>())
	{
		const toml::Value* projName = project->find("Name");
		if ((projName != NULL) && projName->is<std::string>())
		{
			// Keep the workspace's name; it is what dependencies refer to. The
			// declared name is normally identical.
			std::string realName = projName->as<std::string>();
			if (realName != name)
				Log("Project dir %s declares name '%s' (workspace uses '%s')", normalizedDir.c_str(), realName.c_str(), name.c_str());
		}

		const toml::Value* targetType = project->find("TargetType");
		if ((targetType != NULL) && targetType->is<std::string>())
			existing->mTargetType = ParseTargetType(targetType->as<std::string>());

		const toml::Value* startupObject = project->find("StartupObject");
		if ((startupObject != NULL) && startupObject->is<std::string>())
			existing->mStartupObject = startupObject->as<std::string>();
	}

	const toml::Value* dependencies = root.find("Dependencies");
	if ((dependencies != NULL) && dependencies->is<toml::Table>())
	{
		for (const auto& kv : dependencies->as<toml::Table>())
			existing->mDependencies.push_back(kv.first);
	}

	const toml::Value* projectFolder = root.find("ProjectFolder");
	if ((projectFolder != NULL) && projectFolder->is<toml::Table>())
	{
		const toml::Value* items = projectFolder->find("Items");
		if ((items != NULL) && items->is<toml::Array>())
		{
			for (const auto& item : items->as<toml::Array>())
			{
				if (!item.is<toml::Table>())
					continue;
				const toml::Value* typeValue = item.find("Type");
				const toml::Value* pathValue = item.find("Path");
				if ((typeValue == NULL) || !typeValue->is<std::string>())
					continue;
				if ((pathValue == NULL) || !pathValue->is<std::string>())
					continue;
				if (typeValue->as<std::string>() != "Source")
					continue;

				// C/C++ projects list their .cpp/.h files as source items too;
				// those are not Beef sources.
				if (!EndsWith(pathValue->as<std::string>(), ".bf"))
					continue;

				std::string sourcePath = JoinPath(normalizedDir, pathValue->as<std::string>());
				if (PathExists(sourcePath))
					existing->mSources.push_back(sourcePath);
			}
		}
	}

	return true;
}

std::string Workspace::FindDependencyDir(const std::string& name) const
{
	// Dependencies are usually resolved through the workspace file, but when
	// they are not listed there we look for the conventional source layout
	// (BeefLibs/<name>) next to the workspace, next to the beeflsp executable
	// (which lives in <Beef>/build-lsp/bin for this repository) or in a
	// BEEF_PATH / C:/Beef style installation.
	std::vector<std::string> searchRoots;

	std::string dir = mSpaceFile.empty() ? mRootPath : GetDirectory(mSpaceFile);
	for (int depth = 0; depth < 8; depth++)
	{
		searchRoots.push_back(dir);
		std::string parent = GetDirectory(dir);
		if (parent.empty() || (parent == dir))
			break;
		dir = parent;
	}

	std::string executableDir = GetDirectory(GetExecutablePath());
	if (!executableDir.empty())
	{
		searchRoots.push_back(JoinPath(executableDir, ".."));
		searchRoots.push_back(JoinPath(executableDir, "../.."));
		searchRoots.push_back(JoinPath(executableDir, "../../.."));
	}

	const char* beefPath = getenv("BEEF_PATH");
	if ((beefPath != NULL) && (*beefPath != 0))
		searchRoots.push_back(NormalizePath(beefPath));
#ifdef _WIN32
	searchRoots.push_back("C:/Beef");
#endif

	for (const auto& root : searchRoots)
	{
		std::vector<std::string> candidates =
		{
			JoinPath(root, "BeefLibs/" + name),
			JoinPath(root, name),
		};

		for (const auto& candidate : candidates)
		{
			if (PathExists(JoinPath(candidate, "BeefProj.toml")))
				return candidate;
		}
	}

	return "";
}

void Workspace::ResolveDependencyProject(const std::string& name, const std::string& spaceDir)
{
	if (FindProject(name) != NULL)
		return;

	std::string depDir = FindDependencyDir(name);
	if (depDir.empty())
	{
		Log("Could not locate dependency project '%s'", name.c_str());
		return;
	}

	LoadProject(name, depDir, spaceDir);

	const ProjectInfo* info = FindProject(name);
	if (info == NULL)
		return;

	// Dependencies can pull in more projects.
	std::vector<std::string> subDeps = info->mDependencies;
	for (const auto& subDep : subDeps)
		ResolveDependencyProject(subDep, spaceDir);
}

const ProjectInfo* Workspace::FindProject(const std::string& name) const
{
	for (const auto& project : mProjects)
		if (project.mName == name)
			return &project;
	return NULL;
}

const ProjectInfo* Workspace::FindProjectForFile(const std::string& filePath) const
{
	std::string normalized = NormalizePath(filePath);
	const ProjectInfo* bestMatch = NULL;
	size_t bestLength = 0;
	for (const auto& project : mProjects)
	{
		if (project.mDir.empty())
			continue;
		if (normalized.size() < project.mDir.size())
			continue;
		if (normalized.compare(0, project.mDir.size(), project.mDir) != 0)
			continue;
		if ((normalized.size() > project.mDir.size()) && (normalized[project.mDir.size()] != '/'))
			continue;
		if (project.mDir.size() > bestLength)
		{
			bestLength = project.mDir.size();
			bestMatch = &project;
		}
	}
	return bestMatch;
}

}
