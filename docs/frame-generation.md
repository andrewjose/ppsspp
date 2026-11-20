# Frame generation (Lossless Scaling)

Fork-only feature on the `lsfg-framegen` branch (and `sho-lsfg-android-apk`, which adds the Silent
Hill: Origins fixes). Not upstream. Tested on an AYN Thor (Snapdragon 8 Gen 2, Adreno 740).

## 1. What it is (plain language)

Many PSP games run at 30 frames per second. Frame generation makes them look smoother by drawing an
extra picture in between each pair of real frames, so a 30 fps game shows 60 pictures a second (or a
60 fps game shows 120 on a 120 Hz screen). The extra pictures are guesses about the motion: they make
movement look smoother, but the game doesn't run any faster or react any sooner. It actually adds a
little input lag, and fast motion can show some smearing at the edges of moving objects.

The guessing is done by Lossless Scaling, a paid Windows app. PPSSPP doesn't include any of it: you
import the `Lossless.dll` file from your own copy, and PPSSPP reads the frame generation shaders out
of it. Without that file the option stays off.

To use it: Settings > Graphics > Frame generation > Import Lossless.dll, then
turn on Frame generation and pick a multiplier. With the FPS counter on, the overlay shows how many
frames per second actually reach the screen. It needs the Vulkan backend, and it's made for Adreno 7xx
and newer GPUs (Snapdragon 8 Gen 2 class phones and handhelds).

What to expect:
- A 30 fps game at 2x looks like 60. A 60 fps game at 2x needs a 120 Hz screen; PPSSPP asks Android
  for it while frame generation is on.
- Fine repeating textures, like a cobblestone floor, can shimmer or "swim". Turning Performance mode
  off and raising Motion detail (Auto, then 60-100%) reduces it, at a GPU cost.
- The on-screen controls and the game's HUD are part of the picture, so they can smear a little.
- Heavy texture upscaling (NNEDI3) competes with frame generation for the GPU; turn it down if the
  game can't hold its frame rate.
- If PPSSPP ever closes while starting frame generation, the next launch keeps it off and says so in
  red. Restart PPSSPP to try again.

## 2. How it works (for software engineers)

**What's being interpolated.** PPSSPP draws each emulated frame into an image owned by the display
(a *swapchain image*), then hands it to the OS to show (*present*). Frame generation sits at that
hand-off: instead of presenting the real frame straight away, it runs the Lossless Scaling shaders on
the previous and current real frames to produce one to three in-between frames, presents those first,
then the real one. It's a post-process on finished images, so it needs nothing from the game (no
depth or motion vectors), which is what makes it work on an emulator at all.

**Where the shaders come from.** `Lossless.dll` is a Windows PE file whose resources include the
shaders as SPIR-V (Vulkan's shader bytecode). The code walks the PE resource tree, pulls out the
blobs it needs, renumbers their descriptor bindings into the order the passes expect, and caches the
result in the app cache, keyed on the DLL's size, timestamp and hash, so only the first launch pays
for it.

**The display queue.** Vulkan has several present modes. In MAILBOX or IMMEDIATE a newer frame
replaces a queued one, which would throw the generated frames away, so while frame generation is on
the swapchain is built in FIFO (a queue drained once per screen refresh) with an extra image for each
generated frame. Changing the setting rebuilds the swapchain.

**Not interpolating repeats.** PPSSPP sometimes presents a frame that isn't new (a paused game, a
static screen refreshed every 10 vblanks, the "render duplicate frames" option). Interpolating between
two identical frames wastes GPU time and, worse, joins the frames on either side of a pause into
fake motion. So the emulated display counts new game frames, each present carries the count it was
made with, and a present with the same count as the last one goes out without generation.

