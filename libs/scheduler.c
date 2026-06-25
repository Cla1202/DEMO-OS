#include "scheduler.h"
#include "../drivers/irq/controller.h"
#include "mm.h"
#include "fork.h"

// Initialization of the Init process (Idle Task) 
static struct PCB init_process = INIT_PROCESS;
struct PCB *current_process = &init_process;

// Global process table (Kept for PID lookups and exit/wait management)
struct PCB *processes[N_PROCESSES] = { &init_process };
int n_processes = 1; // n_processes = 0 ==> &init_process

// =========================================================================
// DATA STRUCTURES FOR QUEUES (THEORY)
// =========================================================================
typedef struct {
    struct PCB* head;
    struct PCB* tail;
} ProcessQueue;

#define QUEUE_INIT {NULL, NULL}

// Queues defined for the various algorithms
static ProcessQueue ready_queue = QUEUE_INIT;                              // Used by FCFS, RR, SJF, LJF
static ProcessQueue mlq_queues[2] = {QUEUE_INIT, QUEUE_INIT};              // Used by MLQ (0=Foreground, 1=Background)
static ProcessQueue mlfq_queues[3] = {QUEUE_INIT, QUEUE_INIT, QUEUE_INIT}; // Used by MLFQ
static int queue_level[N_PROCESSES] = {0};                                 // Tracks the levels for MLFQ

// =========================================================================
// QUEUE OPERATIONS (O(1) Complexity)
// =========================================================================
void enqueue_process_to(ProcessQueue* queue, struct PCB* process) {
    if (!process || process == &init_process) return;
    process->next_ready = NULL;
    
    if (queue->tail == NULL) {
        queue->head = process;
        queue->tail = process;
    } else {
        queue->tail->next_ready = process;
        queue->tail = process;
    }
}

struct PCB* dequeue_process_from(ProcessQueue* queue) {
    if (queue->head == NULL) return NULL;
    
    struct PCB* process = queue->head;
    queue->head = process->next_ready;
    
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    
    process->next_ready = NULL;
    return process;
}

// Sorted insertion (O(N) only on insertion, used for SJF and LJF)
void enqueue_sorted_to(ProcessQueue* queue, struct PCB* process, int ascending) {
    if (!process || process == &init_process) return;
    process->next_ready = NULL;

    int goes_first = ascending ? (process->priority < queue->head->priority) 
                               : (process->priority > queue->head->priority);

    if (queue->head == NULL || goes_first) {
        process->next_ready = queue->head;
        queue->head = process;
        if (queue->tail == NULL) queue->tail = process;
        return;
    }

    struct PCB* current = queue->head;
    int condition;
    while (current->next_ready != NULL) {
        condition = ascending ? (process->priority >= current->next_ready->priority) 
                              : (process->priority <= current->next_ready->priority);
        if (!condition) break;
        current = current->next_ready;
    }

    process->next_ready = current->next_ready;
    current->next_ready = process;
    if (process->next_ready == NULL) {
        queue->tail = process;
    }
}

// =========================================================================
// INSERTION ROUTER (Called by fork.c, syscalls, and signal handlers)
// =========================================================================
void enqueue_process(struct PCB* process) {
    if (!process || process == &init_process) return;

    // UNCOMMENT THE ONE RELATED TO THE ALGORITHM YOU ARE TESTING IN _schedule()
    
    // --- Standard algorithms: Round Robin, FCFS ---
     enqueue_process_to(&ready_queue, process);

    // --- SJF (Shortest Job First) ---
    // enqueue_sorted_to(&ready_queue, process, 1);

    // --- LJF (Longest Job First) ---
    // enqueue_sorted_to(&ready_queue, process, 0);

    // --- MLQ (Multilevel Queue) ---
    /*
    if (process->pid % 2 != 0) enqueue_process_to(&mlq_queues[0], process);
    else enqueue_process_to(&mlq_queues[1], process);
    */

    // --- MLFQ (Multilevel Feedback Queue) - Active by default for the Demo ---
    //enqueue_process_to(&mlfq_queues[queue_level[process->pid]], process);
}

// =========================================================================
// SYSTEM MANAGEMENT FUNCTIONS
// =========================================================================
int add_process_to_scheduler(struct PCB* process) {
    if(n_processes >= N_PROCESSES || process->pid >= N_PROCESSES) return -1;
    
    processes[process->pid] = process;
    queue_level[process->pid] = 0; // MLFQ Rule: Everyone starts at the top (Queue 0)

    // If it is in the Running state, it is enqueued via the router function
    if (process->state == PROCESS_RUNNING) {
        enqueue_process(process);
    }
    
    n_processes++;
    return 0;
}

void preempt_enable() { current_process->preempt_disabled--; }
void preempt_disable() { current_process->preempt_disabled++; }

