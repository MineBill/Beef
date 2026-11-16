#include "Server.h"

#include "JsonRpc.h"
#include "Log.h"
#include "LspText.h"

#include "BeefySysLib/util/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>

using Beefy::Json;

namespace beflsp
{

using Clock = std::chrono::steady_clock;

// How long the document has to be quiet before the syntax check runs, and
// before the (incremental) workspace resolve runs to update semantic errors.
// The refresh delay can be overridden through initializationOptions
// (beef.server.diagnosticsDelayMs) and is throttled so brief pauses while
// typing do not re-resolve continuously.
static const int ANALYZE_DEBOUNCE_MS = 150;
static const int DEFAULT_REFRESH_DEBOUNCE_MS = 300;
static const int MAX_REFRESH_DEBOUNCE_MS = 5000;

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

static std::string GetString(Json* parent, const char* name, const std::string& defaultValue = "")
{
	if (parent == NULL)
		return defaultValue;
	Json* item = parent->GetObjectItem(name);
	if ((item == NULL) || (item->mType != Json::Type_String) || (item->mValueString == NULL))
		return defaultValue;
	return item->mValueString;
}

static Json* GetItem(Json* parent, const char* name)
{
	if (parent == NULL)
		return NULL;
	return parent->GetObjectItem(name);
}

static int GetInt(Json* parent, const char* name, int defaultValue)
{
	if (parent == NULL)
		return defaultValue;
	Json* item = parent->GetObjectItem(name);
	if ((item == NULL) || (item->mType != Json::Type_Number))
		return defaultValue;
	return item->mValueInt;
}

static Json* CreatePosition(int line, int character)
{
	Json* position = Json::CreateObject();
	position->AddNumberToObject("line", line);
	position->AddNumberToObject("character", character);
	return position;
}

static bool SameDiagnostics(const std::vector<Diagnostic>& lhs, const std::vector<Diagnostic>& rhs)
{
	if (lhs.size() != rhs.size())
		return false;
	for (size_t i = 0; i < lhs.size(); i++)
	{
		const Diagnostic& left = lhs[i];
		const Diagnostic& right = rhs[i];
		if ((left.mFile != right.mFile) || (left.mLine != right.mLine) || (left.mColumn != right.mColumn) ||
			(left.mEndLine != right.mEndLine) || (left.mEndColumn != right.mEndColumn) ||
			(left.mIsWarning != right.mIsWarning) || (left.mCode != right.mCode) || (left.mMessage != right.mMessage))
			return false;
	}
	return true;
}

static Json* CreateDiagnosticJson(const Diagnostic& diagnostic)
{
	Json* item = Json::CreateObject();
	Json* range = AddObject(item, "range");
	range->AddItemToObject("start", CreatePosition(diagnostic.mLine, diagnostic.mColumn));
	range->AddItemToObject("end", CreatePosition(diagnostic.mEndLine, diagnostic.mEndColumn));
	item->AddNumberToObject("severity", diagnostic.mIsWarning ? 2 : 1);
	item->AddStringToObject("source", "beef");
	item->AddStringToObject("message", diagnostic.mMessage.c_str());
	if (diagnostic.mCode != 0)
		item->AddNumberToObject("code", diagnostic.mCode);
	return item;
}

static int GetCompletionKind(const std::string& entryType)
{
	// CompletionItemKind values from the LSP specification.
	if ((entryType == "method") || (entryType == "extmethod"))
		return 2;
	if (entryType == "field")
		return 5;
	if (entryType == "property")
		return 10;
	if (entryType == "namespace")
		return 9;
	if ((entryType == "class") || (entryType == "delegate"))
		return 7;
	if (entryType == "interface")
		return 8;
	if ((entryType == "valuetype") || (entryType == "struct"))
		return 22;
	if (entryType == "enum")
		return 13;
	if (entryType == "payloadEnum")
		return 20;
	if (entryType == "generic")
		return 25;
	if ((entryType == "value") || (entryType == "object") || (entryType == "pointer") ||
		(entryType == "local") || (entryType == "param"))
		return 6;
	if (entryType == "token")
		return 14;
	if (entryType == "folder")
		return 19;
	if (entryType == "file")
		return 17;
	return 1; // Text
}

static std::string MakeSortText(int idx)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%06d", idx);
	return buffer;
}

static bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
{
	if (lhs.size() != rhs.size())
		return false;
	for (size_t i = 0; i < lhs.size(); i++)
	{
		unsigned char a = (unsigned char)lhs[i];
		unsigned char b = (unsigned char)rhs[i];
		if (tolower(a) != tolower(b))
			return false;
	}
	return true;
}

static void SendErrorResponse(Json* idItem, int code, const std::string& message)
{
	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (idItem != NULL) ? Json::Duplicate(idItem, 1) : Json::CreateNull());
	Json* error = AddObject(response, "error");
	error->AddNumberToObject("code", code);
	error->AddStringToObject("message", message.c_str());
	WriteLspMessage(response);
}

// --- threading ---------------------------------------------------------------

void Server::StartThreads()
{
	mReaderThread = std::thread([this] { ReaderLoop(); });
	mWorkerThread = std::thread([this] { WorkerLoop(); });
}

