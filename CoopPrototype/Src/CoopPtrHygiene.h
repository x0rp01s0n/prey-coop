#pragma once

#include <cstddef>
#include <string>

namespace CoopPtrHygiene
{
// Pointer-hygiene tracing for the water "sink" / water-item / healing-item
// crash dissection.
//
// On native Windows the game silently hard-crashes on those paths while the
// same session works on Proton. Working hypothesis: a 64->32 bit pointer
// truncation that Proton's lower address space masks. When enabled, every
// suspect hop logs full-width (16-hex-digit) pointer values so a truncated
// value becomes visible in Game.log and in the crash-trace recent_mod_log
// ring.
//
// Enabling (any of):
//   * env var COOP_PTR_HYGIENE=1 before process start
//   * marker file %USERPROFILE%\Saved Games\Arkane Studios\Prey\CoopPtrHygiene.txt
//     (re-checked every 2 s from the render end-frame tick)
//   * in-game console command: coop_ptr_hygiene on|off|status
//
// Cost when disabled: one atomic load per call site; no file I/O, no
// allocations, no logging. Tick() does a stat() at most every 2 s only while
// tracing could still change state.
//
// All output is greppable: single-space separated key=value tokens, pointer
// values always 16 hex digits (ptr=0x%016llX). Output goes through
// CoopRuntimeLog::Write (the same call LogCoop() in ModMain.cpp wraps), so
// lines land in Game.log and in the crash-trace ring buffer.

// Reads the env flag + marker file once and sets the initial enabled state.
// Idempotent: only the first call computes the state (hot reloads must not
// clobber a runtime coop_ptr_hygiene on/off decision).
void Initialize();

bool Enabled();
void SetEnabled(bool enabled);

// Cheap per-frame call (render end frame). Re-checks the marker file at most
// every 2 seconds: marker appearing enables tracing; marker disappearing
// disables tracing only when it was never explicitly set and the env flag is
// not set.
void Tick();

// When enabled, logs:
//   ptr_hygiene tag=<tag> ptr=0x%016llX above32=<0|1>
void LogPtr(const char* tag, const void* ptr);

// Same line plus pre-formatted extra "key=value" tokens (no leading space),
// e.g. extraTokens = "active=1 force=0 playerInitiated=1". Pass nullptr or
// "" for LogPtr behavior.
void LogPtrWith(const char* tag, const void* ptr, const char* extraTokens);

// When enabled and ((uintptr_t)ptr >> 32) != 0, emits via
// CoopRuntimeLog::WriteRateLimited (key=<tag>, 1.0 s window, burst 3):
//   ptr_hygiene_above32 tag=<tag> ptr=0x%016llX
// Never fails, never asserts.
void CheckAbove32(const char* tag, const void* ptr);

// "enabled=<0|1> env=<0|1> marker=<0|1> explicit=<0|1>" for the status
// command.
std::string StatusReport();
}
