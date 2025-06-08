// 文件名: sim_sched_advanced.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h> // For srand

#include "sim_engine.h"

#define SIM_MAXPROCS 100
#define SIM_CPUMAXBURST 100 // Time slice for preemption

// 定义进程优先级 (数值越小，优先级越高)
#define PRIORITY_HIGH 1
#define PRIORITY_NORMAL 2
#define PRIORITY_LOW 3

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
    int priority; // 新增：进程优先级
    int creation_time; // 新增：进程创建时间，用于计算周转时间

    TAILQ_ENTRY(sim_proc) proc_list;
} procs[SIM_MAXPROCS];
int nextpid = 1;

/* Active Process */
struct sim_proc *activeproc = NULL;
/* Processes Queue for READY procs */
TAILQ_HEAD(ready_queue, sim_proc) ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue);
/* Processes Queue for BLOCKED procs */
TAILQ_HEAD(blocked_queue, sim_proc) blocked_queue = TAILQ_HEAD_INITIALIZER(blocked_queue);

// 函数声明 (如果 sim_logging 定义在后面)
void sim_logging(struct sim_proc *proc_p, const char *msg);

void sched(void) {
    struct sim_proc *p, *highest_priority_proc = NULL;
    int min_priority = PRIORITY_LOW + 1; // Initialize with a value lower than any possible priority

    /* 1. 如果当前有活动进程，保存其状态并放回就绪队列尾部 */
    if (activeproc != NULL) {
        sim_cpustate_save(&activeproc->proc_cpustate);
        TAILQ_INSERT_TAIL(&ready_queue, activeproc, proc_list);
        activeproc->proc_state = READY;
        sim_logging(activeproc, "[Trace] State change RUNNING->READY (scheduler called)");
        activeproc = NULL;
    }

    /* 2. 从就绪队列中挑选一个新进程 (实现优先级调度) */
    // 遍历就绪队列，找到优先级最高的进程 (priority值最小)
    TAILQ_FOREACH(p, &ready_queue, proc_list) {
        if (p->priority < min_priority) {
            min_priority = p->priority;
            highest_priority_proc = p;
        }
    }
    
    activeproc = highest_priority_proc; // 将找到的最高优先级进程设为活动进程

    if (activeproc != NULL) {
        TAILQ_REMOVE(&ready_queue, activeproc, proc_list); // 从就绪队列中移除
        activeproc->proc_state = RUNNING;
        sim_logging(activeproc, "[Trace] State change READY->RUNNING");
        sim_cpustate_restore(&activeproc->proc_cpustate, SIM_CPUMAXBURST);
    } else {
        sim_logging(NULL, "[Trace] No active process, waiting for next interrupt");
        sim_wait_nextintr();
    }
}

// 修改 sim_createproc 以接受优先级参数
int sim_createproc(void (*func)(void), int priority) {
    int i;

    for (i = 0; i < SIM_MAXPROCS; i++) {
        if (procs[i].proc_state == NOEXIST)
            break;
    }
    if (i >= SIM_MAXPROCS)
        return 0; 

    procs[i].proc_pid = nextpid++;
    procs[i].priority = priority; // 设置优先级
    procs[i].creation_time = sim_engine_getclock(); // 记录创建时间
    
    sim_loadproc(func, &procs[i].proc_cpustate, &procs[i]);
    procs[i].proc_state = READY;
    TAILQ_INSERT_TAIL(&ready_queue, &procs[i], proc_list); // 插入就绪队列尾部
    
    char log_msg[100];
    sprintf(log_msg, "Created as state READY with priority %d", priority);
    sim_logging(&procs[i], log_msg);

    return procs[i].proc_pid; 
}

int sim_iorequest(int iowait) {
    if (activeproc == NULL) { 
        sim_logging(NULL, "[Error] I/O request from non-active process context!");
        return 0;
    }
    sim_deviorequest(iowait); 

    sim_cpustate_save(&activeproc->proc_cpustate);
    TAILQ_INSERT_TAIL(&blocked_queue, activeproc, proc_list);
    activeproc->proc_state = BLOCKED;
    sim_logging(activeproc, "[Trace] State change RUNNING->BLOCKED (I/O request)");
    
    activeproc = NULL; 
    sched();
    return 1;
}

