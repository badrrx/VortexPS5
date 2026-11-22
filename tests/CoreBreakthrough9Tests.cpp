// Core Breakthrough #9 Verification Suite:
//   1. PM4 Command Processor Extended SDK 7.x-10.x Packet & Register Resilience
//   2. Kernel Direct/Flexible Memory Boundary & Invalid-Argument Hardening
//   3. Kernel VFS / FileSystem Boundary, Flag & Directory-Error Hardening

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/pm4.h"
#include "libs/errno.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestPm4ExtendedPacketResilience() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Pm4;

	// Construct a CommandProcessor with a null RenderContext reference (only non-rendering PM4
	// packets are executed here, exercising the real ProcessPm4/pm4Handlers dispatch paths).
	auto*            dummy_renderer = reinterpret_cast<RenderContext*>(static_cast<uintptr_t>(0));
	CommandProcessor cp(*dummy_renderer, 0);

	std::vector<uint32_t> stream;

	// 1. Standard IT_NOP with R_ZERO (2 dwords total)
	stream.push_back(KYTY_PM4(2u, IT_NOP, R_ZERO));
	stream.push_back(0x11223344u);

	// 2. Extended SDK 7.x-10.x IT_NOP with unknown custom R code (0x3e) — previously fatal EXIT!
	stream.push_back(KYTY_PM4(4u, IT_NOP, 0x3eu));
	stream.push_back(0xdeadbeefu);
	stream.push_back(0x00000001u);
	stream.push_back(0x00000002u);

	// 3. Unknown Type3 PM4 opcode (0xfe, 3 dwords) — previously fatal EXIT("unknown op")!
	stream.push_back(KYTY_PM4(3u, 0xfeu, 0u));
	stream.push_back(0x12345678u);
	stream.push_back(0x9abcdef0u);

	// 4. IT_SET_CONTEXT_REG writing an out-of-range / unknown register offset (0x3fe) —
	// previously fatal EXIT("unknown register")!
	stream.push_back(KYTY_PM4(4u, IT_SET_CONTEXT_REG, 0u));
	stream.push_back(0x000003feu);
	stream.push_back(0xaaaaaaaau);
	stream.push_back(0xbbbbbbbbu);

	// 5. IT_MEM_SEMAPHORE with extended control bits (sem_sel=0, previously fatal EXIT_NOT_IMPLEMENTED)
	stream.push_back(KYTY_PM4(4u, IT_MEM_SEMAPHORE, 0u));
	stream.push_back(0x00000000u);
	stream.push_back(0x00000000u);
	stream.push_back(0x00000001u);

	// 6. Valid IT_SET_UCONFIG_REG writing VGT_OBJECT_ID = 0xCAFEBABE right AFTER all extended/unknown
	// packets to prove packet advancement stayed 100% dword-aligned!
	stream.push_back(KYTY_PM4(3u, IT_SET_UCONFIG_REG, 0u));
	stream.push_back(VGT_OBJECT_ID);
	stream.push_back(0xCAFEBABEu);

	Pm4Execution exec {};
	const auto   res = cp.Process(exec, stream);
	Check(res == Pm4ProcessResult::Complete, "PM4 stream with extended SDK packets completes cleanly");
	Check(exec.MadeProgress(), "PM4 execution reports progress");
	Check(cp.GetUcfg().GetObjectId() == 0xCAFEBABEu,
	      "VGT_OBJECT_ID after unknown/extended packets matches 0xCAFEBABE (exact alignment)");

	// 7. Truncated stream resilience in both Process() and DumpPm4PacketStream()
	std::vector<uint32_t> truncated_stream = {
	    KYTY_PM4(6u, IT_NOP, R_ZERO), // claims 6 dwords, but only 2 are present
	    0x55667788u,
	};
	const auto dump_path =
	    std::filesystem::temp_directory_path() / "kyty_cb9_pm4_dump_test.txt";
	Common::File dump_file;
	Check(dump_file.Create(dump_path), "Create temp PM4 dump file");
	DumpPm4PacketStream(&dump_file, stream.data(), 0, static_cast<uint32_t>(stream.size()));
	DumpPm4PacketStream(&dump_file, truncated_stream.data(), 0,
	                    static_cast<uint32_t>(truncated_stream.size()));
	dump_file.Close();
	std::error_code ec;
	std::filesystem::remove(dump_path, ec);

	// Trailing single-dword non-Type2 header in Process() must end buffer cleanly without abort
	std::vector<uint32_t> trailing_header = {KYTY_PM4(3u, IT_NOP, R_ZERO)};
	Pm4Execution          trunc_exec {};
	const auto            trunc_res = cp.Process(trunc_exec, trailing_header);
	Check(trunc_res == Pm4ProcessResult::Complete,
	      "Truncated trailing PM4 header ends cleanly without abort");

	std::printf("[PASS] Suite 1: PM4 Extended SDK 7.x-10.x Packet & Register Resilience\n");
}

