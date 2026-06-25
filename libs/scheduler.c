// Include the header file containing scheduler definitions and macros
#include "scheduler.h"
// Include the header for interrupt request (IRQ) controller management
#include "../drivers/irq/controller.h"
// Include the header for memory management functions and structures
#include "mm.h"
// Include the header for process creation (fork) functionalities
#include "fork.h"

// Initialize the init process (the idle task) using a predefined macro
static struct PCB init_process = INIT_PROCESS;
// Set the pointer for the currently running process to the init process
struct PCB *current_process = &init_process;

// Global array holding all processes, initialized with the init process at index 0
struct PCB *processes[N_PROCESSES] = { &init_process };
// Counter for the total number of active processes (starts at 1 because of init)
int n_processes = 1; // n_processes = 0 ==> &init_process

// =========================================================================
// DATA STRUCTURES FOR QUEUES (THEORY)
// =========================================================================
// Define a structure to represent a queue of processes
typedef struct {
    // Pointer to the first process (PCB) in the queue
    struct PCB* head;
    // Pointer to the last process (PCB) in the queue
    struct PCB* tail;
} ProcessQueue;

// Macro to initialize an empty queue with NULL head and tail pointers
#define QUEUE_INIT {NULL, NULL}

// Define the ready queue used by standard algorithms like FCFS, RR, SJF, LJF
static ProcessQueue ready_queue = QUEUE_INIT;                              
// Define an array of 2 queues for Multilevel Queue (MLQ: 0=Foreground, 1=Background)
static ProcessQueue mlq_queues[2] = {QUEUE_INIT, QUEUE_INIT};              
// Define an array of 3 queues for Multilevel Feedback Queue (MLFQ)
static ProcessQueue mlfq_queues[3] = {QUEUE_INIT, QUEUE_INIT, QUEUE_INIT}; 
// Array to track the current MLFQ queue level (priority) for each process by its PID
static int queue_level[N_PROCESSES] = {0};                                 

// =========================================================================
// QUEUE OPERATIONS (O(1) Complexity)
// =========================================================================
// Function to add a process to the end of a specific queue
void enqueue_process_to(ProcessQueue* queue, struct PCB* process) {
    // If the process is NULL or is the idle init process, do not enqueue it
    if (!process || process == &init_process) return;
    // Ensure the process's next pointer is clear before adding it
    process->next_ready = NULL;
    
    // If the queue is currently empty
    if (queue->tail == NULL) {
        // The new process becomes both the head of the queue...
        queue->head = process;
        // ...and the tail of the queue
        queue->tail = process;
    } else {
        // Otherwise, link the current tail's next pointer to the new process
        queue->tail->next_ready = process;
        // Update the queue's tail pointer to be the new process
        queue->tail = process;
    }
}

// Function to remove and return the process at the front of a specific queue
struct PCB* dequeue_process_from(ProcessQueue* queue) {
    // If the queue is empty (head is NULL), return NULL
    if (queue->head == NULL) return NULL;
    
    // Store the process currently at the head of the queue
    struct PCB* process = queue->head;
    // Move the head pointer to the next process in line
    queue->head = process->next_ready;
    
    // If the queue is now empty after removing the head
    if (queue->head == NULL) {
        // Set the tail pointer to NULL as well
        queue->tail = NULL;
    }
    
    // Disconnect the removed process from the queue by clearing its next pointer
    process->next_ready = NULL;
    // Return the extracted process
    return process;
}

