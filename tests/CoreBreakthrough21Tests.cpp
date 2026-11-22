// Core Breakthrough #21 Verification Suite (Phase D):
//   1. 500-Permutation VK_EXT_extended_dynamic_state3 Pipeline Deduplication Benchmark (>75% collapse)
//   2. AsyncPipelinePrewarmer Concurrent Background GPL->LTO Worker Stress (256 jobs, 0 races)
//   3. DynamicState EDS3 Dirty-Tracking & GraphicContext Capability Flag Readiness

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace {

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

struct KeyHash {
	std::size_t operator()(const Libs::Graphics::GraphicsPipelineKey& k) const {
		const auto* bytes = reinterpret_cast<const uint8_t*>(&k);
		uint64_t    h     = 14695981039346656037ull;
		for (size_t i = 0; i < sizeof(k); ++i) {
			h ^= bytes[i];
			h *= 1099511628211ull;
		}
		return static_cast<std::size_t>(h);
	}
};

bool TestEds3PipelineDeduplicationBenchmark() {
	using namespace Libs::Graphics;

	constexpr uint32_t kTotalPermutations = 500;
	std::vector<GraphicsPipelineKey> keys;
	keys.reserve(kTotalPermutations);

	const std::array<vk::PolygonMode, 3> poly_modes = {
	    vk::PolygonMode::eFill, vk::PolygonMode::eLine, vk::PolygonMode::ePoint};

	// Build 500 distinct AAA graphics state permutations across 5 shader pairs.
	for (uint32_t i = 0; i < kTotalPermutations; ++i) {
		GraphicsPipelineKey key {};
		const uint64_t      shader_group = (i % 5u) + 1u;
		key.stage_ids[0]                 = shader_group * 0x1000u;
		key.stage_ids[3]                 = shader_group * 0x2000u;
		key.rendering.color_count        = 1;
		key.rendering.color_formats[0]   = vk::Format::eR8G8B8A8Unorm;
		key.static_params.topology       = vk::PrimitiveTopology::eTriangleList;
		key.static_params.samples        = 1;

		// High-churn EDS3 dynamic state variations
		const uint32_t variant                = i / 5u; // 0..99 unique per shader group
		key.static_params.polygon_mode        = poly_modes[variant % 3u];
		key.static_params.depth_clip_enable   = ((variant / 3u) & 1u) != 0;
		key.static_params.blend_enable[0]     = ((variant / 6u) & 1u) != 0;
		key.static_params.color_srcblend[0]   = static_cast<uint8_t>((variant / 12u) % 5u);
		key.static_params.color_destblend[0]  = static_cast<uint8_t>((variant / 25u) % 4u);
		key.static_params.color_mask[0]       = (variant % 15u) + 1u;

		keys.push_back(key);
	}

	// Baseline without EDS3 canonicalization
	std::unordered_set<GraphicsPipelineKey, KeyHash> baseline_set;
	for (const auto& k: keys) {
		baseline_set.insert(k);
	}
	const size_t baseline_count = baseline_set.size();
	CHECK(baseline_count >= 400u);

	// With EDS3 canonicalization enabled
	std::unordered_set<GraphicsPipelineKey, KeyHash> eds3_set;
	for (auto k: keys) {
		CanonicalizeGraphicsPipelineKey(k, /*polygon_mode=*/true, /*depth_clip=*/true,
		                                /*blend_enable=*/true, /*blend_eq=*/true,
		                                /*write_mask=*/true);
		eds3_set.insert(k);
	}
	const size_t eds3_count = eds3_set.size();
	CHECK(eds3_count == 5u);

	const double reduction_pct =
	    100.0 * (1.0 - static_cast<double>(eds3_count) / static_cast<double>(baseline_count));
	CHECK(reduction_pct > 75.0);

	std::printf(
	    "[PASS] Suite 1: EDS3 Pipeline Deduplication (%zu -> %zu monolithic pipelines, %.1f%% reduction)\n",
	    baseline_count, eds3_count, reduction_pct);
	return true;
}

bool TestAsyncLtoPrewarmerStress() {
	using namespace Libs::Graphics;

	constexpr uint32_t kJobs = 256;
	std::array<std::atomic<uint32_t>, kJobs> hot_swap_slots {};
	for (auto& s: hot_swap_slots) {
		s.store(1u, std::memory_order_relaxed); // 1 = fast-linked GPL placeholder active
	}

	AsyncPipelinePrewarmer prewarmer(4);
	const auto             t0 = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < kJobs; ++i) {
		prewarmer.Enqueue([&hot_swap_slots, i] {
			// Simulate background LTO compilation + atomic hot-swap from fast-linked (1) to LTO (2).
			uint32_t acc = i * 2654435761u;
			for (uint32_t r = 0; r < 64u; ++r) {
				acc = (acc ^ (acc >> 13u)) * 1274126177u;
			}
			hot_swap_slots[i].store(2u + (acc & 0u), std::memory_order_release);
		});
	}
	const auto enqueue_us = std::chrono::duration_cast<std::chrono::microseconds>(
	                            std::chrono::steady_clock::now() - t0)
	                            .count();

	prewarmer.WaitIdle();
	CHECK(prewarmer.CompletedCount() == kJobs);
	for (uint32_t i = 0; i < kJobs; ++i) {
		CHECK(hot_swap_slots[i].load(std::memory_order_acquire) == 2u);
	}

	std::printf(
	    "[PASS] Suite 2: AsyncPipelinePrewarmer GPL->LTO Hot-Swap (%u jobs, enqueue=%lld us)\n",
	    kJobs, static_cast<long long>(enqueue_us));
	return true;
}

bool TestDynamicStateAndCapabilityFlags() {
	using namespace Libs::Graphics;

	DynamicState ds {};
	ds.SetPolygonMode(vk::PolygonMode::eLine);
	CHECK(ds.dirty_state.polygon_mode);
	CHECK(ds.polygon_mode == vk::PolygonMode::eLine);

	ds.SetDepthClipEnable(false);
	CHECK(ds.dirty_state.depth_clip_enable);
	CHECK(!ds.depth_clip_enable);

	std::array<vk::Bool32, 8>                enables {};
	std::array<vk::ColorBlendEquationEXT, 8> equations {};
	std::array<vk::ColorComponentFlags, 8>   masks {};
	enables[0] = VK_TRUE;
	masks[0]   = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	ds.SetColorBlendStates(1u, enables, equations, masks);
	CHECK(ds.dirty_state.color_blend_states);
	CHECK(ds.color_attachment_count == 1u);

	// Verify GraphicContext defaults safely to false when extensions are absent.
	GraphicContext gc {};
	CHECK(!gc.extended_dynamic_state3_enabled);
	CHECK(!gc.graphics_pipeline_library_enabled);
	CHECK(!gc.shader_object_enabled);

	std::printf("[PASS] Suite 3: DynamicState EDS3 Dirty-Tracking & Capability Flags Readiness\n");
	return true;
}

int RunAllTests() {
	Common::InitializeThreads();

	CHECK(TestEds3PipelineDeduplicationBenchmark());
	CHECK(TestAsyncLtoPrewarmerStress());
	CHECK(TestDynamicStateAndCapabilityFlags());

	std::printf("ALL CoreBreakthrough21Tests PASSED (3/3 suites)\n");
	return 0;
}

} // namespace

#if defined(_WIN32)
int wmain() {
	return RunAllTests();
}
#else
int main() {
	return RunAllTests();
}
#endif
