#pragma once

#include <windows.h>

#include <string>

namespace anvil::app {

// Name of the global mutex that serializes Anvil Player instances.
inline constexpr wchar_t kSingleInstanceMutexName[] = L"AnvilPlayer-SingleInstance-Mutex";

// Magic value placed in COPYDATASTRUCT::dwData to validate forwarded payloads.
// A second instance forwards its command line via WM_COPYDATA; the dwData
// magic lets the receiving WM_COPYDATA handler recognize this specific payload
// (WM_COPYDATA is the only message for which Windows marshals the pointed-to
// data across the process boundary).
inline constexpr DWORD kForwardCommandLineMagic = 0x41564E4C;  // 'AVNL'

// Attempts to acquire the single-instance mutex. Returns true when this process
// is the first (and only) one; false when another instance already holds it.
// The mutex handle is kept alive for the lifetime of the process when acquired.
// Calling this more than once returns the original result without reacquiring.
struct SingleInstanceGuard {
    bool acquired = false;
};
SingleInstanceGuard AcquireSingleInstance();

// Forwards a command line to the already-running instance via WM_COPYDATA.
// Returns true when the running instance was found and accepted the payload.
// Returns false when no reachable instance window exists (the caller may then
// fall back to starting normally). Only meaningful to call when
// AcquireSingleInstance() returned acquired == false.
bool ForwardCommandLineToRunningInstance(const std::wstring& commandLine);

}  // namespace anvil::app