void Server::StopThreads()
{
	{
		std::lock_guard<std::mutex> lock(mWorkMutex);
		mWorkerStop = true;
	}
	mWorkCv.notify_all();
	if (mWorkerThread.joinable())
		mWorkerThread.join();

	// The reader is blocked in a synchronous stdin read; leaving it detached is
	// fine because the process is about to exit.
	if (mReaderThread.joinable())
		mReaderThread.detach();
}

void Server::ReaderLoop()
{
	std::string body;
	while (ReadLspMessage(body))
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mMessages.push_back(std::move(body));
		}
		mCv.notify_one();
	}

	{
		std::lock_guard<std::mutex> lock(mMutex);
		mReaderDone = true;
	}
	mCv.notify_all();
}

void Server::WorkerLoop()
{
	while (true)
	{
		WorkItem item;
		{
			std::unique_lock<std::mutex> lock(mWorkMutex);
			mWorkCv.wait(lock, [this] { return mWorkerStop || !mWorkQueue.empty(); });
			if (mWorkQueue.empty())
				break;
			item = std::move(mWorkQueue.front());
			mWorkQueue.pop_front();
		}

		WorkResult result = ExecuteWork(item);
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mResults.push_back(std::move(result));
		}
		mCv.notify_one();
	}
}

Server::WorkResult Server::ExecuteWork(const WorkItem& item)
{
	WorkResult result;
	result.mKind = item.mKind;
	result.mPath = item.mPath;
	result.mText = item.mText;
	result.mRequestId = item.mRequestId;
	result.mRequestVersion = item.mRequestVersion;
	result.mCursorOffset = item.mCursorOffset;

	switch (item.mKind)
	{
	case WorkItem::Kind::Startup:
		{
			std::string error;
			if (!mWorkspace.Discover(mRootPath, error))
			{
				result.mError = error;
				break;
			}
			if (!mCompiler.Initialize(mWorkspace, error))
			{
				result.mError = error;
				break;
			}
			result.mSuccess = true;
			result.mCompileDiagnostics = mCompiler.GetCompileDiagnostics();
		}
		break;
	case WorkItem::Kind::Analyze:
		mCompiler.AnalyzeFile(item.mPath, item.mText, result.mDiagnostics);
		result.mSuccess = true;
		break;
	case WorkItem::Kind::Refresh:
		mCompiler.RefreshFile(item.mPath, item.mText);
		result.mSuccess = true;
		result.mCompileDiagnostics = mCompiler.GetCompileDiagnostics();
		break;
	case WorkItem::Kind::Completion:
		{
			std::string error;
			result.mSuccess = mCompiler.Autocomplete(item.mPath, item.mText, item.mCursorOffset, result.mCompletionInfo, error);
			result.mError = error;
			result.mCompileDiagnostics = mCompiler.GetCompileDiagnostics();
		}
		break;
	case WorkItem::Kind::Definition:
		{
			std::string error;
			result.mSuccess = mCompiler.GoToDefinition(item.mPath, item.mText, item.mCursorOffset, result.mLocations, error);
			result.mError = error;
			result.mCompileDiagnostics = mCompiler.GetCompileDiagnostics();
		}
		break;
	case WorkItem::Kind::CodeLens:
		result.mSuccess = mCompiler.GetEmitRegions(item.mPath, result.mEmitRegions);
		break;
	case WorkItem::Kind::EmitSource:
		{
			int version = -1;
			result.mSuccess = mCompiler.GetEmitSource(item.mPath, result.mEmitText, version);
			result.mEmitVersion = version;
		}
		break;
	case WorkItem::Kind::Hover:
		{
			std::string error;
			result.mSuccess = mCompiler.Hover(item.mPath, item.mText, item.mCursorOffset, result.mHoverMarkdown);
			result.mError = error;
			result.mCompileDiagnostics = mCompiler.GetCompileDiagnostics();
		}
		break;
	case WorkItem::Kind::FoldingRange:
		result.mSuccess = mCompiler.GetFoldingRanges(item.mPath, result.mFoldingRanges);
		break;
	}

	return result;
}

void Server::EnqueueWork(WorkItem item, bool highPriority)
{
	{
		std::lock_guard<std::mutex> lock(mWorkMutex);
		if (highPriority)
			mWorkQueue.push_front(std::move(item));
		else
			mWorkQueue.push_back(std::move(item));
	}
	mWorkCv.notify_one();
}

void Server::DrainResults()
{
	std::deque<WorkResult> results;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		results.swap(mResults);
	}
	for (auto& result : results)
		HandleWorkResult(result);
}

bool Server::TakeMessage(std::string& body, int timeoutMs)
{
	std::unique_lock<std::mutex> lock(mMutex);
	mCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
		return !mMessages.empty() || !mResults.empty() || mReaderDone || mExitRequested;
	});

	if (mMessages.empty())
		return false;

	body = std::move(mMessages.front());
	mMessages.pop_front();
	return true;
}

bool Server::IsReaderDone()
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mReaderDone;
}

// --- main loop ---------------------------------------------------------------

int Server::Run()
{
	StartThreads();

	while (true)
	{
		DrainResults();

		if (mExitRequested)
			break;

		if (IsReaderDone())
		{
			DrainResults();
			break;
		}

		RunTimers();

		std::string body;
		if (!TakeMessage(body, 10))
			continue;

		Json* msg = Json::Parse(body.c_str());
		if (msg == NULL)
		{
			Log("Received malformed JSON message");
			continue;
		}

		try
		{
			Dispatch(msg);
		}
		catch (const std::exception& ex)
		{
			Log("Exception while handling message: %s", ex.what());
		}

		delete msg;
	}

	StopThreads();
	return 0;
}