**Too fast for the screen.** In FIFO each generated frame takes a screen refresh. If the game's own
rate times the multiplier is above the refresh rate (a 60 fps game at x2 on a 60 Hz screen), the
display queue would hold the game back to half speed. With "Pause when the game is too fast for the
screen" on (off by default), the rate of new game frames is measured (a running average), and while
it's too high frames are shown as they are, with "paused, game too fast" in the overlay. Off, it
always generates. On Android the app also asks the screen for 60 x multiplier Hz while frame
generation is on (PPSSPP normally asks for 60 Hz), so a 120 Hz panel actually runs at 120. This idea comes from tico-ppsspp (the Switch build of PPSSPP in the tico
frontend), which does the same for 30 fps games on the Switch's 60 Hz screen.

**Pacing.** The pacer decides how many frames to generate for each real one. It keeps a running
average of the time between game frames so it can tell a steady rhythm from a burst or a pause. A
single long frame (a shader being compiled, a texture being upscaled) only skips generation for that
one frame; three in a row (a pause or a load) make it measure again for a second. The version ported
from Eden started over on every hitch, and could get stuck at zero after the stall when frame
generation starts, so it generated nothing at all.

**Motion detail.** The first step works out how each part of the picture moved, at a lower resolution
than the screen to save GPU time. Auto measures at 432 lines (40% of a 1080p screen); 25-100% picks a
share of the screen's resolution directly. Coarse measurement matches repeating patterns (floor
tiles) to the wrong neighbour, which is the shimmer.

**Android storage.** On Android the memory stick is usually storage opened through Java (a
`content://` address), and Java can only be called from threads registered with it. The render thread
isn't, and reading the DLL from there crashed PPSSPP the moment frame generation was switched on. The
UI thread now copies the DLL into the app's private cache (an ordinary file path) and the render
thread reads only that; its file functions refuse `content://` paths instead of crashing.

**Startup cost.** Building the passes' GPU pipelines takes a moment. They're kept in a pipeline cache
on disk (checked against the GPU and driver), so only the first start after installing pays for it.

**Threads.** PPSSPP renders on a separate thread from emulation and the UI. The settings the shaders
read belong to that render thread. The UI posts a copy under a lock, and the render thread takes it
at the start of each present. Teardown happens where the render thread is already stopped (losing
the backbuffers), because the frame generator holds references to the swapchain's images.

**Lineage and licence.** The passes come from lsfg-vk (the Linux Vulkan port of Lossless Scaling's
frame generation), by way of the Eden emulator and then ARMSX2, which drives them from its own
present path in the same way. Those files are GPL-3.0-or-later. PPSSPP is GPL-2.0-or-later, so a
build that includes them is GPLv3. Because it needs a proprietary file and can't ship on Google Play,
it's built only where the `FRAMEGEN` CMake option is on (Android by default).

## 3. Implementation details

### Files

| File | Role |
|---|---|
| `Common/GPU/Vulkan/FrameGen/Lsfg*.cpp`, `FrameGen.cpp`, `FrameGenPacer.cpp` | The passes (mipmaps, alpha/beta/gamma/delta, generate), chain and pacer, from Eden PR #4263 via ARMSX2. C++20. |
| `FrameGen/LosslessDll.cpp` | PE parsing, shader resource extraction, SPIR-V cache. |
| `FrameGen/LsfgVkCompat.*` | The small Vulkan wrapper layer the Eden code expects, over `VulkanContext` and VMA. |
| `FrameGen/FrameGenHost.cpp` | Logging (`LsfgStep`), file IO through `File::` (no content URIs on the render thread), the settings handoff, the content counter. |
| `FrameGen/VulkanFrameGen.cpp` | Present-path driver, ported from ARMSX2's `GSLsfg.cpp`. |
| `FrameGen/FrameGenTypes.h`, `FrameGenStatus.h` | Vulkan-free headers for Core and UI. |

### Hooks

- `VulkanContext::CreateDevice`: enables `vulkanMemoryModel`, `nullDescriptor` (VK_EXT_robustness2) and
  `shaderFloat16` where available. Nothing else in those feature structs.
- `VulkanContext::InitSwapchain`: when `VulkanFrameGen::Wanted()`, forces `VK_PRESENT_MODE_FIFO_KHR`,
  adds `multiplier - 1` images and `TRANSFER_DST` usage. Records the extra count
  (`GetSwapchainFrameGenImages`) and bumps `GetSwapchainGeneration`.
