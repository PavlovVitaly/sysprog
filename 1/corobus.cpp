#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include<queue>

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

#if 1 /* Uncomment this if want to use */ //TodoV: was disable

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	/* std::vector/queue/deque/list/...<unsigned> data; */
	std::queue<unsigned> data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
	std::queue<int> availible_channels;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

namespace{

	inline bool is_channel_exists(struct coro_bus *bus, int channel){
		return bus 
			&& channel >= 0
			&& channel < bus->channel_count + static_cast<int>(bus->availible_channels.size())
			&& (bus->channels[channel]);
	}

	inline bool is_full_data_buffer(struct coro_bus *bus, int channel){
		return bus->channels[channel]->data.size() >= bus->channels[channel]->size_limit;
	}

	inline bool is_empty_data_buffer(struct coro_bus *bus, int channel){
		return bus->channels[channel]->data.empty();
	}

	inline int get_number_available_channels(struct coro_bus *bus){
		return bus->channel_count + bus->availible_channels.size();
	}

	void suspend_coros_in_channels_with_full_buffer(struct coro_bus *bus, int channel){
		if(is_full_data_buffer(bus, channel)){
			wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
		}
	}

	void wakeup_all_coros_into_queue(struct wakeup_queue *queue){
		struct wakeup_entry *entry;
		rlist_foreach_entry(entry, &queue->coros, base) {
			coro_wakeup(entry->coro);
		}
	}
	
}//namespace

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
struct coro_bus *
coro_bus_new(void)
{
	return new coro_bus{ .channels = new coro_bus_channel*[10]
					   , .channel_count = 0
					   , .availible_channels{}};
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
void
coro_bus_delete(struct coro_bus *bus)
{
	for(int i = 0; i < get_number_available_channels(bus); ++i){
		if(!is_channel_exists(bus, i)){
			continue;
		}
		coro_bus_channel_close(bus, i);
	}
	delete bus;
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
/*
 * One of the tests will force you to reuse the channel
 * descriptors. It means, that if your maximal channel
 * descriptor is N, and you have any free descriptor in
 * the range 0-N, then you should open the new channel on
 * that old descriptor.
 *
 * A more precise instruction - check if any of the
 * bus->channels[i] with i = 0 -> bus->channel_count is
 * free (== NULL). If yes - reuse the slot. Don't grow the
 * bus->channels array, when have space in it.
 */
int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	if (!bus){
		return -1;
	}
	int new_channel = bus->channel_count;
	// Check existing empty channel for reuse it.
	if (!bus->availible_channels.empty()){
		new_channel = bus->availible_channels.front();
		bus->availible_channels.pop();
	}
	// Create channel
	bus->channels[new_channel] = new coro_bus_channel{};
	rlist_create(&bus->channels[new_channel]->recv_queue.coros);
	rlist_create(&bus->channels[new_channel]->send_queue.coros);
	bus->channels[new_channel]->size_limit = size_limit;
	bus->channel_count++;
	return new_channel;
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
/*
 * Be very attentive here. What happens, if the channel is
 * closed while there are coroutines waiting on it? For
 * example, the channel was empty, and some coros were
 * waiting on its recv_queue.
 *
 * If you wakeup those coroutines and just delete the
 * channel right away, then those waiting coroutines might
 * on wakeup try to reference invalid memory.
 *
 * Can happen, for example, if you use an intrusive list
 * (rlist), delete the list itself (by deleting the
 * channel), and then the coroutines on wakeup would try
 * to remove themselves from the already destroyed list.
 *
 * Think how you could address that. Remove all the
 * waiters from the list before freeing it? Yield this
 * coroutine after waking up the waiters but before
 * freeing the channel, so the waiters could safely leave?
 */
void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	if (!is_channel_exists(bus, channel)){
		return;
	}

	wakeup_all_coros_into_queue(&bus->channels[channel]->recv_queue);
	wakeup_all_coros_into_queue(&bus->channels[channel]->send_queue);

	struct wakeup_entry *entry, *tmp;
	rlist_foreach_entry_safe(entry, &bus->channels[channel]->send_queue.coros, base, tmp)
	rlist_del_entry(entry, base);
	rlist_foreach_entry_safe(entry, &bus->channels[channel]->recv_queue.coros, base, tmp)
	rlist_del_entry(entry, base);
	
	delete bus->channels[channel];
	bus->channels[channel] = nullptr;
	bus->channel_count--;
	bus->availible_channels.push(channel);
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
/*
 * Try sending in a loop, until success. If error, then
 * check which one is that. If 'wouldblock', then suspend
 * this coroutine and try again when woken up.
 *
 * If see the channel has space, then wakeup the first
 * coro in the send-queue. That is needed so when there is
 * enough space for many messages, and many coroutines are
 * waiting, they would then wake each other up one by one
 * as lone as there is still space.
 */
int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	while (true){
		auto send_status = coro_bus_try_send(bus, channel, data);
		if (send_status == 0){
			if (is_channel_exists(bus, channel)){
				wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
			}
			return 0;
		}

		if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK){
				return -1;
		}

		wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
	}
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
/*
 * Append data if has space. Otherwise 'wouldblock' error.
 * Wakeup the first coro in the recv-queue! To let it know
 * there is data.
 */
int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} else if (is_full_data_buffer(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	bus->channels[channel]->data.push(data);
	wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
	return 0;
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	
	while (true){
		auto recv_status = coro_bus_try_recv(bus, channel, data);
		if (recv_status == 0){
			if (is_channel_exists(bus, channel) && !is_empty_data_buffer(bus, channel)) {
				wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
			}
			return 0;
		}

		if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK){
			return -1;
		}

		wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
	}
	return 0;
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if(is_empty_data_buffer(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	*data = bus->channels[channel]->data.front();
	bus->channels[channel]->data.pop();
	wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
	return 0;
}


#if NEED_BROADCAST

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!bus){
		return -1;
	} else if (bus->channel_count == 0){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} 

	while (true){
		auto send_status = coro_bus_try_broadcast(bus, data);
		if (send_status == 0){
			return 0;
		}

		if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK){
				return -1;
		}

		for(int channel = 0; channel < get_number_available_channels(bus); ++channel){
			if(!is_channel_exists(bus, channel)){
				continue;
			}
			suspend_coros_in_channels_with_full_buffer(bus, channel);
		}
	}
	
	return 0;
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!bus){
		return -1;
	} else if (bus->channel_count == 0){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} 
	
	for(int channel = 0; channel < get_number_available_channels(bus); ++channel){
		if(is_channel_exists(bus, channel) && is_full_data_buffer(bus, channel)){
			coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_WOULD_BLOCK);
			return -1;
		};
	}

	for(int channel = 0; channel < get_number_available_channels(bus); ++channel){
		if(!is_channel_exists(bus, channel)){
			continue;
		}
		bus->channels[channel]->data.push(data);
		wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
	}
	
	return 0;
}

