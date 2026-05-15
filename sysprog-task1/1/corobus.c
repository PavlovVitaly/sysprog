#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#define INIT_MAX_CHANNEL_NUMBER 10

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

#if 1 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
	const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
			sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

/** Append a single message to the vector. */
static void
data_vector_append(struct data_vector *vector, unsigned data)
{
	data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned
data_vector_pop_first(struct data_vector *vector)
{
	unsigned data = 0;
	data_vector_pop_first_many(vector, &data, 1);
	return data;
}

#endif

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

#if 1 /* Uncomment this if want to use */

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
	struct data_vector data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
	struct data_vector availible_channels;
	int capacity;
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

static inline bool is_channel_exists(struct coro_bus *bus, int channel){
	return bus 
		&& channel >= 0
		&& channel < (int)(bus->channel_count + bus->availible_channels.size)
		&& (bus->channels[channel]);
}

static inline bool is_full_data_buffer(struct coro_bus *bus, int channel){
	return bus->channels[channel]->data.size >= bus->channels[channel]->size_limit;
}

static inline bool is_empty_data_buffer(struct coro_bus *bus, int channel){
	return bus->channels[channel]->data.size <= 0;
}

static inline int get_number_available_channels(struct coro_bus *bus){
	return bus->channel_count + bus->availible_channels.size;
}

static void suspend_coros_in_channels_with_full_buffer(struct coro_bus *bus, int channel){
	if(is_full_data_buffer(bus, channel)){
		wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
	}
}

static void wakeup_all_coros_into_queue(struct wakeup_queue *queue){
	struct wakeup_entry *entry;
	rlist_foreach_entry(entry, &queue->coros, base) {
		coro_wakeup(entry->coro);
	}
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *coro_bus_ptr = (struct coro_bus*) malloc(sizeof(struct coro_bus));

    if (coro_bus_ptr == NULL) {
        printf("failed memory allocation for new coro_bus\n");
        exit(1);
    }

	coro_bus_ptr->channels = (struct coro_bus_channel**) calloc(INIT_MAX_CHANNEL_NUMBER, sizeof(struct coro_bus_channel*));

	if (coro_bus_ptr->channels == NULL) {
        printf("failed memory allocation for channel storage into coro_bus\n");
        exit(1);
    }
	
	coro_bus_ptr->channel_count = 0;
	coro_bus_ptr->availible_channels = (struct data_vector){.data = NULL, .size = 0, .capacity = 0};
	coro_bus_ptr->capacity = INIT_MAX_CHANNEL_NUMBER;
	return coro_bus_ptr;
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
	free(bus);
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
	if (bus->availible_channels.size > 0){
		new_channel = data_vector_pop_first(&bus->availible_channels);
	}

	// realloc memory for new size of channel storage
	if(new_channel >= bus->capacity){
		int new_capacity = bus->capacity * 2;
		struct coro_bus_channel** tmp = (struct coro_bus_channel**) realloc(bus->channels, new_capacity * sizeof(struct coro_bus_channel*));
		if (tmp == NULL) {
        	printf("failed memory increase for coro_bus channel storage\n");
			free(bus->channels);
        	exit(1);
    	}
		bus->channels = tmp;
		bus->capacity = new_capacity;
	}

	// Create channel
	bus->channels[new_channel] = (struct coro_bus_channel*) calloc(size_limit, sizeof(struct coro_bus_channel));

	if (bus->channels[new_channel] == NULL) {
        printf("failed memory allocation for new coro_bus channel\n");
        exit(1);
    }

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
	
	free(bus->channels[channel]);
	bus->channels[channel] = NULL;
	bus->channel_count--;
	data_vector_append(&bus->availible_channels, channel);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	while (true){
		int send_status = coro_bus_try_send(bus, channel, data);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} else if (is_full_data_buffer(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	data_vector_append(&bus->channels[channel]->data, data);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	
	while (true){
		int recv_status = coro_bus_try_recv(bus, channel, data);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if(is_empty_data_buffer(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	*data = data_vector_pop_first(&bus->channels[channel]->data);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} 

	while (true){
		int send_status = coro_bus_try_broadcast(bus, data);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} 
	
	for(int channel = 0; channel < get_number_available_channels(bus); ++channel){
		if(is_channel_exists(bus, channel) && is_full_data_buffer(bus, channel)){
			coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
			return -1;
		};
	}

	for(int channel = 0; channel < get_number_available_channels(bus); ++channel){
		if(!is_channel_exists(bus, channel)){
			continue;
		}
		data_vector_append(&bus->channels[channel]->data, data);
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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	while (true){
		int send_status = coro_bus_try_send_v(bus, channel, data, count);

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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	} else if (is_full_data_buffer(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned cnt = 0;
	for(; cnt < count; ++cnt){
		int send_status = coro_bus_try_send(bus, channel, data[cnt]);
		if(send_status < 0 && cnt == 0){
			return -1;
		} else if(send_status < 0){
			break;
		}
	}
	
	return (int)cnt;
}

/* STUDENT IMPLEMENTATION OF THIS FUNCTION */
int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (!bus){
		return -1;
	} else if (!is_channel_exists(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	while (true){
		int recv_status = coro_bus_try_recv_v(bus, channel, data, capacity);

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
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if(is_empty_data_buffer(bus, channel)){
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned cnt = 0;
	for(; cnt < capacity && !is_empty_data_buffer(bus, channel); ++cnt){
		data[cnt] = data_vector_pop_first(&bus->channels[channel]->data);
	}
	wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
	return (int)cnt;
}


#endif
