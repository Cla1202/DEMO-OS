#include "scheduler.h"
#include "../drivers/irq/controller.h"
#include "mm.h"
#include "fork.h"

// Initialization of the Init process (Idle Task)
static struct PCB init_process = INIT_PROCESS;
struct PCB *current_process = &init_process;
struct PCB *processes[N_PROCESSES] = {
    &init_process,
};

// ==========================================
// MLFQ: Queue tracking
// 0 = High (Q0), 1 = Medium (Q1), 2 = Low (Q2)
// ==========================================
static int queue_level[N_PROCESSES] = {0}; 
int n_processes = 1;

int add_process_to_scheduler(struct PCB* process){
  if(n_processes >= N_PROCESSES || process->pid >= N_PROCESSES)
    return -1;
    
  processes[process->pid] = process;
  
  // MLFQ Rule 1: Every new process enters the VIP queue (Level 0)
  queue_level[process->pid] = 0; 
  
  n_processes++;
  return 0;
}

void handle_process_signals(struct PCB* process);

void preempt_enable() {
  current_process->preempt_disabled--;
}

void preempt_disable() {
  current_process->preempt_disabled++;
}

// =========================================================================
// ALGORITHM 1: Priority Scheduling with Aging (Preemptive)
// =========================================================================
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

    if (max_counter > 0) break; // We found a ready process with counter > 0

    // If all processes have counter 0, recalculate the Time Quantum (Aging)
    for (int i = 0; i < N_PROCESSES; i++) {
      if (processes[i]) {
        // BARE-METAL TRICK: Since SJF uses p->priority to store the file size (>1000 bytes),
        // using that value here would assign too many ticks. We use a safe base priority (e.g., 5)
        // if the value is clearly a file size.
        long safe_priority = (processes[i]->priority > 50) ? 5 : processes[i]->priority;
        
        // Unix-style Aging formula: halves the remaining ticks and adds the base priority
        processes[i]->counter = (processes[i]->counter >> 1) + safe_priority;
      }
    }
  }

  struct PCB* next_process = processes[next_process_index];
  handle_process_signals(next_process);
  
  if (next_process->state == PROCESS_RUNNING) {
    switch_to_process(next_process);
  }
  preempt_enable();
}

// =========================================================================
// ALGORITHM 2: Pure Round Robin (Preemptive)
// =========================================================================
static int last_process_index = 0; // Remembers the last executed process

void _schedule_round_robin() {
  preempt_disable();
  int next_process_index = -1;

  for (int i = 1; i <= N_PROCESSES; i++) {
    int check_index = (last_process_index + i) % N_PROCESSES;
    struct PCB* p = processes[check_index];

    if (p) {
      handle_process_signals(p);

      if (p->state == PROCESS_RUNNING) {
        next_process_index = check_index;
        p->counter = 10; // Time Quantum
        break;
      }
    }
  }

  if (next_process_index != -1) {
    last_process_index = next_process_index;
    struct PCB* next_process = processes[next_process_index];
    
    if (current_process != next_process) {
      switch_to_process(next_process);
    }
  }
  preempt_enable();
}

// =========================================================================
// ALGORITHM 3: FCFS - First-Come, First-Served (Non-Preemptive)
// =========================================================================
void _schedule_fcfs() {
  preempt_disable();

  // FCFS RULE 1: NON-PREEMPTIVE (If a process is running, we don't interrupt it)
  if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
      preempt_enable();
      return; 
  }
    
  struct PCB* next_process = NULL; 
  int oldest_pid = 0; 

  for (int i = 0; i < N_PROCESSES; i++) {
    struct PCB* p = processes[i];
    if (p) {
      handle_process_signals(p);

      if (p->state == PROCESS_RUNNING && p != &init_process) {
        if (next_process == NULL || p->pid < oldest_pid) {
          oldest_pid = p->pid;
          next_process = p;
        }
      }
    }
  }

  // If no process is found, go back to Init (Idle Task)
  if (next_process == NULL) next_process = &init_process;

  if (current_process != next_process) {
    switch_to_process(next_process);
  }
  preempt_enable();
}

// =========================================================================
// ALGORITHM 4: SJF - Shortest-Job-First (Non-Preemptive)
// =========================================================================
void _schedule_sjf() {
  preempt_disable();

  if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
      preempt_enable();
      return; 
  }

  struct PCB* next_process = NULL;
  long shortest_time = 0;

  for (int i = 0; i < N_PROCESSES; i++) {
    struct PCB* p = processes[i];
    if (p) {
      handle_process_signals(p);
          
      if (p->state == PROCESS_RUNNING && p != &init_process) {
        if (next_process == NULL || p->priority < shortest_time) {
          shortest_time = p->priority;
          next_process = p;
        } else if (p->priority == shortest_time && p->pid < next_process->pid) {
          next_process = p;
        }
      }
    }
  }

  if (next_process == NULL) next_process = &init_process;

  if (current_process != next_process) switch_to_process(next_process);
  preempt_enable();
}

