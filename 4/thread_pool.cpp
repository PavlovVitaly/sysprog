#include "thread_pool.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <utility>
#include <condition_variable>
#include <assert.h>

enum thread_task_status : int{
	THREAD_TASK_IDLE = -1,
	THREAD_TASK_WAITING = 0,
	THREAD_TASK_RUNNING = 1,
	THREAD_TASK_FINISHED = 2
};

struct thread_task {
	thread_task_f function;

	/* PUT HERE OTHER MEMBERS */
	std::atomic_int status{THREAD_TASK_IDLE};
	std::mutex mtx;
	std::condition_variable cv;
	bool is_detached;
};

struct thread_task_queue{
	std::mutex mtx;
	std::condition_variable cv;
	std::array<thread_task *, TPOOL_MAX_TASKS> queue{};
	size_t begin_idx{};
	size_t end_idx{};
	size_t size{};
	bool is_stopped{false};
};

static bool
push(thread_task_queue *q, thread_task *t){
	{
		std::lock_guard l(q->mtx);
		if (q->size == q->queue.size()) {
			return false;
		}

		q->queue[q->end_idx] = t;
		q->end_idx = (q->end_idx + 1) % q->queue.size();
		++q->size;
	}
	q->cv.notify_one();
	return true;
}

static thread_task
*pop(thread_task_queue *q){
	std::unique_lock l(q->mtx);
	q->cv.wait(l, [q]{ return q->size != 0 || q->is_stopped; });
	if(q->is_stopped || q->size == 0) return nullptr;

	size_t begin{(q->begin_idx + 1) % q->queue.size()};

	thread_task *res{std::exchange(q->queue[q->begin_idx], nullptr)};
	q->begin_idx = begin;
	--q->size;
	return res;
}

static void
stop(thread_task_queue *q){
	{
		std::unique_lock l(q->mtx);
		q->is_stopped = true;
	}
	q->cv.notify_all();
}

struct thread_pool {
	std::vector<pthread_t> threads;

	/* PUT HERE OTHER MEMBERS */
	size_t max_threads{};
	size_t active_threads{};
	std::atomic_size_t waiting_threads{};
	thread_task_queue tqueue{};
};

int
thread_pool_new(int thread_count, struct thread_pool **pool)
{
	if(thread_count <= 0 || thread_count > TPOOL_MAX_THREADS) return TPOOL_ERR_INVALID_ARGUMENT;

	*pool = new thread_pool();
	(*pool)->max_threads = thread_count;
	(*pool)->threads.reserve(thread_count);
	return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	if (!pool) return TPOOL_ERR_INVALID_ARGUMENT;
	if(pool->active_threads != pool->waiting_threads) return TPOOL_ERR_HAS_TASKS;
	stop(&pool->tqueue);
	for(size_t i = 0; i < pool->threads.size(); ++i){
		pthread_join(pool->threads[i], nullptr);
	}
	delete pool;
	return 0;
}

struct worker_data {
    thread_task_queue* q;
	std::atomic_size_t* waiting_threads;
};

static void* worker_func(void* arg){
	worker_data *data = static_cast<worker_data *>(arg);
	while(auto* task = pop(data->q)){
		bool detached = false;
		{
			std::lock_guard l(task->mtx);
			--(*data->waiting_threads);
			task->status.store(THREAD_TASK_RUNNING, std::memory_order_release);
		}
		
		task->function();

		{
			std::lock_guard l(task->mtx);
			task->status.store(THREAD_TASK_FINISHED, std::memory_order_release);
			++(*data->waiting_threads);
			detached = task->is_detached;
			if(!detached) {
				task->cv.notify_one();
			}
		}

		if(detached) {
			delete task;
		}
	}
	delete data;
	return nullptr;
}

static void
start_new_worker(struct thread_pool *pool){
	if(pool->active_threads >= pool->max_threads) return;
	pool->waiting_threads++;
	worker_data *arg = new worker_data{&pool->tqueue, &pool->waiting_threads};
	pool->threads.emplace_back();
	assert(pthread_create(&pool->threads.back(), NULL, &worker_func, static_cast<void *>(arg)) == 0);
	pool->active_threads++;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	if(pool->waiting_threads == 0 && pool->active_threads < pool->max_threads) start_new_worker(pool);
	task->status.store(THREAD_TASK_WAITING, std::memory_order_release);
	if(!push(&pool->tqueue, task)){
		task->status.store(THREAD_TASK_IDLE, std::memory_order_release);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}
	return 0;
}

int
thread_task_new(struct thread_task **task, const thread_task_f &function)
{
	*task = new thread_task();
	(*task)->function = function;
	(*task)->status.store(THREAD_TASK_IDLE, std::memory_order_release);
	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	return task->status.load(std::memory_order_acquire) == THREAD_TASK_FINISHED;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	return task->status.load(std::memory_order_acquire) == THREAD_TASK_RUNNING;
}

int
thread_task_join(struct thread_task *task)
{
	if(!task) return 0;
	if(task->status.load(std::memory_order_acquire) == THREAD_TASK_IDLE) return TPOOL_ERR_TASK_NOT_PUSHED;
	std::unique_lock l(task->mtx);
	if(task->is_detached) return 0;
	task->cv.wait(l, [task](){return task->status.load(std::memory_order_acquire) == THREAD_TASK_FINISHED;});
	return 0;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout)
{
	if(!task) return 0;
	if(task->status.load(std::memory_order_acquire) == THREAD_TASK_IDLE) return TPOOL_ERR_TASK_NOT_PUSHED;
	std::unique_lock l(task->mtx);
	if(task->is_detached) return 0;
	auto res = task->cv.wait_for(l
		, std::chrono::duration<double>(timeout)
		,[task](){return task->status.load(std::memory_order_acquire) == THREAD_TASK_FINISHED;});
	if(!res) return TPOOL_ERR_TIMEOUT;
	return 0;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	if(task->status.load(std::memory_order_acquire) != THREAD_TASK_IDLE && !thread_task_is_finished(task)) return TPOOL_ERR_TASK_IN_POOL;
	if(task->status.load(std::memory_order_acquire) != THREAD_TASK_IDLE){
		if(auto status = thread_task_join(task); status != 0) return status;
	}
	
	delete task;
	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	if(!task) return 0;
	std::unique_lock l(task->mtx);
	if(task->status.load(std::memory_order_acquire) == THREAD_TASK_IDLE){
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	if(task->status.load(std::memory_order_acquire) == THREAD_TASK_FINISHED){
		l.unlock();
		delete task;
		return 0;
	}
	task->is_detached = true;
	return 0;
}

#endif