#endif

#if NEED_BATCH

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	while (true){
		auto send_status = coro_bus_try_send_v(bus, channel, data, count);

		if (send_status > 0){
			if (is_channel_exists(bus, channel)){
				wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
			}
			return send_status;
		}

		if (send_status < 0 && coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK){
			return -1;
		}

		wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
	}
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} else if (is_full_data_buffer(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned cnt = 0;
	for(; cnt < count; ++cnt){
		auto send_status = coro_bus_try_send(bus, channel, data[cnt]);
		if(send_status < 0 && cnt == 0){
			return -1;
		} else if(send_status < 0){
			break;
		}
	}
	return static_cast<int>(cnt);
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	while (true){
		auto recv_status = coro_bus_try_recv_v(bus, channel, data, capacity);

		if (recv_status > 0){
			if (is_channel_exists(bus, channel) && !is_empty_data_buffer(bus, channel)) {
				wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
			}
			return recv_status;
		}

		if (recv_status < 0 && coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK){
			return -1;
		}

		wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
	}
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(coro_bus_error_code::CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if(is_empty_data_buffer(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned cnt = 0;
	for(; cnt < capacity && !is_empty_data_buffer(bus, channel); ++cnt){
		data[cnt] = bus->channels[channel]->data.front();
		bus->channels[channel]->data.pop();
	}
	wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
	return static_cast<int>(cnt);
}

#endif
