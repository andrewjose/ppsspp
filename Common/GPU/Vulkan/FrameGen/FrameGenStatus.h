// SPDX-FileCopyrightText: Copyright 2026 PPSSPP Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the UI needs from frame generation. No Vulkan includes, so it's safe from UI/ (see AGENTS.md).

#pragma once

#include <string>

namespace VulkanFrameGen {

// For the performance overlay. Empty when frame generation is off. Any thread.
std::string GetStatusText();
// Frames reaching the display per second, real plus generated. Any thread.
float GetDisplayFPS();

// Checks that a file is a Lossless.dll with the shaders we need. Empty string if fine, otherwise
// why not. Reads the whole file, so not for every frame. Any thread.
std::string ValidateDll(const std::string &path);

}  // namespace VulkanFrameGen
