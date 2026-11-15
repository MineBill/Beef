#pragma once

#include <string>

namespace Beefy
{
class Json;
}

namespace beflsp
{

// Reads one LSP message (headers + JSON body) from stdin.
// Returns false on EOF or a malformed header block.
bool ReadLspMessage(std::string& outBody);

// Writes one LSP message to stdout and flushes. Takes ownership of 'msg'.
bool WriteLspMessage(Beefy::Json* msg);

// Convenience: writes a message built by the caller using Json helpers.
void WriteRawLspMessage(const std::string& body);

}
