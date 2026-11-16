#pragma once

#include <string>
#include <utility>
#include <vector>

namespace beflsp
{

struct CompletionEntry
{
	std::string mType;          // "method", "field", "class", ...
	std::string mLabel;         // display text without the '@' prefix markers
	std::string mInsertText;    // text that should end up in the document
	std::string mDocumentation;
};

// One overload of a call the cursor is inside ("invoke" line).
struct InvokeSignature
{
	std::string mLabel;                                // signature, without the \x01 markers
	std::string mDocumentation;
	std::vector<std::pair<int, int>> mParameters;      // label offsets per parameter
};

struct CompletionInfo
{
	std::vector<CompletionEntry> mEntries;
	int mInsertStart = -1;      // byte offsets into the submitted source
	int mInsertEnd = -1;
	std::string mSelect;        // default selection label
	std::string mUncertainReason;
	bool mUncertain = false;
	std::string mDefFile;
	int mDefLine = -1;
	int mDefChar = -1;

	std::vector<InvokeSignature> mInvokes;
	int mInvokeSelect = -1;             // best overload index
	std::vector<int> mInvokePositions;  // start of each argument, in byte offsets
};

// Parses the line protocol produced by BfCompiler::GenerateAutocompleteInfo.
// Entries look like:
//   type \t [@]display [ \t insert ] [ \x02matches X ] [ \x03documentation] \n
// plus control lines (insertRange, select, defLoc, invoke, ...).
void ParseCompletionInfo(const std::string& info, CompletionInfo& outInfo);

}
