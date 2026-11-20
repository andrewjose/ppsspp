// SPDX-FileCopyrightText: Copyright 2026 PPSSPP Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPSSPP side of the host layer declared in FrameGenTypes.h.

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/Log.h"
#include "ext/xxhash.h"

#include "FrameGenTypes.h"

LsfgConfig g_lsfgConfig;
const LsfgConsoleLog LsfgConsole{};
std::atomic<uint32_t> g_frameGenContentCounter{0};
std::atomic<uint32_t> g_frameGenPendingExtraImages{0};
std::atomic<bool> g_frameGenStartSettled{false};

static std::mutex g_pendingMutex;
static LsfgConfig g_pendingConfig;
static std::atomic<uint32_t> g_pendingVersion{0};
static uint32_t g_syncedVersion = 0;

static bool ConfigEquals(const LsfgConfig &a, const LsfgConfig &b) {
	return a.LsfgEnabled == b.LsfgEnabled && a.LsfgMultiplier == b.LsfgMultiplier && a.LsfgFlowScale == b.LsfgFlowScale &&
		a.LsfgPerformance == b.LsfgPerformance && a.LsfgFp16 == b.LsfgFp16 && a.LsfgAutoPause == b.LsfgAutoPause && a.LsfgGameImage == b.LsfgGameImage && a.LsfgTargetRate == b.LsfgTargetRate &&
		a.LsfgDllPath == b.LsfgDllPath && a.LsfgDllGeneration == b.LsfgDllGeneration && a.CacheDir == b.CacheDir &&
		a.BuildId == b.BuildId;
}

void LsfgPostConfig(const LsfgConfig &config) {
	std::lock_guard<std::mutex> lock(g_pendingMutex);
	if (ConfigEquals(config, g_pendingConfig)) {
		return;
	}
	g_pendingConfig = config;
	const bool wanted = config.LsfgEnabled && !config.LsfgDllPath.empty();
	g_frameGenPendingExtraImages.store(wanted ? std::clamp<u32>(config.LsfgMultiplier, 2, 4) - 1 : 0, std::memory_order_relaxed);
	g_pendingVersion.fetch_add(1, std::memory_order_release);
}

LsfgConfig LsfgPendingConfig() {
	std::lock_guard<std::mutex> lock(g_pendingMutex);
	return g_pendingConfig;
}

void LsfgSyncConfig() {
	const uint32_t version = g_pendingVersion.load(std::memory_order_acquire);
	if (version == g_syncedVersion) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_pendingMutex);
	g_lsfgConfig = g_pendingConfig;
	g_syncedVersion = g_pendingVersion.load(std::memory_order_relaxed);
}

static void LsfgLog(LogLevel level, const char *fmt, va_list args) {
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, args);
	switch (level) {
	case LogLevel::LERROR: ERROR_LOG(Log::G3D, "%s", buf); break;
	case LogLevel::LWARNING: WARN_LOG(Log::G3D, "%s", buf); break;
	default: INFO_LOG(Log::G3D, "%s", buf); break;
	}
}

void LsfgStep(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	NOTICE_LOG(Log::G3D, "LSFG step: %s", buf);
}

void LsfgConsoleLog::WriteLn(const char *fmt, ...) const {
	va_list args;
	va_start(args, fmt);
	LsfgLog(LogLevel::LINFO, fmt, args);
	va_end(args);
}

void LsfgConsoleLog::Warning(const char *fmt, ...) const {
	va_list args;
	va_start(args, fmt);
	LsfgLog(LogLevel::LWARNING, fmt, args);
	va_end(args);
}

void LsfgConsoleLog::Error(const char *fmt, ...) const {
	va_list args;
	va_start(args, fmt);
	LsfgLog(LogLevel::LERROR, fmt, args);
	va_end(args);
}

namespace LsfgHost {

bool StatFile(const std::string &path, u64 *size, u64 *mtime) {
	File::FileInfo info;
	if (!File::GetFileInfo(Path(path), &info) || !info.exists || info.isDirectory) {
		return false;
	}
	*size = info.size;
	*mtime = info.mtime;
	return true;
}

static thread_local bool t_render_thread = false;

void MarkRenderThread() {
	t_render_thread = true;
}

// True if this thread mustn't touch the path (see FrameGenTypes.h).
static bool Unreachable(const std::string &path) {
	if (t_render_thread && Path(path).Type() == PathType::CONTENT_URI) {
		ERROR_LOG(Log::G3D, "LSFG: can't open %s from the render thread (content URI)", path.c_str());
		return true;
	}
	return false;
}

bool FileExists(const std::string &path) {
	return !path.empty() && !Unreachable(path) && File::Exists(Path(path));
}

bool ReadWholeFile(const std::string &path, std::vector<u8> *data) {
	if (Unreachable(path)) {
		return false;
	}
	std::string str;
	if (!File::ReadBinaryFileToString(Path(path), &str) || str.empty()) {
		return false;
	}
	data->assign(str.begin(), str.end());
	return true;
}

bool WriteWholeFile(const std::string &path, const void *data, size_t size) {
	if (Unreachable(path)) {
		return false;
	}
	const Path final_path(path);
	const Path temp_path(path + ".tmp");
	if (!File::WriteDataToFile(false, data, size, temp_path)) {
		File::Delete(temp_path, true);
		return false;
	}
	// Rename doesn't replace an existing file everywhere.
	File::Delete(final_path, true);
	if (!File::Rename(temp_path, final_path)) {
		File::Delete(temp_path, true);
		return false;
	}
	return true;
}

void RemoveFile(const std::string &path) {
	if (Unreachable(path)) {
		return;
	}
	File::Delete(Path(path), true);
}

u64 Hash64(const void *data, size_t size) {
	return XXH3_64bits(data, size);
}

}  // namespace LsfgHost