// Function to insert a process into a queue in a sorted manner based on priority
void enqueue_sorted_to(ProcessQueue* queue, struct PCB* process, int ascending) {
    // Ignore NULL processes or the init process
    if (!process || process == &init_process) return;
    // Clear the process's next pointer
    process->next_ready = NULL;

    // Determine if the new process should be placed at the very front of the queue
    // based on whether we are sorting ascending (SJF) or descending (LJF)
    int goes_first = ascending ? (process->priority < queue->head->priority) 
                               : (process->priority > queue->head->priority);

    // If the queue is empty or the new process has the highest/lowest priority
    if (queue->head == NULL || goes_first) {
        // Point the new process's next to the current head
        process->next_ready = queue->head;
        // Update the queue's head to be the new process
        queue->head = process;
        // If the queue was empty, the tail is also the new process
        if (queue->tail == NULL) queue->tail = process;
        // Exit the function since insertion is complete
        return;
    }

    // Initialize a pointer to traverse the queue starting from the head
    struct PCB* current = queue->head;
    // Variable to hold the loop condition evaluation
    int condition;
    // Traverse the queue to find the correct insertion spot
    while (current->next_ready != NULL) {
        // Check if the new process priority fits the sorted order compared to the next node
        condition = ascending ? (process->priority >= current->next_ready->priority) 
                              : (process->priority <= current->next_ready->priority);
        // If the condition is false, we have found the insertion point
        if (!condition) break;
        // Move to the next process in the queue
        current = current->next_ready;
    }

    // Link the new process to the node that will follow it
    process->next_ready = current->next_ready;
    // Link the current node to the new process
    current->next_ready = process;
    // If we inserted at the very end of the queue
    if (process->next_ready == NULL) {
        // Update the queue's tail pointer to the new process
        queue->tail = process;
    }
}

// =========================================================================
// INSERTION ROUTER (Called by fork.c, syscalls, and signal handlers)
// =========================================================================
// Centralized function to enqueue a process into the appropriate queue based on the active algorithm
void enqueue_process(struct PCB* process) {
    // Ignore NULL processes or the init process
    if (!process || process == &init_process) return;

    // --- Standard algorithms: Round Robin, FCFS ---
    // Enqueue the process at the end of the standard ready queue
    // enqueue_process_to(&ready_queue, process);

    // --- SJF (Shortest Job First) ---
    // Insert the process sorted by shortest priority (ascending order)
    // enqueue_sorted_to(&ready_queue, process, 1);

    // --- LJF (Longest Job First) ---
    // Insert the process sorted by longest priority (descending order)
    // enqueue_sorted_to(&ready_queue, process, 0);

    // --- MLQ (Multilevel Queue) ---
    /*
    // If the PID is odd, put it in the foreground queue (index 0)
    if (process->pid % 2 != 0) enqueue_process_to(&mlq_queues[0], process);
    // If the PID is even, put it in the background queue (index 1)
    else enqueue_process_to(&mlq_queues[1], process);
    */

    // --- MLFQ (Multilevel Feedback Queue) - Active by default for the Demo ---
    // Enqueue the process into the specific MLFQ queue corresponding to its current priority level
    enqueue_process_to(&mlfq_queues[queue_level[process->pid]], process);
}

// =========================================================================
// SYSTEM MANAGEMENT FUNCTIONS
// =========================================================================
// Function to register a newly created process into the scheduler's tracking system
int add_process_to_scheduler(struct PCB* process) {
    // Return error (-1) if we reached max capacity or if the PID is out of bounds
    if(n_processes >= N_PROCESSES || process->pid >= N_PROCESSES) return -1;
    
    // Store the process pointer in the global processes array at the index of its PID
    processes[process->pid] = process;
    // MLFQ Rule: Every new process starts at the highest priority queue (Level 0)
    queue_level[process->pid] = 0; 

    // If the process is initialized in a RUNNING state...
    if (process->state == PROCESS_RUNNING) {
        // ...enqueue it into the ready queues using the routing function
        enqueue_process(process);
    }
    
    // Increment the total count of active processes
    n_processes++;
    // Return 0 on success
    return 0;
}

// Function to decrement the preemption disable counter (allows context switches again)
void preempt_enable() { current_process->preempt_disabled--; }
// Function to increment the preemption disable counter (prevents context switches)
void preempt_disable() { current_process->preempt_disabled++; }