void handle_process_signals(struct PCB* process);
void switch_to_process(struct PCB *next_process);
extern void cpu_switch_to_process(struct PCB *prev, struct PCB *next);


// Finds the new process to assign the CPU and switches the context to it
void _schedule_priority_aging() {
  preempt_disable();
  long max_counter, next_process_index;

  while (1) {
    max_counter = 0;
    next_process_index = 0;
    for (int i = 0; i < N_PROCESSES; i++) {
      if (processes[i]) {
        handle_process_signals(processes[i]);
        if (processes[i]->state == PROCESS_RUNNING && processes[i]->counter > max_counter) {
          max_counter = processes[i]->counter;
          next_process_index = i;
        }
      }
    }

    if (max_counter > 0) {
      break;
    }

    // If I didn't find any process, I increment the counter of each one
    for (int i = 0; i < N_PROCESSES; i++) {
      if (processes[i]) {
        processes[i]->counter = (processes[i]->counter >> 1) + processes[i]->priority;
      }
    }
  }

  struct PCB* next_process = processes[next_process_index];
  handle_process_signals(next_process);
  
  // I check again the process state because signals can change it
  if (next_process->state == PROCESS_RUNNING) {
    switch_to_process(next_process);
  }
  preempt_enable();
}
// =========================================================================
// ALGORITHM IMPLEMENTATIONS USING QUEUES
// =========================================================================

void _schedule_round_robin() {
    preempt_disable();

    // 1. Save the current process if interrupted by the timer
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        enqueue_process_to(&ready_queue, current_process);
    }

    // 2. O(1) extraction, skipping terminated/suspended processes
    struct PCB* next_process = NULL;
    while ((next_process = dequeue_process_from(&ready_queue)) != NULL) {
        handle_process_signals(next_process);
        if (next_process->state == PROCESS_RUNNING) break;
    }

    if (next_process == NULL) next_process = &init_process;
    else next_process->counter = 10; // Recharge the time slice (Time Quantum)

    if (current_process != next_process) switch_to_process(next_process);
    preempt_enable();
}

void _schedule_fcfs() {
    preempt_disable();

    // FCFS is Non-Preemptive. If someone is running, do not touch it.
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        preempt_enable();
        return; 
    }

    struct PCB* next_process = NULL;
    while ((next_process = dequeue_process_from(&ready_queue)) != NULL) {
        handle_process_signals(next_process);
        if (next_process->state == PROCESS_RUNNING) break;
    }

    if (next_process == NULL) next_process = &init_process;

    if (current_process != next_process) switch_to_process(next_process);
    preempt_enable();
}

void _schedule_sjf() {
    preempt_disable();

    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        preempt_enable();
        return; 
    }

    // The shortest process is ALREADY at the head of the queue thanks to enqueue_sorted_to
    struct PCB* next_process = NULL;
    while ((next_process = dequeue_process_from(&ready_queue)) != NULL) {
        handle_process_signals(next_process);
        if (next_process->state == PROCESS_RUNNING) break;
    }

    if (next_process == NULL) next_process = &init_process;

    if (current_process != next_process) switch_to_process(next_process);
    preempt_enable();
}

void _schedule_mlq() {
    preempt_disable();

    // Re-inserts the process into its specific queue if preempted
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        if (current_process->pid % 2 != 0) enqueue_process_to(&mlq_queues[0], current_process);
        else enqueue_process_to(&mlq_queues[1], current_process); 
    }

    struct PCB* next_process = NULL;

    // Search in Foreground (High Priority)
    while ((next_process = dequeue_process_from(&mlq_queues[0])) != NULL) {
        handle_process_signals(next_process);
        if (next_process->state == PROCESS_RUNNING) {
            next_process->counter = 10;
            break;
        }
    }

    // Search in Background (Low Priority) only if Foreground is empty
    if (next_process == NULL) {
        while ((next_process = dequeue_process_from(&mlq_queues[1])) != NULL) {
            handle_process_signals(next_process);
            if (next_process->state == PROCESS_RUNNING) break;
        }
    }

    if (next_process == NULL) next_process = &init_process;

    if (current_process != next_process) switch_to_process(next_process);
    preempt_enable();
}