// --- result handling ---------------------------------------------------------

void Server::HandleWorkResult(WorkResult& result)
{
	switch (result.mKind)
	{
	case WorkItem::Kind::Startup:
		{
			mStartupQueued = false;
			if (!result.mSuccess)
			{
				mStartupFailed = true;
				Log("Workspace initialization failed: %s", result.mError.c_str());
				SendLogMessage(1, "beeflsp: " + result.mError);
				return;
			}
			mWorkspaceReady = true;
			UpdateCompileDiagnostics(result.mCompileDiagnostics);
			SendLogMessage(3, "beeflsp: workspace resolved, autocomplete ready");
		}
		return;

	case WorkItem::Kind::Analyze:
		{
			auto itr = mPendingEdits.find(result.mPath);
			if (itr == mPendingEdits.end())
				return; // document already closed

			PendingEdit& pending = itr->second;
			pending.mAnalyzeQueued = false;
			if (pending.mText != result.mText)
			{
				// A newer edit arrived while this ran; the result is stale.
				pending.mAnalyzeDue = Clock::now() + std::chrono::milliseconds(ANALYZE_DEBOUNCE_MS);
				return;
			}

			pending.mAnalyzeDue = Clock::time_point::max();
			mSyntaxDiagnosticsByFile[result.mPath] = std::move(result.mDiagnostics);
			PublishPathDiagnostics(result.mPath);
		}
		return;

	case WorkItem::Kind::Refresh:
		{
			auto itr = mPendingEdits.find(result.mPath);
			if (itr == mPendingEdits.end())
				return;

			PendingEdit& pending = itr->second;
			pending.mRefreshQueued = false;
			pending.mLastRefresh = Clock::now();
			if (pending.mText != result.mText)
			{
				pending.mRefreshDue = Clock::now() + std::chrono::milliseconds(mDiagnosticsDelayMs);
				return;
			}

			pending.mRefreshDue = Clock::time_point::max();
			UpdateCompileDiagnostics(result.mCompileDiagnostics);

			// The refreshed parse may have produced different emit regions.
			SendRequest("workspace/codeLens/refresh", NULL);
		}
		return;

	case WorkItem::Kind::Completion:
		UpdateCompileDiagnostics(result.mCompileDiagnostics);
		SendCompletionResponse(result);
		return;

	case WorkItem::Kind::Definition:
		UpdateCompileDiagnostics(result.mCompileDiagnostics);
		SendDefinitionResponse(result);
		return;

	case WorkItem::Kind::CodeLens:
		SendCodeLensResponse(result);
		return;

	case WorkItem::Kind::EmitSource:
		SendEmitSourceResponse(result);
		return;

	case WorkItem::Kind::Hover:
		UpdateCompileDiagnostics(result.mCompileDiagnostics);
		SendHoverResponse(result);
		return;

	case WorkItem::Kind::FoldingRange:
		SendFoldingRangeResponse(result);
		return;
	}
}

// --- dispatch ----------------------------------------------------------------

void Server::Dispatch(Json* msg)
{
	if ((msg == NULL) || (msg->mType != Json::Type_Object))
		return;

	Json* methodItem = msg->GetObjectItem("method");
	if ((methodItem == NULL) || (methodItem->mType != Json::Type_String) || (methodItem->mValueString == NULL))
		return; // Response from the client; we don't send requests, so ignore.

	std::string method = methodItem->mValueString;
	Json* params = msg->GetObjectItem("params");
	Json* idItem = msg->GetObjectItem("id");

	if (idItem != NULL)
	{
		if (method == "textDocument/completion")
		{
			HandleCompletionRequest(params, idItem);
			return;
		}
		if (method == "textDocument/definition")
		{
			HandleDefinitionRequest(params, idItem);
			return;
		}
		if (method == "textDocument/codeLens")
		{
			HandleCodeLensRequest(params, idItem);
			return;
		}
		if (method == "beef/emitSource")
		{
			HandleEmitSourceRequest(params, idItem);
			return;
		}
		if (method == "textDocument/hover")
		{
			HandleHoverRequest(params, idItem);
			return;
		}
		if (method == "textDocument/foldingRange")
		{
			HandleFoldingRangeRequest(params, idItem);
			return;
		}

		std::string error;
		Json* result = HandleRequest(method, params, idItem, error);

		Json* response = Json::CreateObject();
		response->AddStringToObject("jsonrpc", "2.0");
		response->AddItemToObject("id", Json::Duplicate(idItem, 1));

		if (error.empty())
		{
			response->AddItemToObject("result", (result != NULL) ? result : Json::CreateNull());
		}
		else
		{
			if (result != NULL)
				delete result;
			Json* errorObj = AddObject(response, "error");
			int code = (method == "shutdown") ? -32600 : ((method == "initialize") ? -32603 : -32601);
			errorObj->AddNumberToObject("code", code);
			errorObj->AddStringToObject("message", error.c_str());
		}

		WriteLspMessage(response);
		return;
	}

	HandleNotification(method, params);
}

Json* Server::HandleRequest(const std::string& method, Json* params, Json* id, std::string& outError)
{
	(void)id;

	if (method == "initialize")
		return HandleInitialize(params, outError);
	if (method == "shutdown")
	{
		mShutdownRequested = true;
		return Json::CreateNull();
	}
	if (method == "completionItem/resolve")
		return HandleCompletionResolve(params, outError);

	outError = "Unsupported method: " + method;
	return NULL;
}