void sim_intr_devioready(void *_proc_p) {
    struct sim_proc *proc_p = _proc_p;

    if (proc_p == NULL || proc_p->proc_state == NOEXIST) {
        char log_buf[128];
        sprintf(log_buf, "[Trace] I/O ready for an already exited/invalid process (PID if available: %d)", proc_p ? proc_p->proc_pid : -1);
        sim_logging(NULL, log_buf); // Use NULL if proc_p is truly invalid
        return;
    }
    
    if (proc_p->proc_state != BLOCKED) {
        sim_logging(proc_p, "[Warning] I/O ready for a process not in BLOCKED state!");
    }

    TAILQ_REMOVE(&blocked_queue, proc_p, proc_list); 
    proc_p->proc_state = READY;
    TAILQ_INSERT_TAIL(&ready_queue, proc_p, proc_list); // I/O完成的进程回到就绪队列尾部
    sim_logging(proc_p, "[Trace] State change BLOCKED->READY (I/O ready interrupt)");

    // 考虑抢占：如果当前没有活动进程，或者新就绪的进程优先级高于当前活动进程
    // 为简化，这里我们仅在CPU空闲时或由时间片中断调用sched()。
    // 一个更积极的优先级抢占会在新高优先级进程就绪时立即调用sched。
    // （或者，可以在sched()中实现这种抢占逻辑：如果activeproc存在且优先级低于新来的，则抢占）
    // 当前的sched()总会把activeproc放回队列再选，所以当因其他原因调用sched时，优先级会起作用。
    // 如果希望I/O完成时能立即抢占低优先级当前进程，需要更复杂的逻辑或总是调用sched()。
    // 简单的处理：如果CPU空闲，则调度
    if (activeproc == NULL) {
        sched();
    } 
    // 可选的更积极抢占: 如果新就绪的进程优先级更高
    /* else if (proc_p->priority < activeproc->priority) {
        sim_logging(activeproc, "[Trace] High priority process became ready, attempting preemption");
        sched(); // 强制调度，可能会抢占当前activeproc
    }
    */
}

void sim_intr_cpurunout(void *_proc_p) {
    struct sim_proc *proc_p = (struct sim_proc *)_proc_p;
    if (proc_p == activeproc && proc_p != NULL) { // 确保是当前活动进程的时间片用完
        sim_logging(proc_p, "[Trace] CPU time slice expired (CPU runout interrupt)");
        sched(); // 调用调度器重新选择进程
    } else {
        // 可能是一个延迟的中断，或者activeproc已经被改变
        sim_logging(proc_p, "[Warning] CPU runout for non-active or changed process!");
    }
}

void sim_intr_procexit(void *_proc_p) {
    struct sim_proc *proc_p = _proc_p;
    int turnaround_time = sim_engine_getclock() - proc_p->creation_time;
    
    char log_msg[128];
    sprintf(log_msg, "Terminated. Turnaround Time: %d.%03ds", turnaround_time / 1000, turnaround_time % 1000);
    sim_logging(proc_p, log_msg);

    if (activeproc == proc_p) {
        activeproc = NULL;
    }
    // 如果它在其他队列中（理论上不应该，因为是运行后退出的），也应该移除。
    // 但在此模拟中，它应该是activeproc，或者已经被移出。
    
    proc_p->proc_state = NOEXIST; 
    // 不能立即memset，因为proc_p可能在sim_engine的proc_list中还被引用直到线程结束。
    // engine 的 _sim_loadproc2 中会free(engine_proc_cb_p)，
    // 而 engine_proc_cb_p->proc_cb_p 就是这里的 proc_p。
    // memset(proc_p, 0, sizeof(*proc_p)); // 暂时注释掉以防万一，NOEXIST状态是关键

    sched();
}

void sim_logging(struct sim_proc *proc_p, const char *msg) {
    int clock = sim_engine_getclock();
    if (proc_p != NULL && proc_p->proc_state != NOEXIST) {
        printf("%d.%03d Process#%d(Prio%d) %s\n", clock / 1000, clock % 1000, proc_p->proc_pid, proc_p->priority, msg);
    } else if (proc_p == NULL && msg != NULL) { 
        printf("%d.%03d Scheduler %s\n", clock / 1000, clock % 1000, msg);
    } else if (proc_p != NULL && proc_p->proc_state == NOEXIST && msg != NULL) { // 处理已标记为NOEXIST但仍想记录PID的情况
        printf("%d.%03d Process#%d(Prio%d) %s\n", clock / 1000, clock % 1000, proc_p->proc_pid, proc_p->priority, msg);
    }
     else { 
         printf("%d.%03d System %s\n", clock / 1000, clock % 1000, (msg ? msg : "Unknown event"));
    }
}

/* --- 新增的更实际的进程行为函数 --- */

