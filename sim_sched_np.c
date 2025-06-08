#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/queue.h>

#include "sim_engine.h"

#define SIM_MAXPROCS 100
// 修改 SIM_CPUMAXBURST 以启用抢占式调度，例如设置为100个时间单位
#define SIM_CPUMAXBURST 0

enum sim_proc_state {
    NOEXIST = 0,
    READY,
    RUNNING,
    BLOCKED
};

struct sim_proc {
    int proc_pid;
    enum sim_proc_state proc_state;
    struct sim_cpustate proc_cpustate;

    TAILQ_ENTRY(sim_proc) proc_list;
} procs[SIM_MAXPROCS];
int nextpid = 1;

/* Active Process */
struct sim_proc *activeproc = NULL;
/* Processes Queue for READY procs */
TAILQ_HEAD(ready_queue, sim_proc) ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue);
/* Processes Queue for BLOCKED procs */
TAILQ_HEAD(blocked_queue, sim_proc) blocked_queue = TAILQ_HEAD_INITIALIZER(blocked_queue);

extern void sim_logging(struct sim_proc *proc_p, char *msg);

void sched(void)
{
    /* save active process state */
    if (activeproc != NULL) {
        sim_cpustate_save(&activeproc->proc_cpustate);
        TAILQ_INSERT_TAIL(&ready_queue, activeproc, proc_list);
        activeproc->proc_state = READY;
        sim_logging(activeproc, "[Trace] State change RUNNING->READY (scheduler called)"); // 更明确的日志信息
        activeproc = NULL;
    }

    /* pickup a new proc */
    activeproc = TAILQ_FIRST(&ready_queue);
    if (activeproc != NULL) {
        TAILQ_REMOVE(&ready_queue, activeproc, proc_list);
        activeproc->proc_state = RUNNING;
        sim_logging(activeproc, "[Trace] State change READY->RUNNING");
        // SIM_CPUMAXBURST 现在是一个正值，会传递给引擎用于时间片控制
        sim_cpustate_restore(&activeproc->proc_cpustate, SIM_CPUMAXBURST);
    } else {
        sim_logging(NULL, "[Trace] No active process, waiting for next interrupt"); // NULL proc_p for logging
        sim_wait_nextintr();
    }
}

int sim_createproc(void (*func)(void))
{
    int i;

    for (i = 0; i < SIM_MAXPROCS; i++) {
        if (procs[i].proc_state == NOEXIST)
            break;
    }
    if (i >= SIM_MAXPROCS)
        return 0; // No free process slot

    procs[i].proc_pid = nextpid++;
    // sim_loadproc will call the process function in a new thread
    sim_loadproc(func, &procs[i].proc_cpustate, &procs[i]);
    procs[i].proc_state = READY;
    TAILQ_INSERT_TAIL(&ready_queue, &procs[i], proc_list);
    sim_logging(&procs[i], "Created as state READY");

    return procs[i].proc_pid; // Return pid or some identifier
}

int sim_iorequest(int iowait)
{
    if (activeproc == NULL) { // Should not happen if logic is correct
        sim_logging(NULL, "[Error] I/O request from non-active process context!");
        return 0;
    }
    /* send request to device */
    sim_deviorequest(iowait); // This function is provided by sim_engine

    /* change state to BLOCKED */
    sim_cpustate_save(&activeproc->proc_cpustate);
    TAILQ_INSERT_TAIL(&blocked_queue, activeproc, proc_list);
    activeproc->proc_state = BLOCKED;
    sim_logging(activeproc, "[Trace] State change RUNNING->BLOCKED (I/O request)");
    
    struct sim_proc *old_active = activeproc; // Store temporarily for logging if needed
    activeproc = NULL; // Set current active to NULL before calling scheduler

    /* call scheduler */
    sched();

    return 1;
}

void sim_intr_devioready(void *_proc_p)
{
    struct sim_proc *proc_p = _proc_p;

    if (proc_p == NULL || proc_p->proc_state == NOEXIST) {
         // It's possible proc_p points to a cleared structure if exit happened almost concurrently
        printf("%d.%03d [Trace] I/O ready for an already exited/invalid process?\n", sim_engine_getclock()/1000, sim_engine_getclock()%1000);
        return;
    }
    
    if (proc_p->proc_state != BLOCKED) {
        sim_logging(proc_p, "[Warning] I/O ready for a process not in BLOCKED state!");
        // Decide how to handle, for now, we'll proceed to move it to ready
    }


    /* move this process to ready queue */
    TAILQ_REMOVE(&blocked_queue, proc_p, proc_list); // Ensure it's actually in blocked_queue (might need error check)
    proc_p->proc_state = READY;
    TAILQ_INSERT_TAIL(&ready_queue, proc_p, proc_list);
    sim_logging(proc_p, "[Trace] State change BLOCKED->READY (I/O ready interrupt)");

    /* call scheduler if no active proc OR if new ready process could preempt (for priority, not current FCFS/RR) */
    // For basic Round Robin, only schedule if CPU is idle.
    // For a more advanced preemptive scheduler (e.g. if I/O completion makes a higher priority task ready),
    // one might always call sched() and let it decide.
    if (activeproc == NULL) {
        sched();
    }
}

