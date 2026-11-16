#pragma once

// Declarations for the C entry points that the Beef IDE uses to drive the
// compiler frontend (see IDEHelper/Compiler/BfSystem.cpp, BfParser.cpp and
// BfCompiler.cpp). The definitions carry C linkage, so importing them here
// gives us the exact same API the IDE consumes through its extern block.

#include "BeefySysLib/Common.h"
#include "Compiler/BfSystem.h"
#include "Compiler/BfParser.h"
#include "Compiler/BfResolvePass.h"

// --- BfSystem.cpp -----------------------------------------------------------

BF_EXPORT Beefy::BfSystem* BF_CALLTYPE BfSystem_Create();
BF_EXPORT void BF_CALLTYPE BfSystem_Delete(Beefy::BfSystem* bfSystem);
BF_EXPORT Beefy::BfProject* BF_CALLTYPE BfSystem_CreateProject(Beefy::BfSystem* bfSystem, const char* projectName, const char* projectDir);
BF_EXPORT void BF_CALLTYPE BfProject_Delete(Beefy::BfProject* bfProject);
BF_EXPORT void BF_CALLTYPE BfProject_AddDependency(Beefy::BfProject* bfProject, Beefy::BfProject* depProject);
BF_EXPORT void BF_CALLTYPE BfProject_SetOptions(Beefy::BfProject* bfProject, int targetType, const char* startupObject, const char* preprocessorMacros,
	int optLevel, int ltoType, int relocType, int picLevel, Beefy::BfProjectFlags flags);

BF_EXPORT Beefy::BfParser* BF_CALLTYPE BfSystem_CreateParser(Beefy::BfSystem* bfSystem, Beefy::BfProject* bfProject);
BF_EXPORT void BF_CALLTYPE BfSystem_DeleteParser(Beefy::BfSystem* bfSystem, Beefy::BfParser* bfParser);
BF_EXPORT Beefy::BfCompiler* BF_CALLTYPE BfSystem_CreateCompiler(Beefy::BfSystem* bfSystem, bool isResolveOnly);
BF_EXPORT Beefy::BfPassInstance* BF_CALLTYPE BfSystem_CreatePassInstance(Beefy::BfSystem* bfSystem);
BF_EXPORT void BF_CALLTYPE BfPassInstance_Delete(Beefy::BfPassInstance* bfPassInstance);
BF_EXPORT int BF_CALLTYPE BfPassInstance_GetErrorCount(Beefy::BfPassInstance* bfPassInstance);
BF_EXPORT const char* BF_CALLTYPE BfPassInstance_GetErrorData(Beefy::BfPassInstance* bfPassInstance, int errorIdx, int& outCode,
	bool& outIsWarning, bool& outIsAfter, bool& outIsDeferred, Beefy::BfWhileSpecializingFlags& outIsWhileSpecializing,
	bool& outIsPersistent, char*& projectName, char*& fileName, int& outSrcStart, int& outSrcEnd, int* outLine, int* outColumn,
	int& outMoreInfoCount);

BF_EXPORT void BF_CALLTYPE BfSystem_Lock(Beefy::BfSystem* bfSystem, int priority);
BF_EXPORT void BF_CALLTYPE BfSystem_Unlock(Beefy::BfSystem* bfSystem);
BF_EXPORT void BF_CALLTYPE BfSystem_RemoveOldParsers(Beefy::BfSystem* bfSystem);
BF_EXPORT void BF_CALLTYPE BfSystem_RemoveOldData(Beefy::BfSystem* bfSystem);

// --- BfParser.cpp -----------------------------------------------------------

