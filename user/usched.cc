#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <pthread.h>
typedef uint64_t u64;


#include "util.h"
#include "percpu.hh"
#include "sched.hh"
#include "queue.h"

static int nprocs = 2;
static int the_time = 2;
static uint64_t start;
static volatile int go;

percpu<uint64_t> ops;

struct proc : public sched_link
{
  char x[256];
  LIST_ENTRY(proc) link;
};

//percpu<proc_list> proc_list;

struct stuff {
  bool     flipper;
  uint32_t seed;
  LIST_HEAD(proc_list, proc) proc_list;

  uint32_t rnd() {
    return ::rnd(&seed);
  }

  proc* allocproc() {
    if (LIST_EMPTY(&proc_list))
      die("allocproc");
    proc* p = LIST_FIRST(&proc_list);
    LIST_REMOVE(p, link);
    return p;
  }

  void freeproc(proc* p) {
    LIST_INSERT_HEAD(&proc_list, p, link);
  }

  void
  fillproc(void)
  {
    for (int i = 0; i < 20; i++) {
      proc* p = new proc();
      freeproc(p);
    }
  }
};
percpu<stuff> stuff_;

class schedule
{
public:
  schedule();
  void enq(proc* entry);
  proc* deq();
  proc* steal(bool nonexec);
  void dump();

  volatile u64 ncansteal_ __mpalign__;

private:
  pthread_spinlock_t lock_;
  sched_link head_;
};
percpu<schedule> schedule_;

static __thread unsigned myid_;

int
mycpuid(void)
{
  return myid_;
}

static bool
cansteal(proc* p, bool nonexec)
{
  return true;
}

static void
sighandler(int x)
{
  double tot;
  uint64_t stop;

  go = 2;

  stop = read_tsc();
  tot = 0;
  for (int i = 0; i < NCPU; i++) {
    tot += ops[i];
    printf("  %lu\n", schedule_[i].ncansteal_);
  }

  printf("%f\n", (stop-start)/(tot/NCPU));
}

schedule::schedule(void)
{
  head_.next = &head_;
  head_.prev = &head_;
  ncansteal_ = 0;
  pthread_spin_init(&lock_, 0);
}

void
schedule::enq(proc* p)
{
  sched_link* entry = p;
  // Add to tail
  pthread_spin_lock(&lock_);
  entry->next = &head_;
  entry->prev = head_.prev;
  head_.prev->next = entry;
  head_.prev = entry;
  if (cansteal((proc*)entry, true))
    ncansteal_++;
  pthread_spin_unlock(&lock_);
}

proc*
schedule::deq(void)
{   
  if (head_.next == &head_)
    return nullptr;

  // Remove from head
  pthread_spin_lock(&lock_);
  sched_link* entry = head_.next;
  if (entry == &head_) {
    pthread_spin_unlock(&lock_);
    return nullptr;
  }
  
  entry->next->prev = entry->prev;
  entry->prev->next = entry->next;
  if (cansteal((proc*)entry, true))
    --ncansteal_;
  pthread_spin_unlock(&lock_);
  return (proc*)entry;
}

proc*
schedule::steal(bool nonexec)
{
  if (ncansteal_ == 0 || pthread_spin_trylock(&lock_))
    return nullptr;

  for (sched_link* ptr = head_.next; ptr != &head_; ptr = ptr->next)
    if (cansteal((proc*)ptr, nonexec)) {
      ptr->next->prev = ptr->prev;
      ptr->prev->next = ptr->next;
      --ncansteal_;
      pthread_spin_unlock(&lock_);
      return (proc*)ptr;
    }
  pthread_spin_unlock(&lock_);
  return nullptr;
}

static void
runit(proc* p)
{
  p->x[0]   += myid_;
  p->x[128] += myid_;
  p->x[64]  += myid_;
  p->x[192] += myid_;

  spin_delay(500);
}

static proc*
stealit(void)
{
  proc* p;

  for (int i = 0; i < NCPU; i++) {
    if (i == myid_)
      break;
    p = schedule_[i].steal(true);
    if (p) {
      return p;
    }
  }
  return nullptr;
}

static void
schedit(void)
{
  uint32_t r;
  proc* p;

  p = schedule_->deq();
  if (p == nullptr)
    p = stealit();

  r = stuff_->rnd() % 100;
  if (p) {
    runit(p);
    (*ops)++;

    if (r < 10 && !stuff_->flipper) {
      stuff_->flipper = !stuff_->flipper;
      stuff_->freeproc(p);
    }
    else
      schedule_->enq(p);
  }

  if (r >= 10 && r < 20 && stuff_->flipper) {
    stuff_->flipper = !stuff_->flipper;
    schedule_->enq(stuff_->allocproc());
  }
}

static void*
worker(void* x)
{
  myid_ = (long)x;
  setaffinity(myid_);
  stuff_->seed = myid_+getpid();
  stuff_->fillproc();

  if (myid_ == 0) {
    for (int i = 0; i < nprocs; i++)
      schedule_->enq(new proc());

    if (signal(SIGALRM, sighandler) == SIG_ERR)
      edie("signal failed\n");
    alarm(the_time);
    start = read_tsc();

    go = 1;
  } else {
    while (go == 0)
      ;
  }

  while (go == 1) {
    schedit();
  }

  return nullptr;
}

int
main(int ac, char** av)
{
  myid_ = 0;
  setaffinity(myid_);

  for (int i = 1; i < NCPU; i++) {
    pthread_t th;
    if (pthread_create(&th, nullptr, worker, (void*)(long)i) < 0)
      edie("pthread_create");
  }

  worker((void*)0);
  return 0;
}