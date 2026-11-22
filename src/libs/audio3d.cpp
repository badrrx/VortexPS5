#include "libs/audio3d.h"

#include "common/assert.h"
#include "common/threads.h"
#include "kernel/semaphore.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <atomic>
#include <thread>
#include <vector>

namespace Libs::Audio {

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = LibKernel::Semaphore;

constexpr int32_t AUDIO3D_ERROR_INVALID_PARAMETER = static_cast<int32_t>(0x80f90001u);
constexpr int32_t AUDIO3D_ERROR_OUT_OF_RESOURCES  = static_cast<int32_t>(0x80f90002u);
constexpr int32_t AUDIO3D_ERROR_INVALID_PORT      = static_cast<int32_t>(0x80f90003u);
constexpr int32_t AUDIO3D_ERROR_INVALID_OBJECT    = static_cast<int32_t>(0x80f90004u);

struct Audio3dOpenParameters {
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	// uint32_t num_beds;
};

struct Audio3dData {
	enum class State { Empty, Ready, Play };

	std::atomic<State> state = State::Empty;
};

struct Audio3dInternal {
	Audio3dData*          data                        = nullptr;
	Common::Mutex*        data_mutex                  = nullptr;
	uint64_t              data_delay                  = 0;
	Semaphore::KernelSema playback_sema               = nullptr;
	Audio3dOpenParameters params                      = {};
	int                   user_id                     = 0;
	float                 late_reverb_level           = 0.0f;
	float                 downmix_spread_radius       = 2.0f;
	int                   downmix_spread_height_aware = 0;
	uint32_t              data_index                  = 0;
	bool                  used                        = false;
	std::atomic_bool      playback_stop               = false;
	std::atomic_bool      playback_finished           = false;
	std::vector<bool>     objects_used;
};

constexpr uint32_t MAX_PORTS = 4;

static Audio3dInternal g_ports[MAX_PORTS] = {};

static void playback_simulate(void* arg) {
	auto* port = static_cast<Audio3dInternal*>(arg);
	if (port == nullptr || port->data_mutex == nullptr || port->data == nullptr) {
		return;
	}

	for (;;) {
		int result = Semaphore::KernelWaitSema(port->playback_sema, 1, nullptr);

		if (result != OK || port->playback_stop.load(std::memory_order_acquire)) {
			break;
		}

		Audio3dData* play_data = nullptr;

		port->data_mutex->Lock();
		{
			for (uint32_t i = 0; i < port->params.queue_depth; i++) {
				uint32_t index = (port->data_index + i) % port->params.queue_depth;

				if (port->data[index].state == Audio3dData::State::Play) {
					play_data = &port->data[index];
					break;
				}
			}
		}
		port->data_mutex->Unlock();

		if (play_data != nullptr) {
			// TODO(): Audio output is not yet implemented, so simulate audio delay
			Common::Thread::SleepMicro(port->data_delay);
			play_data->state = Audio3dData::State::Empty;
		}
	}

	port->playback_finished.store(true, std::memory_order_release);
}

int KYTY_SYSV_ABI Audio3dInitialize(int64_t reserved) {
	PRINT_NAME();

	if (reserved != 0) {
		LOGF("\t warning: non-zero reserved = %" PRId64 "\n", reserved);
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dTerminate() {
	PRINT_NAME();

	for (uint32_t p = 0; p < MAX_PORTS; p++) {
		if (g_ports[p].used) {
			Audio3dPortClose(p);
		}
	}

	return OK;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p) {
	PRINT_NAME();

	if (p == nullptr) {
		return;
	}

	*p = Audio3dOpenParameters();
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters,
                                  uint32_t* id) {
	PRINT_NAME();

	if (parameters == nullptr || id == nullptr || parameters->size < 0x20) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	LOGF("\t user_id     = %d\n"
	     "\t granularity = %u\n"
	     "\t rate        = %u\n"
	     "\t max_objects = %u\n"
	     "\t queue_depth = %u\n"
	     "\t buffer_mode = %u\n",
	     user_id, parameters->granularity, parameters->rate, parameters->max_objects,
	     parameters->queue_depth, parameters->buffer_mode);

	uint32_t port = 0;
	for (; port < MAX_PORTS; port++) {
		if (!g_ports[port].used) {
			break;
		}
	}

	if (port >= MAX_PORTS) {
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	Audio3dOpenParameters normalized = *parameters;
	if (normalized.granularity == 0) {
		normalized.granularity = 256;
	}
	if (normalized.queue_depth == 0) {
		normalized.queue_depth = 2;
	}
	if (normalized.max_objects == 0) {
		normalized.max_objects = 512;
	}

	g_ports[port].user_id = user_id;
	g_ports[port].params  = normalized;
	g_ports[port].used    = true;
	g_ports[port].objects_used.assign(normalized.max_objects, false);

	g_ports[port].data       = new Audio3dData[normalized.queue_depth];
	g_ports[port].data_index = 0;
	g_ports[port].data_mutex = new Common::Mutex;
	g_ports[port].data_delay = (1000000 * static_cast<uint64_t>(normalized.granularity)) / 48000;

	for (uint32_t d = 0; d < normalized.queue_depth; d++) {
		g_ports[port].data[d].state = Audio3dData::State::Empty;
	}

	int result = Semaphore::KernelCreateSema(&g_ports[port].playback_sema, "audio3d_play", 0x01, 0,
	                                         static_cast<int>(normalized.queue_depth + 1), nullptr);
	if (result != OK) {
		delete[] g_ports[port].data;
		delete g_ports[port].data_mutex;
		g_ports[port].data       = nullptr;
		g_ports[port].data_mutex = nullptr;
		g_ports[port].used       = false;
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	g_ports[port].playback_stop.store(false, std::memory_order_release);
	g_ports[port].playback_finished.store(false, std::memory_order_release);
	std::thread(playback_simulate, &g_ports[port]).detach();

	*id = port;

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortClose(uint32_t port_id) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	auto* port = &g_ports[port_id];
	port->playback_stop.store(true, std::memory_order_release);
	if (port->playback_sema != nullptr) {
		Semaphore::KernelSignalSema(port->playback_sema, 1);
	}
	while (!port->playback_finished.load(std::memory_order_acquire)) {
		Common::Thread::SleepMicro(500);
	}
	if (port->playback_sema != nullptr) {
		Semaphore::KernelDeleteSema(port->playback_sema);
		port->playback_sema = nullptr;
	}
	delete[] port->data;
	delete port->data_mutex;
	port->data       = nullptr;
	port->data_mutex = nullptr;
	port->objects_used.clear();
	port->used = false;

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id,
                                          const void* attribute, size_t attribute_size) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (attribute == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	LOGF("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id) {
		case 0x10001:
			if (attribute_size < sizeof(float)) {
				return AUDIO3D_ERROR_INVALID_PARAMETER;
			}
			g_ports[port_id].late_reverb_level = *static_cast<const float*>(attribute);
			LOGF("\t late_reverb_level = %f\n", g_ports[port_id].late_reverb_level);
			break;
		case 0x10002:
			if (attribute_size < sizeof(float)) {
				return AUDIO3D_ERROR_INVALID_PARAMETER;
			}
			g_ports[port_id].downmix_spread_radius = *static_cast<const float*>(attribute);
			LOGF("\t downmix_spread_radius = %f\n", g_ports[port_id].downmix_spread_radius);
			break;
		case 0x10003:
			if (attribute_size < sizeof(int)) {
				return AUDIO3D_ERROR_INVALID_PARAMETER;
			}
			g_ports[port_id].downmix_spread_height_aware = *static_cast<const int*>(attribute);
			LOGF("\t downmix_spread_height_aware = %d\n",
			     g_ports[port_id].downmix_spread_height_aware);
			break;
		default:
			LOGF("\t warning: ignoring unhandled Audio3d port attribute 0x%" PRIx32 " (size=%zu)\n",
			     attribute_id, attribute_size);
			break;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level,
                                           uint32_t* queue_available) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (queue_level == nullptr && queue_available == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	auto* port = &g_ports[port_id];

	uint32_t empty_num = 0;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++) {
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Empty) {
				empty_num++;
			} else {
				break;
			}
		}
	}
	port->data_mutex->Unlock();

	LOGF("\t queue_available = %u\n", empty_num);

	if (queue_level != nullptr) {
		*queue_level = port->params.queue_depth - empty_num;
	}
	if (queue_available != nullptr) {
		*queue_available = empty_num;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t port_id) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	auto* port = &g_ports[port_id];

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state == Audio3dData::State::Empty) {
			port->data[current_index].state = Audio3dData::State::Ready;
		}

		port->data_index = next_index;

		LOGF("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	auto* port = &g_ports[port_id];

	LOGF("\t blocking = %u\n", blocking);

	int          data_num   = 0;
	Audio3dData* first_data = nullptr;

	port->data_mutex->Lock();
	{
		first_data = port->data + port->data_index;

		for (uint32_t i = 0; i < port->params.queue_depth; i++) {
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Ready) {
				port->data[index].state = Audio3dData::State::Play;
				data_num++;
			}
		}
	}
	port->data_mutex->Unlock();

	LOGF("\t push num = %d\n", data_num);

	if (data_num > 0) {
		Semaphore::KernelSignalSema(port->playback_sema, data_num);

		if (blocking == 1 && first_data != nullptr) {
			auto wait_time = port->data_delay / 8;
			if (wait_time == 0) {
				wait_time = 100;
			}
			while (first_data->state != Audio3dData::State::Empty &&
			       !port->playback_stop.load(std::memory_order_acquire)) {
				Common::Thread::SleepMicro(wait_time);
			}
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectReserve(uint32_t port_id, uint32_t* object_id) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (object_id == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	auto* port = &g_ports[port_id];
	port->data_mutex->Lock();
	for (uint32_t i = 0; i < port->objects_used.size(); i++) {
		if (!port->objects_used[i]) {
			port->objects_used[i] = true;
			*object_id            = i + 1;
			port->data_mutex->Unlock();
			return OK;
		}
	}
	port->data_mutex->Unlock();
	return AUDIO3D_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI Audio3dObjectUnreserve(uint32_t port_id, uint32_t object_id) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	auto* port = &g_ports[port_id];
	if (object_id == 0 || object_id > port->objects_used.size()) {
		return AUDIO3D_ERROR_INVALID_OBJECT;
	}

	port->data_mutex->Lock();
	if (!port->objects_used[object_id - 1]) {
		port->data_mutex->Unlock();
		return AUDIO3D_ERROR_INVALID_OBJECT;
	}
	port->objects_used[object_id - 1] = false;
	port->data_mutex->Unlock();
	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectSetAttributes(uint32_t port_id, uint32_t object_id,
                                             size_t num_attributes, const void* attributes) {
	PRINT_NAME();

	if (port_id >= MAX_PORTS || !g_ports[port_id].used) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	auto* port = &g_ports[port_id];
	if (object_id == 0 || object_id > port->objects_used.size() ||
	    !port->objects_used[object_id - 1]) {
		return AUDIO3D_ERROR_INVALID_OBJECT;
	}
	if (num_attributes > 0 && attributes == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	// Spatial object attributes (PCM, position, gain, spread, priority) are accepted cleanly.
	return OK;
}

} // namespace Audio3d

} // namespace Libs::Audio
