#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"
#include "timer.h"

struct proc pool[NPROC];

__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];

struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid() {
    return curr_proc()->pid;
}

struct proc *curr_proc() {
    return current_proc;
}

// initialize the proc table at boot time.
void proc_init() {
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        p->state = UNUSED;
        p->kstack = (uint64)kstack[p - pool];
        p->trapframe = (struct trapframe *)trapframe[p - pool];
    }
    idle.kstack = (uint64)boot_stack_top;
    idle.pid = IDLE_PID;
    current_proc = &idle;
    init_queue(&task_queue);
}

int allocpid() {
    static int PID = 1;
    return PID++;
}

//!!
struct proc *fetch_task() {
    // int index = pop_queue(&task_queue);

    //best stores the current best candidate to run next (init to null)
    //    best keeps track of which runnable process currently has the smallest stride
    struct proc *best = NULL;

    //loop thru every process slot in the process table
    //  pool is start of process array, &pool[NPROC] is the last entry +1, p++ moves to next struct proc
    //
    // CH5: "since our experimental test cases are simple, efficiency is not a concern at all...I highly 
    // recommend using a brute-force approach to find the minimum value" (scanning the whole process table)
    for (struct proc *p = pool; p < &pool[NPROC]; p++) {
        //skips any process that is not in RUNNABLE state
        //specs: "select the process with the smallest stride from the currently runnable processes"
        if (p->state != RUNNABLE) {
            continue;
            //In queue-based code the queue already contained only ready to run processes. 
            // Whole process table is scanned now so we have to filter it
        }

        //chooses runnable process w/ smallest stride
        //if none has been chosen yet OR if there is a candidate in best, compare curr process p to it
        if (best == NULL || p->stride < best->stride) {
            //specs: "Whenever scheduling is required, select the process with the smallest stride from 
            // the currently runnable processes."
            best = p;
            
        }
    }

    //if no runnable process was found, there's no process available to run
    if (best == NULL) {
        debugf("No runnable processes available\n");
        return NULL;
    }

    //incr selected process curr stride by it's pass val
    //  best->stride: accum amnt of service the proc has received so far
    //  best->pass: how much its stride should grow every time it's chosen
    //CH5: "Whenever scheduling is req, select the process with the smallest stride from the 
    // currently runnable processes for scheduling"
    best->stride += best->pass;
    //debugf("fetch task %d(pid=%d) stride=%d pass=%d priority=%d\n", (int)(best - pool), best->pid, best->stride, best->pass, best->priority);
    return best; //scheduler takes proc and context-switched into it
}

// void add_task(struct proc *p) {
//     push_queue(&task_queue, p - pool);
//     debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
// }

// Look in the process table for an UNUSED proc.
struct proc *allocproc() {
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        if (p->state == UNUSED) {
            goto found;
        }
    }
    return 0;

found:
    // init proc
    p->pid = allocpid();
    p->state = USED;
    p->ustack = 0;
    p->max_page = 0;
    p->parent = NULL;
    p->exit_code = 0;

    p->priority = 16; //specs: "Process initial priority should be set to 16"
    p->stride = 0; //specs: "Process initial stride should be set to 0" (has rec no CPU time at creation)

    //CH5: P.pass = BigStride / P.priority, where 
    // P.pass is pass val of the proces
    // P.priority is the priority of the process
    // BigStride is a predefined large const
    // then time alloc to each process under this scheduling will be proportional to its priority
    p->pass = BIG_STRIDE / p->priority;

    p->start_time = 0; //proc hasn't been scheduled yet
    //set all syscall counters to 0
    memset(p->syscall_times, 0, sizeof(p->syscall_times));
    
    p->pagetable = uvmcreate((uint64)p->trapframe);
    memset(&p->context, 0, sizeof(p->context));
    memset((void *)p->kstack, 0, KSTACK_SIZE);
    memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
    p->context.ra = (uint64)usertrapret;
    p->context.sp = p->kstack + KSTACK_SIZE;
    return p;
}

void scheduler()
{
	struct proc *p;
	for (;;) {
		p = fetch_task();
		if (p == NULL) {
			panic("all app are over!\n");
		}
		if (p->start_time == 0) {
			p->start_time = get_cycle();
		}
		tracef("swtich to proc %d", p - pool);
		p->state = RUNNING;
		current_proc = p;
		swtch(&idle.context, &p->context);
	}
}