// =========================================================================
// ALGORITHM 5: LJF - Longest-Job-First (Non-Preemptive)
// =========================================================================
void _schedule_ljf() {
  preempt_disable();

  if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
      preempt_enable();
      return; 
  }

  struct PCB* next_process = NULL;
  long longest_time = -1;

  for (int i = 0; i < N_PROCESSES; i++) {
    struct PCB* p = processes[i];
    if (p) {
      handle_process_signals(p);
          
      if (p->state == PROCESS_RUNNING && p != &init_process) {
        // Looks for the HEAVIEST program (largest byte size saved in priority)
        if (next_process == NULL || p->priority > longest_time) {
          longest_time = p->priority;
          next_process = p;
        } 
      }
    }
  }

  if (next_process == NULL) next_process = &init_process;

  if (current_process != next_process) switch_to_process(next_process);
  preempt_enable();
}

// =========================================================================
// ALGORITHM 6: Lottery Scheduling (Preemptive)
// =========================================================================
// Very basic pseudo-random number generator (PRNG) for the lottery
static unsigned long lcg_seed = 123456789;
unsigned int get_random_number() {
    lcg_seed = (1103515245 * lcg_seed + 12345);
    return (unsigned int)(lcg_seed >> 16);
}

void _schedule_lottery() {
  preempt_disable();
  int total_tickets = 0;
  
  // PHASE 1: Count the tickets in play
  for (int i = 0; i < N_PROCESSES; i++) {
    struct PCB* p = processes[i];
    if (p) {
      handle_process_signals(p);
      if (p->state == PROCESS_RUNNING && p != &init_process) {
        total_tickets += 10; // Assign 10 basic tickets to each running process
      }
    }
  }

  struct PCB* next_process = &init_process;

  // PHASE 2: Extraction!
  if (total_tickets > 0) {
    int winning_ticket = get_random_number() % total_tickets;
    int ticket_counter = 0;

    for (int i = 0; i < N_PROCESSES; i++) {
      struct PCB* p = processes[i];
      if (p && p->state == PROCESS_RUNNING && p != &init_process) {
        ticket_counter += 10; 
        if (winning_ticket < ticket_counter) {
          next_process = p;
          next_process->counter = 10; // Time Quantum to the winner
          break;
        }
      }
    }
  }

  if (current_process != next_process) {
    switch_to_process(next_process);
  }
  
  preempt_enable();
}

// =========================================================================
// ALGORITHM 7: MLQ - Multilevel Queue (Multiple Queues)
// =========================================================================
static int mlq_last_process_index = 0;

void _schedule_mlq() {
  preempt_disable();
  struct PCB* next_process = NULL;
  
  // ---------------------------------------------------------
  // QUEUE 1: FOREGROUND (High Priority) - Uses ROUND ROBIN
  // (In this demo, ODD PIDs are Foreground)
  // ---------------------------------------------------------
  int check_index = -1;
  for (int i = 1; i <= N_PROCESSES; i++) {
    int idx = (mlq_last_process_index + i) % N_PROCESSES;
    struct PCB* p = processes[idx];

    if (p && p != &init_process && (p->pid % 2 != 0)) { // Odd PID
      handle_process_signals(p);
      if (p->state == PROCESS_RUNNING) {
        check_index = idx;
        p->counter = 10; // Fast Time Quantum
        next_process = p;
        break;
      }
    }
  }

  // ---------------------------------------------------------
  // QUEUE 2: BACKGROUND (Low Priority) - Uses FCFS
  // (EVEN PIDs. Checked ONLY if Queue 1 is empty)
  // ---------------------------------------------------------
  if (next_process == NULL) {
    // If we were already running a Background process, do not interrupt it (FCFS is Non-Preemptive)
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING && (current_process->pid % 2 == 0)) {
        preempt_enable();
        return; 
    }

    int oldest_pid = 9999;
    for (int i = 0; i < N_PROCESSES; i++) {
      struct PCB* p = processes[i];
      if (p && p != &init_process && (p->pid % 2 == 0)) { // Even PID
        handle_process_signals(p);
        if (p->state == PROCESS_RUNNING) {
          if (next_process == NULL || p->pid < oldest_pid) {
            oldest_pid = p->pid;
            next_process = p;
          }
        }
      }
    }
  } else {
    mlq_last_process_index = check_index;
  }

  // If both queues are empty, go to the Idle Task
  if (next_process == NULL) next_process = &init_process;

  if (current_process != next_process) {
    switch_to_process(next_process);
  }
  
  preempt_enable();
}

