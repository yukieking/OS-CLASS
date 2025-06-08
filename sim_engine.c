#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/queue.h>

#include "sim_engine.h"

int sim_engine_clock = 0;
int sim_engine_procs_count = 0;
sem_t sim_engine_running;

struct sim_engine_proc_cb {
	void *proc_cb_p;
	sem_t cpusem;
	pthread_t tid;
	struct sim_cpustate *cpustate_p;
	int ioready_clock;
	int cpu_maxburst;
	void (*proc_func)(void);
	TAILQ_ENTRY(sim_engine_proc_cb) proc_list;
};

/* Active process queue */
TAILQ_HEAD(sim_engine_active, sim_engine_proc_cb) sim_engine_active = TAILQ_HEAD_INITIALIZER(sim_engine_active);
/* Active process queue */
TAILQ_HEAD(sim_engine_iowait, sim_engine_proc_cb) sim_engine_iowait = TAILQ_HEAD_INITIALIZER(sim_engine_iowait);

pthread_attr_t sim_engine_tattr;
pthread_key_t sim_engine_tkey_proc_cb;

void (*sim_engine_callback_devioready)(void *);
void (*sim_engine_callback_cpurunout)(void *);
void (*sim_engine_callback_exit)(void *);

int sim_engine_init(void (*callback_devioready)(void *), void (*callback_cpurunout)(void *), void (*callback_exit)(void *))
{
	sim_engine_callback_devioready = callback_devioready;
	sim_engine_callback_cpurunout = callback_cpurunout;
	sim_engine_callback_exit = callback_exit;

	pthread_attr_init(&sim_engine_tattr);
	pthread_attr_setdetachstate(&sim_engine_tattr, PTHREAD_CREATE_DETACHED);
	pthread_key_create(&sim_engine_tkey_proc_cb, NULL);

	sem_init(&sim_engine_running, 0, 0);

	return 1;
}

void *_sim_loadproc2(void *_engine_proc_cb_p)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = _engine_proc_cb_p;
	void *proc_cb_p = engine_proc_cb_p->proc_cb_p;

	pthread_setspecific(sim_engine_tkey_proc_cb, engine_proc_cb_p);
	TAILQ_INSERT_TAIL(&sim_engine_active, engine_proc_cb_p, proc_list);

	sem_wait(&engine_proc_cb_p->cpusem);

	engine_proc_cb_p->proc_func();

	TAILQ_REMOVE(&sim_engine_active, engine_proc_cb_p, proc_list);
	sem_destroy(&engine_proc_cb_p->cpusem);
	free(engine_proc_cb_p);

	sim_engine_procs_count--;
	if(sim_engine_procs_count < 1)
		sem_post(&sim_engine_running);

	sim_engine_callback_exit(proc_cb_p);

	return NULL;
}

int sim_loadproc(void (*func)(void), struct sim_cpustate *sim_cpustate_p, void *proc_cb_p)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = malloc(sizeof(*engine_proc_cb_p));

	engine_proc_cb_p->proc_cb_p = proc_cb_p;
	engine_proc_cb_p->proc_func = func;
	sem_init(&engine_proc_cb_p->cpusem, 0, 0);

	sim_cpustate_p->cpustate_uptodate = true;
	sim_cpustate_p->state_info_dummy = engine_proc_cb_p;
	engine_proc_cb_p->cpustate_p = sim_cpustate_p;

	sem_trywait(&sim_engine_running);
	sim_engine_procs_count++;

	pthread_create(&engine_proc_cb_p->tid, NULL, _sim_loadproc2, engine_proc_cb_p);

	return 1;
}


void sim_cpustate_save(struct sim_cpustate *sim_cpustate_p)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = pthread_getspecific(sim_engine_tkey_proc_cb);

	sim_cpustate_p->cpustate_uptodate = true;
	sim_cpustate_p->state_info_dummy = engine_proc_cb_p;
	engine_proc_cb_p->cpustate_p = sim_cpustate_p;
}

