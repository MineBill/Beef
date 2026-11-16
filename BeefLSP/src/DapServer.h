#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Beefy
{

class Breakpoint;
class Json;

} // namespace Beefy

struct BfpFile;

namespace beflsp
{

// Minimal Debug Adapter Protocol server on top of the Beef debugger
// (IDEHelper/DebugManager.cpp). VS Code spawns one `beeflsp --debug-adapter`
// process per debug session, so the debugger singletons are per session.
class DapServer
{
public:
	DapServer();
	~DapServer();

	int Run();

private:
	// --- protocol -----------------------------------------------------------
	void HandleMessage(const std::string& body);
	void HandleRequest(Beefy::Json* message);
	void SendResponse(Beefy::Json* request, Beefy::Json* body);
	void SendErrorResponse(Beefy::Json* request, int code, const std::string& message);
	void SendEvent(const char* eventName, Beefy::Json* body);

	void HandleInitialize(Beefy::Json* request, Beefy::Json* arguments);
	void HandleLaunch(Beefy::Json* request, Beefy::Json* arguments);
	void HandleAttach(Beefy::Json* request, Beefy::Json* arguments);
	void HandleConfigurationDone(Beefy::Json* request);
	void HandleSetBreakpoints(Beefy::Json* request, Beefy::Json* arguments);
	void HandleThreads(Beefy::Json* request);
	void HandleStackTrace(Beefy::Json* request, Beefy::Json* arguments);
	void HandleScopes(Beefy::Json* request, Beefy::Json* arguments);
	void HandleVariables(Beefy::Json* request, Beefy::Json* arguments);
	void HandleEvaluate(Beefy::Json* request, Beefy::Json* arguments);
	void HandleContinue(Beefy::Json* request);
	void HandleNext(Beefy::Json* request);
	void HandleStepIn(Beefy::Json* request);
	void HandleStepOut(Beefy::Json* request);
	void HandlePause(Beefy::Json* request);
	void HandleDisconnect(Beefy::Json* request);
	void HandleTerminate(Beefy::Json* request);

	// --- debugger -----------------------------------------------------------
	bool InitializeDebugger(std::string& outError);
	void ShutdownDebugger();
	bool LaunchTarget(const std::string& targetPath, const std::string& args, const std::string& workingDir,
		const std::string& sdkPath, std::string& outError);
	void PumpLoop();
	void DrainMessages();
	void TryStartStdReaders();
	void ReadStdPipe(BfpFile* file, const char* category);
	void UpdateRunState();
	void EnqueueResume(std::function<void()> command, const char* reason);
	bool IsPaused() const;
	void SendOutput(const std::string& category, const std::string& text);

	// --- frames / variables -------------------------------------------------
	struct FrameInfo
	{
		int mThreadId = 0;
		int mIndex = 0;
		std::string mName;
		std::string mFile;
		int mLine = -1;
		int mColumn = -1;
		bool mHasSource = false;
	};

	struct VarRef
	{
		bool mLocals = false;
		int mThreadId = 0;
		int mFrameIdx = 0;
		std::string mExpr;
	};

	int AllocateFrameId(const FrameInfo& frame);
	bool GetFrame(int frameId, FrameInfo& outFrame);
	int AllocateVarRef(const VarRef& varRef);
	bool GetVarRef(int varRef, VarRef& outVarRef);

	Beefy::Json* CreateVariable(const std::string& name, const std::string& value, const std::string& type,
		const VarRef& varRef, bool expandable);

private:
	std::mutex mWriteMutex;
	std::mutex mDebugMutex;
	std::thread mPumpThread;
	std::thread mStdOutThread;
	std::thread mStdErrThread;
	std::atomic<bool> mPumpStop;
	std::atomic<bool> mStdStop;
	std::atomic<bool> mTerminated;
	BfpFile* mStdOutFile = NULL;
	BfpFile* mStdErrFile = NULL;

	bool mInitialized = false;
	bool mLaunched = false;
	bool mAttaching = false;
	int mLastRunState = 0;
	int mPrevLoggedState = -1;
	bool mWasPaused = false;
	bool mSentTerminated = false;
	bool mStopOnEntry = false;
	bool mFirstStop = true;
	std::string mLastStopReason;
	std::string mSdkPath;

	std::map<std::string, std::vector<Beefy::Breakpoint*>> mBreakpoints;

	std::mutex mFramesMutex;
	std::map<int, FrameInfo> mFrames;
	int mNextFrameId = 1;

	std::mutex mVarsMutex;
	std::map<int, VarRef> mVarRefs;
	int mNextVarRef = 1;

	// Resume commands run on the pump thread so they are applied in the same
	// order as Debugger_Update, exactly like the IDE's own debug loop.
	std::mutex mResumeMutex;
	struct PendingResume
	{
		std::function<void()> mCommand;
		std::string mReason;
	};
	std::deque<PendingResume> mPendingResumes;
};

} // namespace beflsp