void Server::HandleNotification(const std::string& method, Json* params)
{
	if (method == "initialized")
	{
		EnsureStartup();
		return;
	}
	if (method == "exit")
	{
		mExitRequested = true;
		return;
	}
	if (method == "textDocument/didOpen")
	{
		HandleDidOpen(params);
		return;
	}
	if (method == "textDocument/didChange")
	{
		HandleDidChange(params);
		return;
	}
	if (method == "textDocument/didSave")
	{
		HandleDidSave(params);
		return;
	}
	if (method == "textDocument/didClose")
	{
		HandleDidClose(params);
		return;
	}
	if (method == "$/cancelRequest")
		return;

	Log("Ignoring notification %s", method.c_str());
}

// --- requests ----------------------------------------------------------------

Json* Server::HandleInitialize(Json* params, std::string& outError)
{
	(void)outError;

	std::string rootUri = GetString(params, "rootUri");
	std::string rootPath = GetString(params, "rootPath");

	if (!rootUri.empty())
		UriToPath(rootUri, mRootPath);
	else if (!rootPath.empty())
		mRootPath = NormalizePath(rootPath);

	if (mRootPath.empty())
	{
		Json* folderArray = GetItem(params, "workspaceFolders");
		if ((folderArray != NULL) && (folderArray->mType == Json::Type_Array) && (folderArray->GetArraySize() > 0))
		{
			std::string folderUri = GetString(folderArray->GetArrayItem(0), "uri");
			if (!folderUri.empty())
				UriToPath(folderUri, mRootPath);
		}
	}

	Log("Initialize: root='%s'", mRootPath.c_str());

	Json* initOptions = GetItem(params, "initializationOptions");
	mDiagnosticsDelayMs = std::max(0, std::min(GetInt(initOptions, "diagnosticsDelayMs", mDiagnosticsDelayMs), MAX_REFRESH_DEBOUNCE_MS));

	Json* result = Json::CreateObject();
	Json* capabilities = AddObject(result, "capabilities");

	Json* sync = AddObject(capabilities, "textDocumentSync");
	sync->AddBoolToObject("openClose", true);
	sync->AddNumberToObject("change", 1); // Full document sync

	Json* completionProvider = AddObject(capabilities, "completionProvider");
	completionProvider->AddBoolToObject("resolveProvider", true);
	Json* triggerCharacters = AddArray(completionProvider, "triggerCharacters");
	triggerCharacters->AddItemToArray(Json::CreateString("."));

	// Go-to-definition. Without this the client never sends the request.
	capabilities->AddTrueToObject("definitionProvider");

	// "Show generated code" lenses on the lines that emitted code.
	AddObject(capabilities, "codeLensProvider");

	capabilities->AddTrueToObject("hoverProvider");
	capabilities->AddTrueToObject("foldingRangeProvider");

	Json* serverInfo = AddObject(result, "serverInfo");
	serverInfo->AddStringToObject("name", "beeflsp");
	serverInfo->AddStringToObject("version", "0.1.0");

	return result;
}

OpenDocument* Server::GetRequestDocument(Json* params, Json* idItem, const char* requestName, int& outOffset)
{
	Json* textDocument = GetItem(params, "textDocument");
	Json* position = GetItem(params, "position");
	if ((textDocument == NULL) || (position == NULL))
	{
		SendErrorResponse(idItem, -32602, std::string("Invalid ") + requestName + " parameters");
		return NULL;
	}

	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
	{
		SendErrorResponse(idItem, -32602, "Missing document URI");
		return NULL;
	}

	Json* lineItem = position->GetObjectItem("line");
	Json* characterItem = position->GetObjectItem("character");
	if ((lineItem == NULL) || (characterItem == NULL))
	{
		SendErrorResponse(idItem, -32602, "Invalid position");
		return NULL;
	}

	OpenDocument* document = FindOrLoadDocument(uri);
	if (document == NULL)
	{
		SendErrorResponse(idItem, -32602, "Document is not open: " + uri);
		return NULL;
	}

	outOffset = OffsetFromPosition(document->mText, lineItem->mValueInt, characterItem->mValueInt);
	return document;
}

void Server::HandleCompletionRequest(Json* params, Json* idItem)
{
	int offset = 0;
	OpenDocument* document = GetRequestDocument(params, idItem, "completion", offset);
	if (document == NULL)
		return;

	EnsureStartup();

	WorkItem item;
	item.mKind = WorkItem::Kind::Completion;
	item.mPath = document->mPath;
	item.mText = document->mText;
	item.mCursorOffset = offset;
	item.mRequestId = Json::Duplicate(idItem, 1);
	item.mRequestVersion = document->mVersion;
	EnqueueWork(std::move(item), mWorkspaceReady);
}

void Server::HandleDefinitionRequest(Json* params, Json* idItem)
{
	int offset = 0;
	OpenDocument* document = GetRequestDocument(params, idItem, "definition", offset);
	if (document == NULL)
		return;

	EnsureStartup();

	WorkItem item;
	item.mKind = WorkItem::Kind::Definition;
	item.mPath = document->mPath;
	item.mText = document->mText;
	item.mCursorOffset = offset;
	item.mRequestId = Json::Duplicate(idItem, 1);
	item.mRequestVersion = document->mVersion;
	EnqueueWork(std::move(item), mWorkspaceReady);
}