// Forward declaration for the signal handling function
void handle_process_signals(struct PCB* process);
// Forward declaration for the context switch setup function
void switch_to_process(struct PCB *next_process);
// External declaration for the assembly routine that actually swaps CPU registers
extern void cpu_switch_to_process(struct PCB *prev, struct PCB *next);

// Priority scheduling algorithm with an aging mechanism to prevent starvation
void _schedule_priority_aging() {
  // Disable preemption to ensure scheduling isn't interrupted
  preempt_disable();
  // Variables to track the highest counter value and the index of the chosen process
  long max_counter, next_process_index;

  // Infinite loop to find a process to run
  while (1) {
    // Reset the max counter for this pass
    max_counter = 0;
    // Default to the init process (index 0)
    next_process_index = 0;
    
    // Loop through all possible process slots in the global array
    for (int i = 0; i < N_PROCESSES; i++) {
      // If a process exists at this index
      if (processes[i]) {
        // Process any pending signals (kill, stop, resume) for this process
        handle_process_signals(processes[i]);
        // If the process is ready to run and its counter is higher than the current max
        if (processes[i]->state == PROCESS_RUNNING && processes[i]->counter > max_counter) {
          // Update the maximum counter found so far
          max_counter = processes[i]->counter;
          // Save the index of this process as the next potential candidate
          next_process_index = i;
        }
      }
    }

    // If we found at least one ready process with a counter greater than 0
    if (max_counter > 0) {
      // Break out of the infinite loop
      break;
    }

    // If no process had a counter > 0 (all are exhausted), we apply aging
    for (int i = 0; i < N_PROCESSES; i++) {
      // If the process exists
      if (processes[i]) {
        // Recalculate its counter: halve the remaining ticks and add its base priority
        processes[i]->counter = (processes[i]->counter >> 1) + processes[i]->priority;
      }
    }
  } // End of while(1) loop

  // Retrieve the pointer to the selected next process
  struct PCB* next_process = processes[next_process_index];
  // Handle signals for the selected process one more time just in case
  handle_process_signals(next_process);
  
  // Verify the process is still in RUNNING state (signals might have stopped/killed it)
  if (next_process->state == PROCESS_RUNNING) {
    // Perform the context switch to the selected process
    switch_to_process(next_process);
  }
  // Re-enable preemption before leaving the scheduler
  preempt_enable();
}

// =========================================================================
// ALGORITHM IMPLEMENTATIONS USING QUEUES
// =========================================================================

// Round Robin scheduling algorithm implementation
void _schedule_round_robin() {
    // Disable preemption during the scheduling decision
    preempt_disable();

    // 1. If the current process was interrupted but is still runnable (not init)...
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        // ...put it back at the end of the ready queue
        enqueue_process_to(&ready_queue, current_process);
    }

    // 2. Initialize pointer for the next process
    struct PCB* next_process = NULL;
    // Extract processes from the queue in O(1) time until we find a valid one
    while ((next_process = dequeue_process_from(&ready_queue)) != NULL) {
        // Process any pending signals for the extracted process
        handle_process_signals(next_process);
        // If the process is still RUNNING after handling signals, break the loop
        if (next_process->state == PROCESS_RUNNING) break;
    }

    // If the queue was empty or all processes were stopped/zombies
    if (next_process == NULL) next_process = &init_process; // Fallback to idle task
    // Otherwise, recharge its time quantum (e.g., 10 ticks)
    else next_process->counter = 10; 

    // If the selected process is different from the current one, switch context
    if (current_process != next_process) switch_to_process(next_process);
    // Re-enable preemption
    preempt_enable();
}