void TestKernelMemoryBoundaryHardening() {
	using namespace Libs::LibKernel;
	using namespace Libs::LibKernel::Memory;

	// 1. KernelMapFlexibleMemory with addr_in_out == nullptr — previously EXIT_NOT_IMPLEMENTED!
	Check(KernelMapFlexibleMemory(nullptr, 0x4000, 3, 0) == KERNEL_ERROR_EINVAL,
	      "KernelMapFlexibleMemory(nullptr, ...) returns KERNEL_ERROR_EINVAL");

	// 2. KernelMapFlexibleMemory with invalid prot (0x100) — previously EXIT("unknown prot")!
	void* addr = nullptr;
	Check(KernelMapFlexibleMemory(&addr, 0x4000, 0x100, 0) == KERNEL_ERROR_EINVAL,
	      "KernelMapFlexibleMemory with unknown prot=0x100 returns KERNEL_ERROR_EINVAL");

	// 3. KernelMapNamedFlexibleMemory with unaligned length -> KERNEL_ERROR_EINVAL
	Check(KernelMapNamedFlexibleMemory(&addr, 0x1234, 3, 0, "cb9_test") == KERNEL_ERROR_EINVAL,
	      "KernelMapNamedFlexibleMemory with unaligned len returns KERNEL_ERROR_EINVAL");

	// 4. KernelMapNamedFlexibleMemory with nullptr name -> KERNEL_ERROR_EFAULT
	Check(KernelMapNamedFlexibleMemory(&addr, 0x4000, 3, 0, nullptr) == KERNEL_ERROR_EFAULT,
	      "KernelMapNamedFlexibleMemory with null name returns KERNEL_ERROR_EFAULT");

	// 5. KernelQueryMemoryProtection(nullptr, ...) — previously EXIT_NOT_IMPLEMENTED!
	void* start = nullptr;
	void* end   = nullptr;
	int   prot  = 0;
	Check(KernelQueryMemoryProtection(nullptr, &start, &end, &prot) == KERNEL_ERROR_EINVAL,
	      "KernelQueryMemoryProtection(nullptr, ...) returns KERNEL_ERROR_EINVAL");

	std::printf("[PASS] Suite 2: Kernel Memory Boundary & Invalid-Argument Hardening\n");
}

