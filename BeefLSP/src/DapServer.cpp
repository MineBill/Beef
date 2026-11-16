#include "DapServer.h"

#include "BeefApi.h"
#include "BeefDebugApi.h"
#include "JsonRpc.h"
#include "Log.h"

#include "BeefySysLib/util/Json.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>

using Beefy::Json;

namespace beflsp
{

// --- small helpers ----------------------------------------------------------

static std::string GetString(Json* parent, const char* name, const char* defaultValue = "")
{
	if (parent == NULL)
		return defaultValue;
	Json* item = parent->GetObjectItem(name);
	if ((item == NULL) || (item->mType != Json::Type_String) || (item->mValueString == NULL))
		return defaultValue;
	return item->mValueString;
}

static int GetInt(Json* parent, const char* name, int defaultValue = 0)
{
	if (parent == NULL)
		return defaultValue;
	Json* item = parent->GetObjectItem(name);
	if ((item == NULL) || (item->mType != Json::Type_Number))
		return defaultValue;
	return item->mValueInt;
}

static bool GetBool(Json* parent, const char* name, bool defaultValue = false)
{
	if (parent == NULL)
		return defaultValue;
	Json* item = parent->GetObjectItem(name);
	if (item == NULL)
		return defaultValue;
	return item->mType == Json::Type_True;
}

static Json* GetItem(Json* parent, const char* name)
{
	if (parent == NULL)
		return NULL;
	return parent->GetObjectItem(name);
}

static Json* AddObject(Json* parent, const char* name)
{
	Json* obj = Json::CreateObject();
	parent->AddItemToObject(name, obj);
	return obj;
}

static Json* AddArray(Json* parent, const char* name)
{
	Json* arr = Json::CreateArray();
	parent->AddItemToObject(name, arr);
	return arr;
}

static std::string Trim(const std::string& value)
{
	size_t start = 0;
	size_t end = value.size();
	while ((start < end) && ((unsigned char)value[start] <= ' '))
		start++;
	while ((end > start) && ((unsigned char)value[end - 1] <= ' '))
		end--;
	return value.substr(start, end - start);
}

static std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && (line.back() == '\r'))
			line.pop_back();
		lines.push_back(line);
		start = end + 1;
	}
	return lines;
}

static std::vector<std::string> SplitTabs(const std::string& text)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (true)
	{
		size_t end = text.find('\t', start);
		if (end == std::string::npos)
		{
			parts.push_back(text.substr(start));
			break;
		}
		parts.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return parts;
}

static std::string ToForwardSlashes(const std::string& path)
{
	std::string result = path;
	std::replace(result.begin(), result.end(), '\\', '/');
	return result;
}

static std::string BaseName(const std::string& path)
{
	size_t pos = path.find_last_of("/\\");
	return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// The debugger reports source files with the exact-source hash appended
// (`Program.bf#EF200F8E...`) so old sources can be matched. Editors need the
// plain path.
static std::string StripSourceHash(const std::string& path)
{
	size_t hashPos = path.find_last_of('#');
	if ((hashPos == std::string::npos) || (hashPos + 1 >= path.size()))
		return path;

	std::string hash = path.substr(hashPos + 1);
	if ((hash.size() != 32) && (hash.size() != 64))
		return path;
	for (char c : hash)
	{
		bool isHex = ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'F')) || ((c >= 'a') && (c <= 'f'));
		if (!isHex)
			return path;
	}
	return path.substr(0, hashPos);
}

// Beef format strings (`AppendF` style): {}, {0}, {1:00}, {{ and }}.
static std::string FormatTemplate(const std::string& format, const std::vector<std::string>& args,
	size_t* outArgIndex = NULL)
{
	std::string result;
	size_t nextArg = 0;
	for (size_t i = 0; i < format.size(); i++)
	{
		char c = format[i];
		if (c == '{')
		{
			if ((i + 1 < format.size()) && (format[i + 1] == '{'))
			{
				result += '{';
				i++;
				continue;
			}

			size_t end = format.find('}', i + 1);
			if (end == std::string::npos)
			{
				result += c;
				continue;
			}

			std::string spec = format.substr(i + 1, end - i - 1);
			size_t colonPos = spec.find(':');
			std::string indexPart = (colonPos == std::string::npos) ? spec : spec.substr(0, colonPos);
			indexPart = Trim(indexPart);

			size_t argIndex = 0;
			if (indexPart.empty())
			{
				argIndex = nextArg++;
				if (outArgIndex != NULL)
					*outArgIndex = argIndex;
			}
			else
			{
				argIndex = (size_t)atol(indexPart.c_str());
				if (argIndex >= nextArg)
					nextArg = argIndex + 1;
			}

			if (argIndex < args.size())
				result += args[argIndex];
			i = end;
			continue;
		}

		if (c == '}')
		{
			if ((i + 1 < format.size()) && (format[i + 1] == '}'))
			{
				result += '}';
				i++;
				continue;
			}
		}

		result += c;
	}
	return result;
}

// --- evaluation result ------------------------------------------------------

struct EvalMember
{
	std::string mName;
	std::string mEvalFormat;
	std::string mValue;
	std::string mType;
};

