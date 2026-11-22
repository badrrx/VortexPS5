// Core Breakthrough #20 Verification Suite (Phase C):
//   1. BufferCache Page-Walked Sparse Guest Memory Reads (Mapped + Unmapped PRT Holes)
//   2. Descriptors Storage Buffer & Texture View LOD/Layer Boundary Clamping
//   3. RDNA 2 Tile Layout Degenerate & Exotic 3D Fallback Hardening

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "kernel/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

bool TestSparseGuestBufferReads() {
	using namespace Libs::LibKernel::Memory;

	constexpr uint64_t kPageSize = 0x4000u; // 16 KiB PS5 page
	constexpr uint64_t kTotal    = kPageSize * 3u;

	// Map 3 contiguous PS5 pages, fill page 0 (0xA5) and page 2 (0x5A), then unmap page 1 to
	// simulate a PS5 PRT / Virtual Texture sparse hole.
	void* base_ptr = nullptr;
	const int map_rc = KernelMapFlexibleMemory(&base_ptr, kTotal, 3 /* RW */, 0);
	CHECK(map_rc == 0 && base_ptr != nullptr);

	auto* bytes = static_cast<uint8_t*>(base_ptr);
	std::memset(bytes, 0xA5, kPageSize);
	std::memset(bytes + kPageSize, 0xCC, kPageSize);
	std::memset(bytes + kPageSize * 2u, 0x5A, kPageSize);

	const auto base_addr = reinterpret_cast<uint64_t>(base_ptr);
	const int  unmap_rc  = KernelMunmap(base_addr + kPageSize, kPageSize);
	CHECK(unmap_rc == 0);

	// 1. Standard sparse read across mapped + unmapped PRT hole
	std::vector<uint8_t> out(kTotal, 0xEE);
	(void)Libs::Graphics::ReadSparseGuestMemorySafe(base_addr, out.data(), kTotal, false);

	// Verify mapped page 0 == 0xA5, unmapped page 1 == 0x00, mapped page 2 == 0x5A.
	for (uint64_t i = 0; i < kPageSize; ++i) {
		CHECK(out[i] == 0xA5u);
		CHECK(out[kPageSize + i] == 0x00u);
		CHECK(out[kPageSize * 2u + i] == 0x5Au);
	}

	// 2. Backing-store sparse read (TryReadSparseBacking path) across the same PRT hole
	std::vector<uint8_t> out_backing(kTotal, 0xEE);
	(void)Libs::Graphics::ReadSparseGuestMemorySafe(base_addr, out_backing.data(), kTotal, true);
	for (uint64_t i = 0; i < kPageSize; ++i) {
		CHECK(out_backing[i] == 0xA5u);
		CHECK(out_backing[kPageSize + i] == 0x00u);
		CHECK(out_backing[kPageSize * 2u + i] == 0x5Au);
	}

	// 3. Out-of-range unmapped address and null address must cleanly zero-fill without aborting.
	std::vector<uint8_t> unmapped_out(256, 0xFF);
	CHECK(Libs::Graphics::ReadSparseGuestMemorySafe(0x4000u, unmapped_out.data(),
	                                                unmapped_out.size(), false) ==
	      unmapped_out.size());
	for (uint8_t b: unmapped_out) {
		CHECK(b == 0x00u);
	}

	// Null address read must cleanly zero-fill without aborting.
	std::vector<uint8_t> null_out(256, 0xFF);
	CHECK(Libs::Graphics::ReadSparseGuestMemorySafe(0, null_out.data(), null_out.size(), true) ==
	      null_out.size());
	for (uint8_t b: null_out) {
		CHECK(b == 0x00u);
	}

	(void)KernelMunmap(base_addr, kPageSize);
	(void)KernelMunmap(base_addr + kPageSize * 2u, kPageSize);

	std::printf("[PASS] Suite 1: BufferCache Sparse Page-Walked Guest Reads (PRT Holes)\n");
	return true;
}

