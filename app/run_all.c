#include "../user/user.h"
#include "../common/user_syscalls.h"
#include <stddef.h>

// How many programs do we want to launch in parallel?
#define NUM_PROCESSES 4

void main() {
    int pids[NUM_PROCESSES];

    call_syscall_write("[RUN_ALL] Starting multiple load test...\n");

    // Process creation loop
    for (int i = 0; i < NUM_PROCESSES; i++) {
        int pid = call_syscall_fork();

        if (pid == 0) {
            // ==========================================
            // CHILD PROCESS
            // ==========================================
            int error = call_syscall_exec("/bin/benchmark.bin", 0, NULL);
            
            if (error) {
                call_syscall_write("[RUN_ALL] Error: unable to start the binary!\n");
                call_syscall_exit();
            }
        } else {
            // ==========================================
            // PARENT PROCESS
            // ==========================================
            // Save the PID of the newly created child in the array
            pids[i] = pid;
        }
    }

    // The parent process (run_all) now methodically waits for all 
    // the children to finish their work before closing.
    call_syscall_write("[RUN_ALL] All processes started. Waiting for completion...\n");
    
    for (int i = 0; i < NUM_PROCESSES; i++) {
        call_syscall_wait(pids[i]);
    }

    call_syscall_write("[RUN_ALL] Test completed! All processes have terminated.\n");
    call_syscall_exit();
}