struct EvalResult
{
	std::string mValue;
	std::string mType;
	bool mError = false;

	std::vector<EvalMember> mMembers;

	bool mHasSeries = false;
	int mSeriesStart = 0;
	int mSeriesCount = 0;
	int mSeriesMaxShow = 0;
	std::string mSeriesDisplayFormat;
	std::string mSeriesEvalFormat;
	std::vector<std::string> mSeriesParams;

	std::string mContinuation;

	bool HasChildren() const { return !mMembers.empty() || (mHasSeries && (mSeriesCount > 0)) || !mContinuation.empty(); }
};

static const uint16 EVAL_FLAGS =
	BeefDbgEvalFlag_AllowProperties | BeefDbgEvalFlag_AllowStringView;

static bool EvaluateExpression(const std::string& expression, int frameIdx, EvalResult& outResult)
{
	outResult = EvalResult();
	if (expression.empty())
		return false;

	Beefy::StringView view = Debugger_Evaluate(expression.c_str(), frameIdx, -1, -1, EVAL_FLAGS);
	std::string text;
	if (view.mPtr != NULL)
		text.assign(view.mPtr, (size_t)view.mLength);

	std::vector<std::string> lines = SplitLines(text);
	if (lines.empty())
		return false;

	if (!lines[0].empty() && (lines[0][0] == '!'))
	{
		outResult.mError = true;
		std::vector<std::string> fields = SplitTabs(lines[0].substr(1));
		if (fields.size() >= 3)
			outResult.mValue = fields[2];
		else
			outResult.mValue = lines[0].substr(1);
		return true;
	}

	outResult.mValue = lines[0];
	if (lines.size() > 1)
		outResult.mType = lines[1];

	for (size_t lineIdx = 2; lineIdx < lines.size(); lineIdx++)
	{
		const std::string& line = lines[lineIdx];
		if (line.empty())
			continue;

		std::vector<std::string> fields = SplitTabs(line);
		if (fields.empty())
			continue;

		if (fields[0].empty() || (fields[0][0] != ':'))
		{
			if (fields.size() < 2)
				continue;
			EvalMember member;
			member.mName = fields[0];
			member.mEvalFormat = fields[1];
			if (fields.size() > 3)
			{
				// GDB/LLDB supply the value and type inline.
				member.mValue = fields[2];
				member.mType = fields[3];
			}
			outResult.mMembers.push_back(std::move(member));
			continue;
		}

		const std::string& command = fields[0];
		if (command == ":repeat")
		{
			if (fields.size() < 6)
				continue;
			outResult.mHasSeries = true;
			outResult.mSeriesStart = atoi(fields[1].c_str());
			outResult.mSeriesCount = atoi(fields[2].c_str());
			outResult.mSeriesMaxShow = atoi(fields[3].c_str());
			outResult.mSeriesDisplayFormat = fields[4];
			outResult.mSeriesEvalFormat = fields[5];
			for (size_t fieldIdx = 6; fieldIdx < fields.size(); fieldIdx++)
				outResult.mSeriesParams.push_back(fields[fieldIdx]);
		}
		else if (command == ":continuation")
		{
			if (fields.size() > 1)
				outResult.mContinuation = fields[1];
		}
	}

	return true;
}

// --- DapServer --------------------------------------------------------------

DapServer::DapServer()
{
	mPumpStop = false;
	mTerminated = false;
}

DapServer::~DapServer()
{
	ShutdownDebugger();
}

int DapServer::Run()
{
	Log("DAP: debug adapter starting");

	while (true)
	{
		std::string body;
		if (!ReadLspMessage(body))
			break;

		try
		{
			HandleMessage(body);
		}
		catch (const std::exception& ex)
		{
			Log("DAP: exception while handling message: %s", ex.what());
		}

		if (mTerminated && !mLaunched)
			break;
	}

	ShutdownDebugger();
	Log("DAP: debug adapter stopped");
	return 0;
}

void DapServer::HandleMessage(const std::string& body)
{
	Json* message = Json::Parse(body.c_str());
	if (message == NULL)
	{
		Log("DAP: malformed JSON message");
		return;
	}

	Json* typeItem = message->GetObjectItem("type");
	std::string messageType = ((typeItem != NULL) && (typeItem->mType == Json::Type_String) && (typeItem->mValueString != NULL))
		? typeItem->mValueString : "";
	if (messageType == "request")
	{
		HandleRequest(message);
		return;
	}

	// Responses to requests we never send, and protocol chatter, are ignored.
	delete message;
}

