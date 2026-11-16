// SPDX-FileCopyrightText: Copyright 2026 PPSSPP Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPSSPP side of the host layer declared in FrameGenTypes.h.

#include <cstdarg>
#include <cstdio>
#include <string>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/Log.h"
#include "ext/xxhash.h"

#include "FrameGenTypes.h"

LsfgConfig g_lsfgConfig;
const LsfgConsoleLog LsfgConsole{};

static void LsfgLog(LogLevel level, const char *fmt, va_list args) {
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, args);
	switch (level) {
	case LogLevel::LERROR: ERROR_LOG(Log::G3D, "%s", buf); break;
	case LogLevel::LWARNING: WARN_LOG(Log::G3D, "%s", buf); break;
	default: INFO_LOG(Log::G3D, "%s", buf); break;
	}
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

bool FileExists(const std::string &path) {
	return !path.empty() && File::Exists(Path(path));
}

bool ReadWholeFile(const std::string &path, std::vector<u8> *data) {
	std::string str;
	if (!File::ReadBinaryFileToString(Path(path), &str) || str.empty()) {
		return false;
	}
	data->assign(str.begin(), str.end());
	return true;
}

bool WriteWholeFile(const std::string &path, const void *data, size_t size) {
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
	File::Delete(Path(path), true);
}

u64 Hash64(const void *data, size_t size) {
	return XXH3_64bits(data, size);
}

}  // namespace LsfgHost
