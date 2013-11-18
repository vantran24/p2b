#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "pstat.h"

struct {
	struct spinlock lock;
	struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

struct proc *high[NPROC];	//high queue
struct proc *low[NPROC];		//low queue
int totaltickets = 0;
int hightickets = 0;
int lowtickets = 0;
void
pinit(void)
{
	initlock(&ptable.lock, "ptable");
}

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
	struct proc *p;
	char *sp;

	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		if(p->state == UNUSED)
			goto found;
	release(&ptable.lock);
	return 0;

	found:
	p->state = EMBRYO;
	p->pid = nextpid++;
	p->numTickets = 1;
	p->highticks = 0;
	p->lowticks = 0;
	p->current = 0;
	p->whichqueue = 1;
	release(&ptable.lock);

	// Allocate kernel stack if possible.
	if((p->kstack = kalloc()) == 0){
		p->state = UNUSED;
		return 0;
	}
	sp = p->kstack + KSTACKSIZE;

	// Leave room for trap frame.
	sp -= sizeof *p->tf;
	p->tf = (struct trapframe*)sp;

	// Set up new context to start executing at forkret,
	// which returns to trapret.
	sp -= 4;
	*(uint*)sp = (uint)trapret;

	sp -= sizeof *p->context;
	p->context = (struct context*)sp;
	memset(p->context, 0, sizeof *p->context);
	p->context->eip = (uint)forkret;

	return p;
}

// Set up first user process.
void
userinit(void)
{
	struct proc *p;
	extern char _binary_initcode_start[], _binary_initcode_size[];

	p = allocproc();
	acquire(&ptable.lock);
	initproc = p;
	if((p->pgdir = setupkvm()) == 0)
		panic("userinit: out of memory?");
	inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
	p->sz = PGSIZE;
	memset(p->tf, 0, sizeof(*p->tf));
	p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
	p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
	p->tf->es = p->tf->ds;
	p->tf->ss = p->tf->ds;
	p->tf->eflags = FL_IF;
	p->tf->esp = PGSIZE;
	p->tf->eip = 0;  // beginning of initcode.S

	safestrcpy(p->name, "initcode", sizeof(p->name));
	p->cwd = namei("/");

	p->state = RUNNABLE;
	release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
	uint sz;
	sz = proc->sz;
	if(n > 0){
		if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
			return -1;
	} else if(n < 0){
		if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
			return -1;
	}
	proc->sz = sz;
	switchuvm(proc);
	return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
	int i, pid;
	struct proc *np;
	// Allocate process.
	if((np = allocproc()) == 0)
		return -1;
	// Copy process state from p.
	if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
		kfree(np->kstack);
		np->kstack = 0;
		np->state = UNUSED;
		return -1;
	}
	np->sz = proc->sz;
	np->parent = proc;
	*np->tf = *proc->tf;

	// Clear %eax so that fork returns 0 in the child.
	np->tf->eax = 0;
	for(i = 0; i < NOFILE; i++)
		if(proc->ofile[i])
			np->ofile[i] = filedup(proc->ofile[i]);
	np->cwd = idup(proc->cwd);
	pid = np->pid;
	np->state = RUNNABLE;
	safestrcpy(np->name, proc->name, sizeof(proc->name));
	return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
	struct proc *p;
	int fd;

	if(proc == initproc)
		panic("init exiting");

	// Close all open files.
	for(fd = 0; fd < NOFILE; fd++){
		if(proc->ofile[fd]){
			fileclose(proc->ofile[fd]);
			proc->ofile[fd] = 0;
		}
	}

	iput(proc->cwd);
	proc->cwd = 0;

	acquire(&ptable.lock);

	// Parent might be sleeping in wait().
	wakeup1(proc->parent);

	// Pass abandoned children to init.
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->parent == proc){
			p->parent = initproc;
			if(p->state == ZOMBIE)
				wakeup1(initproc);
		}
	}

	// Jump into the scheduler, never to return.
	proc->state = ZOMBIE;
	sched();
	panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
	struct proc *p;
	int havekids, pid;

	acquire(&ptable.lock);
	for(;;){
		// Scan through table looking for zombie children.
		havekids = 0;
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->parent != proc)
				continue;
			havekids = 1;
			if(p->state == ZOMBIE){
				// Found one.
				pid = p->pid;
				kfree(p->kstack);
				p->kstack = 0;
				freevm(p->pgdir);
				p->state = UNUSED;
				p->pid = 0;
				p->parent = 0;
				p->name[0] = 0;
				p->killed = 0;
				release(&ptable.lock);
				return pid;
			}
		}
		// No point waiting if we don't have any children.
		if(!havekids || proc->killed){
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)
		sleep(proc, &ptable.lock);  //DOC: wait-sleep
	}
}
// Random number generator implementation
// give a random number between 0 and lim
int rand1(int bound)
{
	static long a = 100001;
	a = (a * 125) % 2796203;
	//return ((a % lim) + 1);
	return (a % bound);
}
// sets the number of tickets of the calling process
// by default each process should get one ticket
// this syscall allows processes to raise their ticket total
// which allows a higher portion of CPU
// returns 0 if successful, returns -1 otherwise
int settickets (int num)
{
	if(num < 1 ){
		return -1;
	}
	//proc->numTickets = num;
	return 0; 
}
// Returns basic information about the running process
// returns
// how many times it has choosen to run
// pid
// which queue its in
int getpinfo (struct pstat *info )
{
	int i;
	i = 0;
	struct proc *p;
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->state == UNUSED)
		{
			info->inuse [i] = 0;//zero for unused
			// don't really need to initialize anything else
		}
		else 
			info->inuse [i] = 1;
		info->pid [i] = p -> pid;
		info->hticks[i] = p->highticks;//keep track of ticks in high
		info->lticks [i] = p->lowticks;//keep track of ticks in low
		i++;	       
	}
	release(&ptable.lock);

	return 0;
}
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
// via swtch back to the scheduler.