void DapServer::HandleRequest(Json* request)
{
	std::string command = GetString(request, "command");
	Json* arguments = GetItem(request, "arguments");

	Log("DAP request: %s", command.c_str());

	// Commands that reach into the debugger need an open target; asking before
	// that (or crashing inside the C API) must not happen.
	const bool needsSession = (command == "threads") || (command == "stackTrace") ||
		(command == "scopes") || (command == "variables") || (command == "evaluate") ||
		(command == "continue") || (command == "next") || (command == "stepIn") ||
		(command == "stepOut") || (command == "pause") || (command == "setBreakpoints");
	if (needsSession && (!mLaunched))
	{
		SendErrorResponse(request, -32603, "No debug session is active");
		return;
	}

	if (command == "initialize") HandleInitialize(request, arguments);
	else if (command == "launch") HandleLaunch(request, arguments);
	else if (command == "attach") HandleAttach(request, arguments);
	else if (command == "configurationDone") HandleConfigurationDone(request);
	else if (command == "setBreakpoints") HandleSetBreakpoints(request, arguments);
	else if (command == "setExceptionBreakpoints") SendResponse(request, Json::CreateObject());
	else if (command == "threads") HandleThreads(request);
	else if (command == "stackTrace") HandleStackTrace(request, arguments);
	else if (command == "scopes") HandleScopes(request, arguments);
	else if (command == "variables") HandleVariables(request, arguments);
	else if (command == "evaluate") HandleEvaluate(request, arguments);
	else if (command == "continue") HandleContinue(request);
	else if (command == "next") HandleNext(request);
	else if (command == "stepIn") HandleStepIn(request);
	else if (command == "stepOut") HandleStepOut(request);
	else if (command == "pause") HandlePause(request);
	else if (command == "disconnect") HandleDisconnect(request);
	else if (command == "terminate") HandleTerminate(request);
	else SendErrorResponse(request, -32601, "Unsupported command: " + command);
}

void DapServer::SendResponse(Json* request, Json* body)
{
	Json* response = Json::CreateObject();
	response->AddStringToObject("type", "response");
	response->AddNumberToObject("request_seq", GetInt(request, "seq"));
	response->AddBoolToObject("success", true);
	response->AddStringToObject("command", GetString(request, "command").c_str());
	if (body != NULL)
		response->AddItemToObject("body", body);

	std::lock_guard<std::mutex> lock(mWriteMutex);
	WriteLspMessage(response);
}

void DapServer::SendErrorResponse(Json* request, int code, const std::string& message)
{
	Json* response = Json::CreateObject();
	response->AddStringToObject("type", "response");
	response->AddNumberToObject("request_seq", GetInt(request, "seq"));
	response->AddBoolToObject("success", false);
	response->AddStringToObject("command", GetString(request, "command").c_str());
	response->AddStringToObject("message", message.c_str());

	Json* body = AddObject(response, "body");
	Json* error = AddObject(body, "error");
	error->AddNumberToObject("id", code);
	error->AddStringToObject("format", message.c_str());

	std::lock_guard<std::mutex> lock(mWriteMutex);
	WriteLspMessage(response);
}

void DapServer::SendEvent(const char* eventName, Json* body)
{
	Json* event = Json::CreateObject();
	event->AddStringToObject("type", "event");
	event->AddStringToObject("event", eventName);
	if (body != NULL)
		event->AddItemToObject("body", body);

	std::lock_guard<std::mutex> lock(mWriteMutex);
	WriteLspMessage(event);
}

void DapServer::SendOutput(const std::string& category, const std::string& text)
{
	if (text.empty())
		return;
	Json* body = Json::CreateObject();
	body->AddStringToObject("category", category.c_str());
	body->AddStringToObject("output", text.c_str());
	SendEvent("output", body);
}

// --- lifecycle --------------------------------------------------------------

bool DapServer::InitializeDebugger(std::string& outError)
{
	if (mInitialized)
		return true;

	IDEHelper_ProgramStart();
	// The debugger backends read the X86 target from the compiler's target list
	// (`gX86Target`), which normally exists because the compiler created it.
	Targets_Create();
	Debugger_Create();

	// Visualizers and the SDK's gdb pretty printers, the same list the IDE builds.
	if (!mSdkPath.empty())
	{
		std::string visualizers;
		auto addFile = [&visualizers](const std::string& path)
		{
			std::error_code error;
			if (std::filesystem::is_regular_file(path, error))
				visualizers += path + "\n";
		};
		addFile(mSdkPath + "/BeefDbgVis.toml");
		addFile(mSdkPath + "/IDE/dist/BeefDbgVis.toml");
		// Directories (trailing slash) are searched for gdb/*.py pretty printers.
		visualizers += mSdkPath + "/\n";
		visualizers += mSdkPath + "/IDE/dist/";
		Debugger_LoadDebugVisualizers(visualizers.c_str());
	}

	mInitialized = true;
	(void)outError;
	return true;
}

void DapServer::ShutdownDebugger()
{
	mPumpStop = true;
	if (mPumpThread.joinable())
		mPumpThread.join();

	if (!mInitialized)
	{
		mStdStop = true;
		return;
	}

	std::lock_guard<std::mutex> lock(mDebugMutex);

	// The engine requires breakpoints to be removed before the target stops.
	for (auto& entry : mBreakpoints)
	{
		for (auto breakpoint : entry.second)
			Breakpoint_Delete(breakpoint);
	}
	mBreakpoints.clear();

	if (mLaunched)
	{
		Debugger_StopDebugging();
		mLaunched = false;
	}

	// Killing the target closes its stdio; closing our ends as well makes the
	// blocking reads in the reader threads return.
	mStdStop = true;
	if (mStdOutFile != NULL)
	{
		BfpFile_Close(mStdOutFile, NULL);
		mStdOutFile = NULL;
	}
	if (mStdErrFile != NULL)
	{
		BfpFile_Close(mStdErrFile, NULL);
		mStdErrFile = NULL;
	}

	// The reader threads do not touch the debugger, so they can be joined with
	// the lock held.
	if (mStdOutThread.joinable())
		mStdOutThread.join();
	if (mStdErrThread.joinable())
		mStdErrThread.join();

	Debugger_Detach();
	Debugger_Delete();
	Targets_Delete();
	IDEHelper_ProgramDone();
	mInitialized = false;
}

