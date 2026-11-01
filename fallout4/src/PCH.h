#pragma once

#include <RE/Fallout.h>
#include <F4SE/F4SE.h>

// Windows API only after CommonLib; WIN32_LEAN_AND_MEAN is set project-wide so no windows.h include drags in
// the old winsock.h, which clashes with winsock2.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <cstdarg>
#include <cstdio>

using namespace std::literals;

// printf-style logging used by the plug code shared with the Skyrim plugins (src/), forwards to spdlog
inline void _MESSAGE(const char* fmt, ...)
{
	char buffer[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	spdlog::info("{}", buffer);
}