void
scheduler(void)
{
	struct proc *p;

	int j;
	for (j = 0; j < NPROC; j++)
	{//set everything in queues to NULL
		high[j] = NULL;
		low[j] = NULL;
	}
	acquire(&ptable.lock);
	int i = 0;
	//filling up high queue with all runnable processes
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if(p->state != RUNNABLE)
		{
			continue;
		}
		hightickets++;//increment high tickets
		high[i] = p;//filled the high queue with all runnable processes
		i++;
	}
	release(&ptable.lock);

	for(;;)
	{
		// Enable interrupts on this processor.
		sti();
		int emptyHigh = 0;
		int emptyLow = 0;

		acquire(&ptable.lock); 

		i =0;
		hightickets = 0;
		lowtickets = 0;
		// Loop over process table looking for process to run.
		//refill the queue
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if(p->state != RUNNABLE)
			{
				continue;
			}
			if (p->whichqueue == 1)
			{//in high queue
				emptyHigh = 1;//means high not empty
				high[i] = p;
				hightickets += (high[i]->numTickets);
				i++;
			}
			else if(p->whichqueue == 0)
			{//in low queue
				emptyLow = 1;//means low it not empty
				low [i] = p;
				lowtickets += (low[i]->numTickets);
				i++;
			}
		}

		
		if (emptyHigh == 0 && emptyLow == 0)
		{
			release(&ptable.lock);
			continue;
		}
		else if (emptyHigh == 1)
		{//something in high
			int rannum = rand1(hightickets);//random process choosen

			int pickhighprocess = (high[0]->numTickets);
			j = 0;
			while(pickhighprocess < rannum)
			{
				j++;
				pickhighprocess += (high[j]->numTickets);
			}
			proc = high[j];
			switchuvm(proc);//switching to user kernel page table
			proc ->state = RUNNING;
			proc ->highticks++;
			proc ->current = 1;
			swtch(&cpu->scheduler, proc->context);//context switch, go to new process
			switchkvm();//switching to the kernel page table
			proc ->current = 0;
			if((proc->state) == RUNNABLE)
			{
				p->whichqueue = 0; //put in low queue if it didn't finish running
			}
			proc = 0;
		}
		//high queue is empty
		//go to the low queue
		else if (emptyHigh == 0)
		{
			//run lottery on low queue to p **
			if (emptyLow == 1){//something in low
				int rannum2 = rand1(lowtickets);//random process choosen
				j = 0;
				int picklowprocess = low[0]->numTickets;
				while(picklowprocess < rannum2){
					j++;
					picklowprocess += (low[j]->numTickets);
				}
				proc = low[j];
				switchuvm(p);
				proc->state = RUNNING;
				proc->lowticks++;
				proc->current = 1;
				swtch(&cpu->scheduler, proc->context);//context switch, go to new process
				switchkvm();//comes back to kernel
				proc->current = 0;
				proc = 0;
			}
		}
		
		for (j = 0; j < NPROC; j++)
		{//clear queues to NULL
			high[j] = NULL;
			low[j] = NULL;
		}
		release(&ptable.lock);
	}
}
// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
	int intena;
	if(!holding(&ptable.lock))
		panic("sched ptable.lock");
	if(cpu->ncli != 1)
		panic("sched locks");
	if(proc->state == RUNNING)
		panic("sched running");
	if(readeflags()&FL_IF)
		panic("sched interruptible");
	intena = cpu->intena;
	swtch(&proc->context, cpu->scheduler);
	cpu->intena = intena;
}
// Give up the CPU for one scheduling round.
//might need to change this *****
void 
yield(void)
{
	acquire(&ptable.lock);  //DOC: yieldlock
	proc->state = RUNNABLE;
	sched();
	release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
	// Still holding ptable.lock from scheduler.
	release(&ptable.lock);

	// Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
	if(proc == 0)
		panic("sleep");

	if(lk == 0)
		panic("sleep without lk");

	// Must acquire ptable.lock in order to
	// change p->state and then call sched.
	// Once we hold ptable.lock, we can be
	// guaranteed that we won't miss any wakeup
	// (wakeup runs with ptable.lock locked),
	// so it's okay to release lk.
	if(lk != &ptable.lock){  //DOC: sleeplock0
		acquire(&ptable.lock);  //DOC: sleeplock1
		release(lk);
	}

	// Go to sleep.
	proc->chan = chan;
	proc->state = SLEEPING;
	sched();

	// Tidy up.
	proc->chan = 0;

	// Reacquire original lock.
	if(lk != &ptable.lock){  //DOC: sleeplock2
		release(&ptable.lock);
		acquire(lk);
	}
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
	struct proc *p;

	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		if(p->state == SLEEPING && p->chan == chan)
			p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
	acquire(&ptable.lock);
	wakeup1(chan);
	release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
	struct proc *p;

	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->pid == pid){
			p->killed = 1;
			// Wake process from sleep if necessary.
			if(p->state == SLEEPING)
				p->state = RUNNABLE;
			release(&ptable.lock);
			return 0;
		}
	}
	release(&ptable.lock);
	return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
	static char *states[] = {
			[UNUSED]    "unused",
			[EMBRYO]    "embryo",
			[SLEEPING]  "sleep ",
			[RUNNABLE]  "runble",
			[RUNNING]   "run   ",
			[ZOMBIE]    "zombie"
	};
	int i;
	struct proc *p;
	char *state;
	uint pc[10];

	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->state == UNUSED)
			continue;
		if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
			state = states[p->state];
		else
			state = "???";
		cprintf("%d %s %s", p->pid, state, p->name);
		if(p->state == SLEEPING){
			getcallerpcs((uint*)p->context->ebp+2, pc);
			for(i=0; i<10 && pc[i] != 0; i++)
				cprintf(" %p", pc[i]);
		}
		cprintf("\n");
	}
}