bool DapServer::LaunchTarget(const std::string& targetPath, const std::string& args, const std::string& workingDir,
	const std::string& sdkPath, std::string& outError)
{
	mSdkPath = sdkPath;
	if (!InitializeDebugger(outError))
		return false;

	int openFlags = 1 /* RedirectStdInput */ | 2 /* RedirectStdOutput */ | 4 /* RedirectStdError */ | 8 /* Minimized */;

	std::lock_guard<std::mutex> lock(mDebugMutex);
	if (!Debugger_OpenFile(targetPath.c_str(), targetPath.c_str(), args.c_str(), workingDir.c_str(),
		NULL, 0, false, openFlags))
	{
		outError = "Unable to open '" + targetPath + "'";
		return false;
	}

	mLaunched = true;
	mLastStopReason = "entry";
	mLastRunState = BeefDbgRunState_NotStarted;
	mWasPaused = false;
	mSentTerminated = false;
	mFirstStop = true;

	mPumpStop = false;
	mPumpThread = std::thread([this] { PumpLoop(); });
	return true;
}

/**
 * The stdio pipes only exist once the debugger has launched the target, so they
 * are picked up from the pump (the C API hands each pipe over exactly once).
 */
void DapServer::TryStartStdReaders()
{
	if ((!mLaunched) || (mStdOutFile != NULL) || (mStdErrFile != NULL))
		return;
	if (Debugger_GetProcessId() == 0)
		return;

	BfpFile* stdIn = NULL;
	Debugger_GetStdHandles(&stdIn, &mStdOutFile, &mStdErrFile);
	if (mStdOutFile != NULL)
		mStdOutThread = std::thread([this] { ReadStdPipe(mStdOutFile, "stdout"); });
	if (mStdErrFile != NULL)
		mStdErrThread = std::thread([this] { ReadStdPipe(mStdErrFile, "stderr"); });
}

void DapServer::ReadStdPipe(BfpFile* file, const char* category)
{
	char buffer[4096];
	while (!mStdStop)
	{
		// A blocking read (-1) is unblocked by closing the handle below; pipes
		// do not support the async timeout mode.
		BfpFileResult result = BfpFileResult_Ok;
		intptr readSize = BfpFile_Read(file, buffer, (intptr)sizeof(buffer), -1, &result);
		if (readSize > 0)
		{
			SendOutput(category, std::string(buffer, (size_t)readSize));
		}

		if ((result != BfpFileResult_Ok) && (result != BfpFileResult_PartialData))
			break;
	}
}

void DapServer::HandleInitialize(Json* request, Json* arguments)
{
	(void)arguments;

	Json* body = Json::CreateObject();
	body->AddBoolToObject("supportsConfigurationDoneRequest", true);
	body->AddBoolToObject("supportsTerminateRequest", true);
	body->AddBoolToObject("supportsConditionalBreakpoints", true);
	body->AddBoolToObject("supportsHitConditionalBreakpoints", true);
	body->AddBoolToObject("supportsLogPoints", true);
	body->AddBoolToObject("supportsEvaluateForHovers", true);
	body->AddBoolToObject("supportsRestartRequest", false);
	body->AddBoolToObject("supportsSetVariable", false);
	body->AddBoolToObject("supportsFunctionBreakpoints", false);
	body->AddBoolToObject("supportsDelayedStackTraceLoading", false);
	body->AddNullToObject("exceptionBreakpointFilters");
	SendResponse(request, body);

	SendEvent("initialized", NULL);
}

void DapServer::HandleLaunch(Json* request, Json* arguments)
{
	std::string program = GetString(arguments, "program");
	std::string cwd = GetString(arguments, "cwd");
	std::string sdkPath = GetString(arguments, "sdkPath");
	mStopOnEntry = GetBool(arguments, "stopOnEntry");

	if (program.empty())
	{
		SendErrorResponse(request, -32602, "No program to launch");
		return;
	}

	std::string args;
	Json* argsItem = GetItem(arguments, "args");
	if ((argsItem != NULL) && (argsItem->mType == Json::Type_Array))
	{
		for (int idx = 0; idx < argsItem->GetArraySize(); idx++)
		{
			Json* item = argsItem->GetArrayItem(idx);
			if ((item == NULL) || (item->mType != Json::Type_String) || (item->mValueString == NULL))
				continue;
			if (!args.empty())
				args += " ";
			args += item->mValueString;
		}
	}

	std::string error;
	if (!LaunchTarget(ToForwardSlashes(program), args, cwd, sdkPath, error))
	{
		SendErrorResponse(request, -32603, error);
		return;
	}

	SendResponse(request, NULL);
}

