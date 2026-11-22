# KytyPS5-Prospero Architecture Audit & Engineering Upgrade Plan

**Repository:** `C:/Users/badrk/.gemini/antigravity/scratch/kyty-prospero`  
**Upstream Base:** `KytyPS5/KytyPS5` (`GPL-2.0` intact)  
**Target Platform:** Windows 11 x64, Vulkan 1.3, 16 GB System RAM / Shared-or-8GB VRAM Hardware  

---

## 1. Codebase Architecture Map (Audited Source Files)

### 1.1 Vulkan 1.3 Pipeline & Shader Disk Caching
- `src/graphics/host_gpu/renderer/pipeline/pipelineCache.h` & `pipelineCache.cpp` — `PipelineCache`, `GraphicsPipelineKey`, `ShaderModule`, `GetGraphicsPrograms`, `WriteDriverCache` (`DriverCacheSaveInterval = 1 min`), and `spvtools::SpirvTools(SPV_ENV_VULKAN_1_3)` validation.
- `src/graphics/host_gpu/renderer/pipeline/pipelineSerialization.cpp` — Append-only zstd-compressed `ShaderStoreWriter` (records: `Module`, `GraphicsPipeline`, `TessellationLayout`, XXH3-64 checksummed frames, invalidated by `BuildStamp()` executable `last_write_time`).
- `src/graphics/host_gpu/renderer/pipeline/shaders.cpp` & `descriptors.h` — Shader module creation, descriptor set layout binding, and `imageHeap.cpp` bindless heap.

### 1.2 RDNA 2 / GCN Shader Recompilation
- `src/graphics/shader/recompiler/ShaderRecompiler.h` & `ShaderRecompiler.cpp` — Entry points `PrepareShaderSource`, `TranslateProgram`, `CompileProgram`.
- **Frontend:** `frontend/decode/` (`ShaderDecoder`, `ExportOps`), `frontend/cfg/ShaderCFG.cpp`, `frontend/translate/` (`Float.cpp`, `Integer.cpp`, `Memory.cpp`, `Scalar.cpp`, `Vector.cpp`, `Convert.cpp`, `Dispatch.cpp`).
- **IR & Optimization Passes:** `ir/` (`Block`, `Program`, `IREmitter`, `Value`, `Type`, `Serialize.h`) and `ir/passes/` (`SsaRewrite`, `ConstantPropagation`, `DeadCodeElimination`, `ReadLaneElimination`, `ResourceTracking`, `ResourceMaterialization`, `SrtWalker`, `RayQueryTraversal`, `ShaderInfoCollection`).
- **SPIR-V 1.3 Backend:** `backend/spirv/` (`SpirvBuilder`, `SpirvEmitter`, `spirvEmitterAlu`, `spirvEmitterFlow`, `spirvEmitterImage`, `spirvEmitterMemory`, `spirvEmitterMesh`, `spirvEmitterRayQuery`, `spirvEmitterTessellation`).

### 1.3 GUI & Diagnostic HUD Overlay
- `src/graphics/presentation/performanceOverlay.h` & `performanceOverlay.cpp` — Dear ImGui overlay (`DrawPerformanceOverlay`, 120-frame interval history, >50 ms / 2x average hitch recorder, `TogglePerformanceOverlay` / `TogglePerformanceOverlayDetails`).
- `src/graphics/presentation/systemOverlay.h` & `systemOverlay.cpp` — System notifications / dialog overlays.
- `src/graphics/presentation/window/` (`window.cpp`, `vulkanWindow.cpp`, `swapchain.cpp`, `hostInput.cpp`) — SDL3 window, Vulkan 1.3 swapchain presentation, and keyboard/controller hotkey routing.

### 1.4 Memory & VRAM Management
- `src/kernel/memory.h` & `src/kernel/memory.cpp` — Guest virtual address space (`GUEST_MEMORY_BUDGET = 12800 MiB`, `DEFAULT_FLEXIBLE_MEMORY_SIZE = 448 MiB`, `MEM_RESERVE_PLACEHOLDER` / `MEM_REPLACE_PLACEHOLDER` Windows virtual memory mapping).
- `src/graphics/host_gpu/vma.cpp` — Vulkan Memory Allocator (`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`, `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`, `GetDeviceMemoryUsage()`, `GetHeapUsage()`).
- `src/graphics/host_gpu/renderer/cache/` — `bufferCache.h` / `bufferCache.cpp` (`RunGarbageCollector()`, BDA page table, staging/stream/download buffers), `faultManager.cpp`, `samplerCache.cpp`, and `src/graphics/host_gpu/memoryTracker.cpp` / `pageManager.cpp`.