static std::string GetEmitTypeName(const std::string& file)
{
	if (file.compare(0, 6, "$Emit$") == 0)
		return file.substr(6);
	return file;
}

static std::string GetEmitUri(const std::string& typeName)
{
	return "beef-emit:/" + EncodeUriComponent(typeName);
}

void Server::SendDefinitionResponse(WorkResult& result)
{
	Json* id = result.mRequestId;
	result.mRequestId = NULL;

	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (id != NULL) ? id : Json::CreateNull());

	if (!result.mSuccess)
	{
		Json* errorObj = AddObject(response, "error");
		errorObj->AddNumberToObject("code", -32603);
		errorObj->AddStringToObject("message", result.mError.c_str());
		WriteLspMessage(response);
		return;
	}

	Json* locations = Json::CreateArray();
	for (const auto& location : result.mLocations)
	{
		Json* item = Json::CreateObject();
		// Generated code lives only inside the compiler; expose it through the
		// beef-emit: scheme so the client can peek it.
		std::string uri = location.mIsEmit ? GetEmitUri(GetEmitTypeName(location.mFile)) : PathToUri(location.mFile);
		item->AddStringToObject("uri", uri.c_str());
		Json* range = AddObject(item, "range");
		range->AddItemToObject("start", CreatePosition(location.mLine, location.mColumn));
		range->AddItemToObject("end", CreatePosition(location.mEndLine, location.mEndColumn));
		locations->AddItemToArray(item);
	}

	response->AddItemToObject("result", locations);

	Log("Definition for %s: %d location(s)", GetFileName(result.mPath).c_str(), (int)result.mLocations.size());
	WriteLspMessage(response);
}

void Server::HandleCodeLensRequest(Json* params, Json* idItem)
{
	Json* textDocument = GetItem(params, "textDocument");
	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
	{
		SendErrorResponse(idItem, -32602, "Missing document URI");
		return;
	}

	OpenDocument* document = FindOrLoadDocument(uri);
	if (document == NULL)
	{
		SendErrorResponse(idItem, -32602, "Document is not open: " + uri);
		return;
	}

	EnsureStartup();

	WorkItem item;
	item.mKind = WorkItem::Kind::CodeLens;
	item.mPath = document->mPath;
	item.mText = document->mText;
	item.mRequestId = Json::Duplicate(idItem, 1);
	item.mRequestVersion = document->mVersion;
	EnqueueWork(std::move(item), mWorkspaceReady);
}

void Server::SendCodeLensResponse(WorkResult& result)
{
	Json* id = result.mRequestId;
	result.mRequestId = NULL;

	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (id != NULL) ? id : Json::CreateNull());

	Json* lenses = Json::CreateArray();
	for (const auto& region : result.mEmitRegions)
	{
		Json* lens = Json::CreateObject();
		Json* range = AddObject(lens, "range");
		range->AddItemToObject("start", CreatePosition(region.mLine, 0));
		range->AddItemToObject("end", CreatePosition(region.mLine, 0));

		Json* command = AddObject(lens, "command");
		command->AddStringToObject("title", "Show generated code");
		command->AddStringToObject("command", "beef.showEmit");
		Json* arguments = AddArray(command, "arguments");
		arguments->AddItemToArray(Json::CreateString(GetEmitUri(region.mTypeName).c_str()));
		arguments->AddItemToArray(Json::CreateNumber(region.mEmitStartLine));
		arguments->AddItemToArray(Json::CreateNumber(region.mEmitEndLine));

		lenses->AddItemToArray(lens);
	}

	response->AddItemToObject("result", lenses);
	WriteLspMessage(response);
}

void Server::HandleEmitSourceRequest(Json* params, Json* idItem)
{
	std::string typeName = GetString(params, "typeName");
	if (typeName.empty())
	{
		static const std::string prefix = "beef-emit:/";
		std::string uri = GetString(params, "uri");
		if (uri.compare(0, prefix.size(), prefix) == 0)
			typeName = DecodeUriComponent(uri.substr(prefix.size()));
	}

	if (typeName.empty())
	{
		SendErrorResponse(idItem, -32602, "Invalid emit source request");
		return;
	}

	WorkItem item;
	item.mKind = WorkItem::Kind::EmitSource;
	item.mPath = typeName;
	item.mRequestId = Json::Duplicate(idItem, 1);
	EnqueueWork(std::move(item), true);
}

void Server::SendEmitSourceResponse(WorkResult& result)
{
	Json* id = result.mRequestId;
	result.mRequestId = NULL;

	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (id != NULL) ? id : Json::CreateNull());

	if (!result.mSuccess)
	{
		response->AddItemToObject("result", Json::CreateNull());
		WriteLspMessage(response);
		return;
	}

	Json* resultObj = Json::CreateObject();
	resultObj->AddStringToObject("text", result.mEmitText.c_str());
	resultObj->AddNumberToObject("version", result.mEmitVersion);
	response->AddItemToObject("result", resultObj);
	WriteLspMessage(response);
}