- `VulkanGraphicsContext::Poll`: rebuilds the swapchain when the recorded extra count differs from
  the posted settings.
- `VulkanRenderManager::Present` (emu thread): stamps `FrameData::frameGenContentId` from
  `g_frameGenContentCounter`.
- `FrameData::QueuePresent` (render thread): calls `VulkanFrameGen::PresentWithGeneration` with the
  swapchain images, the frame's `renderingCompleteSemaphore` and the stamp, passing the present-ID /
  display-timing chain through to the real present. Falls back to the normal present on false.
- `VulkanRenderManager::DestroyBackbuffers`: `VulkanFrameGen::Shutdown()` after the render thread
  stops and the queue idles.
- `__DisplayFlip` (sceDisplay): `g_frameGenContentCounter++` when `fbReallyDirty` and the frame is
  flipped (next to `DisplayFireActualFlip`).
- Android refresh rate: the render loop in `app-android.cpp` pushes `set_frame_rate` (60 x multiplier,
  or 60) when it changes, and `PpssppActivity.processCommand` passes it to `applyFrameRate`, which
  honours the existing Android framerate mode setting (0 means don't request).
- Rate check (`UpdatePassthrough`, after the repeat check, only with `FrameGenAutoPause`): EMA (1/8) of the interval between new
  game frames, samples outside 4-100 ms ignored, no generation until 8 samples. Passthrough when
  `fps * multiplier > SYSPROP_DISPLAY_REFRESH_RATE * 1.1`, entered after 8 frames that agree and left
  after 16. From tico-ppsspp's `SwitchLSFG.cpp`, which hardcodes 60 Hz and x2.
- `NativeFrame`: `PostFrameGenConfig()` builds an `LsfgConfig` from `g_Config` and posts it
  (`LsfgPostConfig` compares and only bumps the version on change). A `content://` DLL path is
  replaced by a copy in `<app cache>/lsfg/Lossless.dll`, made on this (JVM-attached) thread once per
  session and after each import (`FrameGenRenderThreadDllPath`).
- Crash guard (same function): `PSP/SYSTEM/lsfg_starting.txt` is written before the first enabled
  config is posted and deleted once `g_frameGenStartSettled` is set (first generated frame presented,
  or a clean `SetFailure`), when frame generation is turned off, or in `NativeShutdown`. Found at the
  next start: frame generation is forced off for the session and an OSD error is shown. Note that
  PPSSPP's own `FailedGraphicsBackends.txt` guard may also switch the next start to OpenGL.
- `LsfgStep()` logs each startup step at NOTICE (settings posted, DLL copied, device features,
  resources, pipeline cache, shaders loaded, passes built, first three frames, first generated
  present), so Developer tools > Log to file (`PSP/SYSTEM/DUMP/log.txt`, flushed per line) shows
  where a driver crash happened.

### Present sequence

Per new frame, on a slot (its own fence, command buffer, acquire fences, signal semaphores and one
generation image per interpolated frame). There are `min(imageCount, LSFG_MAX_TARGETS / (multiplier -
1))` slots: FrameGen caches a descriptor set per generation image view and has 7, and wrapping
rewrote sets a submitted frame still read (it did at 3x and 4x before this was capped).

0. Not a swapchain built for frame generation (`GetSwapchainFrameGenImages() == 0`, or not FIFO):
   return false and wait for `Poll` to rebuild it. (Treating this as a failure made frame generation
   never start: the first swapchain is always created before the settings are posted.) A sticky
   failure is keyed on the DLL path, its import generation and the swapchain generation.
1. Wait on the slot fence (200 ms cap), read last time's timestamps.
2. Real image PRESENT_SRC to GENERAL, `FrameGen::Process` copies it into the chain.
3. For each generation the pacer wants (capped at `imageCount - minImageCount` and the multiplier):
   acquire a swapchain image with a fence (50 ms timeout; zero never succeeds under FIFO, seen on an
   Adreno 740 in ARMSX2). Once acquired, wait on its fence without a timeout: the image is ours and
   will come back, and giving up leaked it and left the fence pending. `GenerateInto` a private
   storage image, copy that into the acquired image. Fences instead of semaphores for these
   acquires, and private images instead of storage views on swapchain images, both avoid Turnip
   crashes ARMSX2 hit (Adreno WSI images are UBWC-compressed).
4. One submit waiting on the frame's rendering semaphore, signalling one semaphore per present.
5. Present every generated frame in order, even after one fails (so every image and signalled
   semaphore is handed back), then the real one. An error from a generated present (OUT_OF_DATE,
   SURFACE_LOST, not SUBOPTIMAL) is passed on if the real present succeeded.
6. A failure while running (bad DLL shaders) shuts frame generation down on the next frame.

### Pacer

`FrameGenPacer` (Eden) with these changes, each marked `PORT:` in the source:
- A burst (a frame more than 3x faster than the average) defers; 8 in a row are taken as the new
  cadence (`smoothed_interval = interval`). Eden returns without averaging, so after the startup
  stall pushed the average to several frame times, every frame was a burst and nothing was generated.
- An interval over 100 ms (`1 / MINIMUM_BASE_RATE`) or over 2.5x the average is a hitch: skip that
  frame, don't average it. Three in a row: `Stabilize` (over 100 ms) or accept the slower cadence.
  Eden stabilises (a second without generation) on the first one.
- Measured by driving `Plan()` with timed frames: one 150 ms hitch used to cost about a second of
  generation and now costs none after the hitch; a 45 ms hitch used to cost 29 frames.

### Pipeline cache

`CreatePipelineCache` loads `<CacheDir>/pipelines.bin` if its `VkPipelineCacheHeaderVersionOne`
matches the device's vendor ID, device ID and `pipelineCacheUUID`, otherwise starts empty (and retries
empty if creation fails). `Vulkan::Device::SetPipelineCache` feeds it to `CreateComputePipeline`.
`Shutdown` saves it when it grew.

