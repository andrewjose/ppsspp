// SPDX-FileCopyrightText: Copyright 2026 PPSSPP Project
// SPDX-FileCopyrightText: Copyright 2026 ARMSX2 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from ARMSX2's GSLsfg.cpp. Kept: the per-image frame slots with their fences, generating
// into our own images and copying them into the swapchain, fence-waited extra acquires, the single
// submit and the present order. Left out for now: the "game frame" mode that interpolates the
// emulator's image before it's scaled, and redrawing the overlay on generated frames. Here the
// finished screen (UI and touch controls included) is what gets interpolated.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/Log.h"
#include "Common/System/System.h"

#include "FrameGen.h"
#include "FrameGenTypes.h"
#include "LosslessDll.h"
#include "LsfgCommon.h"
#include "LsfgVkCompat.h"
#include "VulkanFrameGen.h"

using namespace PPSSPP_VK;

namespace VulkanFrameGen {

namespace {

constexpr size_t MAX_GEN = VideoCore::FrameGen::MAX_GENERATIONS;

// Readable from any thread, for the overlay.
std::atomic<float> s_display_fps{0.0f};
std::atomic<float> s_gpu_ms{0.0f};
std::atomic<bool> s_fp16_active{false};
std::atomic<bool> s_no_headroom{false};
std::atomic<bool> s_failed{false};
std::atomic<bool> s_passthrough{false};
std::atomic<float> s_source_fps{0.0f};
std::mutex s_failure_mutex;
std::string s_failure_reason;

std::optional<Vulkan::Device> s_device;
std::optional<Vulkan::MemoryAllocator> s_allocator;
std::optional<Vulkan::FrameGen> s_frame_gen;

bool s_active = false;
VkDevice s_vk_device = VK_NULL_HANDLE;
VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
uint32_t s_swapchain_generation = 0;
uint32_t s_failed_swapchain_generation = 0;
uint32_t s_image_count = 0;
// Frame slots: each holds a command buffer, a fence and one generation image per interpolated
// frame. FrameGen caches a descriptor set per generation image view and has LSFG_MAX_TARGETS of
// them, so the ring stays small enough for every view to keep its own (wrapping would rewrite
// sets a submitted frame still reads).
uint32_t s_slot_count = 0;
u32 s_first_frames_logged = 0;
bool s_generated_logged = false;
uint32_t s_multiplier = 1;
u8 s_flow_scale_percent = 0;
bool s_performance_requested = false;
bool s_fp16_requested = false;
std::string s_dll_path;
u32 s_dll_generation = 0;
VkExtent2D s_extent{};
VkFormat s_format = VK_FORMAT_UNDEFINED;

uint32_t s_last_content_id = 0;
bool s_have_content_id = false;

// How fast new game frames arrive, as in tico-ppsspp's SwitchLSFG.cpp. Under FIFO, generated
// frames take display refreshes, so a game whose rate times the multiplier is above the refresh
// rate would be held back (a 60 fps game at x2 on a 60 Hz screen runs at 30). Those frames go out
// as they are instead.
std::chrono::steady_clock::time_point s_last_source_time{};
bool s_have_source_time = false;
double s_source_interval_ns = 0.0;
u32 s_source_samples = 0;
u32 s_passthrough_flip_count = 0;

// The passes' pipelines, kept on disk next to the extracted shaders so that only the first start
// compiles them (tico-ppsspp does the same). Vulkan checks a cache's header against the device,
// but some drivers have crashed on foreign data, so it's checked here too.
VkPipelineCache s_pipeline_cache = VK_NULL_HANDLE;
size_t s_pipeline_cache_loaded_size = 0;

std::string PipelineCachePath() {
	return g_lsfgConfig.CacheDir.empty() ? std::string() : g_lsfgConfig.CacheDir + "/pipelines.bin";
}

void CreatePipelineCache(VulkanContext *vulkan) {
	std::vector<u8> data;
	const std::string path = PipelineCachePath();
	if (!path.empty() && LsfgHost::FileExists(path) && LsfgHost::ReadWholeFile(path, &data)) {
		const VkPhysicalDeviceProperties &props = vulkan->GetPhysicalDeviceProperties().properties;
		VkPipelineCacheHeaderVersionOne header{};
		bool ok = data.size() >= sizeof(header);
		if (ok) {
			memcpy(&header, data.data(), sizeof(header));
			ok = header.headerSize >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
				header.vendorID == props.vendorID && header.deviceID == props.deviceID &&
				memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
		}
		if (!ok) {
			INFO_LOG(Log::G3D, "LSFG: pipeline cache is from another GPU or driver; starting a new one.");
			data.clear();
		}
	}
	VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	ci.initialDataSize = data.size();
	ci.pInitialData = data.empty() ? nullptr : data.data();
	if (vkCreatePipelineCache(vulkan->GetDevice(), &ci, nullptr, &s_pipeline_cache) != VK_SUCCESS) {
		// Retry empty, in case the driver didn't like the data.
		ci.initialDataSize = 0;
		ci.pInitialData = nullptr;
		data.clear();
		if (vkCreatePipelineCache(vulkan->GetDevice(), &ci, nullptr, &s_pipeline_cache) != VK_SUCCESS) {
			s_pipeline_cache = VK_NULL_HANDLE;
		}
	}
	s_pipeline_cache_loaded_size = data.size();
	Vulkan::Device::SetPipelineCache(s_pipeline_cache);
	LsfgStep("pipeline cache: %s (%zu bytes)", data.empty() ? "new" : "loaded", data.size());
}

void SaveAndDestroyPipelineCache(VkDevice device) {
	Vulkan::Device::SetPipelineCache(VK_NULL_HANDLE);
	if (s_pipeline_cache == VK_NULL_HANDLE) {
		return;
	}
	size_t size = 0;
	const std::string path = PipelineCachePath();
	if (!path.empty() && vkGetPipelineCacheData(device, s_pipeline_cache, &size, nullptr) == VK_SUCCESS &&
		size > s_pipeline_cache_loaded_size) {
		std::vector<u8> data(size);
		if (vkGetPipelineCacheData(device, s_pipeline_cache, &size, data.data()) == VK_SUCCESS &&
			LsfgHost::WriteWholeFile(path, data.data(), size)) {
			INFO_LOG(Log::G3D, "LSFG: saved the pipeline cache (%zu bytes).", size);
		}
	}
	vkDestroyPipelineCache(device, s_pipeline_cache, nullptr);
	s_pipeline_cache = VK_NULL_HANDLE;
	s_pipeline_cache_loaded_size = 0;
}

void ResetRate() {
	s_have_source_time = false;
	s_source_interval_ns = 0.0;
	s_source_samples = 0;
	s_passthrough_flip_count = 0;
	s_passthrough.store(false, std::memory_order_relaxed);
	s_source_fps.store(0.0f, std::memory_order_relaxed);
}

// Called for each new game frame. True if it should be presented without generation.
bool UpdatePassthrough(u32 multiplier) {
	const auto now = std::chrono::steady_clock::now();
	if (s_have_source_time) {
		const double interval = std::chrono::duration<double, std::nano>(now - s_last_source_time).count();
		// Ignore gaps (loading, menus) and bursts.
		if (interval >= 4e6 && interval <= 100e6) {
			s_source_interval_ns = s_source_samples == 0 ? interval : s_source_interval_ns * 0.875 + interval * 0.125;
			if (s_source_samples < 120) {
				s_source_samples++;
			}
		}
	}
	s_last_source_time = now;
	s_have_source_time = true;

	// Until the rate is known, don't generate: starting and stopping is what looks bad.
	if (s_source_samples < 8 || s_source_interval_ns <= 0.0) {
		return true;
	}
	const float source_fps = (float)(1e9 / s_source_interval_ns);
	s_source_fps.store(source_fps, std::memory_order_relaxed);
	float refresh = System_GetPropertyFloat(SYSPROP_DISPLAY_REFRESH_RATE);
	if (!(refresh > 20.0f)) {
		refresh = 60.0f;
	}
	const bool too_fast = source_fps * (float)multiplier > refresh * 1.1f;
	bool passthrough = s_passthrough.load(std::memory_order_relaxed);
	// Hysteresis: quick to stop generating (8 frames), slower to start again (16).
	if (too_fast != passthrough) {
		if (++s_passthrough_flip_count >= (passthrough ? 16u : 8u)) {
			passthrough = too_fast;
			s_passthrough.store(passthrough, std::memory_order_relaxed);
			s_passthrough_flip_count = 0;
			INFO_LOG(Log::G3D, "LSFG: %s (game %.1f fps, x%u, display %.0f Hz).", passthrough ? "passing frames through" : "generating",
				source_fps, multiplier, refresh);
		}
	} else {
		s_passthrough_flip_count = 0;
	}
	return passthrough;
}

// Interpolated frames are generated into images we own, then copied into the acquired swapchain
// image. Dispatching straight into a swapchain image through a storage view crashed Turnip at
// submit in ARMSX2 (Adreno WSI images are typically UBWC-compressed).
struct GenImage {
	Vulkan::vk::Image image;
	VkImageView view = VK_NULL_HANDLE;
};
std::vector<GenImage> s_gen_images;

// One slot per swapchain image, each with its own fence. A slot isn't reused until its fence
// says the GPU is done with it: resetting a pending command buffer, or signalling a binary
// semaphore the previous present hasn't consumed, is undefined.
struct FrameSlot {
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	// Fences, not semaphores, for the extra acquires: waiting on an acquire semaphore in our own
	// submit is where Turnip segfaulted in ARMSX2. Costs a short CPU wait per generated frame.
	std::array<VkFence, MAX_GEN> acquire_fences{};
	std::array<VkSemaphore, MAX_GEN + 1> done_sems{};
	bool submitted = false;
	bool timed = false;
};
std::vector<FrameSlot> s_slots;
VkCommandPool s_cmd_pool = VK_NULL_HANDLE;

VkQueryPool s_query_pool = VK_NULL_HANDLE;
double s_ns_per_tick = 0.0;
u64 s_tick_mask = 0;
double s_gpu_ns_sum = 0.0;
u32 s_gpu_samples = 0;
u32 s_gpu_log_windows = 0;

u64 s_frame_index = 0;

std::chrono::steady_clock::time_point s_fps_window_start{};
bool s_fps_window_open = false;
u32 s_fps_real = 0;
u32 s_fps_generated = 0;

void SetFailure(const char *why) {
	{
		std::lock_guard<std::mutex> lock(s_failure_mutex);
		s_failure_reason = why;
	}
	if (!s_failed.exchange(true)) {
		ERROR_LOG(Log::G3D, "LSFG: frame generation turned off: %s", why);
		LsfgStep("failed cleanly: %s", why);
	}
	g_frameGenStartSettled.store(true, std::memory_order_relaxed);
}

void NoteFramesDisplayed(u32 real, u32 generated) {
	s_fps_real += real;
	s_fps_generated += generated;

	const auto now = std::chrono::steady_clock::now();
	if (!s_fps_window_open) {
		s_fps_window_start = now;
		s_fps_window_open = true;
		return;
	}
	const double secs = std::chrono::duration<double>(now - s_fps_window_start).count();
	if (secs < 1.0) {
		return;
	}

	s_display_fps.store(static_cast<float>((s_fps_real + s_fps_generated) / secs), std::memory_order_relaxed);
	const float gpu_ms = s_gpu_samples ? static_cast<float>(s_gpu_ns_sum / s_gpu_samples / 1e6) : 0.0f;
	s_gpu_ms.store(gpu_ms, std::memory_order_relaxed);
	s_gpu_ns_sum = 0.0;
	s_gpu_samples = 0;
	if (gpu_ms > 0.0f && ++s_gpu_log_windows >= 10) {
		s_gpu_log_windows = 0;
		INFO_LOG(Log::G3D, "LSFG GPU: %.2f ms per frame (%ux%u%s), %.1f fps displayed.", gpu_ms, s_extent.width, s_extent.height,
			s_fp16_active.load(std::memory_order_relaxed) ? ", fp16" : "", s_display_fps.load(std::memory_order_relaxed));
	}
	s_fps_window_start = now;
	s_fps_real = 0;
	s_fps_generated = 0;
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
	VkAccessFlags src_access, VkAccessFlags dst_access) {
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void DestroyResources() {
	if (s_vk_device == VK_NULL_HANDLE) {
		return;
	}
	for (GenImage &g : s_gen_images) {
		if (g.view != VK_NULL_HANDLE) {
			vkDestroyImageView(s_vk_device, g.view, nullptr);
		}
		g.image = Vulkan::vk::Image();
	}
	s_gen_images.clear();

	for (FrameSlot &slot : s_slots) {
		for (VkFence f : slot.acquire_fences) {
			if (f != VK_NULL_HANDLE) {
				vkDestroyFence(s_vk_device, f, nullptr);
			}
		}
		for (VkSemaphore sem : slot.done_sems) {
			if (sem != VK_NULL_HANDLE) {
				vkDestroySemaphore(s_vk_device, sem, nullptr);
			}
		}
		if (slot.fence != VK_NULL_HANDLE) {
			vkDestroyFence(s_vk_device, slot.fence, nullptr);
		}
	}
	s_slots.clear();

	if (s_query_pool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(s_vk_device, s_query_pool, nullptr);
		s_query_pool = VK_NULL_HANDLE;
	}
	s_gpu_ns_sum = 0.0;
	s_gpu_samples = 0;
	s_gpu_ms.store(0.0f, std::memory_order_relaxed);

	if (s_cmd_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(s_vk_device, s_cmd_pool, nullptr);
		s_cmd_pool = VK_NULL_HANDLE;
	}
}

bool CreateResources(VulkanContext *vulkan) {
	const VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
		VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, (uint32_t)vulkan->GetGraphicsQueueFamilyIndex()};
	if (vkCreateCommandPool(s_vk_device, &pool_ci, nullptr, &s_cmd_pool) != VK_SUCCESS) {
		return false;
	}

	const u32 per_frame_images = std::max<u32>(s_multiplier, 2u) - 1u;
	s_slot_count = std::clamp<u32>((u32)Vulkan::LSFG_MAX_TARGETS / per_frame_images, 1u, s_image_count);
	s_slots.resize(s_slot_count);
	VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	for (FrameSlot &slot : s_slots) {
		const VkCommandBufferAllocateInfo cmd_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
			s_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
		if (vkAllocateCommandBuffers(s_vk_device, &cmd_ai, &slot.cmd) != VK_SUCCESS) {
			return false;
		}
		// Unsignalled, with submitted false: the first use mustn't wait on a fence never submitted.
		if (vkCreateFence(s_vk_device, &fence_ci, nullptr, &slot.fence) != VK_SUCCESS) {
			return false;
		}
		for (VkFence &f : slot.acquire_fences) {
			if (vkCreateFence(s_vk_device, &fence_ci, nullptr, &f) != VK_SUCCESS) {
				return false;
			}
		}
		for (VkSemaphore &sem : slot.done_sems) {
			if (vkCreateSemaphore(s_vk_device, &sem_ci, nullptr, &sem) != VK_SUCCESS) {
				return false;
			}
		}
	}

	// One generation image per slot per interpolated frame, so the slot fence covers them too.
	s_gen_images.resize(static_cast<size_t>(s_slot_count) * per_frame_images);
	for (GenImage &g : s_gen_images) {
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = s_format;
		ici.extent = {s_extent.width, s_extent.height, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		g.image = s_allocator->CreateImage(ici);
		if (!g.image) {
			return false;
		}

		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = *g.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = s_format;
		vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vkCreateImageView(s_vk_device, &vci, nullptr, &g.view) != VK_SUCCESS) {
			return false;
		}
	}

	// Two timestamps per slot for frame generation's own GPU time, if the queue supports them.
	const VkPhysicalDeviceLimits &limits = vulkan->GetPhysicalDeviceProperties().properties.limits;
	u32 family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vulkan->GetCurrentPhysicalDevice(), &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(vulkan->GetCurrentPhysicalDevice(), &family_count, families.data());
	const u32 family = (u32)vulkan->GetGraphicsQueueFamilyIndex();
	const u32 valid_bits = family < family_count ? families[family].timestampValidBits : 0;
	if (limits.timestampComputeAndGraphics && limits.timestampPeriod > 0.0f && valid_bits > 0) {
		VkQueryPoolCreateInfo query_ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		query_ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		query_ci.queryCount = 2 * s_slot_count;
		if (vkCreateQueryPool(s_vk_device, &query_ci, nullptr, &s_query_pool) == VK_SUCCESS) {
			s_ns_per_tick = limits.timestampPeriod;
			s_tick_mask = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1);
		}
	}
	return true;
}

// How many images we may hold at once on top of the one being presented.
uint32_t ExtraAcquirable(VulkanContext *vulkan, uint32_t image_count) {
	const uint32_t min_count = vulkan->GetSurfaceCapabilities().minImageCount;
	return image_count > min_count ? image_count - min_count : 0;
}

bool SettingsMatch() {
	const u32 multiplier = std::clamp<u32>(g_lsfgConfig.LsfgMultiplier, 2, 4);
	const u8 flow = g_lsfgConfig.LsfgFlowScale;
	return multiplier == s_multiplier && flow == s_flow_scale_percent &&
		g_lsfgConfig.LsfgPerformance == s_performance_requested && g_lsfgConfig.LsfgFp16 == s_fp16_requested &&
		g_lsfgConfig.LsfgDllPath == s_dll_path && g_lsfgConfig.LsfgDllGeneration == s_dll_generation;
}

// Brings frame generation up against the current swapchain, or keeps it as it is. False if it
// can't run.
bool EnsureInitialized(VulkanContext *vulkan, uint32_t image_count) {
	const VkExtent2D extent{(uint32_t)vulkan->GetBackbufferWidth(), (uint32_t)vulkan->GetBackbufferHeight()};
	const VkFormat format = vulkan->GetSwapchainFormat();

	if (s_active && vulkan->GetSwapchain() == s_swapchain && vulkan->GetSwapchainGeneration() == s_swapchain_generation &&
		image_count == s_image_count && extent.width == s_extent.width && extent.height == s_extent.height &&
		format == s_format && SettingsMatch()) {
		return true;
	}
	// The swapchain isn't built for frame generation yet (it was created before the settings were
	// posted, or they just changed): the graphics context rebuilds it. Waiting, not failing.
	const VkPresentModeKHR mode = vulkan->GetPresentMode();
	if (vulkan->GetSwapchainFrameGenImages() == 0 ||
		(mode != VK_PRESENT_MODE_FIFO_KHR && mode != VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
		if (s_active) {
			Shutdown();
		}
		return false;
	}
	if (s_failed.load(std::memory_order_relaxed) && !s_active && g_lsfgConfig.LsfgDllPath == s_dll_path &&
		g_lsfgConfig.LsfgDllGeneration == s_dll_generation && vulkan->GetSwapchainGeneration() == s_failed_swapchain_generation) {
		// Already failed with these settings and this swapchain; don't retry every frame.
		return false;
	}

	Shutdown();
	LsfgStep("starting: swapchain %ux%u format %d, %u images (min %u), present mode %d, usage 0x%x", extent.width, extent.height,
		(int)format, image_count, vulkan->GetSurfaceCapabilities().minImageCount, (int)mode, (unsigned)vulkan->GetSwapchainImageUsage());
	s_failed.store(false, std::memory_order_relaxed);
	s_dll_path = g_lsfgConfig.LsfgDllPath;
	s_dll_generation = g_lsfgConfig.LsfgDllGeneration;

	s_failed_swapchain_generation = vulkan->GetSwapchainGeneration();

	const VkImageUsageFlags usage = vulkan->GetSwapchainImageUsage();
	if (!(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
		SetFailure("the swapchain images can't be copied to and from");
		return false;
	}
	// The generated frames are written by compute shaders as storage images in the swapchain format.
	VkFormatProperties format_props{};
	vkGetPhysicalDeviceFormatProperties(vulkan->GetCurrentPhysicalDevice(), format, &format_props);
	if (!(format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
		SetFailure("the display format can't be a storage image");
		return false;
	}
	if (ExtraAcquirable(vulkan, image_count) == 0) {
		if (!s_no_headroom.exchange(true)) {
			ERROR_LOG(Log::G3D, "LSFG: the display can't spare a swapchain image for generated frames.");
		}
		return false;
	}

	s_vk_device = vulkan->GetDevice();
	s_swapchain = vulkan->GetSwapchain();
	s_swapchain_generation = vulkan->GetSwapchainGeneration();
	s_image_count = image_count;
	s_extent = extent;
	s_format = format;
	s_multiplier = std::clamp<u32>(g_lsfgConfig.LsfgMultiplier, 2, 4);
	s_flow_scale_percent = g_lsfgConfig.LsfgFlowScale;
	s_performance_requested = g_lsfgConfig.LsfgPerformance;
	s_fp16_requested = g_lsfgConfig.LsfgFp16;

	// The device adapter and allocator must exist before CreateResources, which allocates through them.
	LsfgStep("checking device features");
	s_device.emplace(vulkan);
	if (!s_device->IsVulkanMemoryModelSupported() || !s_device->HasNullDescriptor()) {
		s_device.reset();
		s_vk_device = VK_NULL_HANDLE;
		SetFailure("the GPU driver lacks the Vulkan memory model or nullDescriptor");
		return false;
	}
	s_allocator.emplace(vulkan->Allocator());

	LsfgStep("creating images and command buffers");
	if (!CreateResources(vulkan)) {
		DestroyResources();
		s_allocator.reset();
		s_device.reset();
		s_vk_device = VK_NULL_HANDLE;
		SetFailure("couldn't create its images and command buffers");
		return false;
	}

	CreatePipelineCache(vulkan);
	LsfgStep("creating the frame generator");
	s_frame_gen.emplace(*s_allocator, vulkan);
	s_first_frames_logged = 0;
	s_frame_index = 0;
	s_have_content_id = false;
	s_active = true;
	s_no_headroom.store(false, std::memory_order_relaxed);
	INFO_LOG(Log::G3D, "LSFG: frame generation active (x%u, %ux%u, %u swapchain images, %u to spare).", s_multiplier,
		extent.width, extent.height, image_count, ExtraAcquirable(vulkan, image_count));
	return true;
}

}  // namespace

void Shutdown() {
	if (!s_active && s_cmd_pool == VK_NULL_HANDLE && !s_frame_gen) {
		return;
	}
	// Everything below is destroyed immediately, so the device has to be idle.
	if (s_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(s_vk_device);
	}
	s_frame_gen.reset();
	if (s_vk_device != VK_NULL_HANDLE) {
		SaveAndDestroyPipelineCache(s_vk_device);
	}
	DestroyResources();
	s_allocator.reset();
	s_device.reset();

	s_active = false;
	s_multiplier = 1;
	s_fp16_active.store(false, std::memory_order_relaxed);
	s_extent = {};
	s_format = VK_FORMAT_UNDEFINED;
	s_swapchain = VK_NULL_HANDLE;
	s_image_count = 0;
	s_slot_count = 0;
	s_frame_index = 0;
	s_vk_device = VK_NULL_HANDLE;
	s_have_content_id = false;
	ResetRate();

	s_display_fps.store(0.0f, std::memory_order_relaxed);
	s_fps_window_open = false;
	s_fps_real = 0;
	s_fps_generated = 0;
}

bool PresentWithGeneration(VulkanContext *vulkan, const VkImage *swapchainImages, uint32_t imageCount,
	uint32_t realIndex, VkSemaphore renderFinished, uint32_t contentId, const void *realPresentNext,
	VkResult *presentResult) {
	LsfgHost::MarkRenderThread();
	LsfgSyncConfig();
	if (!g_lsfgConfig.LsfgEnabled || g_lsfgConfig.LsfgDllPath.empty()) {
		if (s_active) {
			Shutdown();
		}
		return false;
	}
	if (!EnsureInitialized(vulkan, imageCount) || realIndex >= imageCount) {
		return false;
	}
	if (s_failed.load(std::memory_order_relaxed)) {
		// Failed while running (the DLL's shaders, say). Nothing will be generated, so don't keep
		// running the passes; the don't-retry check in EnsureInitialized keeps it down.
		Shutdown();
		return false;
	}

	// Nothing new since the last present: a repeated frame, a pause. Drop the history, so the frames
	// either side of the gap are never stitched together.
	const bool new_content = !s_have_content_id || contentId != s_last_content_id;
	s_last_content_id = contentId;
	s_have_content_id = true;
	if (!new_content) {
		NoteFramesDisplayed(1, 0);
		return false;
	}
	if (g_lsfgConfig.LsfgAutoPause) {
		if (UpdatePassthrough(s_multiplier)) {
			NoteFramesDisplayed(1, 0);
			return false;
		}
	} else if (s_passthrough.load(std::memory_order_relaxed)) {
		ResetRate();
	}

	const VkImage real_image = swapchainImages[realIndex];
	// Step logging for the first few frames after starting, where a driver crash would be.
	const bool log_steps = s_first_frames_logged < 3;
	if (log_steps) {
		s_first_frames_logged++;
		LsfgStep("frame %u: processing (real image %u of %u)", s_first_frames_logged, realIndex, imageCount);
	}

	const u32 slot_index = static_cast<u32>(s_frame_index % s_slots.size());
	FrameSlot &slot = s_slots[slot_index];
	if (slot.submitted) {
		// Bounded: a lost surface mustn't wedge the render thread.
		constexpr u64 kSlotTimeoutNs = 200ull * 1000 * 1000;
		if (vkWaitForFences(s_vk_device, 1, &slot.fence, VK_TRUE, kSlotTimeoutNs) != VK_SUCCESS) {
			NoteFramesDisplayed(1, 0);
			return false;
		}
	}
	vkResetFences(s_vk_device, 1, &slot.fence);
	slot.submitted = false;
	if (slot.timed) {
		u64 ticks[2]{};
		if (vkGetQueryPoolResults(s_vk_device, s_query_pool, slot_index * 2, 2, sizeof(ticks), ticks, sizeof(u64),
				VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
			s_gpu_ns_sum += static_cast<double>((ticks[1] - ticks[0]) & s_tick_mask) * s_ns_per_tick;
			s_gpu_samples++;
		}
		slot.timed = false;
	}

	const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	vkResetCommandBuffer(slot.cmd, 0);
	if (vkBeginCommandBuffer(slot.cmd, &begin) != VK_SUCCESS) {
		return false;
	}
	if (s_query_pool != VK_NULL_HANDLE) {
		vkCmdResetQueryPool(slot.cmd, s_query_pool, slot_index * 2, 2);
		vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_query_pool, slot_index * 2);
	}

	// The passes expect the presented image in GENERAL; it arrives in PRESENT_SRC_KHR and goes back so.
	TransitionImage(slot.cmd, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

	// Process only null-checks the view; the presented frame is copied into the chain.
	const Vulkan::vk::CommandBuffer cmdbuf{slot.cmd};
	s_frame_gen->Process(*s_device, cmdbuf, real_image, s_gen_images[0].view, s_extent, s_format, s_extent);
	s_fp16_active.store(s_frame_gen->UsingFp16(), std::memory_order_relaxed);
	if (log_steps) {
		LsfgStep("processed (generator %s)", s_frame_gen->IsUnavailable() ? "unavailable" : "ok");
	}

	if (s_frame_gen->IsUnavailable()) {
		SetFailure(s_frame_gen->UnavailableReason());
	}

	// The budget is what Vulkan lets us hold, not the image count: acquiring past it is undefined.
	const size_t acquire_budget = ExtraAcquirable(vulkan, imageCount);
	const size_t per_frame_images = std::max<u32>(s_multiplier, 2u) - 1u;
	constexpr u64 kGeneratedAcquireTimeoutNs = 50ull * 1000 * 1000;
	const size_t wanted = s_frame_gen->WantedGenerations(std::min<size_t>(s_multiplier - 1u, acquire_budget));
	const size_t generations = std::min(wanted, s_frame_gen->GeneratedFrameCount());

	u32 acquired_index[MAX_GEN]{};
	size_t acquired = 0;
	for (size_t i = 0; i < generations && i < acquire_budget && i < MAX_GEN; i++) {
		// Bounded, but not zero: under FIFO an image only comes back at a vblank, so a zero timeout
		// returns VK_NOT_READY every time and drops every generated frame (seen on an Adreno 740).
		u32 image_index = 0;
		const VkResult acq = vkAcquireNextImageKHR(s_vk_device, s_swapchain, kGeneratedAcquireTimeoutNs, VK_NULL_HANDLE,
			slot.acquire_fences[i], &image_index);
		if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
			break;
		}
		// The image is ours now and its fence will signal, so no timeout: giving up here would leak
		// the image (it counts against what we may hold) and leave the fence pending for next time.
		vkWaitForFences(s_vk_device, 1, &slot.acquire_fences[i], VK_TRUE, UINT64_MAX);
		vkResetFences(s_vk_device, 1, &slot.acquire_fences[i]);
		if (image_index >= imageCount) {
			break;
		}

		GenImage &gen = s_gen_images[slot_index * per_frame_images + i];
		if (!s_generated_logged) {
			LsfgStep("first generation into image %u", image_index);
		}
		s_frame_gen->GenerateInto(*s_device, cmdbuf, *gen.image, gen.view, i);

		const VkImage dst = swapchainImages[image_index];
		TransitionImage(slot.cmd, *gen.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
			VK_ACCESS_TRANSFER_WRITE_BIT);
		VkImageCopy region{};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.extent = {s_extent.width, s_extent.height, 1};
		vkCmdCopyImage(slot.cmd, *gen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);

		acquired_index[acquired++] = image_index;
	}

	TransitionImage(slot.cmd, real_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);

	if (s_query_pool != VK_NULL_HANDLE) {
		vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_query_pool, slot_index * 2 + 1);
	}
	if (vkEndCommandBuffer(slot.cmd) != VK_SUCCESS) {
		// The acquired images are lost to us until the swapchain is rebuilt; better than a crash.
		SetFailure("vkEndCommandBuffer failed");
		return false;
	}

	// One submit: waits on the frame's rendering, signals one semaphore per present that follows.
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &renderFinished;
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &slot.cmd;
	submit.signalSemaphoreCount = static_cast<u32>(acquired) + 1u;
	submit.pSignalSemaphores = slot.done_sems.data();
	if (log_steps) {
		LsfgStep("submitting (%zu generated images acquired)", acquired);
	}
	if (vkQueueSubmit(vulkan->GetGraphicsQueue(), 1, &submit, slot.fence) != VK_SUCCESS) {
		SetFailure("vkQueueSubmit failed");
		return false;
	}
	slot.submitted = true;
	slot.timed = s_query_pool != VK_NULL_HANDLE;
	s_frame_index++;

	// Generated frames first, they sit between the previous real frame and this one. Presents on
	// one queue are processed in call order.
	u32 presented_generated = 0;
	VkResult worst_generated = VK_SUCCESS;
	for (size_t i = 0; i < acquired; i++) {
		VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &slot.done_sems[i];
		present.swapchainCount = 1;
		present.pSwapchains = &s_swapchain;
		present.pImageIndices = &acquired_index[i];
		// Keep going after a failure: every acquired image has to be handed back, and every signalled
		// semaphore waited on. VK_SUBOPTIMAL_KHR is a success: the frame was presented.
		const VkResult pres = vkQueuePresentKHR(vulkan->GetGraphicsQueue(), &present);
		if (pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR) {
			presented_generated++;
			if (!s_generated_logged) {
				s_generated_logged = true;
				LsfgStep("first generated frame presented: frame generation works");
				g_frameGenStartSettled.store(true, std::memory_order_relaxed);
			}
		}
		if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR && worst_generated == VK_SUCCESS) {
			worst_generated = pres;
		}
	}

	// The real frame last.
	VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	present.pNext = realPresentNext;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &slot.done_sems[acquired];
	present.swapchainCount = 1;
	present.pSwapchains = &s_swapchain;
	present.pImageIndices = &realIndex;
	*presentResult = vkQueuePresentKHR(vulkan->GetGraphicsQueue(), &present);
	if (log_steps) {
		LsfgStep("presented (%u generated, real frame result %d)", presented_generated, (int)*presentResult);
	}
	// The caller rebuilds the swapchain on what the real present says, so pass on an earlier error
	// (OUT_OF_DATE, SURFACE_LOST) that it would otherwise miss. SUBOPTIMAL isn't one: Android returns
	// it for rotation, and the real present reports it too.
	if ((*presentResult == VK_SUCCESS || *presentResult == VK_SUBOPTIMAL_KHR) && worst_generated != VK_SUCCESS) {
		*presentResult = worst_generated;
	}
	NoteFramesDisplayed(1, presented_generated);
	return true;
}

std::string GetStatusText() {
	// Not g_lsfgConfig, which belongs to the render thread.
	const LsfgConfig config = LsfgPendingConfig();
	if (!config.LsfgEnabled) {
		return {};
	}
	if (config.LsfgDllPath.empty()) {
		return "LSFG: no Lossless.dll";
	}
	if (s_failed.load(std::memory_order_relaxed)) {
		std::lock_guard<std::mutex> lock(s_failure_mutex);
		return "LSFG: off (" + s_failure_reason + ")";
	}
	if (s_no_headroom.load(std::memory_order_relaxed)) {
		return "LSFG: no spare swapchain image";
	}
	const float fps = s_display_fps.load(std::memory_order_relaxed);
	if (fps <= 0.0f) {
		return "LSFG: starting";
	}
	if (s_passthrough.load(std::memory_order_relaxed)) {
		char buf[96];
		snprintf(buf, sizeof(buf), "LSFG: paused, game too fast (%.0f fps)", s_source_fps.load(std::memory_order_relaxed));
		return buf;
	}
	char buf[96];
	const float gpu_ms = s_gpu_ms.load(std::memory_order_relaxed);
	if (gpu_ms > 0.0f) {
		snprintf(buf, sizeof(buf), "LSFG: %.1f fps%s, %.2f ms", fps, s_fp16_active.load() ? " fp16" : "", gpu_ms);
	} else {
		snprintf(buf, sizeof(buf), "LSFG: %.1f fps%s", fps, s_fp16_active.load() ? " fp16" : "");
	}
	return buf;
}

float GetDisplayFPS() {
	return s_display_fps.load(std::memory_order_relaxed);
}

std::string ValidateDll(const std::string &path) {
	using VideoCore::FrameGen::LosslessStatus;
	switch (VideoCore::FrameGen::ValidateLosslessDll(path)) {
	case LosslessStatus::Ok: return {};
	case LosslessStatus::NotInstalled: return "File not found";
	case LosslessStatus::UnreadableFile: return "Couldn't read the file";
	case LosslessStatus::NotPortableExecutable: return "Not a Windows DLL";
	case LosslessStatus::MissingShaders: return "No frame generation shaders in this DLL (update Lossless Scaling?)";
	default: return "Unusable DLL";
	}
}

}  // namespace VulkanFrameGen