void DapServer::HandleAttach(Json* request, Json* arguments)
{
	(void)arguments;
	SendErrorResponse(request, -32601, "Attaching is not supported yet");
}

void DapServer::HandleConfigurationDone(Json* request)
{
	if (!mLaunched)
	{
		SendErrorResponse(request, -32603, "No target has been launched");
		return;
	}

	mLastStopReason = "entry";
	EnqueueResume([] { Debugger_Run(); }, "entry");
	SendResponse(request, NULL);
}

void DapServer::HandleDisconnect(Json* request)
{
	SendResponse(request, NULL);
	mTerminated = true;
	ShutdownDebugger();
}

void DapServer::HandleTerminate(Json* request)
{
	{
		std::lock_guard<std::mutex> lock(mDebugMutex);
		if (mLaunched)
			Debugger_StopDebugging();
	}
	SendResponse(request, NULL);
}

// --- breakpoints ------------------------------------------------------------

void DapServer::HandleSetBreakpoints(Json* request, Json* arguments)
{
	Json* source = GetItem(arguments, "source");
	std::string path = ToForwardSlashes(GetString(source, "path"));

	Json* body = Json::CreateObject();
	Json* responseBreakpoints = AddArray(body, "breakpoints");

	if (path.empty())
	{
		SendResponse(request, body);
		return;
	}

	std::lock_guard<std::mutex> lock(mDebugMutex);

	auto itr = mBreakpoints.find(path);
	if (itr != mBreakpoints.end())
	{
		for (auto breakpoint : itr->second)
			Breakpoint_Delete(breakpoint);
		itr->second.clear();
	}
	std::vector<Beefy::Breakpoint*>& breakpoints = mBreakpoints[path];

	Json* breakpointItems = GetItem(arguments, "breakpoints");
	if ((breakpointItems != NULL) && (breakpointItems->mType == Json::Type_Array))
	{
		for (int idx = 0; idx < breakpointItems->GetArraySize(); idx++)
		{
			Json* item = breakpointItems->GetArrayItem(idx);
			int line = GetInt(item, "line") - 1; // The debugger uses 0-based lines
			if (line < 0)
				continue;

			Beefy::Breakpoint* breakpoint = Debugger_CreateBreakpoint(path.c_str(), line, -1, 0);
			if (breakpoint == NULL)
				continue;
			breakpoints.push_back(breakpoint);

			std::string condition = GetString(item, "condition");
			if (!condition.empty())
				Breakpoint_SetCondition(breakpoint, condition.c_str());

			std::string logMessage = GetString(item, "logMessage");
			if (!logMessage.empty())
				Breakpoint_SetLogging(breakpoint, logMessage.c_str(), false);

			std::string hitCondition = GetString(item, "hitCondition");
			if (!hitCondition.empty())
				Breakpoint_SetHitCountTarget(breakpoint, atoi(hitCondition.c_str()), BeefDbgHitCountKind_GreaterEquals);

			Json* responseBreakpoint = Json::CreateObject();
			intptr address = Breakpoint_GetAddress(breakpoint, NULL);
			responseBreakpoint->AddBoolToObject("verified", address != 0);
			responseBreakpoint->AddNumberToObject("line", (int)Breakpoint_GetLineNum(breakpoint) + 1);
			if (address != 0)
				responseBreakpoint->AddNumberToObject("instructionReference", 0);
			responseBreakpoints->AddItemToArray(responseBreakpoint);
		}
	}

	SendResponse(request, body);
}

// --- threads ----------------------------------------------------------------

void DapServer::HandleThreads(Json* request)
{
	Json* body = Json::CreateObject();
	Json* threads = AddArray(body, "threads");

	std::lock_guard<std::mutex> lock(mDebugMutex);
	const char* info = Debugger_GetThreadInfo();
	std::vector<std::string> lines = SplitLines((info != NULL) ? info : "");

	int activeThreadId = 0;
	for (size_t idx = 0; idx < lines.size(); idx++)
	{
		if (lines[idx].empty())
			continue;
		std::vector<std::string> fields = SplitTabs(lines[idx]);
		if (idx == 0)
		{
			activeThreadId = atoi(fields[0].c_str());
			continue;
		}

		int threadId = atoi(fields[0].c_str());
		std::string name = (fields.size() > 1) ? fields[1] : ("Thread " + fields[0]);
		Json* thread = Json::CreateObject();
		thread->AddNumberToObject("id", threadId);
		thread->AddStringToObject("name", name.c_str());
		threads->AddItemToArray(thread);
	}

	if ((threads->GetArraySize() == 0) && (activeThreadId != 0))
	{
		Json* thread = Json::CreateObject();
		thread->AddNumberToObject("id", activeThreadId);
		thread->AddStringToObject("name", "Main Thread");
		threads->AddItemToArray(thread);
	}

	SendResponse(request, body);
}

