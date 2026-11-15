#include "CompletionParser.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace beflsp
{

static std::string StripAtPrefixes(const std::string& str)
{
	size_t idx = 0;
	while ((idx < str.size()) && (str[idx] == '@'))
		idx++;
	return str.substr(idx);
}

void ParseCompletionInfo(const std::string& info, CompletionInfo& outInfo)
{
	outInfo = CompletionInfo();

	size_t lineStart = 0;
	while (lineStart <= info.size())
	{
		size_t lineEnd = info.find('\n', lineStart);
		if (lineEnd == std::string::npos)
			lineEnd = info.size();

		std::string line = info.substr(lineStart, lineEnd - lineStart);
		lineStart = lineEnd + 1;

		if (line.empty())
			continue;

		size_t tabPos = line.find('\t');
		std::string entryType = (tabPos == std::string::npos) ? line : line.substr(0, tabPos);
		std::string entryRest = (tabPos == std::string::npos) ? "" : line.substr(tabPos + 1);

		if (entryType == "uncertain")
		{
			outInfo.mUncertain = true;
			continue;
		}

		if (entryType == "select")
		{
			outInfo.mSelect = entryRest;
			continue;
		}

		if (entryType == "insertRange")
		{
			size_t spacePos = entryRest.find(' ');
			if (spacePos != std::string::npos)
			{
				outInfo.mInsertStart = atoi(entryRest.substr(0, spacePos).c_str());
				outInfo.mInsertEnd = atoi(entryRest.substr(spacePos + 1).c_str());
			}
			continue;
		}

		if (entryType == "defLoc")
		{
			size_t firstTab = entryRest.find('\t');
			size_t secondTab = (firstTab == std::string::npos) ? std::string::npos : entryRest.find('\t', firstTab + 1);
			if (secondTab != std::string::npos)
			{
				outInfo.mDefFile = entryRest.substr(0, firstTab);
				outInfo.mDefLine = atoi(entryRest.substr(firstTab + 1, secondTab - firstTab - 1).c_str());
				outInfo.mDefChar = atoi(entryRest.substr(secondTab + 1).c_str());
			}
			continue;
		}

		// Signature/invoke widget data. Not used for completion yet.
		if ((entryType == "invoke") || (entryType == "invoke_cur") || (entryType == "invokeInfo") ||
			(entryType == "invokeLeftParen") || (entryType == "isMember"))
		{
			continue;
		}

		// Fixits are code actions, not completions, and embed a payload after
		// the display text.
		if (entryType == "fixit")
			continue;

		CompletionEntry entry;
		entry.mType = entryType;

		// Split documentation off first: the documentation may itself contain
		// tabs and newlines are already handled by the line splitting.
		size_t docPos = entryRest.find('\x03');
		if (docPos != std::string::npos)
		{
			entry.mDocumentation = entryRest.substr(docPos + 1);
			// BfCommentNode::GetDocString encodes line breaks in documentation as
			// 0x03 too, so everything after the first marker is documentation.
			for (char& c : entry.mDocumentation)
				if (c == '\x03')
					c = '\n';
			entryRest = entryRest.substr(0, docPos);
		}

		// Drop the match index list if present (fuzzy highlighting).
		size_t matchPos = entryRest.find('\x02');
		if (matchPos != std::string::npos)
			entryRest = entryRest.substr(0, matchPos);

		// The override/insert text follows a second tab.
		size_t insertPos = entryRest.find('\t');
		std::string insertText;
		if (insertPos != std::string::npos)
		{
			insertText = entryRest.substr(insertPos + 1);
			entryRest = entryRest.substr(0, insertPos);
		}

		entry.mLabel = StripAtPrefixes(entryRest);
		if (entry.mLabel.empty())
			continue;

		entry.mInsertText = insertText.empty() ? entry.mLabel : StripAtPrefixes(insertText);

		outInfo.mEntries.push_back(std::move(entry));
	}
}

}
