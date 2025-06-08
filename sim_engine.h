#include <stdbool.h>



struct sim_cpustate {
	bool cpustate_uptodate;
	void *state_info_dummy;
};

extern int sim_engine_init(void (*callback_devioready)(void *), void (*callback_cpurunout)(void *), void (*callback_exit)(void *));
extern int sim_loadproc(void (*func)(void), struct sim_cpustate *sim_cpustate_p, void *proc_cb_p);
extern void sim_cpustate_save(struct sim_cpustate *sim_cpustate_p);
extern void sim_cpustate_restore(struct sim_cpustate *sim_cpustate_p, int cpu_maxburst);
extern void sim_cpuburst(int time);
extern void sim_deviorequest(int time);
extern void sim_wait_nextintr(void);
extern int sim_engine_getclock(void);
extern void sim_engine_wait_allfinish(void);
