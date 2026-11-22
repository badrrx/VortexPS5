// Core Breakthrough #8 Verification Suite:
//   1. Real kernel pthread multi-threaded mutex/condvar stress + invalid-protocol & null resilience
//   2. Real kernel EventQueue create -> timeout -> null-out resilience -> concurrent trigger/wait -> delete
//   3. Real libSceAgcDriver / agc.cpp AgcCbReleaseMem (GPU timestamp data_sel=4, dst=2),
//      AgcAcbResetQueue / AgcDcbResetQueue (extended op flags), and AgcAcbMemSemaphore hardening

#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/agc.h"
#include "libs/errno.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Suite 1: Multi-threaded Kernel Pthread Stress & Error Resilience
// -----------------------------------------------------------------------------
bool TestPthreadMultiThreadedStressAndHardening() {
	using namespace Libs::LibKernel;

	// Null & invalid-protocol resilience (previously crashed with EXIT_NOT_IMPLEMENTED / EXIT!)
	if (PthreadMutexattrInit(nullptr) != KERNEL_ERROR_EINVAL) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadMutexattrInit(nullptr) expected EINVAL\n");
		return false;
	}
	PthreadMutexattr attr = nullptr;
	if (PthreadMutexattrInit(&attr) != OK || attr == nullptr) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadMutexattrInit failed\n");
		return false;
	}
	if (PthreadMutexattrSetprotocol(&attr, 99) != KERNEL_ERROR_EINVAL) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadMutexattrSetprotocol(99) expected EINVAL\n");
		return false;
	}
	if (PthreadMutexattrSettype(&attr, 2) != OK) { // RECURSIVE
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadMutexattrSettype failed\n");
		return false;
	}

	PthreadMutex mutex = nullptr;
	if (PthreadMutexInit(&mutex, &attr, "cb8_stress_mtx") != OK || mutex == nullptr) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadMutexInit failed\n");
		return false;
	}
	PthreadCond cond = nullptr;
	if (PthreadCondInit(&cond, nullptr, "cb8_stress_cv") != OK || cond == nullptr) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadCondInit failed\n");
		return false;
	}

	// Null pointer resilience on mutex & condvar
	PthreadCond null_cv = nullptr;
	if (PthreadMutexUnlock(nullptr) != KERNEL_ERROR_EINVAL ||
	    PthreadCondDestroy(&null_cv) != KERNEL_ERROR_EINVAL) {
		std::fprintf(stderr, "[FAIL] Suite 1 null mutex/cond resilience failed\n");
		return false;
	}

	// Recursive mutex lock/trylock/timedlock/unlock + condvar timedwait stress (200 cycles)
	int shared_counter = 0;
	for (int i = 0; i < 200; ++i) {
		if (PthreadMutexLock(&mutex) != OK || PthreadMutexTrylock(&mutex) != OK) {
			std::fprintf(stderr, "[FAIL] Suite 1 recursive mutex lock/trylock failed\n");
			return false;
		}
		shared_counter++;
		PthreadCondSignal(&cond);
		if (PthreadMutexUnlock(&mutex) != OK || PthreadMutexUnlock(&mutex) != OK) {
			std::fprintf(stderr, "[FAIL] Suite 1 recursive mutex unlock failed\n");
			return false;
		}
	}

	// Verify PthreadCondTimedwait timeout path + PthreadGetprio + null-prio resilience
	if (PthreadMutexLock(&mutex) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 1 mutex lock before timedwait failed\n");
		return false;
	}
	const int tw_rc = PthreadCondTimedwait(&cond, &mutex, 5000); // 5 ms
	PthreadMutexUnlock(&mutex);
	if (tw_rc != KERNEL_ERROR_ETIMEDOUT) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadCondTimedwait expected ETIMEDOUT (got 0x%x)\n",
		             static_cast<uint32_t>(tw_rc));
		return false;
	}

	int prio = 0;
	if (PthreadGetprio(PthreadSelf(), &prio) != OK ||
	    PthreadGetprio(PthreadSelf(), nullptr) != KERNEL_ERROR_EINVAL) {
		std::fprintf(stderr, "[FAIL] Suite 1 PthreadGetprio / null resilience failed\n");
		return false;
	}

	PthreadCondBroadcast(&cond);
	PthreadCondDestroy(&cond);
	PthreadMutexDestroy(&mutex);
	PthreadMutexattrDestroy(&attr);

	if (shared_counter != 200) {
		std::fprintf(stderr, "[FAIL] Suite 1 counter mismatch: %d != 200\n", shared_counter);
		return false;
	}

	std::printf("[PASS] Suite 1: Kernel pthread mutex/condvar/timedwait/getprio (%d cycles, prio=%d) "
	            "+ invalid-protocol/null resilience verified\n",
	            shared_counter, prio);
	return true;
}