void DapServer::HandleStackTrace(Json* request, Json* arguments)
{
	int threadId = GetInt(arguments, "threadId");
	int startFrame = GetInt(arguments, "startFrame");
	int levels = GetInt(arguments, "levels");

	Json* body = Json::CreateObject();
	Json* frames = AddArray(body, "stackFrames");

	std::lock_guard<std::mutex> lock(mDebugMutex);
	Debugger_SetActiveThread(threadId);
	CallStack_Update();
	int frameCount = CallStack_GetCount();

	int endFrame = frameCount;
	if ((levels > 0) && (startFrame + levels < endFrame))
		endFrame = startFrame + levels;

	for (int idx = startFrame; idx < endFrame; idx++)
	{
		intptr address = 0;
		const char* file = NULL;
		int32 hotIdx = -1;
		int32 defLineStart = -1;
		int32 defLineEnd = -1;
		int32 line = -1;
		int32 column = -1;
		int32 language = 0;
		int32 stackSize = 0;
		int8 frameFlags = 0;

		const char* location = CallStack_GetStackFrameInfo(idx, &address, &file, &hotIdx, &defLineStart,
			&defLineEnd, &line, &column, &language, &stackSize, &frameFlags);
		std::string locationText = (location != NULL) ? location : "";

		FrameInfo frame;
		frame.mThreadId = threadId;
		frame.mIndex = idx;
		frame.mName = locationText.empty() ? ("Frame " + std::to_string(idx)) : locationText;
		if (file != NULL)
			frame.mFile = StripSourceHash(ToForwardSlashes(file));
		frame.mLine = line + 1;
		frame.mColumn = column + 1;
		frame.mHasSource = !frame.mFile.empty() && (frame.mFile.compare(0, 5, "$Emit") != 0);

		int frameId = AllocateFrameId(frame);

		Json* frameJson = Json::CreateObject();
		frameJson->AddNumberToObject("id", frameId);
		frameJson->AddStringToObject("name", frame.mName.c_str());
		frameJson->AddNumberToObject("line", frame.mHasSource ? frame.mLine : 0);
		frameJson->AddNumberToObject("column", frame.mHasSource ? frame.mColumn : 0);
		if (frame.mHasSource)
		{
			Json* source = AddObject(frameJson, "source");
			std::string name = BaseName(frame.mFile);
			source->AddStringToObject("name", name.c_str());
			source->AddStringToObject("path", frame.mFile.c_str());
		}
		frames->AddItemToArray(frameJson);
	}

	body->AddNumberToObject("totalFrames", frameCount);
	SendResponse(request, body);
}

// --- frames / variables -----------------------------------------------------

int DapServer::AllocateFrameId(const FrameInfo& frame)
{
	std::lock_guard<std::mutex> lock(mFramesMutex);
	int frameId = mNextFrameId++;
	mFrames[frameId] = frame;
	return frameId;
}

bool DapServer::GetFrame(int frameId, FrameInfo& outFrame)
{
	std::lock_guard<std::mutex> lock(mFramesMutex);
	auto itr = mFrames.find(frameId);
	if (itr == mFrames.end())
		return false;
	outFrame = itr->second;
	return true;
}

int DapServer::AllocateVarRef(const VarRef& varRef)
{
	std::lock_guard<std::mutex> lock(mVarsMutex);
	int varRefId = mNextVarRef++;
	mVarRefs[varRefId] = varRef;
	return varRefId;
}

bool DapServer::GetVarRef(int varRefId, VarRef& outVarRef)
{
	std::lock_guard<std::mutex> lock(mVarsMutex);
	auto itr = mVarRefs.find(varRefId);
	if (itr == mVarRefs.end())
		return false;
	outVarRef = itr->second;
	return true;
}

Json* DapServer::CreateVariable(const std::string& name, const std::string& value, const std::string& type,
	const VarRef& varRef, bool expandable)
{
	Json* variable = Json::CreateObject();
	variable->AddStringToObject("name", name.c_str());
	variable->AddStringToObject("value", value.c_str());
	if (!type.empty())
		variable->AddStringToObject("type", type.c_str());
	variable->AddNumberToObject("variablesReference", expandable ? AllocateVarRef(varRef) : 0);
	return variable;
}

void DapServer::HandleScopes(Json* request, Json* arguments)
{
	int frameId = GetInt(arguments, "frameId");
	FrameInfo frame;
	if (!GetFrame(frameId, frame))
	{
		SendErrorResponse(request, -32602, "Unknown frame");
		return;
	}

	Json* body = Json::CreateObject();
	Json* scopes = AddArray(body, "scopes");

	VarRef locals;
	locals.mLocals = true;
	locals.mThreadId = frame.mThreadId;
	locals.mFrameIdx = frame.mIndex;

	Json* scope = Json::CreateObject();
	scope->AddStringToObject("name", "Locals");
	scope->AddNumberToObject("variablesReference", AllocateVarRef(locals));
	scope->AddBoolToObject("expensive", false);
	scopes->AddItemToArray(scope);

	SendResponse(request, body);
}

