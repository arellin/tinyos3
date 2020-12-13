
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"  //added it to include PTCB structure
#include "kernel_threads.h"


/*
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

static file_ops procinfo_ops = {
  .Open = NULL,
  .Read = procinfo_read,
  .Write = procinfo_write,
  .Close = procinfo_close
};

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;

  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  newproc->thread_count=0; // Initialize thread_count if process created
  rlnode_init(&newproc->ptcb_list,NULL);  // NULL cause its the head!

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process)
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;
//TODO: Init PTCB_list & thread_count

  /*
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
    newproc->main_thread = spawn_thread(newproc, start_main_thread);

    newproc->thread_count++;

    /*
    If call==NULL then no need of TCB creation and no nead of PTCB
    ->If above is wrong how we pass TCB to PTCB?
    */
    PTCB* ptcb_new;  //the address of the new PTCB
    ptcb_new=new_ptcb(call,argl,args);

    //TODO: check if line below is needed
    // rlnode_init(&ptcb_new->ptcb_list_node, ptcb_new);  //or can point to parent PCB
    ptcb_new->tcb=newproc->main_thread;  // link PTCB--->TCB

    newproc->main_thread->ptcb = ptcb_new;  // link PTCB<-----TCB

    rlist_push_back(&newproc->ptcb_list, &ptcb_new->ptcb_list_node);  // CAREFULL: link PCB--->PTCB

    wakeup(newproc->main_thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);

  cleanup_zombie(child, status);

finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  /*
    Here, we must check that we are not the init task.
    If we are, we must wait until all child processes exit.
   */
  if(get_pid(curproc)==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

  sys_ThreadExit(exitval);
}


Fid_t sys_OpenInfo()
{
  Fid_t fid;
  FCB*  fcb;

  if(!FCB_reserve(1,&fid,&fcb)){
    return NOFILE;  // not -1 because nofile is handled without throwing an exception in SysInfo()
  }

  procinfo* proc_info = init_procinfo();

  fcb->streamobj = proc_info;  // Link FCB--->procinfo struct

  fcb->streamfunc = &procinfo_ops;  // Link FCB--->procinfo_ops

  return fid;
}


procinfo* init_procinfo(){

  procinfo* info= (procinfo*)xmalloc(sizeof(procinfo));
  /*set everything to 0*/
  info->PT_cursor = 0;
  info->pid = 0;
  info->ppid = 0;
  info->alive = 0;
  info->thread_count = 0;
  info->argl = 0;
  //info->args = '';  // causes weird behaviour
  return info;
}


int procinfo_read(void* procinfo_arg, char *buf, unsigned int size){
  /*the cast is neccessary from a void pointer*/
  procinfo* proc_info  = (procinfo*)procinfo_arg;

  /*might be possible to do just with the procinfo from the arguments*/
  procinfo* new_proc_info = init_procinfo();

  if (proc_info->PT_cursor == (MAX_PROC-1)  || proc_info == NULL  || buf == NULL)
    return -1;

  /*get the "current" pcb*/
  PCB* current_pcb = &PT[proc_info->PT_cursor];

  /*bypass every non-active pcb*/
  while(current_pcb->pstate == FREE) {
    if (proc_info->PT_cursor == (MAX_PROC-1))
      return -1;  // could be 0? or not??
    proc_info->PT_cursor++;
    current_pcb = &PT[proc_info->PT_cursor];
  }

  /*start filling the proc_info struct with the data of the current PCB*/
  new_proc_info->pid = get_pid(current_pcb);
  new_proc_info->ppid = get_pid(current_pcb->parent);  // get the pid of the parent 

  if(current_pcb->pstate == ZOMBIE)
    new_proc_info->alive = 0;
  else
    new_proc_info->alive = 1;

  new_proc_info->thread_count = current_pcb->thread_count;
  new_proc_info->main_task = current_pcb->main_task;

  /*get args and argv*/


  proc_info->PT_cursor++;


  /*memcpy new_proc_info as byte array to the buff given from the arguments*/
  memcpy(buf,(char*)new_proc_info,sizeof(new_proc_info));

  free(new_proc_info);

  return sizeof(new_proc_info);
}


int procinfo_write(void* procinfo, const char *buf, unsigned int size){
  return -1;
}


int procinfo_close(void* info){         
  procinfo* proc_info = (procinfo*) info;  

  if (proc_info == NULL)  // if already NULL we may not be able to free it
    return -1;  //signal failure

  free(proc_info);

  return 0;
}