void Server::SendCompletionResponse(WorkResult& result)
{
	Json* id = result.mRequestId;
	result.mRequestId = NULL;

	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (id != NULL) ? id : Json::CreateNull());

	if (!result.mSuccess)
	{
		Json* errorObj = AddObject(response, "error");
		errorObj->AddNumberToObject("code", -32603);
		errorObj->AddStringToObject("message", result.mError.c_str());
		WriteLspMessage(response);
		return;
	}

	CompletionInfo completionInfo;
	ParseCompletionInfo(result.mCompletionInfo, completionInfo);

	const std::string& text = result.mText;
	int cursorOffset = result.mCursorOffset;
	if ((cursorOffset < 0) || (cursorOffset > (int)text.size()))
		cursorOffset = (int)text.size();

	int replaceStart = completionInfo.mInsertStart;
	int replaceEnd = completionInfo.mInsertEnd;
	if ((replaceStart < 0) || (replaceStart > (int)text.size()))
		replaceStart = WordStartBefore(text, cursorOffset);
	if ((replaceEnd < 0) || (replaceEnd > (int)text.size()))
		replaceEnd = WordStartBefore(text, cursorOffset);
	if (replaceEnd < replaceStart)
		replaceEnd = replaceStart;

	int startLine = 0, startCharacter = 0, endLine = 0, endCharacter = 0;
	PositionFromOffset(text, replaceStart, startLine, startCharacter);
	PositionFromOffset(text, replaceEnd, endLine, endCharacter);

	mResolveDocumentation.clear();

	Json* resultObj = Json::CreateObject();
	resultObj->AddBoolToObject("isIncomplete", false);
	Json* items = AddArray(resultObj, "items");

	bool selected = false;
	int entryIdx = 0;
	for (const auto& entry : completionInfo.mEntries)
	{
		Json* item = Json::CreateObject();
		item->AddStringToObject("label", entry.mLabel.c_str());
		item->AddNumberToObject("kind", GetCompletionKind(entry.mType));
		item->AddStringToObject("filterText", entry.mLabel.c_str());
		item->AddStringToObject("sortText", MakeSortText(entryIdx).c_str());

		Json* textEdit = AddObject(item, "textEdit");
		const std::string& newText = entry.mInsertText.empty() ? entry.mLabel : entry.mInsertText;
		textEdit->AddStringToObject("newText", newText.c_str());
		Json* range = AddObject(textEdit, "range");
		range->AddItemToObject("start", CreatePosition(startLine, startCharacter));
		range->AddItemToObject("end", CreatePosition(endLine, endCharacter));

		if ((!selected) && (!completionInfo.mSelect.empty()))
		{
			if (EqualsIgnoreCase(entry.mLabel, completionInfo.mSelect))
			{
				item->AddBoolToObject("preselect", true);
				selected = true;
			}
		}

		if (!entry.mDocumentation.empty())
		{
			mResolveDocumentation.push_back(entry.mDocumentation);
			item->AddNumberToObject("data", (int)mResolveDocumentation.size() - 1);
		}

		items->AddItemToArray(item);
		entryIdx++;
	}

	response->AddItemToObject("result", resultObj);

	Log("Completion for %s: %d entries", GetFileName(result.mPath).c_str(), (int)completionInfo.mEntries.size());
	WriteLspMessage(response);
}

Json* Server::HandleCompletionResolve(Json* params, std::string& outError)
{
	(void)outError;

	Json* item = Json::Duplicate(params, 1);
	if (item == NULL)
		return Json::CreateObject();

	Json* dataItem = item->GetObjectItem("data");
	if ((dataItem != NULL) && (dataItem->mType == Json::Type_Number))
	{
		int docIdx = dataItem->mValueInt;
		if ((docIdx >= 0) && (docIdx < (int)mResolveDocumentation.size()))
		{
			Json* documentation = AddObject(item, "documentation");
			documentation->AddStringToObject("kind", "markdown");
			documentation->AddStringToObject("value", mResolveDocumentation[docIdx].c_str());
		}
	}

	return item;
}

// --- notifications -----------------------------------------------------------

void Server::HandleDidOpen(Json* params)
{
	Json* textDocument = GetItem(params, "textDocument");
	if (textDocument == NULL)
		return;

	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
		return;

	OpenDocument document;
	document.mUri = uri;
	document.mText = GetString(textDocument, "text");
	Json* versionItem = textDocument->GetObjectItem("version");
	if ((versionItem != NULL) && (versionItem->mType == Json::Type_Number))
		document.mVersion = versionItem->mValueInt;
	if (!UriToPath(uri, document.mPath))
		return;

	mDocuments[uri] = document;
	// Opening a file that is already on disk needs syntax diagnostics only; the
	// workspace resolve already covers its types.
	ScheduleEdit(document.mPath, document.mText, false, false);
}

void Server::HandleDidChange(Json* params)
{
	Json* textDocument = GetItem(params, "textDocument");
	if (textDocument == NULL)
		return;

	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
		return;

	auto itr = mDocuments.find(uri);
	if (itr == mDocuments.end())
		return;

	Json* versionItem = textDocument->GetObjectItem("version");
	if ((versionItem != NULL) && (versionItem->mType == Json::Type_Number))
		itr->second.mVersion = versionItem->mValueInt;

	// We advertise full sync, so the last change event holds the whole text.
	Json* changes = GetItem(params, "contentChanges");
	if ((changes == NULL) || (changes->mType != Json::Type_Array))
		return;

	for (int i = 0; i < changes->GetArraySize(); i++)
	{
		Json* change = changes->GetArrayItem(i);
		itr->second.mText = GetString(change, "text");
	}

	ScheduleEdit(itr->second.mPath, itr->second.mText, false, true);
}