void sim_proc_data_processing(void) {
    int i;
    sim_logging(activeproc, "[App] Data Processing Task: Starting");

    sim_logging(activeproc, "[App] Data Processing: Loading initial data (I/O 150 units)");
    sim_iorequest(150); 

    sim_logging(activeproc, "[App] Data Processing: Performing intensive calculations (CPU 800 units)");
    sim_cpuburst(800); 

    for (i = 0; i < 2; i++) {
        sim_logging(activeproc, "[App] Data Processing: Storing intermediate results (I/O 50 units)");
        sim_iorequest(50);
        int random_cpu_burst = (rand() % 100) + 50; 
        char burst_msg[60];
        sprintf(burst_msg, "[App] Data Processing: Quick processing (%d CPU units)", random_cpu_burst);
        sim_logging(activeproc, burst_msg);
        sim_cpuburst(random_cpu_burst);
        sim_logging(activeproc, "[App] Data Processing: Loading more data (I/O 70 units)");
        sim_iorequest(70);
    }

    sim_logging(activeproc, "[App] Data Processing: Finalizing calculations (CPU 400 units)");
    sim_cpuburst(400);

    sim_logging(activeproc, "[App] Data Processing: Saving final report (I/O 100 units)");
    sim_iorequest(100);

    sim_logging(activeproc, "[App] Data Processing Task: Finished");
}

void sim_proc_interactive(void) {
    int i;
    sim_logging(activeproc, "[App] Interactive Process: Started");
    for (i = 0; i < 5; i++) { // 假设有5轮交互
        int user_think_time = (rand() % 200) + 50; 
        int short_cpu_burst = (rand() % 20) + 5;   
        char log_msg_io[80], log_msg_cpu[80];

        sprintf(log_msg_io, "[App] Interactive: Waiting for user input (%d I/O units)", user_think_time);
        sim_logging(activeproc, log_msg_io);
        sim_iorequest(user_think_time); 
        
        sprintf(log_msg_cpu, "[App] Interactive: Processing input (%d CPU units)", short_cpu_burst);
        sim_logging(activeproc, log_msg_cpu);
        sim_cpuburst(short_cpu_burst);  
    }
    sim_logging(activeproc, "[App] Interactive Process: Session ended");
}

// 原始的进程行为函数
void sim_proc_cpubound(void) {
    int i;
    sim_logging(activeproc, "[App] Standard CPU-Bound Task: Starting");
    for (i = 0; i < 2; i++) { // 减少循环次数以更快看到混合效果
        sim_logging(activeproc, "[App] Standard CPU-Bound: Requesting I/O (10 units)");
        sim_iorequest(10); 
        sim_logging(activeproc, "[App] Standard CPU-Bound: Starting CPU burst (1000 units)");
        sim_cpuburst(1000);   
    }
    sim_logging(activeproc, "[App] Standard CPU-Bound Task: Finished");
}

void sim_proc_iobound(void) {
    int i;
    sim_logging(activeproc, "[App] Standard I/O-Bound Task: Starting");
    for (i = 0; i < 3; i++) { // 减少循环次数
        sim_logging(activeproc, "[App] Standard I/O-Bound: Requesting I/O (100 units)");
        sim_iorequest(100);  
        sim_logging(activeproc, "[App] Standard I/O-Bound: Starting CPU burst (10 units)");
        sim_cpuburst(10);    
    }
    sim_logging(activeproc, "[App] Standard I/O-Bound Task: Finished");
}


int main(int argc, char **argv) {
    int i;

    srand(time(NULL)); // 初始化随机数种子，为 interactive 和 data_processing 进程

    sim_engine_init(sim_intr_devioready, sim_intr_cpurunout, sim_intr_procexit);
    // memset(procs, 0, sizeof(struct sim_proc) * SIM_MAXPROCS); // proc_state=NOEXIST 已经是0

    sim_logging(NULL, "System Initialized. Creating processes...");

    // 创建不同类型的进程和不同优先级
    sim_createproc(sim_proc_interactive, PRIORITY_HIGH);      // 交互式进程，高优先级
    sim_createproc(sim_proc_data_processing, PRIORITY_NORMAL); // 数据处理进程，普通优先级
    sim_createproc(sim_proc_cpubound, PRIORITY_LOW);         // CPU密集型，低优先级
    
    for (i = 0; i < 2; i++) { // 创建几个I/O密集型进程
        sim_createproc(sim_proc_iobound, PRIORITY_NORMAL); // I/O密集型，普通优先级
    }

    sim_logging(NULL, "All processes created. Starting scheduler.");
    sched(); 

    sim_engine_wait_allfinish(); 

    sim_logging(NULL, "All processes terminated. Simulation finished.");

    return 0;
}