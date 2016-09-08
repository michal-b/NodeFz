#include "scheduler.h"

/* Include the various schedule implementations. */
#include "scheduler_CBTree.h"
#include "scheduler_Fuzzing_Timer.h"
#include "scheduler_Fuzzing_ThreadOrder.h"

#include "list.h"
#include "map.h"
#include "mylog.h"
#include "timespec_funcs.h"
#include "synchronization.h"

#include "unix/internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

/* Functions for scheduler typedefs. */

char *scheduler_type_strings[SCHEDULER_MODE_MAX - SCHEDULER_MODE_MIN + 1] = 
  {
    "CBTREE",
    "FUZZER_TIMER",
    "FUZZER_THREAD_ORDER"
  };

const char * scheduler_type_to_string (scheduler_type_t type)
{
  char *str = NULL;
  assert(SCHEDULER_MODE_MIN <= type && type < SCHEDULER_MODE_MAX);
  str = scheduler_type_strings[type - SCHEDULER_MODE_MIN];
  return str;
}

char *scheduler_mode_strings[SCHEDULER_MODE_MAX - SCHEDULER_MODE_MIN + 1] = 
  {
    "RECORD",
    "REPLAY"
  };
  
const char * scheduler_mode_to_string (scheduler_mode_t mode)
{
  char *str = NULL;
  assert(SCHEDULER_MODE_MIN <= mode && mode < SCHEDULER_MODE_MAX);
  str = scheduler_mode_strings[mode - SCHEDULER_MODE_MIN];
  return str;
}

char *thread_type_strings[SCHEDULER_MODE_MAX - SCHEDULER_MODE_MIN + 1] = 
  {
    "LOOPER",
    "THREADPOOL"
  };
  
const char * thread_type_to_string (thread_type_t type)
{
  char *str = NULL;
  assert(SCHEDULER_MODE_MIN <= type && type < SCHEDULER_MODE_MAX);
  str = thread_type_strings[type - SCHEDULER_MODE_MIN];
  return str;
}

char *schedule_point_strings[SCHEDULER_MODE_MAX - SCHEDULER_MODE_MIN + 1] = 
  {
    "BEFORE_EXEC_CB",
    "AFTER_EXEC_CB",

    "TP_BEFORE_GET_WORK",
    "TP_AFTER_GET_WORK",

    "TP_BEFORE_PUT_DONE",
    "TP_AFTER_PUT_DONE",
  };

const char * schedule_point_to_string (schedule_point_t point)
{
  char *str = NULL;
  assert(SCHEDULER_MODE_MIN <= type && type < SCHEDULER_MODE_MAX);
  str = schedule_point_strings[type - SCHEDULER_MODE_MIN];
  return str;
}

/* Scheduler. */

static int SCHEDULER_MAGIC = 8675309; /* Jenny. */

int scheduler_initialized = 0;
struct
{
  int magic;

  /* Constants. */
  schedule_type_t type;
  scheduler_mode_t mode;
  char schedule_file[1024];
  void *args;

  /* Things we can track ourselves. */
  int n_executed;
  struct map *tidToType;

  /* Synchronization. */
  reentrant_mutex_t mutex;

  /* Implementation-dependent. */
  schedulerImpl_t impl;
  void *implDetails;
} scheduler;

/***********************
 * Private scheduler API declarations
 ***********************/

int scheduler__looks_valid (void);

/***********************
 * Public scheduler API definitions
 ***********************/

