#pragma once

// Declarations for the debugger entry points in IDEHelper/DebugManager.cpp
// (see IDEHelper/Debugger.h and IDEHelper/DebugManager.h). beeflsp uses these
// for the `--debug-adapter` mode, the same API the Beef IDE drives.

#include "BeefySysLib/Common.h"

namespace Beefy
{

class Breakpoint;

} // namespace Beefy

// --- lifecycle --------------------------------------------------------------

BF_EXPORT void BF_CALLTYPE IDEHelper_ProgramStart();
BF_EXPORT void BF_CALLTYPE IDEHelper_ProgramDone();
BF_EXPORT void BF_CALLTYPE Targets_Create();
BF_EXPORT void BF_CALLTYPE Targets_Delete();
BF_EXPORT void BF_CALLTYPE Debugger_Create();
BF_EXPORT void BF_CALLTYPE Debugger_Delete();
BF_EXPORT bool BF_CALLTYPE Debugger_LoadDebugVisualizers(const char* fileName);
BF_EXPORT void BF_CALLTYPE Debugger_SetSymSrvOptions(const char* symCacheDir, const char* symSrvStr, int flags);
BF_EXPORT void BF_CALLTYPE Debugger_SetSourcePathRemap(const char* remapStr);

// --- target -----------------------------------------------------------------

BF_EXPORT bool BF_CALLTYPE Debugger_OpenFile(const char* launchPath, const char* targetPath, const char* args,
	const char* workingDir, void* envBlockPtr, int envBlockSize, bool hotSwapEnabled, int openFileFlags);
BF_EXPORT bool BF_CALLTYPE Debugger_Attach(int processId, int attachFlags);
BF_EXPORT void BF_CALLTYPE Debugger_Run();
BF_EXPORT void BF_CALLTYPE Debugger_Update();
BF_EXPORT int BF_CALLTYPE Debugger_GetRunState();
BF_EXPORT bool BF_CALLTYPE Debugger_HasPendingDebugLoads();
BF_EXPORT bool Debugger_GetStdHandles(BfpFile** outStdIn, BfpFile** outStdOut, BfpFile** outStdErr);
BF_EXPORT void BF_CALLTYPE Debugger_Continue();
BF_EXPORT void BF_CALLTYPE Debugger_BreakAll();
BF_EXPORT void BF_CALLTYPE Debugger_StepInto(bool inAssembly);
BF_EXPORT void BF_CALLTYPE Debugger_StepOver(bool inAssembly);
BF_EXPORT void BF_CALLTYPE Debugger_StepOut(bool inAssembly);
BF_EXPORT void BF_CALLTYPE Debugger_StopDebugging();
BF_EXPORT void BF_CALLTYPE Debugger_Terminate();
BF_EXPORT void BF_CALLTYPE Debugger_Detach();
BF_EXPORT int BF_CALLTYPE Debugger_GetProcessId();
BF_EXPORT const char* BF_CALLTYPE Debugger_GetCurrentException();

// Run states, from IDEHelper/Debugger.h.
enum BeefDbgRunState
{
	BeefDbgRunState_NotStarted,
	BeefDbgRunState_Running,
	BeefDbgRunState_Running_ToTempBreakpoint,
	BeefDbgRunState_Paused,
	BeefDbgRunState_Breakpoint,
	BeefDbgRunState_DebugEval,
	BeefDbgRunState_DebugEval_Done,
	BeefDbgRunState_HotStep,
	BeefDbgRunState_Exception,
	BeefDbgRunState_Terminating,
	BeefDbgRunState_Terminated,
	BeefDbgRunState_SearchingSymSrv,
	BeefDbgRunState_HotResolve,
	BeefDbgRunState_TargetUnloaded,
};

// --- messages ---------------------------------------------------------------

BF_EXPORT const char* BF_CALLTYPE Debugger_PopMessage();
BF_EXPORT bool BF_CALLTYPE Debugger_HasMessages();

// --- breakpoints ------------------------------------------------------------

