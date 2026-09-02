#include "thread_pool.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <errno.h>
#include <time.h>

enum thread_task_status : int{
	THREAD_TASK_IDLE = -1,
	THREAD_TASK_WAITING = 0,
	THREAD_TASK_RUNNING = 1,
	THREAD_TASK_FINISHED = 2
};

struct thread_task {
	thread_task_f function;
	void *arg;

	/* PUT HERE OTHER MEMBERS */
	atomic_int status;
	pthread_mutex_t mtx;
	pthread_cond_t cv;
	bool is_detached;
};

static void
deinit_thread_task(struct thread_task *t){
	pthread_mutex_destroy(&t->mtx);
	pthread_cond_destroy(&t->cv);
}

struct thread_task_queue{
	pthread_mutex_t mtx;
	pthread_cond_t cv;
	struct thread_task *queue[TPOOL_MAX_TASKS];
	size_t begin_idx;
	size_t end_idx;
	size_t size;
	bool is_stopped;
};

static bool
init_thread_task_queue(struct thread_task_queue *q){
	if(pthread_mutex_init(&q->mtx, NULL) != 0) return false;
	if(pthread_cond_init(&q->cv, NULL) != 0) return false;
	memset(q->queue, 0, TPOOL_MAX_TASKS * sizeof(q->queue[0]));
	q->begin_idx = 0;
	q->end_idx = 0;
	q->size = 0;
	q->is_stopped = 0;
	return true;
}

static bool
push(struct thread_task_queue *q, struct thread_task *t){
	{
		pthread_mutex_lock(&q->mtx);
		if (q->size == TPOOL_MAX_TASKS) {
			pthread_mutex_unlock(&q->mtx);
			return false;
		}

		q->queue[q->end_idx] = t;
		q->end_idx = (q->end_idx + 1) % TPOOL_MAX_TASKS;
		++q->size;
	}
	pthread_cond_signal(&q->cv);
	pthread_mutex_unlock(&q->mtx);
	return true;
}

static struct thread_task
*pop(struct thread_task_queue *q){
	pthread_mutex_lock(&q->mtx);
	while(q->size == 0 && !q->is_stopped){
		pthread_cond_wait(&q->cv, &q->mtx);
	}
	if(q->is_stopped || q->size == 0){
		pthread_mutex_unlock(&q->mtx);
		return NULL;
	}

	size_t begin = (q->begin_idx + 1) % TPOOL_MAX_TASKS;

	struct thread_task *res = q->queue[q->begin_idx];
	q->queue[q->begin_idx] = NULL;
	q->begin_idx = begin;
	--q->size;
	pthread_mutex_unlock(&q->mtx);
	return res;
}

static void
stop(struct thread_task_queue *q){
	pthread_mutex_lock(&q->mtx);
	q->is_stopped = true;
	pthread_cond_broadcast(&q->cv);
	pthread_mutex_unlock(&q->mtx);
}

static void
deinit_task_queue(struct thread_task_queue *q){
	pthread_mutex_destroy(&q->mtx);
	pthread_cond_destroy(&q->cv);
}

struct thread_pool {
	pthread_t *threads;

	/* PUT HERE OTHER MEMBERS */
	size_t max_threads;
	size_t active_threads;
	atomic_size_t waiting_threads;
	struct thread_task_queue tqueue;
};

int
thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
	if(max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) return TPOOL_ERR_INVALID_ARGUMENT;

	*pool = malloc(sizeof(struct thread_pool));
	if(!*pool) return -1;
	(*pool)->max_threads = max_thread_count;
	(*pool)->threads = calloc(max_thread_count, sizeof(pthread_t));
	if(!(*pool)->threads) return -1;
	(*pool)->waiting_threads = 0;
	(*pool)->active_threads = 0;
	if(!init_thread_task_queue(&(*pool)->tqueue)) return -1;
	return 0;
}

int
thread_pool_thread_count(const struct thread_pool *pool)
{
	return pool->active_threads;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	if (!pool) return TPOOL_ERR_INVALID_ARGUMENT;
	if(pool->active_threads != pool->waiting_threads) return TPOOL_ERR_HAS_TASKS;
	stop(&pool->tqueue);
	for(size_t i = 0; i < pool->active_threads; ++i){
		pthread_join(pool->threads[i], NULL);
	}
	deinit_task_queue(&pool->tqueue);
	free(pool->threads);
	free(pool);
	return 0;
}


struct worker_data {
    struct thread_task_queue *q;
	atomic_size_t *waiting_threads;
};