// =========================================================================
// ALGORITHM 8: MLFQ - Multilevel Feedback Queue (Windows/macOS algorithm)
// =========================================================================
static int mlfq_last_index = 0;

void _schedule_mlfq() {
  preempt_disable();

  int target_queue = -1;
  int next_process_index = -1;

  // We look for the process starting from Queue 0, then Queue 1, then Queue 2
  for (int q = 0; q < 3; q++) {
      for (int i = 1; i <= N_PROCESSES; i++) {
          int check_index = (mlfq_last_index + i) % N_PROCESSES;
          struct PCB* p = processes[check_index];

          // If the process is RUNNING and belongs to the queue we are checking (q)
          if (p && p != &init_process && p->state == PROCESS_RUNNING && queue_level[p->pid] == q) {
              target_queue = q;
              next_process_index = check_index;
              break;
          }
      }
      // If we found someone in this queue, we don't look at the lower queues!
      if (target_queue != -1) break; 
  }

  struct PCB* next_process = &init_process;

  if (next_process_index != -1) {
      mlfq_last_index = next_process_index;
      next_process = processes[next_process_index];

      // Assign the Time Quantum based on the queue level
      if (target_queue == 0) {
          next_process->counter = 5;   // Queue 0: Very responsive
      } else if (target_queue == 1) {
          next_process->counter = 10;  // Queue 1: Compromise/Middle ground
      } else {
          next_process->counter = 20;  // Queue 2: Heavy computation "bricks"
      }
  }

  if (current_process != next_process) {
      switch_to_process(next_process);
  }

  preempt_enable();
}


// =========================================================================
// CENTRAL SCHEDULER DISPATCHER
// =========================================================================
void _schedule() {
    // Uncomment ONLY THE ALGORITHM YOU WANT TO TEST.

    // --- Preemptive Algorithms ---
    //_schedule_priority_aging();
    //_schedule_round_robin();
    //_schedule_lottery();

    // --- Advanced / Mixed Algorithms ---
    //_schedule_mlq();
    _schedule_mlfq(); // ACTIVE BY DEFAULT

    // --- Non-Preemptive Algorithms ---
    //_schedule_fcfs();
    //_schedule_sjf();
    //_schedule_ljf();
}

void schedule() {
  current_process->counter = 0;
  _schedule();
}

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
  }
}

void switch_to_process(struct PCB *next_process) {
  if (current_process == next_process) return;
  struct PCB *previous_process = current_process;
  current_process = next_process;

  set_pgd(next_process->mm.pgd);
  cpu_switch_to_process(previous_process, current_process);
}

void schedule_tail(void) {
  preempt_enable();
}

// =========================================================================
// TIMER TICK: MANAGES MLFQ PROMOTIONS/DEMOTIONS
// =========================================================================
static int mlfq_ticks_since_boost = 0;

void handle_timer_tick() {
  current_process->counter -= 1;

  // MLFQ Rule 3 (Priority Boost): Every 1000 ticks (about 1 second), 
  // we promote ALL processes to Queue 0 to prevent starvation.
  mlfq_ticks_since_boost++;
  if (mlfq_ticks_since_boost >= 1000) {
      for (int i = 0; i < N_PROCESSES; i++) {
          queue_level[i] = 0; 
      }
      mlfq_ticks_since_boost = 0;
  }

  // If the process still has time, we do nothing.
  if (current_process->counter > 0 || current_process->preempt_disabled > 0) {
      return;
  }
  
  // If we reach here, the process has EXHAUSTED its Time Quantum.
  current_process->counter = 0;

  // MLFQ Rule 2 (Demotion): We punish heavy CPU-Bound processes
  // by demoting them by one level (max Level 2).
  if (queue_level[current_process->pid] < 2) {
      queue_level[current_process->pid]++;
  }

  enable_irq();
  _schedule();
  disable_irq();
}

void exit_process() {
  preempt_disable();
  current_process->state = PROCESS_ZOMBIE;
  
  for (int i = 0; i < N_PROCESSES; i++) {
    if (!processes[i]) continue;

    // Wake up any parent processes waiting (sys_wait)
    if (processes[i]->state == PROCESS_WAITING_ANOTHER_PROCESS && processes[i]->pid_to_wait == current_process->pid) {
      processes[i]->state = PROCESS_RUNNING;
      processes[i]->pid_to_wait = -1;
    }
  }

  preempt_enable();
  schedule();
}