BF_EXPORT void BF_CALLTYPE BfParser_SetSource(Beefy::BfParser* bfParser, const char* data, int length, const char* fileName, int textVersion);
BF_EXPORT void BF_CALLTYPE BfParser_SetIsClassifying(Beefy::BfParser* bfParser);
BF_EXPORT void BF_CALLTYPE BfParser_SetNextRevision(Beefy::BfParser* bfParser, Beefy::BfParser* nextRevision);
BF_EXPORT void BF_CALLTYPE BfParser_SetAutocomplete(Beefy::BfParser* bfParser, int cursorIdx);
BF_EXPORT bool BF_CALLTYPE BfParser_Parse(Beefy::BfParser* bfParser, Beefy::BfPassInstance* bfPassInstance, bool compatMode);
BF_EXPORT bool BF_CALLTYPE BfParser_Reduce(Beefy::BfParser* bfParser, Beefy::BfPassInstance* bfPassInstance);
BF_EXPORT bool BF_CALLTYPE BfParser_BuildDefs(Beefy::BfParser* bfParser, Beefy::BfPassInstance* bfPassInstance, Beefy::BfResolvePassData* resolvePassData, bool fullRefresh);
BF_EXPORT void BF_CALLTYPE BfParser_CreateClassifier(Beefy::BfParser* bfParser, Beefy::BfPassInstance* bfPassInstance, Beefy::BfResolvePassData* resolvePassData, Beefy::BfSourceClassifier::CharData* charData);
BF_EXPORT void BF_CALLTYPE BfParser_FinishClassifier(Beefy::BfParser* bfParser, Beefy::BfResolvePassData* resolvePassData);
BF_EXPORT Beefy::BfResolvePassData* BF_CALLTYPE BfParser_CreateResolvePassData(Beefy::BfParser* bfParser, Beefy::BfResolveType resolveType, bool doFuzzyAutoComplete);
BF_EXPORT void BF_CALLTYPE BfResolvePassData_Delete(Beefy::BfResolvePassData* resolvePassData);

// --- BfCompiler.cpp ---------------------------------------------------------

BF_EXPORT void BF_CALLTYPE BfCompiler_SetOptions(Beefy::BfCompiler* bfCompiler, Beefy::BfProject* hotProject, int hotIdx,
	const char* targetTriple, const char* targetCPU, int toolsetType, int simdSetting, int allocStackCount, int maxWorkerThreads,
	Beefy::BfCompilerOptionFlags optionFlags, char* mallocLinkName, char* freeLinkName);
BF_EXPORT bool BF_CALLTYPE BfCompiler_Compile(Beefy::BfCompiler* bfCompiler, Beefy::BfPassInstance* bfPassInstance, const char* outputPath);
BF_EXPORT bool BF_CALLTYPE BfCompiler_ClassifySource(Beefy::BfCompiler* bfCompiler, Beefy::BfPassInstance* bfPassInstance, Beefy::BfResolvePassData* resolvePassData);
BF_EXPORT const char* BF_CALLTYPE BfCompiler_GetAutocompleteInfo(Beefy::BfCompiler* bfCompiler);
BF_EXPORT void BF_CALLTYPE BfCompiler_Delete(Beefy::BfCompiler* bfCompiler);
BF_EXPORT void BF_CALLTYPE BfCompiler_ClearResults(Beefy::BfCompiler* bfCompiler);

// Generated ("emitted") source produced by comptime code. 'fileName' is
// "$Emit$<project:TypeName>"; GetEmitSource returns the generated text.
BF_EXPORT const char* BF_CALLTYPE BfCompiler_GetEmitSource(Beefy::BfCompiler* bfCompiler, const char* fileName);
BF_EXPORT int BF_CALLTYPE BfCompiler_GetEmitSourceVersion(Beefy::BfCompiler* bfCompiler, const char* fileName);
// Collapse/fold regions for a parsed document, including the emit regions the
// IDE turns into "Emit" buttons.
BF_EXPORT const char* BF_CALLTYPE BfCompiler_GetCollapseRegions(Beefy::BfCompiler* bfCompiler, Beefy::BfParser* bfParser,
	Beefy::BfResolvePassData* resolvePassData, char* explicitEmitTypeNames);

// --- DebugManager.cpp -------------------------------------------------------

// Initializes process-wide compiler/codegen state (LLVM target registration).
// The IDE calls this once before creating its BfSystem.
BF_EXPORT void BF_CALLTYPE IDEHelper_ProgramStart();
BF_EXPORT void BF_CALLTYPE IDEHelper_ProgramDone();
