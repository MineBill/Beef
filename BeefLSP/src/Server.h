#pragma once

#include <map>
#include <string>
#include <vector>

#include "BeefCompiler.h"
#include "CompletionParser.h"
#include "Workspace.h"

namespace Beefy
{
class Json;
}

namespace beflsp
{

struct OpenDocument
{
	std::string mUri;
	std::string mPath;
	std::string mText;
	int mVersion = 0;
};

class Server
{
public:
	int Run();

private:
	void Dispatch(Beefy::Json* msg);
	Beefy::Json* HandleRequest(const std::string& method, Beefy::Json* params, Beefy::Json* id, std::string& outError);
	void HandleNotification(const std::string& method, Beefy::Json* params);

	Beefy::Json* HandleInitialize(Beefy::Json* params, std::string& outError);
	Beefy::Json* HandleCompletion(Beefy::Json* params, std::string& outError);
	Beefy::Json* HandleCompletionResolve(Beefy::Json* params, std::string& outError);

	void HandleDidOpen(Beefy::Json* params);
	void HandleDidChange(Beefy::Json* params);
	void HandleDidClose(Beefy::Json* params);

	bool EnsureWorkspace();
	OpenDocument* FindOrLoadDocument(const std::string& uri);
	void SendNotification(const std::string& method, Beefy::Json* params);
	void SendLogMessage(int type, const std::string& message);

	std::string mRootPath;
	bool mWorkspaceReady = false;
	bool mWorkspaceLoadAttempted = false;
	bool mShutdownRequested = false;
	bool mExitRequested = false;

	Workspace mWorkspace;
	BeefCompilerHost mCompiler;

	std::map<std::string, OpenDocument> mDocuments;
	std::vector<std::string> mResolveDocumentation;
};

}