void Server::HandleDidSave(Json* params)
{
	Json* textDocument = GetItem(params, "textDocument");
	if (textDocument == NULL)
		return;

	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
		return;

	auto itr = mDocuments.find(uri);
	if (itr == mDocuments.end())
		return;

	ScheduleEdit(itr->second.mPath, itr->second.mText, true, true);
}

void Server::HandleDidClose(Json* params)
{
	Json* textDocument = GetItem(params, "textDocument");
	if (textDocument == NULL)
		return;

	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
		return;

	std::string path;
	auto itr = mDocuments.find(uri);
	if (itr != mDocuments.end())
	{
		path = itr->second.mPath;
		mDocuments.erase(itr);
	}
	else
	{
		UriToPath(uri, path);
	}

	if (path.empty())
		return;

	mPendingEdits.erase(path);
	mSyntaxDiagnosticsByFile.erase(path);
	// Falls back to the last compile diagnostics for the file, or clears them.
	PublishPathDiagnostics(path);
}

// --- scheduling --------------------------------------------------------------

void Server::EnsureStartup()
{
	if (mStartupQueued || mWorkspaceReady || mStartupFailed)
		return;

	if (mRootPath.empty())
	{
		mStartupFailed = true;
		SendLogMessage(1, "beeflsp: no workspace folder was provided; completion and diagnostics are unavailable");
		return;
	}

	mStartupQueued = true;
	WorkItem item;
	item.mKind = WorkItem::Kind::Startup;
	EnqueueWork(std::move(item));
	SendLogMessage(3, "beeflsp: loading Beef workspace at " + mRootPath);
}

void Server::ScheduleEdit(const std::string& path, const std::string& text, bool immediate, bool scheduleRefresh)
{
	PendingEdit& pending = mPendingEdits[path];
	pending.mText = text;

	auto now = Clock::now();
	pending.mAnalyzeDue = now + std::chrono::milliseconds(ANALYZE_DEBOUNCE_MS);
	if (scheduleRefresh)
		pending.mRefreshDue = immediate ? now : (now + std::chrono::milliseconds(mDiagnosticsDelayMs));
	else
		pending.mRefreshDue = Clock::time_point::max();
}

void Server::RunTimers()
{
	auto now = Clock::now();
	for (auto& entry : mPendingEdits)
	{
		PendingEdit& pending = entry.second;

		if (!pending.mAnalyzeQueued && (now >= pending.mAnalyzeDue))
		{
			pending.mAnalyzeQueued = true;
			WorkItem item;
			item.mKind = WorkItem::Kind::Analyze;
			item.mPath = entry.first;
			item.mText = pending.mText;
			EnqueueWork(std::move(item));
		}

		if (mWorkspaceReady && !pending.mRefreshQueued && (now >= pending.mRefreshDue))
		{
			int minIntervalMs = std::max(200, mDiagnosticsDelayMs * 2);
			auto earliestRefresh = pending.mLastRefresh + std::chrono::milliseconds(minIntervalMs);
			if (now < earliestRefresh)
			{
				pending.mRefreshDue = earliestRefresh;
			}
			else
			{
				pending.mRefreshQueued = true;
				WorkItem item;
				item.mKind = WorkItem::Kind::Refresh;
				item.mPath = entry.first;
				item.mText = pending.mText;
				EnqueueWork(std::move(item));
			}
		}
	}
}

// --- diagnostics -------------------------------------------------------------

void Server::UpdateCompileDiagnostics(const std::vector<Diagnostic>& diagnostics)
{
	mCompileDiagnosticsByFile.clear();
	for (const auto& diagnostic : diagnostics)
		mCompileDiagnosticsByFile[diagnostic.mFile].push_back(diagnostic);

	// Republish every file the compiler mentioned, plus files that currently
	// have diagnostics published but are now clean.
	std::set<std::string> paths = mPublishedDiagnosticPaths;
	for (const auto& entry : mCompileDiagnosticsByFile)
		paths.insert(entry.first);
	for (const auto& path : paths)
		PublishPathDiagnostics(path);
}

void Server::PublishPathDiagnostics(const std::string& path)
{
	std::vector<Diagnostic> empty;
	const std::vector<Diagnostic>* diagnostics = &empty;

	auto syntaxItr = mSyntaxDiagnosticsByFile.find(path);
	if ((syntaxItr != mSyntaxDiagnosticsByFile.end()) && !syntaxItr->second.empty())
	{
		diagnostics = &syntaxItr->second;
	}
	else
	{
		auto compileItr = mCompileDiagnosticsByFile.find(path);
		if (compileItr != mCompileDiagnosticsByFile.end())
			diagnostics = &compileItr->second;
	}

	bool wasPublished = mPublishedDiagnosticPaths.count(path) != 0;
	auto previousItr = mLastPublishedDiagnostics.find(path);
	bool unchanged = (previousItr != mLastPublishedDiagnostics.end()) && SameDiagnostics(previousItr->second, *diagnostics);
	if (unchanged)
		return;
	if (diagnostics->empty() && !wasPublished)
	{
		mLastPublishedDiagnostics.erase(path);
		return;
	}
	if (diagnostics->empty())
	{
		mPublishedDiagnosticPaths.erase(path);
		mLastPublishedDiagnostics.erase(path);
	}
	else
	{
		mPublishedDiagnosticPaths.insert(path);
		mLastPublishedDiagnostics[path] = *diagnostics;
	}

	std::string uri = PathToUri(path);
	Json* notification = Json::CreateObject();
	notification->AddStringToObject("jsonrpc", "2.0");
	notification->AddStringToObject("method", "textDocument/publishDiagnostics");
	Json* params = AddObject(notification, "params");
	params->AddStringToObject("uri", uri.c_str());
	Json* items = AddArray(params, "diagnostics");
	for (const auto& diagnostic : *diagnostics)
		items->AddItemToArray(CreateDiagnosticJson(diagnostic));

	auto documentItr = mDocuments.find(uri);
	if (documentItr != mDocuments.end())
		params->AddNumberToObject("version", documentItr->second.mVersion);

	WriteLspMessage(notification);
}

