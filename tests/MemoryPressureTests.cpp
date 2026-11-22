#include "graphics/host_gpu/graphicContext.h"
#include "kernel/memory.h"

#include <array>
#include <cmath>
#include <iostream>

using namespace Libs::Graphics;
using namespace Libs::LibKernel::Memory;

namespace {

bool TestMemoryPressureWatermarks() {
	constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;

	// Integrated / shared-memory GPU (discrete=false): considers all heaps
	{
		std::array<GraphicContext::HeapUsage, 1> heaps = {{
		    {.usage = 6ull * kGiB, .budget = 10ull * kGiB, .allocations = 40, .device_local = true},
		}};
		const auto snap = ClassifyMemoryPressure(heaps, false);
		if (snap.tier != MemoryPressureTier::Normal || std::fabs(snap.peak_ratio - 0.60) > 1e-6) {
			std::cerr << "[FAIL] Expected Normal at 60%, got tier=" << static_cast<int>(snap.tier)
			          << "\n";
			return false;
		}
	}

	// Boundary 75.0% -> Elevated
	{
		std::array<GraphicContext::HeapUsage, 1> heaps = {{
		    {.usage = 7500, .budget = 10000, .allocations = 120, .device_local = true},
		}};
		const auto snap = ClassifyMemoryPressure(heaps, false);
		if (snap.tier != MemoryPressureTier::Elevated) {
			std::cerr << "[FAIL] Expected Elevated at 75.0%\n";
			return false;
		}
	}

	// Boundary 88.0% -> Elevated, 88.1% -> Critical
	{
		std::array<GraphicContext::HeapUsage, 1> heaps_e = {{
		    {.usage = 8800, .budget = 10000, .allocations = 200, .device_local = true},
		}};
		std::array<GraphicContext::HeapUsage, 1> heaps_c = {{
		    {.usage = 8810, .budget = 10000, .allocations = 200, .device_local = true},
		}};
		if (ClassifyMemoryPressure(heaps_e, false).tier != MemoryPressureTier::Elevated ||
		    ClassifyMemoryPressure(heaps_c, false).tier != MemoryPressureTier::Critical) {
			std::cerr << "[FAIL] Expected Elevated at 88.0% and Critical at 88.1%\n";
			return false;
		}
	}

	// Discrete GPU: ignores host-only system heap pressure when device-local VRAM is healthy
	{
		std::array<GraphicContext::HeapUsage, 2> heaps = {{
		    {.usage = 4ull * kGiB, .budget = 8ull * kGiB, .allocations = 10, .device_local = true},
		    {.usage = 15ull * kGiB, .budget = 16ull * kGiB, .allocations = 50, .device_local = false},
		}};
		const auto snap = ClassifyMemoryPressure(heaps, true);
		if (snap.tier != MemoryPressureTier::Normal) {
			std::cerr << "[FAIL] Discrete GPU should evaluate device-local heaps only\n";
			return false;
		}
	}

	std::cout << "[PASS 1/2] VMA Watermark Classifier: Normal (<75%), Elevated (75-88%), "
	             "Critical (>88%) + discrete/iGPU heap filtering verified\n";
	return true;
}

bool TestAdaptiveGuestMemoryBudget() {
	constexpr uint64_t kMiB = 1024ull * 1024ull;
	constexpr uint64_t kGiB = 1024ull * kMiB;

	// 16 GB host -> safe 8192 MiB profile
	const auto b16 = ComputeAdaptiveGuestMemoryBudget(16ull * kGiB, false, 0);
	if (b16 != 8192ull * kMiB) {
		std::cerr << "[FAIL] 16 GiB host expected 8192 MiB, got " << (b16 / kMiB) << " MiB\n";
		return false;
	}

	// 24 GB host -> 10240 MiB profile
	const auto b24 = ComputeAdaptiveGuestMemoryBudget(24ull * kGiB, false, 0);
	if (b24 != 10240ull * kMiB) {
		std::cerr << "[FAIL] 24 GiB host expected 10240 MiB, got " << (b24 / kMiB) << " MiB\n";
		return false;
	}

	// 32 GB host -> full 12800 MiB (base) / 14016 MiB (trinity)
	const auto b32_base    = ComputeAdaptiveGuestMemoryBudget(32ull * kGiB, false, 0);
	const auto b32_trinity = ComputeAdaptiveGuestMemoryBudget(32ull * kGiB, true, 0);
	if (b32_base != 12800ull * kMiB || b32_trinity != 14016ull * kMiB) {
		std::cerr << "[FAIL] 32 GiB host unexpected budget: base=" << (b32_base / kMiB)
		          << " trinity=" << (b32_trinity / kMiB) << "\n";
		return false;
	}

	// Explicit CLI overrides: clamped to [4096, 14016] and 2 MiB aligned
	const auto b_low  = ComputeAdaptiveGuestMemoryBudget(16ull * kGiB, false, 1024);
	const auto b_odd  = ComputeAdaptiveGuestMemoryBudget(16ull * kGiB, false, 9001);
	const auto b_high = ComputeAdaptiveGuestMemoryBudget(16ull * kGiB, false, 99999);
	if (b_low != 4096ull * kMiB || b_odd != 9000ull * kMiB || b_high != 14016ull * kMiB) {
		std::cerr << "[FAIL] CLI override clamping/alignment failed: low=" << (b_low / kMiB)
		          << " odd=" << (b_odd / kMiB) << " high=" << (b_high / kMiB) << "\n";
		return false;
	}

	// Live host RAM query sanity check
	const auto host_ram = QueryHostPhysicalMemoryBytes();
	if (host_ram < 2ull * kGiB) {
		std::cerr << "[FAIL] QueryHostPhysicalMemoryBytes returned implausible value: " << host_ram
		          << "\n";
		return false;
	}

	std::cout << "[PASS 2/2] Adaptive Guest Budget: live host RAM=" << (host_ram / kMiB)
	          << " MiB -> auto budget="
	          << (ComputeAdaptiveGuestMemoryBudget(host_ram, false, 0) / kMiB)
	          << " MiB (16G->8192, 24G->10240, 32G->12800/14016, CLI clamp/2MiB align verified)\n";
	return true;
}

} // namespace

int main() {
	if (!TestMemoryPressureWatermarks()) {
		return 1;
	}
	if (!TestAdaptiveGuestMemoryBudget()) {
		return 1;
	}
	std::cout << "ALL UPGRADE #2 VERIFICATION TESTS PASSED!\n";
	return 0;
}