void sched() {
    struct proc *p = curr_proc();
    if (p->state == RUNNING)
        panic("sched running");
    swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield() {
    current_proc->state = RUNNABLE;
    //add_task(current_proc);
    sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p) {
    if (p->pagetable)
        freepagetable(p->pagetable, p->max_page);
    p->pagetable = 0;
    p->state = UNUSED;
}

int fork() {
    struct proc *np;
    struct proc *p = curr_proc();

    // Allocate process.
    if ((np = allocproc()) == 0) {
        panic("allocproc\n");
    }

    // Copy user memory from parent to child.
    if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
        panic("uvmcopy\n");
    }

    np->max_page = p->max_page;

    // copy saved user registers.
    *(np->trapframe) = *(p->trapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->a0 = 0;

    np->parent = p;
    np->state = RUNNABLE;
    //add_task(np);

    return np->pid;
}

//creates a new child
int spawn(char *name) {
    //get applic name in kernel app list and get internal appID
    int id = get_id_by_name(name);

    //if filename invalid
    if (id < 0) {
        return -1;
    }

    //CH5.2: a child process should have a parent ptr set to the creating process
    struct proc *p = curr_proc();
    //gives child fresh PID, page table, kernel context
    struct proc *np = allocproc();

    //if alloc fails
    if (np == 0) {
        return -1;
    }

    //set new child's par ptr (CH5.2)
    np->parent = p;

    if (loader(id, np) < 0) {
        freeproc(np); //clean up if loading fails
        return -1;
    }

    //add_task(np);

    //ret child's process ID. specs: "return child pid on success"
    return np->pid;
}

//CH5.2: exec() must clean up resources occupied by curr process, then load new executable
int exec(char *name) {
    int id = get_id_by_name(name);

    //if filename invalid
    if (id < 0){
        return -1;
    }
        
    //get curr proc
    struct proc *p = curr_proc();
    //unmap curr process user mem pages and free them
    uvmunmap(p->pagetable, 0, p->max_page, 1);
    //reset curr proc tracked user-mem size after unmapping old program
    p->max_page = 0;
    //load new prgrm into curr proc (5.2: once old mem is reclaimed, call loader to load new program image)
    loader(id, p);

    return 0;
}

int wait(int pid, int *code) {
    struct proc *np;
    int havekids;
    struct proc *p = curr_proc();

    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (np = pool; np < &pool[NPROC]; np++) {
            if (np->state != UNUSED && np->parent == p && (pid <= 0 || np->pid == pid)) {
                havekids = 1;
                if (np->state == ZOMBIE) {
                    int ret_pid = np->pid;
                    int ret_code = np->exit_code;
                    freeproc(np);
                    if (code != 0) *code = ret_code;
                    return ret_pid;
                }
            }
        }

        //ret immediately if no children exist
        if (!havekids) {
            return -1;
        }

        //mark curr par process as ready to run again
        p->state = RUNNABLE;

        //add_task(p);

        sched(); //context switch to scheduler
    }
}

//set priority of the calling process
int set_priority(long long prio) {
    //upper bound satisfied bc prio is long long (specs: prio must be in [2, isize_max])
    if (prio < 2) {
        return -1;
    }

    struct proc *p = curr_proc();
    //update procs priority
    p->priority = prio;
    //recompute pass val after priority change
    p->pass = BIG_STRIDE / p->priority;

    return prio;
}

// Exit the current process.
void exit(int code) {
    struct proc *p = curr_proc();

    p->exit_code = code;

    //freeproc(p);

    //if exiting proc has a parent
    if (p->parent != NULL) {
        //unmap the user mem pages and free the phys mem
        uvmunmap(p->pagetable, 0, p->max_page, 1);
        p->max_page = 0;

        //mar child as a zombie instead of removing
        p->state = ZOMBIE;
    } else {
        freeproc(p); //can be reclaimed immediately
    }

    struct proc *np;
    //scan process table for children of exiting proc
    // any children pting to dying current proc need to be updated
    for (np = pool; np < &pool[NPROC]; np++) {

        //detach child from dying par so they dont pt to proc that DNE
        if (np->parent == p) {
            np->parent = NULL;
        }
    }

    //give control back to schedular so it can choose another runnable proc
    sched();
}