void sim_cpustate_restore(struct sim_cpustate *sim_cpustate_p, int cpu_maxburst)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = pthread_getspecific(sim_engine_tkey_proc_cb);

	if (!sim_cpustate_p->cpustate_uptodate || sim_cpustate_p != ((struct sim_engine_proc_cb *)(sim_cpustate_p->state_info_dummy))->cpustate_p) {
		/* error */
		return;
	}
	sim_cpustate_p->cpustate_uptodate = false;
	((struct sim_engine_proc_cb *)(sim_cpustate_p->state_info_dummy))->cpu_maxburst = cpu_maxburst;
	sem_post(&((struct sim_engine_proc_cb *)(sim_cpustate_p->state_info_dummy))->cpusem);
	if (engine_proc_cb_p != NULL) {
		sem_wait(&engine_proc_cb_p->cpusem);
	}
}

void sim_cpuburst(int wait)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = pthread_getspecific(sim_engine_tkey_proc_cb);

	while (wait > 0) {
		struct sim_engine_proc_cb *nextioready = TAILQ_FIRST(&sim_engine_iowait);
		if (nextioready != NULL && nextioready->ioready_clock < ((engine_proc_cb_p->cpu_maxburst == 0 || wait < engine_proc_cb_p->cpu_maxburst) ? wait : engine_proc_cb_p->cpu_maxburst) + sim_engine_clock) {
			wait -= nextioready->ioready_clock - sim_engine_clock;
			if (engine_proc_cb_p->cpu_maxburst > 0)
				engine_proc_cb_p->cpu_maxburst -= nextioready->ioready_clock - sim_engine_clock;
			sim_engine_clock = nextioready->ioready_clock;

			TAILQ_REMOVE(&sim_engine_iowait, nextioready, proc_list);
			TAILQ_INSERT_TAIL(&sim_engine_active, nextioready, proc_list);

			/* call iointr */
			sim_engine_callback_devioready(nextioready->proc_cb_p);

			continue;
		}

		if (engine_proc_cb_p->cpu_maxburst > 0 && wait > engine_proc_cb_p->cpu_maxburst) {
			/* process cpu runout */
			sim_engine_clock += engine_proc_cb_p->cpu_maxburst;
			wait -= engine_proc_cb_p->cpu_maxburst;
			engine_proc_cb_p->cpu_maxburst = 0;

			/* call cpurunout intr */
			sim_engine_callback_cpurunout(engine_proc_cb_p->proc_cb_p);
		} else {
			sim_engine_clock += wait;
			if (engine_proc_cb_p->cpu_maxburst > 0)
				engine_proc_cb_p->cpu_maxburst -= wait;
			wait = 0;
		}
	}
}

void sim_deviorequest(int wait)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = pthread_getspecific(sim_engine_tkey_proc_cb);
	struct sim_engine_proc_cb *ent;

	TAILQ_REMOVE(&sim_engine_active, engine_proc_cb_p, proc_list);
	engine_proc_cb_p->ioready_clock = sim_engine_clock + wait;

	ent = TAILQ_FIRST(&sim_engine_iowait);
	while (ent != NULL) {
		if (ent->ioready_clock > engine_proc_cb_p->ioready_clock)
			break;
		ent = TAILQ_NEXT(ent, proc_list);
	}
	if (ent != NULL)
		TAILQ_INSERT_BEFORE(ent, engine_proc_cb_p, proc_list);
	else
		TAILQ_INSERT_TAIL(&sim_engine_iowait, engine_proc_cb_p, proc_list);
}

void sim_wait_nextintr(void)
{
	struct sim_engine_proc_cb *engine_proc_cb_p = pthread_getspecific(sim_engine_tkey_proc_cb);
	struct sim_engine_proc_cb *nextioready = TAILQ_FIRST(&sim_engine_iowait);

	if (nextioready == NULL)
		return;

	sim_engine_clock = nextioready->ioready_clock;

	TAILQ_REMOVE(&sim_engine_iowait, nextioready, proc_list);
	TAILQ_INSERT_TAIL(&sim_engine_active, nextioready, proc_list);

	/* call iointr */
	sim_engine_callback_devioready(nextioready->proc_cb_p);
}

int sim_engine_getclock(void)
{
	return sim_engine_clock;
}

void sim_engine_wait_allfinish(void)
{
	sem_wait(&sim_engine_running);
}
