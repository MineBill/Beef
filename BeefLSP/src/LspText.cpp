#include "LspText.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace beflsp
{

using namespace std::filesystem;
using path_t = std::filesystem::path;

std::string GetExecutablePath()
{
#ifdef _WIN32
	char buffer[MAX_PATH * 4];
	DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)sizeof(buffer));
	if ((length > 0) && (length < sizeof(buffer)))
		return NormalizePath(std::string(buffer, length));
#else
	char buffer[4096];
	ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length > 0)
		return NormalizePath(std::string(buffer, (size_t)length));
#endif
	return "";
}

std::string NormalizePath(const std::string& path)
{
	if (path.empty())
		return path;

	std::error_code ec;
	path_t normalized = weakly_canonical(path_t(path), ec);
	if (ec)
		normalized = absolute(path_t(path), ec);
	if (ec)
		normalized = path_t(path);

	std::string str = normalized.generic_string();
	while ((str.size() > 1) && (str.back() == '/') && (str[str.size() - 2] != ':'))
		str.pop_back();
	return str;
}

std::string JoinPath(const std::string& dir, const std::string& rel)
{
	if (rel.empty())
		return dir;
	path_t relPath(rel);
	if (relPath.is_absolute())
		return NormalizePath(rel);
	return NormalizePath((path_t(dir) / relPath).generic_string());
}

std::string GetDirectory(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return "";
	if (slash == 0)
		return "/";
	return path.substr(0, slash);
}

std::string GetFileName(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return path;
	return path.substr(slash + 1);
}

std::string GetExtension(const std::string& path)
{
	std::string fileName = GetFileName(path);
	size_t dot = fileName.find_last_of('.');
	if (dot == std::string::npos)
		return "";
	return fileName.substr(dot);
}

bool PathExists(const std::string& path)
{
	std::error_code ec;
	return exists(path_t(path), ec);
}

bool IsDirectory(const std::string& path)
{
	std::error_code ec;
	return is_directory(path_t(path), ec);
}

static int HexDigit(char c)
{
	if ((c >= '0') && (c <= '9'))
		return c - '0';
	if ((c >= 'a') && (c <= 'f'))
		return c - 'a' + 10;
	if ((c >= 'A') && (c <= 'F'))
		return c - 'A' + 10;
	return -1;
}

static std::string PercentDecode(const std::string& str)
{
	std::string result;
	result.reserve(str.size());
	for (size_t i = 0; i < str.size(); i++)
	{
		if ((str[i] == '%') && (i + 2 < str.size()))
		{
			int hi = HexDigit(str[i + 1]);
			int lo = HexDigit(str[i + 2]);
			if ((hi >= 0) && (lo >= 0))
			{
				result += (char)((hi << 4) | lo);
				i += 2;
				continue;
			}
		}
		result += str[i];
	}
	return result;
}

static bool IsUriSafe(char c)
{
	if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')))
		return true;
	return (c == '-') || (c == '.') || (c == '_') || (c == '~') || (c == '/') || (c == ':');
}

static std::string PercentEncode(const std::string& str)
{
	static const char* hex = "0123456789ABCDEF";
	std::string result;
	result.reserve(str.size());
	for (char c : str)
	{
		if (IsUriSafe(c))
		{
			result += c;
		}
		else
		{
			unsigned char uc = (unsigned char)c;
			result += '%';
			result += hex[uc >> 4];
			result += hex[uc & 0xF];
		}
	}
	return result;
}

std::string EncodeUriComponent(const std::string& value)
{
	return PercentEncode(value);
}

std::string DecodeUriComponent(const std::string& value)
{
	return PercentDecode(value);
}

bool UriToPath(const std::string& uri, std::string& outPath)
{
	const std::string prefix = "file://";
	if (uri.compare(0, prefix.size(), prefix) != 0)
		return false;

	std::string rest = PercentDecode(uri.substr(prefix.size()));

	// file://host/path is not something we can resolve, but file:///path is.
	size_t slashPos = rest.find('/');
	if ((slashPos != std::string::npos) && (slashPos > 0))
		rest = rest.substr(slashPos);

	// Windows: file:///C:/dir/file -> C:/dir/file
	if ((rest.size() >= 3) && (rest[0] == '/') && (isalpha((unsigned char)rest[1])) && (rest[2] == ':'))
		rest = rest.substr(1);

	outPath = NormalizePath(rest);
	return true;
}

std::string PathToUri(const std::string& path)
{
	std::string normalized = NormalizePath(path);
#ifdef _WIN32
	if ((normalized.size() >= 2) && (normalized[1] == ':'))
		normalized = "/" + normalized;
#endif
	return "file://" + PercentEncode(normalized);
}

static int Utf8CharLength(unsigned char lead)
{
	if (lead < 0x80)
		return 1;
	if ((lead & 0xE0) == 0xC0)
		return 2;
	if ((lead & 0xF0) == 0xE0)
		return 3;
	if ((lead & 0xF8) == 0xF0)
		return 4;
	return 1;
}

int OffsetFromPosition(const std::string& text, int line, int character)
{
	if (line < 0)
		line = 0;
	if (character < 0)
		character = 0;

	size_t pos = 0;
	for (int lineIdx = 0; lineIdx < line; lineIdx++)
	{
		size_t newLine = text.find('\n', pos);
		if (newLine == std::string::npos)
		{
			pos = text.size();
			character = 0;
			break;
		}
		pos = newLine + 1;
	}

	int units = 0;
	while ((pos < text.size()) && (units < character))
	{
		unsigned char c = (unsigned char)text[pos];
		if (c == '\n')
			break;
		int charLen = Utf8CharLength(c);
		if (pos + charLen > text.size())
			break;
		units += (charLen == 4) ? 2 : 1;
		pos += charLen;
	}

	return (int)pos;
}

void PositionFromOffset(const std::string& text, int offset, int& line, int& character)
{
	line = 0;
	character = 0;
	if (offset < 0)
		offset = 0;
	if ((size_t)offset > text.size())
		offset = (int)text.size();

	size_t pos = 0;
	while (pos < (size_t)offset)
	{
		unsigned char c = (unsigned char)text[pos];
		if (c == '\n')
		{
			line++;
			character = 0;
			pos++;
			continue;
		}
		int charLen = Utf8CharLength(c);
		if (pos + charLen > (size_t)offset)
			break;
		character += (charLen == 4) ? 2 : 1;
		pos += charLen;
	}
}

static bool IsWordChar(char c)
{
	return (isalnum((unsigned char)c) != 0) || (c == '_');
}

int WordStartBefore(const std::string& text, int offset)
{
	if (offset > (int)text.size())
		offset = (int)text.size();
	int start = offset;
	while ((start > 0) && (IsWordChar(text[start - 1])))
		start--;
	return start;
}

}
