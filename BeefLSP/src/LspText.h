#pragma once

#include <string>

namespace beflsp
{

// Paths are kept in a normalized "forward slash" form internally (on Windows
// that means C:/dir/file.bf) so that they can be compared and round-tripped
// through file URIs without surprises.
std::string NormalizePath(const std::string& path);
std::string JoinPath(const std::string& dir, const std::string& rel);
std::string GetDirectory(const std::string& path);
std::string GetFileName(const std::string& path);
std::string GetExtension(const std::string& path);
bool PathExists(const std::string& path);
bool IsDirectory(const std::string& path);

// Absolute path of the running executable (normalized), or "" if unknown.
std::string GetExecutablePath();

bool UriToPath(const std::string& uri, std::string& outPath);
std::string PathToUri(const std::string& path);

// Percent-encoding helpers for custom URI schemes (beef-emit:).
std::string EncodeUriComponent(const std::string& value);
std::string DecodeUriComponent(const std::string& value);

// LSP positions use zero-based lines and UTF-16 code units for the character
// offset. Beef sources are UTF-8, so these convert between the two.
int OffsetFromPosition(const std::string& text, int line, int character);
void PositionFromOffset(const std::string& text, int offset, int& line, int& character);

// Finds the start of the identifier-like token that ends at 'offset' in 'text'.
int WordStartBefore(const std::string& text, int offset);

}