void sim_intr_cpurunout(void *_proc_p)
{
    struct sim_proc *proc_p = _proc_p;

    if (proc_p == NULL || proc_p->proc_state != RUNNING) {
        sim_logging(proc_p, "[Warning] CPU runout for a non-running or NULL process!");
        // This might indicate a logic issue or a race if not handled carefully.
        // For now, we proceed to call sched() which should handle NULL activeproc if proc_p was activeproc.
    } else {
        sim_logging(proc_p, "[Trace] CPU time slice expired (CPU runout interrupt)");
    }
    
    /* call scheduler */
    // The `sched` function will handle saving the state of `activeproc` (which should be `proc_p`)
    // and moving it to the ready queue.
    sched();
}

void sim_intr_procexit(void *_proc_p)
{
    struct sim_proc *proc_p = _proc_p;

    sim_logging(proc_p, "Terminated");

    /* clear process cb */
    // Ensure this proc_p is indeed the active one or handle appropriately
    if (activeproc == proc_p) {
        activeproc = NULL;
    } else {
        // This case might mean the process exited while not being 'activeproc'
        // (e.g. if it was in ready or blocked queue and an error caused exit, though sim_engine calls this for the thread ending)
        // Or, activeproc might have been set to NULL by some other path just before this.
        // Need to ensure it's safely removed from any queue it might be in.
        // Given current engine design, exit callback is for the process whose thread is ending.
        // If it was in ready or blocked queue, it needs removal.
        // However, sim_engine's _sim_loadproc2 removes from active list itself.
        // This scheduler's queues (ready_queue, blocked_queue) are separate.
        // A robust system would check and remove from these queues if present.
        // For now, assume it was active or already handled by engine's own active list.
    }
    
    // Mark as NOEXIST and clear. Important to do before sched() might try to pick it.
    proc_p->proc_state = NOEXIST; 
    memset(proc_p, 0, sizeof(*proc_p)); // Clears pid, state, cpustate, and list pointers.

    /* call scheduler */
    sched();
}

// Modified sim_logging to handle NULL proc_p for system messages
void sim_logging(struct sim_proc *proc_p, char *msg)
{
    int clock = sim_engine_getclock();
    if (proc_p != NULL && proc_p->proc_state != NOEXIST) { // Check if proc_p is valid
        printf("%d.%03d Process#%d %s\n", clock / 1000, clock % 1000, proc_p->proc_pid, msg);
    } else if (proc_p == NULL && msg != NULL) { // For general scheduler messages not tied to a specific proc
        printf("%d.%03d Scheduler %s\n", clock / 1000, clock % 1000, msg);
    } else { // Fallback for other odd cases
         printf("%d.%03d System %s\n", clock / 1000, clock % 1000, (msg ? msg : "Unknown event"));
    }
}


/* ---
 * simulation execution code
 */

// Process behavior functions remain the same
void sim_proc_cpubound(void)
{
    int i;
    // A CPU-bound process simulation that also does some I/O
    for (i = 0; i < 3; i++) { // Reduced loops for quicker testing if needed
        sim_logging(activeproc, "[App] Requesting I/O (10 units)");
        sim_iorequest(10); // Simulate some I/O
        sim_logging(activeproc, "[App] Starting CPU burst (1000 units)");
        sim_cpuburst(1000);   // Simulate a long CPU burst
    }
    sim_logging(activeproc, "[App] CPU-bound task finished");
}

void sim_proc_iobound(void)
{
    int i;
    // An I/O-bound process simulation
    for (i = 0; i < 5; i++) { // Reduced loops for quicker testing if needed
        sim_logging(activeproc, "[App] Requesting I/O (100 units)");
        sim_iorequest(100);  // Simulate a longer I/O operation
        sim_logging(activeproc, "[App] Starting CPU burst (10 units)");
        sim_cpuburst(10);    // Simulate a short CPU burst
    }
    sim_logging(activeproc, "[App] I/O-bound task finished");
}

int main(int argc, char **argv)
{
    int i;

    sim_engine_init(sim_intr_devioready, sim_intr_cpurunout, sim_intr_procexit);
    memset(procs, 0, sizeof(struct sim_proc) * SIM_MAXPROCS); // Corrected sizeof usage

    sim_logging(NULL, "System Initialized. Creating processes...");

    sim_createproc(sim_proc_cpubound);
    for (i = 0; i < 5; i++) {
        sim_createproc(sim_proc_iobound);
    }

    sim_logging(NULL, "All processes created. Starting scheduler.");
    sched(); // Start the scheduling process

    sim_engine_wait_allfinish(); // Wait for all pthreads in sim_engine to complete

    sim_logging(NULL, "All processes terminated. Simulation finished.");

    return 0;
}