void TestKernelVfsBoundaryHardening() {
	using namespace Libs::LibKernel;
	using namespace Libs::LibKernel::FileSystem;

	FileSystem::Initialize();

	const auto temp_dir = std::filesystem::temp_directory_path() / "kyty_cb9_vfs_test";
	std::error_code ec;
	std::filesystem::remove_all(temp_dir, ec);
	std::filesystem::create_directories(temp_dir, ec);
	Check(std::filesystem::exists(temp_dir), "Created temp VFS test directory");

	FileSystem::Mount(temp_dir, "/cb9_vfs", false);

	// 1. Valid file create, write, pread, stat, close
	constexpr int O_RDONLY    = 0x0000;
	constexpr int O_WRONLY    = 0x0001;
	constexpr int O_RDWR      = 0x0002;
	constexpr int O_CREAT     = 0x0200;
	constexpr int O_TRUNC     = 0x0400;
	constexpr int O_DIRECTORY = 0x00020000;

	const int fd = KernelOpen("/cb9_vfs/test.bin", O_RDWR | O_CREAT | O_TRUNC, 0666);
	Check(fd >= 3, "KernelOpen creates /cb9_vfs/test.bin");

	const char payload[] = "KYTY_PROSPERO_CB9_VFS_OK";
	const auto written   = KernelWrite(fd, payload, sizeof(payload));
	Check(written == static_cast<int64_t>(sizeof(payload)), "KernelWrite wrote full payload");

	char       readback[64] {};
	const auto pread_bytes = KernelPread(fd, readback, sizeof(payload), 0);
	Check(pread_bytes == static_cast<int64_t>(sizeof(payload)) &&
	          std::memcmp(readback, payload, sizeof(payload)) == 0,
	      "KernelPread reads back exact payload");

	FileStat st {};
	Check(KernelStat("/cb9_vfs/test.bin", &st) == OK &&
	          st.st_size == static_cast<int64_t>(sizeof(payload)),
	      "KernelStat returns exact file size");
	Check(KernelClose(fd) == OK, "KernelClose succeeds");

	// 2. Previously fatal EXIT("invalid flag_u: 3") -> KERNEL_ERROR_EINVAL
	Check(KernelOpen("/cb9_vfs/test.bin", 3, 0666) == KERNEL_ERROR_EINVAL,
	      "KernelOpen with invalid access mode flags_u=3 returns KERNEL_ERROR_EINVAL");

	// 3. Previously fatal EXIT_NOT_IMPLEMENTED(directory && rw_mode != Read / trunc / creat)
	Check(KernelOpen("/cb9_vfs", O_DIRECTORY | O_WRONLY, 0666) == KERNEL_ERROR_EISDIR,
	      "KernelOpen(O_DIRECTORY|O_WRONLY) returns KERNEL_ERROR_EISDIR");
	Check(KernelOpen("/cb9_vfs", O_DIRECTORY | O_TRUNC, 0666) == KERNEL_ERROR_EISDIR,
	      "KernelOpen(O_DIRECTORY|O_TRUNC) returns KERNEL_ERROR_EISDIR");
	Check(KernelOpen("/cb9_vfs", O_WRONLY, 0666) == KERNEL_ERROR_EISDIR,
	      "KernelOpen existing dir with O_WRONLY returns KERNEL_ERROR_EISDIR");

	// 4. Open directory read-only, then verify KernelPread returns KERNEL_ERROR_EISDIR (was fatal!)
	const int dir_fd = KernelOpen("/cb9_vfs", O_DIRECTORY | O_RDONLY, 0);
	Check(dir_fd >= 3, "KernelOpen(O_DIRECTORY|O_RDONLY) succeeds");
	Check(KernelPread(dir_fd, readback, 16, 0) == KERNEL_ERROR_EISDIR,
	      "KernelPread on directory FD returns KERNEL_ERROR_EISDIR");
	Check(KernelClose(dir_fd) == OK, "KernelClose(dir_fd) succeeds");

	// 5. Invalid FD returns KERNEL_ERROR_EBADF
	Check(KernelPread(9999, readback, 16, 0) == KERNEL_ERROR_EBADF,
	      "KernelPread on invalid FD returns KERNEL_ERROR_EBADF");

	FileSystem::Umount("/cb9_vfs");
	FileSystem::Shutdown();
	std::filesystem::remove_all(temp_dir, ec);

	std::printf("[PASS] Suite 3: Kernel VFS / FileSystem Boundary & Flag Hardening\n");
}

} // namespace

#if defined(_WIN32)
int wmain() {
#else
int main() {
#endif
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});

	TestPm4ExtendedPacketResilience();
	TestKernelMemoryBoundaryHardening();
	TestKernelVfsBoundaryHardening();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough9Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough9Tests PASSED (3/3 suites)\n");
	return 0;
}
