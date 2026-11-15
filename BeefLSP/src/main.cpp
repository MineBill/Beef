#include "BeefCompiler.h"
#include "CompletionParser.h"
#include "LspText.h"
#include "Server.h"
#include "Workspace.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// Prints a symbolicated stack for crashes inside the compiler DLL, which is
// otherwise very hard to diagnose from a language server.
static LONG WINAPI BeefLspCrashHandler(EXCEPTION_POINTERS* exceptionInfo)
{
	static bool isHandling = false;
	if (isHandling)
		return EXCEPTION_EXECUTE_HANDLER;
	isHandling = true;

	fprintf(stderr, "[beeflsp] Unhandled exception 0x%08X at %p\n",
		(unsigned)exceptionInfo->ExceptionRecord->ExceptionCode,
		exceptionInfo->ExceptionRecord->ExceptionAddress);

	HANDLE process = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(process, NULL, TRUE);

	CONTEXT context = *exceptionInfo->ContextRecord;
	STACKFRAME64 frame = {};
	frame.AddrPC.Offset = context.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrStack.Offset = context.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = context.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;

	HANDLE thread = GetCurrentThread();
	for (int idx = 0; idx < 48; idx++)
	{
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL,
			SymFunctionTableAccess64, SymGetModuleBase64, NULL))
			break;
		if (frame.AddrPC.Offset == 0)
			break;

		char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
		SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 511;

		DWORD64 displacement = 0;
		const char* moduleName = "?";
		IMAGEHLP_MODULE64 moduleInfo = {};
		moduleInfo.SizeOfStruct = sizeof(moduleInfo);
		if (SymGetModuleInfo64(process, frame.AddrPC.Offset, &moduleInfo))
			moduleName = moduleInfo.ModuleName;

		if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
			fprintf(stderr, "  #%02d %s!%s+0x%llx\n", idx, moduleName, symbol->Name, (unsigned long long)displacement);
		else
			fprintf(stderr, "  #%02d %s!0x%llx\n", idx, moduleName, (unsigned long long)frame.AddrPC.Offset);
	}

	fflush(stderr);
	_exit(2);
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Smoke test helper: resolves a workspace and runs one autocomplete request
// without the LSP framing, so the compiler pipeline can be exercised on its own.
//   beeflsp --completion <workspaceRoot> <file> <line> <character>
static int RunCompletion(const std::string& rootPath, const std::string& filePath, int line, int character)
{
	beflsp::Workspace workspace;
	std::string error;
	if (!workspace.Discover(rootPath, error))
	{
		fprintf(stderr, "Workspace discovery failed: %s\n", error.c_str());
		return 1;
	}

	beflsp::BeefCompilerHost compiler;
	if (!compiler.Initialize(workspace, error))
	{
		fprintf(stderr, "Compiler initialization failed: %s\n", error.c_str());
		return 1;
	}

	auto root = std::filesystem::path(workspace.GetRootPath());

	std::ifstream stream(root / filePath, std::ios::in | std::ios::binary);
	if (!stream.is_open())
	{
		fprintf(stderr, "Could not open %s\n", filePath.c_str());
		return 1;
	}

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	std::string text = buffer.str();

	int offset = beflsp::OffsetFromPosition(text, line, character);

	std::string info;
	if (!compiler.Autocomplete(beflsp::NormalizePath(filePath), text, offset, info, error))
	{
		fprintf(stderr, "Autocomplete failed: %s\n", error.c_str());
		return 1;
	}

	beflsp::CompletionInfo completionInfo;
	beflsp::ParseCompletionInfo(info, completionInfo);

	printf("insertRange: %d %d\n", completionInfo.mInsertStart, completionInfo.mInsertEnd);
	printf("select: %s\n", completionInfo.mSelect.c_str());
	printf("entries: %d\n", (int)completionInfo.mEntries.size());
	for (const auto& entry : completionInfo.mEntries)
	{
		printf("  %-12s %s", entry.mType.c_str(), entry.mLabel.c_str());
		if (!entry.mInsertText.empty() && (entry.mInsertText != entry.mLabel))
			printf("  -> %s", entry.mInsertText.c_str());
		if (!entry.mDocumentation.empty())
		{
			std::string firstLine = entry.mDocumentation.substr(0, entry.mDocumentation.find('\n'));
			printf("  [doc: %s]", firstLine.c_str());
		}
		printf("\n");
	}

	return 0;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	SetUnhandledExceptionFilter(BeefLspCrashHandler);
#endif

	if (argc >= 2)
	{
		std::string arg = argv[1];
		if ((arg == "--completion") && (argc >= 6))
			return RunCompletion(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
		if ((arg == "--version") || (arg == "-v"))
		{
			printf("beeflsp 0.1.0\n");
			return 0;
		}
		if ((arg == "--help") || (arg == "-h"))
		{
			printf("beeflsp - Beef language server (completion)\n"
				"Speaks the Language Server Protocol over stdin/stdout.\n\n"
				"  beeflsp                      run the language server\n"
				"  beeflsp --completion <root> <file> <line> <character>\n"
				"                               run one autocomplete request and print the result\n");
			return 0;
		}
	}

#ifdef _WIN32
	// The JSON-RPC framing is byte exact; text mode would translate CRLF and
	// swallow 0x1A.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	beflsp::Server server;
	return server.Run();
}
