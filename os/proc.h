#ifndef PROC_H
#define PROC_H

#define MAX_SYSCALL_NUM 500

#define BIG_STRIDE 65536 //CH5: "a good number for BIG_STRIDE is 65536"

#include "riscv.h"
#include "types.h"
#include "queue.h"

#define NPROC (512)
#define FD_BUFFER_SIZE (16)

struct file;

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	pagetable_t pagetable; // User page table
	uint64 ustack; //virtual addr of kernel stack
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	uint64 max_page;
	struct proc *parent; // Parent process

	//!!
	uint64 exit_code; //stores the code a proc passes to exit(code)

	long long priority; //the proc's priority (>=2) higher priority = more CPU time
	uint64 stride; //stores a process's curr stride total
	uint64 pass; //stores how much the stride should incr each time it runs

	struct file *files[FD_BUFFER_SIZE]; //file desc table for the process (each proc can have an arrray of open file ptrs)

	//mark the time process started being scheduled
    uint64 start_time;                          //cycle counting
    unsigned int syscall_times[MAX_SYSCALL_NUM]; //syscall counter by id
};

typedef enum {
    UnInit,
    Ready,
    Running,
    Exited,
} TaskStatus;

typedef struct {
    TaskStatus status;
    unsigned int syscall_times[MAX_SYSCALL_NUM];
    int time;
} TaskInfo;

struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();

//!!
int fork();
int exec(char *);
int wait(int, int *);
int spawn(char *);
void add_task(struct proc *);

//struct proc *pop_task();

int set_priority(long long);
struct proc *fetch_task();
struct proc *allocproc();
//--

// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H