bool TestDescriptorBoundaryClamping() {
	using namespace Libs::Graphics;

	// 1. Oversized storage buffer range + large unaligned offset adjustment
	uint64_t aligned_offset = 0;
	uint64_t effective_size = 0;
	uint32_t adjustment     = 0;
	SanitizeStorageBufferRange(/*raw_size=*/0x2000'0000ULL, /*raw_offset=*/512u + 300u,
	                           /*alignment=*/512u, /*max_range=*/0x0800'0000ULL, aligned_offset,
	                           effective_size, adjustment);
	CHECK(aligned_offset == 512u);
	CHECK(adjustment == 255u);
	CHECK(effective_size <= 0x0800'0000ULL);

	// 2. Texture view with min_lod > last_level*256 and out-of-bounds array base_layer
	uint32_t out_min_lod     = 0;
	uint32_t out_base_layer  = 0;
	uint32_t out_layer_count = 0;
	SanitizeTextureViewBounds(/*raw_min_lod=*/2048u, /*base_level=*/1u, /*last_level=*/4u,
	                          /*raw_base_layer=*/12u, /*image_layers=*/6u, /*is_array=*/true,
	                          out_min_lod, out_base_layer, out_layer_count);
	CHECK(out_min_lod == (4u - 1u) * 256u);
	CHECK(out_base_layer == 5u);
	CHECK(out_layer_count == 1u);

	// 3. Non-array 2D texture view with out-of-bounds base_layer
	SanitizeTextureViewBounds(/*raw_min_lod=*/128u, /*base_level=*/2u, /*last_level=*/5u,
	                          /*raw_base_layer=*/9u, /*image_layers=*/1u, /*is_array=*/false,
	                          out_min_lod, out_base_layer, out_layer_count);
	CHECK(out_min_lod == 0u);
	CHECK(out_base_layer == 0u);
	CHECK(out_layer_count == 1u);

	std::printf("[PASS] Suite 2: Descriptors Storage Buffer & Texture View Boundary Clamping\n");
	return true;
}

bool TestTileDegenerateAndExoticLayouts() {
	using namespace Libs::Graphics;

	// 1. Degenerate depth == 0 (previously EXIT_NOT_IMPLEMENTED)
	TileSizeAlign total {};
	TileGetTextureTotalSize(Prospero::BufferFormat::k8_8_8_8UNorm, 64, 64, /*depth=*/0, 1,
	                        Prospero::TileMode::kLinear, false, total);
	CHECK(total.size > 0);

	// 2. Unknown/invalid format fallback (previously EXIT("unknown format"))
	TileSizeAlign fallback {};
	TileGetTextureSize(Prospero::BufferFormat::kInvalid, 32, 32, 1, Prospero::TileMode::kLinear,
	                   &fallback, nullptr, nullptr);
	CHECK(fallback.size == 32u * 32u * 4u);
	CHECK(fallback.align == 256u);

	// 3. Unsupported 3D volume texture layout fallback (previously EXIT("unsupported 3D..."))
	TileSizeAlign vol {};
	TileGetTextureTotalSize(Prospero::BufferFormat::kInvalid, 16, 16, 4, 1,
	                        Prospero::TileMode::kStandard64KB, /*volume_texture=*/true, vol);
	CHECK(vol.size > 0);

	std::printf("[PASS] Suite 3: RDNA 2 Tile Degenerate & Exotic 3D Fallback Hardening\n");
	return true;
}

int RunAllTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});
	Libs::LibKernel::Memory::Initialize();

	CHECK(TestSparseGuestBufferReads());
	CHECK(TestDescriptorBoundaryClamping());
	CHECK(TestTileDegenerateAndExoticLayouts());

	Libs::LibKernel::Memory::Shutdown();
	std::printf("ALL CoreBreakthrough20Tests PASSED (3/3 suites)\n");
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