// First-Come, First-Served scheduling algorithm implementation
void _schedule_fcfs() {
    // Disable preemption during the scheduling decision
    preempt_disable();

    // FCFS is Non-Preemptive. If a valid user process is currently running...
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        // ...re-enable preemption and exit immediately (do not switch)
        preempt_enable();
        return; 
    }

    // Initialize pointer for the next process
    struct PCB* next_process = NULL;
    // Extract processes from the front of the queue
    while ((next_process = dequeue_process_from(&ready_queue)) != NULL) {
        // Handle pending signals
        handle_process_signals(next_process);
        // If it's ready to run, break the loop and choose it
        if (next_process->state == PROCESS_RUNNING) break;
    }

    // If no valid process was found, fallback to the init process
    if (next_process == NULL) next_process = &init_process;

    // Perform context switch if we are changing processes
    if (current_process != next_process) switch_to_process(next_process);
    // Re-enable preemption
    preempt_enable();
}

// Shortest Job First scheduling algorithm implementation
void _schedule_sjf() {
    // Disable preemption during the scheduling decision
    preempt_disable();

    // Non-preemptive behavior: do not interrupt the currently running process
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        // Re-enable preemption and exit
        preempt_enable();
        return; 
    }

    // Initialize pointer for the next process
    struct PCB* next_process = NULL;
    // The shortest job is already at the head of the queue due to enqueue_sorted_to
    while ((next_process = dequeue_process_from(&ready_queue)) != NULL) {
        // Handle pending signals
        handle_process_signals(next_process);
        // If it's ready to run, break the loop
        if (next_process->state == PROCESS_RUNNING) break;
    }

    // Fallback to init process if queue is empty
    if (next_process == NULL) next_process = &init_process;

    // Switch context to the new process
    if (current_process != next_process) switch_to_process(next_process);
    // Re-enable preemption
    preempt_enable();
}

// Multilevel Queue scheduling algorithm implementation
void _schedule_mlq() {
    // Disable preemption during scheduling
    preempt_disable();

    // Re-insert the preempted process into its respective fixed priority queue
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING) {
        // If PID is odd, put it in the Foreground queue
        if (current_process->pid % 2 != 0) enqueue_process_to(&mlq_queues[0], current_process);
        // If PID is even, put it in the Background queue
        else enqueue_process_to(&mlq_queues[1], current_process); 
    }

    // Initialize pointer for the next process
    struct PCB* next_process = NULL;

    // Search in the Foreground queue first (Highest Priority)
    while ((next_process = dequeue_process_from(&mlq_queues[0])) != NULL) {
        // Handle pending signals
        handle_process_signals(next_process);
        // If ready to run
        if (next_process->state == PROCESS_RUNNING) {
            // Assign a time quantum of 10 ticks
            next_process->counter = 10;
            // Break loop as we found a process
            break;
        }
    }

    // If the Foreground queue was empty, search in the Background queue (Low Priority)
    if (next_process == NULL) {
        // Dequeue from Background queue
        while ((next_process = dequeue_process_from(&mlq_queues[1])) != NULL) {
            // Handle pending signals
            handle_process_signals(next_process);
            // Break if ready to run
            if (next_process->state == PROCESS_RUNNING) break;
        }
    }

    // Fallback to init process if both queues are empty
    if (next_process == NULL) next_process = &init_process;

    // Perform context switch if necessary
    if (current_process != next_process) switch_to_process(next_process);
    // Re-enable preemption
    preempt_enable();
}

