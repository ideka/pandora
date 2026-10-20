// Crash reporting: on an unhandled exception or a fatal error, log where the guest was (function,
// backtrace) and write a minidump next to the log (Windows; open it with the PDB in a debugger).
#pragma once

#include <string>

class Runtime;

namespace crash {

// dir: where pandora.dmp goes. rt: the runtime whose guest state is described (may be null).
void install(const std::string& dir, Runtime* rt);
// Logs the guest context and writes the minidump (e.g. before aborting on a fatal error).
void report(const char* reason);

}  // namespace crash
