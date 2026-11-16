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

		// Signature/invoke widget data.
		if ((entryType == "invoke") || (entryType == "invoke_cur"))
		{
			std::string documentation;
			size_t docPos = entryRest.find('\x03');
			if (docPos != std::string::npos)
			{
				documentation = entryRest.substr(docPos + 1);
				for (char& c : documentation)
				{
					if (c == '\x03')
						c = '\n';
				}
				entryRest = entryRest.substr(0, docPos);
			}

			// Drop the trailing "\t<argMatchCount>".
			size_t lastTab = entryRest.rfind('\t');
			std::string text = (lastTab == std::string::npos) ? entryRest : entryRest.substr(0, lastTab);

			// The compiler marks each parameter with \x01, e.g.
			//   "void Console.WriteLine(\x01String value\x01)"
			InvokeSignature signature;
			signature.mDocumentation = documentation;

			std::vector<std::string> parts;
			size_t partStart = 0;
			while (true)
			{
				size_t marker = text.find('\x01', partStart);
				if (marker == std::string::npos)
				{
					parts.push_back(text.substr(partStart));
					break;
				}
				parts.push_back(text.substr(partStart, marker - partStart));
				partStart = marker + 1;
			}

			std::vector<int> partOffsets;
			for (const auto& part : parts)
			{
				partOffsets.push_back((int)signature.mLabel.size());
				signature.mLabel += part;
			}

			for (size_t idx = 1; idx + 1 < parts.size(); idx++)
			{
				int start = partOffsets[idx];
				int end = start + (int)parts[idx].size();
				while ((start < end) && (signature.mLabel[start] == ' '))
					start++;
				while ((end > start) && ((signature.mLabel[end - 1] == ' ') || (signature.mLabel[end - 1] == ',')))
					end--;
				signature.mParameters.emplace_back(start, end);
			}

			outInfo.mInvokes.push_back(std::move(signature));
			continue;
		}

		if (entryType == "invokeInfo")
		{
			// "<bestIdx> <argStartPos>..."
			size_t pos = 0;
			bool first = true;
			while (pos < entryRest.size())
			{
				size_t space = entryRest.find(' ', pos);
				std::string token = (space == std::string::npos) ? entryRest.substr(pos) : entryRest.substr(pos, space - pos);
				pos = (space == std::string::npos) ? entryRest.size() : space + 1;
				if (token.empty())
					continue;
				int value = atoi(token.c_str());
				if (first)
				{
					outInfo.mInvokeSelect = value;
					first = false;
				}
				else
					outInfo.mInvokePositions.push_back(value);
			}
			continue;
		}

		if ((entryType == "invokeLeftParen") || (entryType == "isMember"))
			continue;

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