// Multilevel Feedback Queue scheduling algorithm implementation
void _schedule_mlfq() {
    // Disable preemption during scheduling
    preempt_disable();

    // If the process yielded voluntarily (e.g., for I/O) and still has time left (counter > 0)
    // we do not demote it. We put it back in its current priority queue.
    if (current_process != &init_process && current_process->state == PROCESS_RUNNING && current_process->counter > 0) {
         // Re-insert into the queue matching its current tracked level
         enqueue_process_to(&mlfq_queues[queue_level[current_process->pid]], current_process);
    }

    // Initialize pointer for the next process
    struct PCB* next_process = NULL;
    // Variable to track which queue the process was extracted from
    int target_queue = -1;

    // O(1) Cascade: iterate through queues starting from 0 (Highest Priority) to 2 (Lowest)
    for (int q = 0; q < 3; q++) {
        // Dequeue processes from the current queue level
        while ((next_process = dequeue_process_from(&mlfq_queues[q])) != NULL) {
            // Handle pending signals
            handle_process_signals(next_process);
            // If the process is runnable
            if (next_process->state == PROCESS_RUNNING) {
                // Record the queue level it came from
                target_queue = q;
                // Break out of the while loop
                break;
            }
        }
        // If we found a process in this queue level, break the outer for-loop (do not check lower priority queues)
        if (target_queue != -1) break; 
    }

    // If all queues were completely empty
    if (next_process == NULL) {
        // Set the next process to the idle task
        next_process = &init_process;
    } else {
        // Assign differentiated time quanta based on the priority queue level
        // Highest priority gets the shortest time slice
        if (target_queue == 0) next_process->counter = 5;
        // Medium priority gets a medium time slice
        else if (target_queue == 1) next_process->counter = 10;
        // Lowest priority gets the longest time slice (best for CPU-bound tasks)
        else next_process->counter = 20;
    }

    // Perform context switch if necessary
    if (current_process != next_process) switch_to_process(next_process);
    // Re-enable preemption
    preempt_enable();
}

// =========================================================================
// CENTRAL DISPATCHER
// =========================================================================
// Master scheduling function that routes to the specific algorithm implementation
void _schedule() {
    // --- Preemptive Algorithms ---
    _schedule_priority_aging();
    // _schedule_round_robin();
    // _schedule_lottery();

    // --- Advanced / Mixed Algorithms ---
    // _schedule_mlq();
    // _schedule_mlfq(); 
    // --- Non-Preemptive Algorithms ---
    // _schedule_fcfs();
    // _schedule_sjf();
    // _schedule_ljf();
}

// Wrapper function to trigger a manual schedule (e.g., when a process yields or blocks)
void schedule() {
    // Force the current process's time slice to 0 so it gets preempted/re-evaluated
    current_process->counter = 0;
    // Call the master dispatcher
    _schedule();
}

// =========================================================================
// SIGNALS AND CONTEXT MANAGEMENT
// =========================================================================
// Function to evaluate and apply pending signals for a specific process
void handle_process_signals(struct PCB* process) {
    // If the bitmask for pending signals is 0, exit immediately (nothing to do)
    if (!process->pending_signals) return;

    // Check if the SIGNAL_KILL bit is set
    if (process->pending_signals & (1 << SIGNAL_KILL)) {
        // Change process state to ZOMBIE (terminated but waiting for parent to read status)
        process->state = PROCESS_ZOMBIE;
        // Clear the SIGNAL_KILL bit from the pending signals mask
        process->pending_signals &= ~(1 << SIGNAL_KILL);
        
    // Check if the SIGNAL_STOP bit is set
    } else if (process->pending_signals & (1 << SIGNAL_STOP)) {
        // Change process state to STOPPED (suspended execution)
        process->state = PROCESS_STOPPED;
        // Clear the SIGNAL_STOP bit from the pending signals mask
        process->pending_signals &= ~(1 << SIGNAL_STOP);
        
    // Check if the SIGNAL_RESUME bit is set
    } else if (process->pending_signals & (1 << SIGNAL_RESUME)) {
        // Change process state back to RUNNING (ready to execute)
        process->state = PROCESS_RUNNING;
        // Clear the SIGNAL_RESUME bit from the pending signals mask
        process->pending_signals &= ~(1 << SIGNAL_RESUME);
        // Wake up the process by re-inserting it into the ready queues
        enqueue_process(process); 
    }
}

// Function to handle the high-level context switch preparation
void switch_to_process(struct PCB *next_process) {
    // If the selected process is already running, do nothing
    if (current_process == next_process) return;
    // Save the pointer to the currently running process
    struct PCB *previous_process = current_process;
    // Update the global current_process pointer to the new process
    current_process = next_process;

    // Update the Memory Management Unit's Page Global Directory to the new process's address space
    set_pgd(next_process->mm.pgd);
    // Call the assembly routine to swap CPU registers and stack pointers
    cpu_switch_to_process(previous_process, current_process);
}