BF_EXPORT Beefy::Breakpoint* BF_CALLTYPE Debugger_CreateBreakpoint(const char* fileName, int lineNum, int wantColumn, int instrOffset);
BF_EXPORT Beefy::Breakpoint* BF_CALLTYPE Debugger_GetActiveBreakpoint();
BF_EXPORT void BF_CALLTYPE Breakpoint_Delete(Beefy::Breakpoint* breakpoint);
BF_EXPORT void BF_CALLTYPE Breakpoint_Check(Beefy::Breakpoint* breakpoint);
BF_EXPORT intptr BF_CALLTYPE Breakpoint_GetAddress(Beefy::Breakpoint* breakpoint, Beefy::Breakpoint** outLinkedSibling);
BF_EXPORT intptr BF_CALLTYPE Breakpoint_GetLineNum(Beefy::Breakpoint* breakpoint);
BF_EXPORT void BF_CALLTYPE Breakpoint_SetCondition(Beefy::Breakpoint* breakpoint, const char* condition);
BF_EXPORT void BF_CALLTYPE Breakpoint_SetLogging(Beefy::Breakpoint* breakpoint, const char* logging, bool breakAfterLogging);
BF_EXPORT void BF_CALLTYPE Breakpoint_SetHitCountTarget(Beefy::Breakpoint* breakpoint, int targetHitCount, int breakKind);

// Hit count kinds, from IDEHelper/Debugger.h.
enum BeefDbgHitCountKind
{
	BeefDbgHitCountKind_None,
	BeefDbgHitCountKind_Equals,
	BeefDbgHitCountKind_GreaterEquals,
	BeefDbgHitCountKind_Multiple,
};

// --- threads ----------------------------------------------------------------

BF_EXPORT const char* BF_CALLTYPE Debugger_GetThreadInfo();
BF_EXPORT void BF_CALLTYPE Debugger_SetActiveThread(int threadId);
BF_EXPORT int BF_CALLTYPE Debugger_GetActiveThread();

// --- call stack -------------------------------------------------------------

BF_EXPORT void BF_CALLTYPE CallStack_Update();
BF_EXPORT int BF_CALLTYPE CallStack_GetCount();
BF_EXPORT const char* BF_CALLTYPE CallStack_GetStackFrameInfo(int stackFrameIdx, intptr* addr, const char** outFile,
	int32* outHotIdx, int32* outDefLineStart, int32* outDefLineEnd, int32* outLine, int32* outColumn,
	int32* outLanguage, int32* outStackSize, int8* outFlags);

// --- evaluation -------------------------------------------------------------

// EvalExpressionFlags, from IDEHelper/Debugger.h.
enum BeefDbgEvalFlags
{
	BeefDbgEvalFlag_None = 0,
	BeefDbgEvalFlag_FullPrecision = 0x01,
	BeefDbgEvalFlag_ValidateOnly = 0x02,
	BeefDbgEvalFlag_DeselectCallStackIdx = 0x04,
	BeefDbgEvalFlag_AllowSideEffects = 0x08,
	BeefDbgEvalFlag_AllowCalls = 0x10,
	BeefDbgEvalFlag_AllowProperties = 0x20,
	BeefDbgEvalFlag_MemoryAddress = 0x40,
	BeefDbgEvalFlag_MemoryWatch = 0x80,
	BeefDbgEvalFlag_Symbol = 0x100,
	BeefDbgEvalFlag_StepIntoCalls = 0x200,
	BeefDbgEvalFlag_RawStr = 0x400,
	BeefDbgEvalFlag_AllowStringView = 0x800,
};

BF_EXPORT Beefy::StringView BF_CALLTYPE Debugger_Evaluate(const char* expr, int callStackIdx, int cursorPos,
	int32 language, uint16 expressionFlags);
BF_EXPORT const char* BF_CALLTYPE Debugger_GetAutoLocals(int callStackIdx, bool showRegs);
BF_EXPORT const char* BF_CALLTYPE Debugger_GetCollectionContinuation(const char* continuationData, int callStackIdx, int count);
