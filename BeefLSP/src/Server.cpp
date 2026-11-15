#include "Server.h"

#include "JsonRpc.h"
#include "Log.h"
#include "LspText.h"

#include "BeefySysLib/util/Json.h"

#include <cctype>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>

using Beefy::Json;

namespace beflsp
{

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

static Json* CreatePosition(int line, int character)
{
	Json* position = Json::CreateObject();
	position->AddNumberToObject("line", line);
	position->AddNumberToObject("character", character);
	return position;
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

bool Server::EnsureWorkspace()
{
	if (mWorkspaceReady)
		return true;
	if (mWorkspaceLoadAttempted)
		return false;

	mWorkspaceLoadAttempted = true;

	if (mRootPath.empty())
	{
		Log("No workspace root; autocomplete will be unavailable");
		SendLogMessage(2, "beeflsp: no workspace folder was provided");
		return false;
	}

	SendLogMessage(3, "beeflsp: loading Beef workspace at " + mRootPath);

	std::string error;
	if (!mWorkspace.Discover(mRootPath, error))
	{
		Log("Workspace discovery failed: %s", error.c_str());
		SendLogMessage(1, "beeflsp: " + error);
		return false;
	}

	if (!mCompiler.Initialize(mWorkspace, error))
	{
		Log("Compiler initialization failed: %s", error.c_str());
		SendLogMessage(1, "beeflsp: " + error);
		return false;
	}

	mWorkspaceReady = true;
	SendLogMessage(3, "beeflsp: workspace resolved, autocomplete ready");
	return true;
}

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

int Server::Run()
{
	std::string body;
	while (ReadLspMessage(body))
	{
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

		if (mExitRequested)
			break;
	}

	return 0;
}

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
	if (method == "textDocument/completion")
		return HandleCompletion(params, outError);
	if (method == "completionItem/resolve")
		return HandleCompletionResolve(params, outError);

	outError = "Unsupported method: " + method;
	return NULL;
}

void Server::HandleNotification(const std::string& method, Json* params)
{
	if (method == "initialized")
	{
		EnsureWorkspace();
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
	if (method == "textDocument/didClose")
	{
		HandleDidClose(params);
		return;
	}
	if (method == "$/cancelRequest")
		return;
	if (method == "textDocument/didSave")
		return;

	Log("Ignoring notification %s", method.c_str());
}

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

	Json* result = Json::CreateObject();
	Json* capabilities = AddObject(result, "capabilities");

	Json* sync = AddObject(capabilities, "textDocumentSync");
	sync->AddBoolToObject("openClose", true);
	sync->AddNumberToObject("change", 1); // Full document sync

	Json* completionProvider = AddObject(capabilities, "completionProvider");
	completionProvider->AddBoolToObject("resolveProvider", true);
	Json* triggerCharacters = AddArray(completionProvider, "triggerCharacters");
	triggerCharacters->AddItemToArray(Json::CreateString("."));

	Json* serverInfo = AddObject(result, "serverInfo");
	serverInfo->AddStringToObject("name", "beeflsp");
	serverInfo->AddStringToObject("version", "0.1.0");

	return result;
}

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
		std::string text = GetString(change, "text");
		itr->second.mText = text;
	}
}

void Server::HandleDidClose(Json* params)
{
	Json* textDocument = GetItem(params, "textDocument");
	if (textDocument == NULL)
		return;

	std::string uri = GetString(textDocument, "uri");
	if (!uri.empty())
		mDocuments.erase(uri);
}

Json* Server::HandleCompletion(Json* params, std::string& outError)
{
	Json* result = Json::CreateObject();
	result->AddBoolToObject("isIncomplete", false);
	Json* items = AddArray(result, "items");

	Json* textDocument = GetItem(params, "textDocument");
	Json* position = GetItem(params, "position");
	if ((textDocument == NULL) || (position == NULL))
	{
		outError = "Invalid completion parameters";
		return result;
	}

	std::string uri = GetString(textDocument, "uri");
	if (uri.empty())
	{
		outError = "Missing document URI";
		return result;
	}

	Json* lineItem = position->GetObjectItem("line");
	Json* characterItem = position->GetObjectItem("character");
	if ((lineItem == NULL) || (characterItem == NULL))
	{
		outError = "Invalid position";
		return result;
	}

	OpenDocument* document = FindOrLoadDocument(uri);
	if (document == NULL)
	{
		outError = "Document is not open: " + uri;
		return result;
	}

	if (!EnsureWorkspace())
		return result;

	int offset = OffsetFromPosition(document->mText, lineItem->mValueInt, characterItem->mValueInt);

	Log("Completion request: uri=%s textLen=%d position=%d:%d offset=%d",
		uri.c_str(), (int)document->mText.size(), lineItem->mValueInt, characterItem->mValueInt, offset);

	std::string info;
	std::string avError;
	if (!mCompiler.Autocomplete(document->mPath, document->mText, offset, info, avError))
	{
		SendLogMessage(1, "beeflsp: " + avError);
		return result;
	}

	CompletionInfo completionInfo;
	ParseCompletionInfo(info, completionInfo);

	// Range that the insert text replaces. The compiler hands us exact byte
	// offsets; fall back to the identifier before the cursor when it doesn't.
	int replaceStart = completionInfo.mInsertStart;
	int replaceEnd = completionInfo.mInsertEnd;
	if ((replaceStart < 0) || (replaceStart > (int)document->mText.size()))
		replaceStart = WordStartBefore(document->mText, offset);
	if ((replaceEnd < 0) || (replaceEnd > (int)document->mText.size()))
		replaceEnd = offset;
	if (replaceEnd < replaceStart)
		replaceEnd = replaceStart;

	int startLine = 0, startCharacter = 0, endLine = 0, endCharacter = 0;
	PositionFromOffset(document->mText, replaceStart, startLine, startCharacter);
	PositionFromOffset(document->mText, replaceEnd, endLine, endCharacter);

	mResolveDocumentation.clear();

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

	Log("Completion for %s: %d entries", GetFileName(document->mPath).c_str(), (int)completionInfo.mEntries.size());

	return result;
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

}