// Function called at the end of a newly created process's first context switch to enable preemption
void schedule_tail(void) { preempt_enable(); }

// =========================================================================
// TIMER TICK: MANAGES MLFQ DYNAMICS (Promotions / Demotions)
// =========================================================================
// Static variable to count hardware timer ticks for the MLFQ boost mechanism
static int mlfq_ticks_since_boost = 0;

// Function called by the hardware timer interrupt handler at regular intervals
void handle_timer_tick() {
    // Decrement the time slice counter of the currently running process
    current_process->counter -= 1;

    // Increment the counter tracking time since the last MLFQ priority boost
    mlfq_ticks_since_boost++;
    // Anti-Starvation Rule: If 1000 ticks have passed
    if (mlfq_ticks_since_boost >= 1000) {
        // Physically empty the lower priority queues (1 and 2) and move everyone to queue 0
        for (int q = 1; q < 3; q++) {
            struct PCB* p;
            // Dequeue until empty
            while ((p = dequeue_process_from(&mlfq_queues[q])) != NULL) {
                // Reset their tracked priority level to 0
                queue_level[p->pid] = 0;
                // Enqueue them into the highest priority queue
                enqueue_process_to(&mlfq_queues[0], p);
            }
        }
        
        // Also reset the priority level tracking array for all processes (even blocked ones)
        for (int i = 0; i < N_PROCESSES; i++) {
            queue_level[i] = 0; 
        }
        // Reset the boost timer
        mlfq_ticks_since_boost = 0;
    }

    // If the current process still has time left OR preemption is explicitly disabled
    if (current_process->counter > 0 || current_process->preempt_disabled > 0) {
        // Return immediately, allowing the process to continue running
        return; 
    }
    
    // Safety catch: ensure counter doesn't go negative
    current_process->counter = 0;

    // Penalty (Demotion): If the process exhausted its time slice without doing I/O
    if (current_process != &init_process) {
        // If it's not already in the lowest priority queue (Level 2)
        if (queue_level[current_process->pid] < 2) {
            // Demote it to the next lower queue level
            queue_level[current_process->pid]++;
        }
        // Physically park the process in its new (or same, if already at bottom) queue
        enqueue_process_to(&mlfq_queues[queue_level[current_process->pid]], current_process);
    }

    // Re-enable hardware interrupts before calling the scheduler
    enable_irq();
    // Call the master dispatcher to choose a new process
    _schedule();
    // Disable hardware interrupts upon returning from the scheduler
    disable_irq();
}

// =========================================================================
// EXIT AND SYSCALL WAIT MANAGEMENT
// =========================================================================
// Function called when a process terminates
void exit_process() {
    // Disable preemption to ensure atomicity of the exit process
    preempt_disable();
    // Set the current process's state to ZOMBIE
    current_process->state = PROCESS_ZOMBIE;
    
    // Scan the entire process array to find if any parent process is waiting for this child
    for (int i = 0; i < N_PROCESSES; i++) {
        // Skip empty slots
        if (!processes[i]) continue;

        // If a process is blocked (WAITING) and specifically waiting for the exiting process's PID
        if (processes[i]->state == PROCESS_WAITING_ANOTHER_PROCESS && processes[i]->pid_to_wait == current_process->pid) {
            // Wake up the waiting parent process
            processes[i]->state = PROCESS_RUNNING;
            // Clear the wait condition
            processes[i]->pid_to_wait = -1;
            
            // Re-enqueue the freshly woken parent into the ready queues
            enqueue_process(processes[i]);
        }
    }

    // Re-enable preemption
    preempt_enable();
    // Call the scheduler to switch away from the dying process permanently
    schedule();
}