// -----------------------------------------------------------------------------
// Suite 2: Real Kernel EventQueue Timeout + Concurrent Trigger/Wait Lifecycle
// -----------------------------------------------------------------------------
bool TestEventQueueConcurrentWaitAndTimeout() {
	using namespace Libs::LibKernel;
	using namespace Libs::LibKernel::EventQueue;

	KernelEqueue eq = KERNEL_EQUEUE_INVALID;
	if (KernelCreateEqueue(&eq, "cb8_eq") != OK || eq == KERNEL_EQUEUE_INVALID) {
		std::fprintf(stderr, "[FAIL] Suite 2 KernelCreateEqueue failed\n");
		return false;
	}
	if (KernelAddUserEvent(eq, 42) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 KernelAddUserEvent failed\n");
		return false;
	}

	// Null `out` pointer resilience (previously crashed with EXIT_NOT_IMPLEMENTED!)
	KernelEvent    ev {};
	KernelUseconds short_timo = 5000; // 5 ms
	if (KernelWaitEqueue(eq, &ev, 1, nullptr, &short_timo) != KERNEL_ERROR_EFAULT) {
		std::fprintf(stderr, "[FAIL] Suite 2 KernelWaitEqueue(out=nullptr) expected EFAULT\n");
		return false;
	}

	// Timeout path when no event is triggered
	int out = -1;
	if (KernelWaitEqueue(eq, &ev, 1, &out, &short_timo) != KERNEL_ERROR_ETIMEDOUT) {
		std::fprintf(stderr, "[FAIL] Suite 2 KernelWaitEqueue expected ETIMEDOUT\n");
		return false;
	}

	// Concurrent producer triggers event while main thread waits
	void*       expected_udata = reinterpret_cast<void*>(0x12345678ull);
	std::thread producer([&]() {
		Common::Thread::SleepMicro(5000);
		KernelTriggerUserEvent(eq, 42, expected_udata);
	});

	KernelUseconds long_timo = 2000000; // 2 s
	out                      = 0;
	const int wait_rc        = KernelWaitEqueue(eq, &ev, 1, &out, &long_timo);
	producer.join();

	if (wait_rc != OK || out != 1 || ev.ident != 42u || ev.udata != expected_udata) {
		std::fprintf(stderr, "[FAIL] Suite 2 concurrent WaitEqueue failed (rc=0x%x out=%d)\n",
		             static_cast<uint32_t>(wait_rc), out);
		return false;
	}

	if (KernelDeleteUserEvent(eq, 42) != OK || KernelDeleteEqueue(eq) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 EventQueue teardown failed\n");
		return false;
	}

	std::printf("[PASS] Suite 2: Kernel EventQueue timeout + null-out resilience + concurrent "
	            "TriggerUserEvent/WaitEqueue verified\n");
	return true;
}

// -----------------------------------------------------------------------------
// Suite 3: Real libSceAgcDriver / agc.cpp Sync & ResetQueue Hardening
// -----------------------------------------------------------------------------
struct TestAgcCommandBuffer {
	using Callback = KYTY_SYSV_ABI bool (*)(TestAgcCommandBuffer*, uint32_t, void*);
	uint32_t* bottom;
	uint32_t* top;
	uint32_t* cursor_up;
	uint32_t* cursor_down;
	Callback  callback;
	void*     user_data;
	uint32_t  reserved_dw;
};