---

## 2. Top 3 Highest-Impact Engineering Upgrades

### Upgrade #1 — Zero-Stutter Async Pipeline Compilation & Resilient Disk Cache **[COMPLETED & VERIFIED]**
- **Problem Uncovered in Audit:**
  1. `pipelineSerialization.cpp:46-62` (`BuildStamp()`) hashes the executable's filesystem `last_write_time`, wiping the entire zstd shader store after every minor rebuild even when the IR/SPIR-V serialization format is unchanged.
  2. `PipelineCache::GetGraphicsPrograms()` + `vkCreateGraphicsPipelines()` compiles new graphics pipelines **synchronously on the GPU thread**, causing >50 ms frame hitches on first draw of new materials.
  3. `VK_EXT_graphics_pipeline_library` (GPL) is not used to pre-link vertex-input / fragment-output state independently from shader stages.
- **Engineering Solution:**
  1. Replace `BuildStamp()` with an explicit **semantic cache schema version** (`kShaderStoreSchemaVersion` + `SpirvProfile` hash) so local rebuilds preserve warm shader caches.
  2. Add an **Async Background Pipeline Worker Pool** in `PipelineCache` that compiles full optimized pipelines off-thread while allowing optional draw-skip or fast-link placeholder pipelines during shader warm-up, plus flush `VkPipelineCache` on idle frames when dirty count exceeds a threshold (not only every 60 seconds).
  3. Instrument `Common::DebugCounters` with cache hit/miss rates and per-pipeline compilation time (`us`).

### Upgrade #2 — 16 GB System RAM / Shared-VRAM Adaptive Memory Pressure Engine **[COMPLETED & VERIFIED]**
- **Problem Uncovered in Audit:**
  1. `src/kernel/memory.cpp:67` hardcodes `GUEST_MEMORY_BUDGET = 12800 MiB` (`12.5 GiB`), which leaves <3.5 GiB for Windows 11 + Vulkan driver + VMA device-local copies on 16 GB systems (causing severe paging or `VK_ERROR_OUT_OF_DEVICE_MEMORY`).
  2. `BufferCache::RunGarbageCollector()` and `TextureCache` eviction are not proactively driven by `VK_EXT_memory_budget` (`GraphicContext::GetHeapUsage()`) watermarks.
- **Engineering Solution:**
  1. Add a **VMA Budget Watermark Controller** in `GraphicContext` / `RenderContext` that samples `vmaGetHeapBudgets` every N frames and defines `Normal (<75%)`, `Elevated (75–88%)`, and `Critical (>88%)` pressure tiers.
  2. Under `Elevated` pressure, proactively trigger `BufferCache::RunGarbageCollector()` and evict cold LRU images from `TextureCache` / `ImageHeap`. Under `Critical` pressure, force-drain deferred deletions and trim staging/stream ring buffers.
  3. Expose a configurable **Low-Memory Profile** (e.g., capping high-res mipmap uploads / staging buffer high-water marks) for 16 GB RAM / integrated GPU systems without breaking guest memory reporting invariants.

### Upgrade #3 — Live F1 / F2 / F3 Developer Diagnostic HUD **[COMPLETED & VERIFIED]**
- **Problem Uncovered in Audit:**
  - `performanceOverlay.cpp` provides a strong FPS + hitch foundation, but developers cannot inspect live shader compilation stalls, pipeline cache health, or VMA/BufferCache memory pressure in real time without attaching external profilers.
- **Engineering Solution:**
  - Extend `performanceOverlay.cpp` and `window/hostInput.cpp` with three dedicated hotkeys (preserving existing toggle behavior):
    - **`F1` — Performance & Hitch Profiler:** FPS, 120-frame frametime graph, recent >50 ms hitches broken down by subsystem delta (`DebugCounters`), and CPU/GPU frame pacing.
    - **`F2` — Live Shader & Pipeline Cache Inspector:** Total compiled modules, graphics/compute pipelines, disk shader store hit rate, async compile queue depth, and slowest recent shader compiles (hash + stage + ms).
    - **`F3` — Memory & VRAM Pressure Monitor:** Per-heap VMA usage vs. `VK_EXT_memory_budget` bars, `BufferCache` / `TextureCache` residency, active GC tier (`Normal`/`Elevated`/`Critical`), and interactive buttons to force GC or flush `VkPipelineCache` to disk.