void scheduler_init (scheduler_type_t type, scheduler_mode_t mode, char *schedule_file, void *args)
{
  assert(!initialized);

  /* Shared amongst all scheduler implementations. */
  scheduler.magic = SCHEDULER_MAGIC;
  scheduler.type = type;
  scheduler.mode = mode;
  strncpy(scheduler.schedule_file, schedule_file, sizeof scheduler.schedule_file);
  scheduler.args = args;

  scheduler.n_executed = 0;
  scheduler.tidToType = map_create();
  assert(scheduler.tidToType != NULL);

  reentrant_mutex_init(&scheduler.mutex);

  /* Specifics based on the scheduler type. */
  switch (scheduler.type)
  {
    case SCHEDULER_TYPE_CBTREE:
#if defined(ENABLE_SCHEDULER_CBTREE)
      scheduler_cbTree_init(mode, args, &scheduler.impl, &scheduler.implDetails);
      break;
#endif
    SCHEDULER_TYPE_FUZZER_TIMER:
#if defined(ENABLE_SCHEDULER_FUZZER_TIMER)
      scheduler_fuzzer_timer_init(mode, args, &scheduler.impl, &scheduler.implDetails);
      break;
#endif
    SCHEDULER_TYPE_FUZZER_THREAD_ORDER:
#if defined(ENABLE_SCHEDULER_FUZZER_THREAD_ORDER)
      scheduler_fuzzer_threadOrder_init(mode, args, &scheduler.impl, &scheduler.implDetails);
      break;
#endif
    default:
      assert(!"How did we get here?");
  }

  return;
}

void scheduler_register_thread (thread_type_t type)
{
  assert(scheduler__looks_valid());
  map_insert(scheduler.tidToType, (int) uv_thread_self(), (void *) type);
  return;
}

void scheduler_register_lcbn (lcbn_t *lcbn)
{
  assert(scheduler__looks_valid());
  scheduler.impl.register_lcbn(lcbn, scheduler.implDetails);
  return;
}

enum callback_type scheduler_next_lcbn_type (void)
{
  enum callback_type ret;
  assert(scheduler__looks_valid());
  
  ret = scheduler.impl.next_lcbn_type(scheduler.implDetails);
  return ret;
}

void scheduler_thread_yield (schedule_point_t point, void *pointDetails)
{
  assert(scheduler__looks_valid());
  scheduler.impl.thread_yield(point, pointDetails, scheduler.implDetails);
  return;
}

char * scheduler_emit (void)
{
  char output_file[1024];
  assert(scheduler__looks_valid());

  strcpy(output_file, scheduler.schedule_file);
  if (scheduler.mode == SCHEDULER_MODE_REPLAY)
    strcat(output_file, "-replay");
  scheduler.impl.emit(output_file, scheduler.implDetails);
  return output_file;
}

int scheduler_lcbns_remaining (void)
{
  int n_remaining = 0;

  assert(scheduler__looks_valid());
  n_remaining = scheduler.impl.lcbns_remaining(scheduler.implDetails);
  return n_remaining;
}

int scheduler_schedule_has_diverged (void)
{
  int has_diverged = 0;
  assert(scheduler__looks_valid());
  has_diverged = scheduler.impl.has_diverged(scheduler.implDetails);
  return has_diverged;
}

int scheduler_n_executed (void)
{
  assert(scheduler__looks_valid());
  /* Not thread-safe, but monotonically increasing so NBD. */
  return scheduler.n_executed;
}

scheduler_mode_t scheduler_get_scheduler_mode (void)
{
  assert(scheduler__looks_valid());
  return scheduler.mode;
}

/***********************
 * "Protected" scheduler API definitions.
 ***********************/

void scheduler__lock (void)
{
  reentrant_mutex_lock(&scheduler.mutex);
}

void scheduler__unlock (void)
{
  reentrant_mutex_unlock(&scheduler.mutex);
}

thread_type_t scheduler__get_thread_type (uv_thread_t tid)
{
  int found = 0;
  thread_type_t type;
  
  type = (thread_type_t) map_lookup(scheduler.tidToType, (int) tid, &found);
  assert(found);

  return type;
}

/***********************
 * Private scheduler API definitions.
 ***********************/

int scheduler__looks_valid (void)
{
  return (scheduler_initialized &&
          scheduler.magic == SCHEDULER_MAGIC);
}
