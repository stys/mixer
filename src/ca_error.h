// CoreAudio OSStatus error helpers. Setup-path only — never call from the
// realtime audio thread (these throw / format strings / allocate).
#pragma once

#include <CoreAudio/CoreAudio.h>

#include <string>

// Render a four-char-code OSStatus (e.g. "who?") when printable, else the raw number.
std::string fourCC(OSStatus s);

// Throw std::runtime_error on any non-success OSStatus.
void check(OSStatus status, const char* op);