### Upgrade #4 — Native PS5-Style Boot Sequence & Home Dashboard **[COMPLETED & VERIFIED]**
- **Objective:** Replace the CLI-only exit on zero-argument launch with a zero-Qt native SDL3 + Dear ImGui PS5 Home Dashboard.
- **Engineering Solution:**
  - `src/graphics/presentation/homeDashboard.h` & `homeDashboard.cpp` + `imgui_impl_sdl3` / `imgui_impl_sdlrenderer3` in `3rdparty/CMakeLists.txt`.
  - **2-Second Skippable Boot Intro:** Midnight-blue ambient radial glow + 72 drifting golden/cyan light particles + smooth fade-in of `KYTY-PROSPERO`.
  - **Automatic PS5 Library Scanner (`ScanPs5GameLibrary`):** Scans `./games/` (and configurable custom folders) for `eboot.bin` / `.elf`, parses `sce_sys/param.json` (`titleName`, `localizedParameters`, `titleId`, `contentVersion`), loads `sce_sys/icon0.png` & `pic0.png` via `stb_image`, and renders procedural cover tiles / empty-library onboarding card when missing.
  - **PS5 Horizontal Carousel & Settings:** Smooth spring-scaled rounded tiles, top status bar (clock, gamepad status, Settings modal with `--guest-memory-mb`, `--shader-cache`, `--perf-overlay`), full keyboard + SDL3 Gamepad (D-Pad / Left Stick / Cross / Circle / Start) navigation, and direct launch into `Run(options)`.

### 5. Beyond-PS5 Enhancement Suite (Vulkan FSR/CAS Upscaler, Per-Game Patch/FPS Engine & Live Trophy System) — **[COMPLETED & VERIFIED]**
- **Status:** Implemented, compiled cleanly via Ninja (`_Build/win-mingw`), and verified with `beyond_ps5_suite_tests.exe` (**3/3 suites PASS**, including live Vulkan 1.3 compute pipeline creation on host GPU), `kyty_emulator.exe --help` (`exit=0`), and `kyty_emulator.exe --dashboard-demo 25` (`exit=0`).
- **What Changed:**
  - **Vulkan 1.3 Compute Upscaler & Sharpener (`gpu_present_fsr_cas.comp`, `presentUpscaler.h/cpp`, `swapchain.cpp`):** Implemented a real Vulkan 1.3 compute post-processing pass supporting **AMD FidelityFX CAS** (5-tap luma-adaptive sharpening) and **FSR 1.0 EASU + RCAS** (12-tap edge-adaptive directional Lanczos-2 upscaling + anti-ringing + sharpness), configurable resolution scaling (`1.0x`, `1.5x`, `2.0x / 4K`), and zero overhead when disabled. Exposed via `--upscaler`, `--sharpness`, `--res-scale`, global Settings, and per-game profiles.
  - **Per-Game Patch & FPS Unlocker Engine (`gameProfile.h/cpp`, `gamePatch.cpp`, `emulator.cpp`, `homeDashboard.cpp`):** Built a JSON patch loader (`patches/<TitleID>.json`) with byte-verified memory patching, atomic rollback, and FPS/vblank override (`--fps-cap`), plus per-game profiles (`profiles/<TitleID>.json`) accessible directly from each game card (**Game Settings [G]** / Gamepad Square) in the Home Dashboard.
  - **Native PS5-Style Trophy Notification & Progress Tracker (`trophyTracker.h/cpp`, `libTrophy.cpp`, `systemOverlay.h/cpp`, `homeDashboard.cpp`):** Bridged `libSceNpTrophy2` unlocks to `saves/trophies/<TitleID>.json`, enhanced the animated slide-in ImGui trophy toast with trophy descriptions + Platinum/Gold/Silver/Bronze badges, added a deterministic `TrophyPopupQueue` state machine, and surfaced Trophy completion badges directly on Home Dashboard game cards and hero details.