static void* worker_func(void* arg){
	struct worker_data *data = (struct worker_data *)arg;
	struct thread_task* task = pop(data->q);
	while(task){
		bool detached = false;
		pthread_mutex_lock(&task->mtx);
		atomic_fetch_sub_explicit(data->waiting_threads, 1, memory_order_relaxed);
		atomic_store_explicit(&task->status, THREAD_TASK_RUNNING, memory_order_release);
		pthread_mutex_unlock(&task->mtx);

		task->function(task->arg);

		{
			pthread_mutex_lock(&task->mtx);
			atomic_store_explicit(&task->status, THREAD_TASK_FINISHED, memory_order_release);
			atomic_fetch_add_explicit(data->waiting_threads, 1, memory_order_relaxed);
			detached = task->is_detached;
			if(!detached) {
				pthread_cond_signal(&task->cv);
			}
			pthread_mutex_unlock(&task->mtx);
		}

		if(detached) {
			deinit_thread_task(task);
			free(task);
		}
		task = pop(data->q);
	}
	free(data);
	return NULL;
}

static void
start_new_worker(struct thread_pool *pool){
	if(pool->active_threads >= pool->max_threads) return;
	pool->waiting_threads++;
	struct worker_data *arg = malloc(sizeof(struct worker_data));
	arg->q = &pool->tqueue;
	arg->waiting_threads = &pool->waiting_threads;
	assert(pthread_create(&pool->threads[pool->active_threads], NULL, &worker_func, (void *)arg) == 0);
	pool->active_threads++;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	if(pool->waiting_threads == 0 && pool->active_threads < pool->max_threads) start_new_worker(pool);
	atomic_store_explicit(&task->status, THREAD_TASK_WAITING, memory_order_release);
	if(!push(&pool->tqueue, task)){
		atomic_store_explicit(&task->status, THREAD_TASK_IDLE, memory_order_release);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}
	return 0;
}

int
thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
	*task = malloc(sizeof(struct thread_task));
	if(pthread_mutex_init(&(*task)->mtx, NULL) != 0) return -1;
	if(pthread_cond_init(&(*task)->cv, NULL) != 0) return -1;
	(*task)->function = function;
	(*task)->arg = arg;
	(*task)->is_detached = false;
	atomic_store_explicit(&(*task)->status, THREAD_TASK_IDLE, memory_order_release);
	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	return atomic_load_explicit(&task->status, memory_order_acquire) == THREAD_TASK_FINISHED;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	return atomic_load_explicit(&task->status, memory_order_acquire) == THREAD_TASK_RUNNING;
}

int
thread_task_join(struct thread_task *task, void **result)
{
	if(!task) return 0;
	if(atomic_load_explicit(&task->status, memory_order_acquire) == THREAD_TASK_IDLE) return TPOOL_ERR_TASK_NOT_PUSHED;
	pthread_mutex_lock(&task->mtx);
	if(task->is_detached) return 0;
	while(atomic_load_explicit(&task->status, memory_order_acquire) != THREAD_TASK_FINISHED){
		pthread_cond_wait(&task->cv, &task->mtx);
	}
	*result = task->arg;
	pthread_mutex_unlock(&task->mtx);
	return 0;
}

#if NEED_TIMED_JOIN

struct timespec double_to_timespec(double seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long total_ns = (long long)(seconds * 1e9 + 0.5) + 50000;
	ts.tv_sec += total_ns / 1000000000LL;
    ts.tv_nsec += total_ns % 1000000000LL;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
	if(!task) return 0;
	if(timeout < 0) return TPOOL_ERR_TIMEOUT;
	if(atomic_load_explicit(&task->status, memory_order_acquire) == THREAD_TASK_IDLE) return TPOOL_ERR_TASK_NOT_PUSHED;
	pthread_mutex_lock(&task->mtx);
	if(task->is_detached) {
		pthread_mutex_unlock(&task->mtx);
		return 0;
	}
	struct timespec t = double_to_timespec(timeout);
	int res = 0;
	while(atomic_load_explicit(&task->status, memory_order_acquire) != THREAD_TASK_FINISHED
		&& res == 0){
		res = pthread_cond_timedwait(&task->cv, &task->mtx, &t);
	}
	*result = task->arg;
	pthread_mutex_unlock(&task->mtx);
	if(res == ETIMEDOUT) return TPOOL_ERR_TIMEOUT;
	return 0;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	if(!task) return TPOOL_ERR_INVALID_ARGUMENT;
	if(atomic_load_explicit(&task->status, memory_order_acquire) != THREAD_TASK_IDLE
		&& !thread_task_is_finished(task)) return TPOOL_ERR_TASK_IN_POOL;
	if(atomic_load_explicit(&task->status, memory_order_acquire) != THREAD_TASK_IDLE){
		void *result;
		int status = thread_task_join(task, &result);
		if(status != 0) return status;
	}
	deinit_thread_task(task);
	free(task);
	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	if(!task) return 0;
	pthread_mutex_lock(&task->mtx);
	if(atomic_load_explicit(&task->status, memory_order_acquire) == THREAD_TASK_IDLE){
		pthread_mutex_unlock(&task->mtx);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	if(atomic_load_explicit(&task->status, memory_order_acquire) == THREAD_TASK_FINISHED){
		pthread_mutex_unlock(&task->mtx);
		deinit_thread_task(task);
		free(task);
		return 0;
	}
	task->is_detached = true;
	pthread_mutex_unlock(&task->mtx);
	return 0;
}

#endif
