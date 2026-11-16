#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
	struct WorkItem
	{
		enum class Kind
		{
			Startup,
			Analyze,
			Refresh,
			Completion,
			Definition,
			CodeLens,
			EmitSource,
			Hover,
			FoldingRange
		};

		Kind mKind = Kind::Startup;
		std::string mPath;
		std::string mText;
		int mCursorOffset = 0;
		Beefy::Json* mRequestId = NULL; // owned, Completion/Definition only
		int mRequestVersion = 0;
	};

	struct WorkResult
	{
		WorkItem::Kind mKind = WorkItem::Kind::Startup;
		bool mSuccess = false;
		std::string mError;
		std::string mPath;
		std::string mText;
		std::vector<Diagnostic> mDiagnostics;
		std::vector<Diagnostic> mCompileDiagnostics;
		std::string mCompletionInfo;
		std::vector<DefinitionLocation> mLocations;
		std::vector<EmitRegion> mEmitRegions;
		std::string mEmitText;
		int mEmitVersion = -1;
		std::string mHoverMarkdown;
		std::vector<FoldingRange> mFoldingRanges;
		int mCursorOffset = 0;
		Beefy::Json* mRequestId = NULL;
		int mRequestVersion = 0;
	};

	struct PendingEdit
	{
		std::string mText;
		std::chrono::steady_clock::time_point mAnalyzeDue;
		std::chrono::steady_clock::time_point mRefreshDue;
		std::chrono::steady_clock::time_point mLastRefresh;
		bool mAnalyzeQueued = false;
		bool mRefreshQueued = false;
	};

	// --- threading -----------------------------------------------------------
	void StartThreads();
	void StopThreads();
	void ReaderLoop();
	void WorkerLoop();
	Server::WorkResult ExecuteWork(const WorkItem& item);
	void EnqueueWork(WorkItem item, bool highPriority = false);
	void DrainResults();
	void HandleWorkResult(WorkResult& result);
	bool TakeMessage(std::string& body, int timeoutMs);
	bool IsReaderDone();

	// --- dispatch ------------------------------------------------------------
	void Dispatch(Beefy::Json* msg);
	Beefy::Json* HandleRequest(const std::string& method, Beefy::Json* params, Beefy::Json* id, std::string& outError);
	void HandleNotification(const std::string& method, Beefy::Json* params);

	Beefy::Json* HandleInitialize(Beefy::Json* params, std::string& outError);
	OpenDocument* GetRequestDocument(Beefy::Json* params, Beefy::Json* idItem, const char* requestName, int& outOffset);
	void HandleCompletionRequest(Beefy::Json* params, Beefy::Json* idItem);
	void SendCompletionResponse(WorkResult& result);
	void HandleDefinitionRequest(Beefy::Json* params, Beefy::Json* idItem);
	void SendDefinitionResponse(WorkResult& result);
	void HandleCodeLensRequest(Beefy::Json* params, Beefy::Json* idItem);
	void SendCodeLensResponse(WorkResult& result);
	void HandleEmitSourceRequest(Beefy::Json* params, Beefy::Json* idItem);
	void SendEmitSourceResponse(WorkResult& result);
	void HandleHoverRequest(Beefy::Json* params, Beefy::Json* idItem);
	void SendHoverResponse(WorkResult& result);
	void HandleFoldingRangeRequest(Beefy::Json* params, Beefy::Json* idItem);
	void SendFoldingRangeResponse(WorkResult& result);
	Beefy::Json* HandleCompletionResolve(Beefy::Json* params, std::string& outError);

	void HandleDidOpen(Beefy::Json* params);
	void HandleDidChange(Beefy::Json* params);
	void HandleDidSave(Beefy::Json* params);
	void HandleDidClose(Beefy::Json* params);

	// --- scheduling / diagnostics -------------------------------------------
	void EnsureStartup();
	void ScheduleEdit(const std::string& path, const std::string& text, bool immediate, bool scheduleRefresh);
	void RunTimers();
	void UpdateCompileDiagnostics(const std::vector<Diagnostic>& diagnostics);
	void PublishPathDiagnostics(const std::string& path);
	void SendNotification(const std::string& method, Beefy::Json* params);
	void SendRequest(const std::string& method, Beefy::Json* params);
	void SendLogMessage(int type, const std::string& message);

	OpenDocument* FindOrLoadDocument(const std::string& uri);

	// --- state ---------------------------------------------------------------
	std::string mRootPath;
	bool mStartupQueued = false;
	bool mWorkspaceReady = false;
	bool mStartupFailed = false;
	bool mShutdownRequested = false;
	bool mExitRequested = false;
	int mDiagnosticsDelayMs = 300;
	int mNextRequestId = 1000;

	Workspace mWorkspace;
	BeefCompilerHost mCompiler;

	std::map<std::string, OpenDocument> mDocuments;
	std::map<std::string, PendingEdit> mPendingEdits;
	std::vector<std::string> mResolveDocumentation;

	std::map<std::string, std::vector<Diagnostic>> mCompileDiagnosticsByFile;
	std::map<std::string, std::vector<Diagnostic>> mSyntaxDiagnosticsByFile;
	std::map<std::string, std::vector<Diagnostic>> mLastPublishedDiagnostics;
	std::set<std::string> mPublishedDiagnosticPaths;

	// --- threads -------------------------------------------------------------
	std::thread mReaderThread;
	std::thread mWorkerThread;

	std::mutex mMutex;
	std::condition_variable mCv;
	std::deque<std::string> mMessages;
	std::deque<WorkResult> mResults;
	bool mReaderDone = false;

	std::mutex mWorkMutex;
	std::condition_variable mWorkCv;
	std::deque<WorkItem> mWorkQueue;
	bool mWorkerStop = false;
};

}
