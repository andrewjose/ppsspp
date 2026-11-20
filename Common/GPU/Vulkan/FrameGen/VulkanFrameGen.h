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
#include <functional>

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

// Frame generation on the game's own image (the "Generate from the game image" setting): the image
// PPSSPP's present scaled to the screen, captured before scaling (CaptureGameFrame). Generated
// frames are made at that size, scaled into the same rectangle, and get the UI redrawn over them.
struct GameFramePresent {
	uint64_t frameId = 0;
	// This frame's game image was captured.
	bool captured = false;
	// Redraws the frame's UI into swapchain image imageIndex, which is in COLOR_ATTACHMENT_OPTIMAL,
	// leaving it in the present layout. Empty when nothing was drawn over the game.
	std::function<void(VkCommandBuffer cmd, uint32_t imageIndex)> recordOverlay;
	// Signalled once the generated frames that redrew the UI are done; *resourcesFenceSubmitted is
	// set when it was submitted.
	VkFence resourcesFence = VK_NULL_HANDLE;
	bool *resourcesFenceSubmitted = nullptr;
};

// Render thread: generation is running and the setting is on, so a game image is wanted.
bool GameImageWanted();

// Records a copy of the game image (in TRANSFER_SRC_OPTIMAL) into frame generation's input, shrunk
// to fit the screen but never grown. srcRect is its visible part (left, top, right, bottom) and
// dstRect where the present drew it, in backbuffer pixels. Record into the frame's own command
// buffer, after the backbuffer pass. False if it can't be used.
bool CaptureGameFrame(VkCommandBuffer cmd, VkImage src, const int srcRect[4], const float dstRect[4], bool linear,
	uint64_t frameId);

// Presents the interpolated frames and then the real one, in place of the caller's own
// vkQueuePresentKHR. renderFinished is consumed. realPresentNext is chained to the real frame's
// present (present IDs and the like). game may be null. Returns false when generation can't run
// this frame, in which case nothing was submitted or presented and the caller presents normally.
bool PresentWithGeneration(VulkanContext *vulkan, const VkImage *swapchainImages, uint32_t imageCount,
	uint32_t realIndex, VkSemaphore renderFinished, uint32_t contentId, const void *realPresentNext,
	const GameFramePresent *game, VkResult *presentResult);

// Releases everything. Call with the device idle, before the swapchain or device goes away.
void Shutdown();

}  // namespace VulkanFrameGen