void DapServer::HandleVariables(Json* request, Json* arguments)
{
	int varRefId = GetInt(arguments, "variablesReference");
	VarRef varRef;
	if (!GetVarRef(varRefId, varRef))
	{
		SendErrorResponse(request, -32602, "Unknown variables reference");
		return;
	}

	Json* body = Json::CreateObject();
	Json* variables = AddArray(body, "variables");

	std::lock_guard<std::mutex> lock(mDebugMutex);

	std::vector<std::string> names;
	if (varRef.mLocals)
	{
		const char* locals = Debugger_GetAutoLocals(varRef.mFrameIdx, false);
		for (const auto& line : SplitLines((locals != NULL) ? locals : ""))
		{
			std::string name = Trim(line);
			if (name.empty() || (name[0] == '$'))
				continue;
			names.push_back(name);
		}
	}
	else
	{
		names.push_back(varRef.mExpr);
	}

	for (const auto& name : names)
	{
		EvalResult result;
		if (!EvaluateExpression(name, varRef.mFrameIdx, result))
		{
			Json* variable = Json::CreateObject();
			variable->AddStringToObject("name", name.c_str());
			variable->AddStringToObject("value", "<unavailable>");
			variable->AddNumberToObject("variablesReference", 0);
			variables->AddItemToArray(variable);
			continue;
		}

		if (varRef.mLocals)
		{
			VarRef childRef;
			childRef.mLocals = false;
			childRef.mThreadId = varRef.mThreadId;
			childRef.mFrameIdx = varRef.mFrameIdx;
			childRef.mExpr = name;
			variables->AddItemToArray(CreateVariable(name, result.mValue, result.mType, childRef, result.HasChildren()));
			continue;
		}

		// Expanding a member: emit its members, series and continuation entries.
		for (const auto& member : result.mMembers)
		{
			std::string childExpr = FormatTemplate(member.mEvalFormat, { varRef.mExpr });
			EvalResult childResult;
			bool evaluated = EvaluateExpression(childExpr, varRef.mFrameIdx, childResult);
			std::string value = !member.mValue.empty() ? member.mValue : (evaluated ? childResult.mValue : "");
			std::string type = !member.mType.empty() ? member.mType : (evaluated ? childResult.mType : "");

			VarRef childRef;
			childRef.mLocals = false;
			childRef.mThreadId = varRef.mThreadId;
			childRef.mFrameIdx = varRef.mFrameIdx;
			childRef.mExpr = childExpr;
			variables->AddItemToArray(CreateVariable(member.mName, value, type, childRef,
				evaluated && childResult.HasChildren()));
		}

		if (result.mHasSeries)
		{
			int count = result.mSeriesCount;
			int cap = (result.mSeriesMaxShow > 0) ? (result.mSeriesMaxShow * 4) : 100;
			if (cap > 100)
				cap = 100;
			if (count > cap)
				count = cap;

			for (int idx = 0; idx < count; idx++)
			{
				std::vector<std::string> args;
				args.push_back(std::to_string(result.mSeriesStart + idx));
				for (const auto& param : result.mSeriesParams)
					args.push_back(param);

				std::string childName = FormatTemplate(result.mSeriesDisplayFormat, args);
				std::string childExpr = FormatTemplate(result.mSeriesEvalFormat, args);

				EvalResult childResult;
				bool evaluated = EvaluateExpression(childExpr, varRef.mFrameIdx, childResult);

				VarRef childRef;
				childRef.mLocals = false;
				childRef.mThreadId = varRef.mThreadId;
				childRef.mFrameIdx = varRef.mFrameIdx;
				childRef.mExpr = childExpr;
				variables->AddItemToArray(CreateVariable(childName, evaluated ? childResult.mValue : "",
					evaluated ? childResult.mType : "", childRef, evaluated && childResult.HasChildren()));
			}
		}
	}

	SendResponse(request, body);
}

void DapServer::HandleEvaluate(Json* request, Json* arguments)
{
	std::string expression = GetString(arguments, "expression");
	int frameId = GetInt(arguments, "frameId");
	FrameInfo frame;
	if (!GetFrame(frameId, frame))
	{
		SendErrorResponse(request, -32602, "Unknown frame");
		return;
	}

	EvalResult result;
	{
		std::lock_guard<std::mutex> lock(mDebugMutex);
		EvaluateExpression(expression, frame.mIndex, result);
	}

	Json* body = Json::CreateObject();
	body->AddStringToObject("result", result.mValue.c_str());
	if (!result.mType.empty())
		body->AddStringToObject("type", result.mType.c_str());

	if (result.HasChildren())
	{
		VarRef varRef;
		varRef.mLocals = false;
		varRef.mThreadId = frame.mThreadId;
		varRef.mFrameIdx = frame.mIndex;
		varRef.mExpr = expression;
		body->AddNumberToObject("variablesReference", AllocateVarRef(varRef));
	}
	else
	{
		body->AddNumberToObject("variablesReference", 0);
	}

	SendResponse(request, body);
}

// --- execution control ------------------------------------------------------

void DapServer::EnqueueResume(std::function<void()> command, const char* reason)
{
	std::lock_guard<std::mutex> lock(mResumeMutex);
	PendingResume resume;
	resume.mCommand = std::move(command);
	resume.mReason = reason;
	mPendingResumes.push_back(std::move(resume));
}

void DapServer::HandleContinue(Json* request)
{
	EnqueueResume([] { Debugger_Continue(); }, "");
	SendResponse(request, Json::CreateObject());
}

void DapServer::HandleNext(Beefy::Json* request)
{
	EnqueueResume([] { Debugger_StepOver(false); }, "step");
	SendResponse(request, Json::CreateObject());
}