// --- misc --------------------------------------------------------------------

OpenDocument* Server::FindOrLoadDocument(const std::string& uri)
{
	auto itr = mDocuments.find(uri);
	if (itr != mDocuments.end())
		return &itr->second;

	OpenDocument document;
	document.mUri = uri;
	if (!UriToPath(uri, document.mPath))
		return NULL;

	std::ifstream stream(document.mPath, std::ios::in | std::ios::binary);
	if (!stream.is_open())
		return NULL;

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	document.mText = buffer.str();

	auto inserted = mDocuments.insert(std::make_pair(uri, document));
	return &inserted.first->second;
}

void Server::HandleHoverRequest(Json* params, Json* idItem)
{
	int offset = 0;
	OpenDocument* document = GetRequestDocument(params, idItem, "hover", offset);
	if (document == NULL)
		return;

	EnsureStartup();

	WorkItem item;
	item.mKind = WorkItem::Kind::Hover;
	item.mPath = document->mPath;
	item.mText = document->mText;
	item.mCursorOffset = offset;
	item.mRequestId = Json::Duplicate(idItem, 1);
	item.mRequestVersion = document->mVersion;
	EnqueueWork(std::move(item), mWorkspaceReady);
}

void Server::SendHoverResponse(WorkResult& result)
{
	Json* id = result.mRequestId;
	result.mRequestId = NULL;

	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (id != NULL) ? id : Json::CreateNull());

	if (!result.mSuccess || result.mHoverMarkdown.empty())
	{
		response->AddItemToObject("result", Json::CreateNull());
		WriteLspMessage(response);
		return;
	}

	Json* resultObj = Json::CreateObject();
	Json* contents = AddObject(resultObj, "contents");
	contents->AddStringToObject("kind", "markdown");
	contents->AddStringToObject("value", result.mHoverMarkdown.c_str());
	response->AddItemToObject("result", resultObj);
	WriteLspMessage(response);
}

void Server::HandleFoldingRangeRequest(Json* params, Json* idItem)
{
	Json* textDocument = GetItem(params, "textDocument");
	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
	{
		SendErrorResponse(idItem, -32602, "Missing document URI");
		return;
	}

	OpenDocument* document = FindOrLoadDocument(uri);
	if (document == NULL)
	{
		SendErrorResponse(idItem, -32602, "Document is not open: " + uri);
		return;
	}

	EnsureStartup();

	WorkItem item;
	item.mKind = WorkItem::Kind::FoldingRange;
	item.mPath = document->mPath;
	item.mText = document->mText;
	item.mRequestId = Json::Duplicate(idItem, 1);
	item.mRequestVersion = document->mVersion;
	EnqueueWork(std::move(item), mWorkspaceReady);
}

void Server::SendFoldingRangeResponse(WorkResult& result)
{
	Json* id = result.mRequestId;
	result.mRequestId = NULL;

	Json* response = Json::CreateObject();
	response->AddStringToObject("jsonrpc", "2.0");
	response->AddItemToObject("id", (id != NULL) ? id : Json::CreateNull());

	Json* ranges = Json::CreateArray();
	for (const auto& range : result.mFoldingRanges)
	{
		Json* item = Json::CreateObject();
		item->AddNumberToObject("startLine", range.mStartLine);
		item->AddNumberToObject("endLine", range.mEndLine);
		if (!range.mKind.empty())
			item->AddStringToObject("kind", range.mKind.c_str());
		ranges->AddItemToArray(item);
	}

	response->AddItemToObject("result", ranges);
	WriteLspMessage(response);
}

void Server::SendLogMessage(int type, const std::string& message)
{
	Log("%s", message.c_str());

	Json* notification = Json::CreateObject();
	notification->AddStringToObject("jsonrpc", "2.0");
	notification->AddStringToObject("method", "window/logMessage");
	Json* params = AddObject(notification, "params");
	params->AddNumberToObject("type", type);
	params->AddStringToObject("message", message.c_str());
	WriteLspMessage(notification);
}

void Server::SendNotification(const std::string& method, Json* params)
{
	Json* notification = Json::CreateObject();
	notification->AddStringToObject("jsonrpc", "2.0");
	notification->AddStringToObject("method", method.c_str());
	if (params != NULL)
		notification->AddItemToObject("params", params);
	WriteLspMessage(notification);
}

void Server::SendRequest(const std::string& method, Json* params)
{
	Json* request = Json::CreateObject();
	request->AddStringToObject("jsonrpc", "2.0");
	request->AddNumberToObject("id", mNextRequestId++);
	request->AddStringToObject("method", method.c_str());
	if (params != NULL)
		request->AddItemToObject("params", params);
	WriteLspMessage(request);
}

}