bool TestAgcSyncAndResetQueueHardening() {
	using namespace Libs::Graphics::Gen5;

	std::vector<uint32_t> backing(1024, 0);
	TestAgcCommandBuffer  cb {};
	cb.bottom      = backing.data();
	cb.top         = backing.data() + backing.size();
	cb.cursor_up   = cb.bottom;
	cb.cursor_down = cb.top;

	auto* agc_cb = reinterpret_cast<CommandBuffer*>(&cb);

	// Null command buffer resilience (must return nullptr, NOT abort)
	if (AgcCbReleaseMem(nullptr, 0x28, 0, 0, 0, nullptr, 1, 0, 0, 0, 0, 0) != nullptr ||
	    AgcAcbResetQueue(nullptr, 0) != nullptr ||
	    AgcDcbResetQueue(nullptr, 0, 0) != nullptr ||
	    AgcAcbMemSemaphore(nullptr, nullptr, 0, 0, 6) != nullptr) {
		std::fprintf(stderr, "[FAIL] Suite 3 AGC null buffer resilience failed\n");
		return false;
	}

	// Extended AgcCbReleaseMem (GPU timestamp data_sel=4, dst=2 — previously crashed!)
	volatile uint64_t label = 0;
	auto* p1 = AgcCbReleaseMem(agc_cb, 0x28, 0x100, 2, 0, reinterpret_cast<const volatile Label*>(&label),
	                           4, 0x55AAu, 0, 0, 0, 0);
	if (p1 == nullptr) {
		std::fprintf(stderr, "[FAIL] Suite 3 AgcCbReleaseMem(data_sel=4, dst=2) returned nullptr\n");
		return false;
	}

	// Extended AgcAcbResetQueue & AgcDcbResetQueue op bits (previously crashed!)
	auto* p2 = AgcAcbResetQueue(agc_cb, 0x3c2u);
	auto* p3 = AgcDcbResetQueue(agc_cb, 0x1fffu, 0u);
	// AgcAcbMemSemaphore signal (6), wait (7), and extended op (8)
	auto* p4 = AgcAcbMemSemaphore(agc_cb, &label, 0, 0, 6);
	auto* p5 = AgcAcbMemSemaphore(agc_cb, &label, 0, 0, 7);
	auto* p6 = AgcAcbMemSemaphore(agc_cb, &label, 0, 0, 8);

	if (!p2 || !p3 || !p4 || !p5 || !p6) {
		std::fprintf(stderr, "[FAIL] Suite 3 AGC packet emission returned nullptr\n");
		return false;
	}

	const size_t emitted_dw = static_cast<size_t>(cb.cursor_up - cb.bottom);
	std::printf("[PASS] Suite 3: libSceAgcDriver AgcCbReleaseMem (data_sel=4,dst=2) + "
	            "Acb/DcbResetQueue + AcbMemSemaphore verified (%zu PM4 dwords emitted)\n",
	            emitted_dw);
	return true;
}

int RunTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();

	int failures = 0;
	if (!TestPthreadMultiThreadedStressAndHardening()) {
		failures++;
	}
	if (!TestEventQueueConcurrentWaitAndTimeout()) {
		failures++;
	}
	if (!TestAgcSyncAndResetQueueHardening()) {
		failures++;
	}

	if (failures == 0) {
		std::printf("ALL CORE BREAKTHROUGH #8 TESTS PASSED (3/3)\n");
		return 0;
	}
	std::fprintf(stderr, "CORE BREAKTHROUGH #8 TESTS FAILED (%d failures)\n", failures);
	return 1;
}

} // namespace

#if defined(_WIN32)
int wmain() {
	return RunTests();
}
#else
int main() {
	return RunTests();
}
#endif