void DapServer::HandleStepIn(Beefy::Json* request)
{
	EnqueueResume([] { Debugger_StepInto(false); }, "step");
	SendResponse(request, Json::CreateObject());
}

void DapServer::HandleStepOut(Beefy::Json* request)
{
	EnqueueResume([] { Debugger_StepOut(false); }, "step");
	SendResponse(request, Json::CreateObject());
}

void DapServer::HandlePause(Beefy::Json* request)
{
	EnqueueResume([] { Debugger_BreakAll(); }, "pause");
	SendResponse(request, Json::CreateObject());
}

// --- pump / events ----------------------------------------------------------

bool DapServer::IsPaused() const
{
	int runState = Debugger_GetRunState();
	bool paused = (runState == BeefDbgRunState_Paused) || (runState == BeefDbgRunState_Breakpoint) ||
		(runState == BeefDbgRunState_Exception) || (runState == BeefDbgRunState_DebugEval_Done);
	return paused && !Debugger_HasPendingDebugLoads();
}

void DapServer::DrainMessages()
{
	while (true)
	{
		const char* message = Debugger_PopMessage();
		if (message == NULL)
			break;

		std::string text = message;
		size_t spacePos = text.find(' ');
		std::string command = (spacePos == std::string::npos) ? text : text.substr(0, spacePos);
		std::string param = (spacePos == std::string::npos) ? "" : text.substr(spacePos + 1);

		if (command == "dbgEvalMsg")
			SendOutput("console", param);
		else if ((command == "error") || (command == "errorsoft"))
			SendOutput("stderr", param);
		else
			Log("DAP debugger message: %s", text.c_str());
	}
}

void DapServer::UpdateRunState()
{
	int runState = Debugger_GetRunState();
	bool paused = IsPaused();

	if (paused && !mWasPaused)
	{
		// The debugger stops at the process entry on its own. Unless the client
		// asked for an entry stop, run until a breakpoint or exit instead.
		if ((mFirstStop) && (!mStopOnEntry) && (mLastStopReason == "entry"))
		{
			mFirstStop = false;
			Debugger_Continue();
			return;
		}
		mFirstStop = false;

		mWasPaused = true;

		std::string reason = mLastStopReason;
		if (runState == BeefDbgRunState_Exception)
			reason = "exception";
		else if ((reason == "step") || (reason == "pause"))
		{
			// keep the reason the client requested
		}
		else if (Debugger_GetActiveBreakpoint() != NULL)
			reason = "breakpoint";
		else if (reason.empty())
			reason = "pause";

		Json* body = Json::CreateObject();
		body->AddStringToObject("reason", reason.c_str());
		body->AddNumberToObject("threadId", Debugger_GetActiveThread());
		body->AddBoolToObject("allThreadsStopped", true);

		if (runState == BeefDbgRunState_Exception)
		{
			const char* exception = Debugger_GetCurrentException();
			if ((exception != NULL) && (*exception != 0))
			{
				std::vector<std::string> lines = SplitLines(exception);
				body->AddStringToObject("text", lines[0].c_str());
			}
		}

		// Frames are only valid for the pause they were captured in.
		{
			std::lock_guard<std::mutex> lock(mFramesMutex);
			mFrames.clear();
		}
		{
			std::lock_guard<std::mutex> lock(mVarsMutex);
			mVarRefs.clear();
		}

		SendEvent("stopped", body);
		Log("DAP: stopped reason=%s state=%d", reason.c_str(), runState);
	}
	else if (!paused)
	{
		mWasPaused = false;
	}

	if (((runState == BeefDbgRunState_Terminated) || (runState == BeefDbgRunState_TargetUnloaded) ||
		(runState == BeefDbgRunState_NotStarted)) && (!mSentTerminated) && (mLastRunState != BeefDbgRunState_NotStarted))
	{
		mSentTerminated = true;
		Json* body = Json::CreateObject();
		if (runState == BeefDbgRunState_TargetUnloaded)
			body->AddNumberToObject("exitCode", 0);
		SendEvent("terminated", body);
		mTerminated = true;
	}

	mLastRunState = runState;
	if (runState != mPrevLoggedState)
	{
		Log("DAP: run state %d -> %d (paused=%d)", mPrevLoggedState, runState, (int)paused);
		mPrevLoggedState = runState;
	}
}

void DapServer::PumpLoop()
{
	while (!mPumpStop)
	{
		{
			std::lock_guard<std::mutex> lock(mDebugMutex);
			if (mLaunched)
			{
				// Apply one queued continue/step command per tick and let the
				// engine process it, so a following command cannot override it.
				PendingResume resume;
				bool hasResume = false;
				{
					std::lock_guard<std::mutex> resumeLock(mResumeMutex);
					if (!mPendingResumes.empty())
					{
						resume = std::move(mPendingResumes.front());
						mPendingResumes.pop_front();
						hasResume = true;
					}
				}
				if (hasResume)
				{
					resume.mCommand();
					mLastStopReason = resume.mReason;
					mWasPaused = false;
				}

				Debugger_Update();
				DrainMessages();
				TryStartStdReaders();
				UpdateRunState();
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

} // namespace beflsp
