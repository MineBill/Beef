#include "JsonRpc.h"
#include "Log.h"

#include "BeefySysLib/util/Json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace beflsp
{

static bool ReadLine(std::string& outLine)
{
	outLine.clear();
	int c;
	bool gotAny = false;
	while ((c = fgetc(stdin)) != EOF)
	{
		gotAny = true;
		if (c == '\n')
			return true;
		if (c != '\r')
			outLine += (char)c;
	}
	return gotAny && !outLine.empty();
}

static bool ReadExact(char* buffer, size_t length)
{
	size_t read = 0;
	while (read < length)
	{
		size_t got = fread(buffer + read, 1, length - read, stdin);
		if (got == 0)
			return false;
		read += got;
	}
	return true;
}

bool ReadLspMessage(std::string& outBody)
{
	outBody.clear();

	size_t contentLength = 0;
	bool hadHeader = false;
	std::string line;
	while (ReadLine(line))
	{
		if (line.empty())
		{
			if (!hadHeader)
				continue; // stray blank line between messages
			break;
		}

		hadHeader = true;
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		std::string key = line.substr(0, colon);
		std::string value = line.substr(colon + 1);
		while (!value.empty() && (value[0] == ' '))
			value.erase(value.begin());
		if ((key == "Content-Length") || (key == "content-length"))
			contentLength = (size_t)strtoull(value.c_str(), NULL, 10);
	}

	if (contentLength == 0)
		return false;

	outBody.resize(contentLength);
	if (!ReadExact(&outBody[0], contentLength))
		return false;

	return true;
}

void WriteRawLspMessage(const std::string& body)
{
	fprintf(stdout, "Content-Length: %d\r\n\r\n", (int)body.size());
	fwrite(body.data(), 1, body.size(), stdout);
	fflush(stdout);
}

bool WriteLspMessage(Beefy::Json* msg)
{
	if (msg == NULL)
		return false;
	char* body = msg->PrintUnformatted();
	bool result = false;
	if (body != NULL)
	{
		WriteRawLspMessage(std::string(body));
		Beefy::Json::free(body);
		result = true;
	}
	delete msg;
	return result;
}

}
