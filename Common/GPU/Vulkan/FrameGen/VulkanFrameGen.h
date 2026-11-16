// SPDX-FileCopyrightText: Copyright 2026 PPSSPP Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lossless Scaling frame generation, driven from PPSSPP's own Vulkan present path. Ported from
// ARMSX2's GSLsfg (pcsx2/GS/Renderers/Vulkan/GSLsfg.cpp), which drives the Eden passes in
// FrameGen/. Nothing proprietary is bundled: the shaders are read at runtime out of the user's own
// Lossless.dll.
//
// Only built when PPSSPP_HAS_FRAMEGEN is defined (the CMake build). This header is plain C++17 so
// the rest of the Vulkan backend can include it; the implementation needs C++20.
//
// Threading: everything except the settings checks and GetStatusText/GetDisplayFPS runs on the
// render thread, the only one that submits.

#pragma once

#include <cstdint>

#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "FrameGenStatus.h"
#include "FrameGenTypes.h"

class VulkanContext;

namespace VulkanFrameGen {

// Most swapchain images PresentWithGeneration takes.
constexpr uint32_t MAX_SWAPCHAIN_IMAGES = 16;

// Swapchain images to ask for on top of the usual: one per interpolated frame. 0 when frame
// generation is off. From the posted settings (FrameGenTypes.h), so any thread.
inline uint32_t ExtraSwapchainImages() {
	return g_frameGenPendingExtraImages.load(std::memory_order_relaxed);
}
// Turned on in the settings, with a DLL. Decides the swapchain's present mode and image count.
inline bool Wanted() {
	return ExtraSwapchainImages() != 0;
}

// Presents the interpolated frames and then the real one, in place of the caller's own
// vkQueuePresentKHR. renderFinished is consumed. realPresentNext is chained to the real frame's
// present (present IDs and the like). Returns false when generation can't run this frame, in which
// case nothing was submitted or presented and the caller presents normally.
bool PresentWithGeneration(VulkanContext *vulkan, const VkImage *swapchainImages, uint32_t imageCount,
	uint32_t realIndex, VkSemaphore renderFinished, uint32_t contentId, const void *realPresentNext,
	VkResult *presentResult);

// Releases everything. Call with the device idle, before the swapchain or device goes away.
void Shutdown();

}  // namespace VulkanFrameGen
