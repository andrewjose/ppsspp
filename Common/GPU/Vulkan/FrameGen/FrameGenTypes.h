// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host layer for the frame-generation code ported from Eden (eden-emu PR #4263) by way of
// ARMSX2. The pass code stays close to those so their fixes stay easy to follow; what they took
// from their emulators (settings, logging, files) comes from here instead.
//
// LICENSING: the files in this folder are GPL-3.0-or-later. PPSSPP is GPL-2.0-or-later, so a
// build that includes them is distributed under GPLv3.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

using f32 = float;
using f64 = double;

namespace VideoCore::FrameGen {
// Hard ceiling on interpolated frames per rendered frame. Matches LSFG_MAX_GENERATIONS in the
// pass code (the shader set only carries weights for three).
inline constexpr size_t MAX_GENERATIONS = 3;
} // namespace VideoCore::FrameGen

// The settings the frame generator reads. Written by the app (from g_Config) and read on the
// render thread; the values are plain words, so a torn update at worst affects one frame.
struct LsfgConfig {
	bool LsfgEnabled = false;
	u32 LsfgMultiplier = 2;     // frames displayed per rendered frame, 2..4
	u8 LsfgFlowScale = 100;     // optical-flow resolution, 25..100 percent
	bool LsfgPerformance = true;
	bool LsfgFp16 = false;
	bool LsfgAutoPause = false; // present as is when the game rate times the multiplier is above the refresh rate
	u32 LsfgTargetRate = 0;     // output Hz the pacer aims for, 0 = fixed multiplier
	std::string LsfgDllPath;    // the user's Lossless.dll, copied into our own storage
	u32 LsfgDllGeneration = 0;  // bumped when a DLL is imported over the same path
	std::string CacheDir;       // where the extracted SPIR-V is cached
	std::string BuildId;        // the cache is invalidated when this changes
};

// The render thread's copy, which the pass code reads. Only LsfgSyncConfig writes it.
extern LsfgConfig g_lsfgConfig;

// Hands new settings to the render thread, if they changed. Any thread.
void LsfgPostConfig(const LsfgConfig &config);
// The settings last posted, which the render thread may not have picked up yet. Any thread.
LsfgConfig LsfgPendingConfig();
// Render thread: takes the posted settings into g_lsfgConfig.
void LsfgSyncConfig();
// From the posted settings: interpolated frames per real one, 0 when frame generation is off.
extern std::atomic<uint32_t> g_frameGenPendingExtraImages;

// Counts new game frames (see sceDisplay). Each present carries the value it had when the frame
// was finished, and one that repeats the previous value isn't interpolated.
extern std::atomic<uint32_t> g_frameGenContentCounter;

// Set by the render thread once frame generation has shown a generated frame, or failed cleanly.
// The UI's crash guard (NativeApp.cpp) uses it to tell a start that worked from one that died.
extern std::atomic<bool> g_frameGenStartSettled;

// Logs a step of starting frame generation at NOTICE, so it's in a file log (Developer tools >
// Log to file, which flushes every line) whatever the log level. The last one before a crash
// says where it died.
void LsfgStep(const char *fmt, ...);

// printf-style logging for the ported code, in the G3D log channel.
struct LsfgConsoleLog {
	void WriteLn(const char *fmt, ...) const;
	void Warning(const char *fmt, ...) const;
	void Error(const char *fmt, ...) const;
};
extern const LsfgConsoleLog LsfgConsole;

// File access, through PPSSPP's File:: functions. Content URIs (Android storage) go through Java,
// which needs a thread attached to the JVM, and the render thread isn't. So after
// LsfgHost::MarkRenderThread() these refuse content URIs on that thread instead of crashing in
// getEnv(). Keep what the render thread reads (the DLL copy, the cache) on plain paths.
namespace LsfgHost {
bool StatFile(const std::string &path, u64 *size, u64 *mtime);
// Marks the calling thread as one without JVM access (see above).
void MarkRenderThread();
bool FileExists(const std::string &path);
bool ReadWholeFile(const std::string &path, std::vector<u8> *data);
// Writes a temporary file and renames it into place.
bool WriteWholeFile(const std::string &path, const void *data, size_t size);
void RemoveFile(const std::string &path);
u64 Hash64(const void *data, size_t size);
} // namespace LsfgHost

namespace Vulkan::FrameGenSettings {

// Interpolated frames per rendered frame. g_lsfgConfig stores the multiplier (2 = one
// interpolated frame) because that's what the UI shows; the pass code counts generations.
inline size_t Generations() {
	const u32 mult = std::max<u32>(g_lsfgConfig.LsfgMultiplier, 2u);
	return std::min<size_t>(mult - 1u, VideoCore::FrameGen::MAX_GENERATIONS);
}

// Upper bound the pacer may probe up to.
inline size_t MaxGenerations() {
	return g_lsfgConfig.LsfgEnabled ? Generations() : 0;
}

// Target output rate in Hz, or 0 to hold the multiplier fixed. With a target the pacer varies the
// generation count to hold the output near it, so a game that swings between 60 and 30 fps doesn't judder.
inline f32 TargetRate() {
	return static_cast<f32>(g_lsfgConfig.LsfgTargetRate);
}

} // namespace Vulkan::FrameGenSettings