### Settings

`[Graphics]` in ppsspp.ini, per-game: `FrameGen`, `FrameGenMultiplier` (2-4), `FrameGenPerformance`
(3.1p shader family, default on), `FrameGenMotionDetail` (0 = automatic, 432 lines; otherwise 25-100% of the screen resolution), `FrameGenFp16`. Global:
`FrameGenDllPath`. `FrameGenAutoPause` (per game, default off). The import copies the picked file to `PSP/SYSTEM/Lossless.dll`, validates it
(PE with the performance shader resources), then renames it into place.

### Known gaps

- Fine repeating textures (Dracula X Chronicles' cobblestone floor) still shimmer somewhat at full
  motion detail with Performance mode off: optical flow on periodic patterns.
- The finished screen is interpolated, so the touch controls and on-screen UI get interpolated too.
  ARMSX2's mode that interpolates the emulator's own image before scaling isn't ported.
- No GPU check beyond the memory model and null descriptors. On anything other than Adreno 7xx+ it
  may fail to start (the overlay says why) or misbehave.
- Coming out of passthrough, the first generated frame pairs the last interpolated frame with the
  new one, which can show one frame of wrong motion.
- A settings change or rotation rebuilds the passes (with `vkDeviceWaitIdle`), a short stall.
- Built only with CMake. The vcxproj, UWP, legacy Android.mk and libretro builds don't include it.
- `VK_EXT_robustness2` is enabled even when the feature is compiled out.

## Changelog (test builds, `sho-lsfg-android-apk`)

| Run | Change |
|---|---|
| 18-19 | First builds: present-path hook, settings page, DLL import, optional too-fast pause (from tico-ppsspp), Android refresh-rate request. |
| 20 | Fixed never starting (first swapchain counted as a failure), leaked acquired images, unhandled present errors, descriptor reuse at 3x/4x; storage-format check; shut down after a failure. |
| 22 | Fixed the Android crash on enabling (DLL read from a `content://` path on the render thread); fixed the pacer never generating after the startup stall; startup step logging and crash guard. |
| 24 | Pacer rides out single hitches; on-disk pipeline cache; Motion detail scale with Auto and a real 100%. |