void _schedule_mlfq() {
    preempt_disable();

    // If the process voluntarily yielded the CPU (e.g., for I/O), we do not demote it.
    // It stays in its queue. The Timer Tick handles demotions for those who exceed their time quantum.
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING && current_process->counter > 0) {
         enqueue_process_to(&mlfq_queues[queue_level[current_process->pid]], current_process);
    }

    struct PCB* next_process = NULL;
    int target_queue = -1;

    // O(1) Cascade: check queues starting from 0 (Highest Priority)
    for (int q = 0; q < 3; q++) {
        while ((next_process = dequeue_process_from(&mlfq_queues[q])) != NULL) {
            handle_process_signals(next_process);
            if (next_process->state == PROCESS_RUNNING) {
                target_queue = q;
                break;
            }
        }
        if (target_queue != -1) break; // Found, do not explore lower queues
    }

    if (next_process == NULL) {
        next_process = &init_process;
    } else {
        // Differentiated time quanta
        if (target_queue == 0) next_process->counter = 5;
        else if (target_queue == 1) next_process->counter = 10;
        else next_process->counter = 20;
    }

    if (current_process != next_process) switch_to_process(next_process);
    preempt_enable();
}

// =========================================================================
// CENTRAL DISPATCHER
// =========================================================================
void _schedule() {
   // --- Preemptive Algorithms ---
    _schedule_priority_aging();
    //_schedule_round_robin();
    //_schedule_lottery();

    // --- Advanced / Mixed Algorithms ---
    //_schedule_mlq();
    //_schedule_mlfq(); // ACTIVE BY DEFAULT

    // --- Non-Preemptive Algorithms ---
    //_schedule_fcfs();
    //_schedule_sjf();
    //_schedule_ljf();
}

void schedule() {
    current_process->counter = 0;
    _schedule();
}

// =========================================================================
// SIGNALS AND CONTEXT MANAGEMENT
// =========================================================================
void handle_process_signals(struct PCB* process) {
    if (!process->pending_signals) return;

    if (process->pending_signals & (1 << SIGNAL_KILL)) {
        process->state = PROCESS_ZOMBIE;
        process->pending_signals &= ~(1 << SIGNAL_KILL);
    } else if (process->pending_signals & (1 << SIGNAL_STOP)) {
        process->state = PROCESS_STOPPED;
        process->pending_signals &= ~(1 << SIGNAL_STOP);
    } else if (process->pending_signals & (1 << SIGNAL_RESUME)) {
        process->state = PROCESS_RUNNING;
        process->pending_signals &= ~(1 << SIGNAL_RESUME);
        // Wake up: re-insert the process into the ready queue
        enqueue_process(process); 
    }
}

void switch_to_process(struct PCB *next_process) {
    if (current_process == next_process) return;
    struct PCB *previous_process = current_process;
    current_process = next_process;

    set_pgd(next_process->mm.pgd);
    cpu_switch_to_process(previous_process, current_process);
}

void schedule_tail(void) { preempt_enable(); }

// =========================================================================
// TIMER TICK: MANAGES MLFQ DYNAMICS (Promotions / Demotions)
// =========================================================================
static int mlfq_ticks_since_boost = 0;

void handle_timer_tick() {
    current_process->counter -= 1;

    // Anti-Starvation Rule (Real and Physical Priority Boost)
    mlfq_ticks_since_boost++;
    if (mlfq_ticks_since_boost >= 1000) {
        // Physically empties queues 1 and 2 and pumps all PCBs into queue 0
        for (int q = 1; q < 3; q++) {
            struct PCB* p;
            while ((p = dequeue_process_from(&mlfq_queues[q])) != NULL) {
                queue_level[p->pid] = 0;
                enqueue_process_to(&mlfq_queues[0], p);
            }
        }
        
        // Level reset also for waiting processes (in the array)
        for (int i = 0; i < N_PROCESSES; i++) {
            queue_level[i] = 0; 
        }
        mlfq_ticks_since_boost = 0;
    }

    if (current_process->counter > 0 || current_process->preempt_disabled > 0) {
        return; // It still has time, do not interrupt
    }
    
    current_process->counter = 0;

    // Penalty (Demotion): Exhausted time without doing I/O
    if (current_process != &init_process) {
        if (queue_level[current_process->pid] < 2) {
            queue_level[current_process->pid]++;
        }
        // Physically parked in the lower queue
        enqueue_process_to(&mlfq_queues[queue_level[current_process->pid]], current_process);
    }

    enable_irq();
    _schedule();
    disable_irq();
}

// =========================================================================
// EXIT AND SYSCALL WAIT MANAGEMENT
// =========================================================================
void exit_process() {
    preempt_disable();
    current_process->state = PROCESS_ZOMBIE;
    
    // Scan the array to find who was waiting for this pid (Sys_Wait)
    for (int i = 0; i < N_PROCESSES; i++) {
        if (!processes[i]) continue;

        if (processes[i]->state == PROCESS_WAITING_ANOTHER_PROCESS && processes[i]->pid_to_wait == current_process->pid) {
            processes[i]->state = PROCESS_RUNNING;
            processes[i]->pid_to_wait = -1;
            
            // Puts the freshly woken parent back in the queue
            enqueue_process(processes[i]);
        }
    }

    preempt_enable();
    schedule();
}
