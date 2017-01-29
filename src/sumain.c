/*
 * sumain.c
 *
 * Copyright (c) 2015-2017, Kristofer Berggren
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the author nor the names of its contributors may
 *       be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/* ----------- Includes ------------------------------------------ */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#define __USE_GNU
#include <dlfcn.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif


/* ----------- Defines ------------------------------------------- */
#define SU_ENV_STDERR   "SU_STDERR"
#define SU_ENV_SYSLOG   "SU_SYSLOG"
#define SU_FILL_BYTE    0xcd
#define SU_FILL_OFFSET  128


/* ----------- Macros -------------------------------------------- */
#define SU_LOG(...)  do { if(su_log_stderr) fprintf(stderr, __VA_ARGS__); \
                          if(su_log_syslog) syslog(LOG_ERR, __VA_ARGS__); \
                        } while(0)
#define SU_LOG_ERR   SU_LOG("%s (pid %d): %s:%d error\n", \
                            su_name, getpid(), __FUNCTION__, __LINE__)
#define SU_LOG_WARN  SU_LOG("%s (pid %d): %s:%d warning\n", \
                            su_name, getpid(), __FUNCTION__, __LINE__)


/* ----------- Types --------------------------------------------- */
typedef struct
{
  void *(*start_routine) (void *);
  void **arg;
  pthread_attr_t *attr;
} su_threadstart_t;

typedef enum
{
  SU_THREAD_MAIN = 0,
  SU_THREAD_CHILD,
} su_threadtype_t;

typedef struct su_threadinfo_s
{
  int id;
  su_threadtype_t threadtype;
  pthread_t pthread;
  pid_t tid;
  void *stack_addr;
  void *stack_end;
  size_t stack_req_size;
  size_t stack_size;
  size_t stack_max_usage;
  size_t guard_size;
  time_t time_start;
  time_t time_stop;
  int time_duration;
  void *func_ptr;
  struct su_threadinfo_s *next;
} su_threadinfo_t;


/* ----------- Local Function Prototypes ------------------------- */
static void *su_start_thread(void *arg);
static void su_thread_init(su_threadtype_t threadtype, pthread_attr_t *rattr,
                           void *func_ptr);
static void su_thread_fini(void *key);
static void su_get_env(void);
static int su_get_stack_growth(char *stack_addr);

static void su_get_stack_usage(struct su_threadinfo_s *threadinfo);
static void su_log_stack_usage(void);


/* ----------- File Global Variables ----------------------------- */
static const char *su_name = "libstackusage";
static int (*real_pthread_create) (pthread_t *thread,
                                   const pthread_attr_t *attr,
                                   void *(*start_routine) (void *),
                                   void *arg) = NULL;
static int su_inited = 0;
static int su_log_stderr = 0;
static int su_log_syslog = 0;
static struct su_threadinfo_s *threadinfo_head = NULL;
static pthread_mutex_t threadinfo_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t key;


/* ----------- Global Functions ---------------------------------- */
void __attribute__ ((constructor)) su_init(void)
{
  if(su_inited == 0)
  {
    /* Get environment variable settings */
    su_get_env();

    /* Get function ptr to real pthread_create */
    real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    if(real_pthread_create == NULL)
    {
      SU_LOG_ERR;
    }

    /* Initialize thread key, to with callback at thread termination */
    pthread_key_create(&key, su_thread_fini);
 
    /* Register main thread */
    su_thread_init(SU_THREAD_MAIN, NULL, NULL);

    /* Store initialization state */
    su_inited = 1;
  }
}


void __attribute__ ((destructor)) su_fini(void)
{
  if(su_inited == 1)
  {
    /* Unegister main thread */
    su_thread_fini(NULL);

    /* Log stack usage in process */
    su_log_stack_usage();

    /* Store initialization state */
    su_inited = 0;
  }
}


int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg)
{
  int rv = -1;
  su_threadstart_t *tstart = NULL;

  if(real_pthread_create)
  {
    /* Perform pthread_create via wrapper */
    tstart = calloc(1, sizeof(su_threadstart_t));
    if(tstart)
    {
      tstart->start_routine = start_routine;
      tstart->arg = arg;
      if(attr)
      {
        tstart->attr = calloc(1, sizeof(pthread_attr_t));
        if(tstart->attr)
        {
          memcpy((void *) tstart->attr, (void *) attr, sizeof(pthread_attr_t));
        }
        else
        {
          SU_LOG_ERR;
        }
      }
      else
      {
        tstart->attr = NULL;
      }
      rv = real_pthread_create(thread, attr, su_start_thread, (void*) tstart);
    }
    else
    {
      SU_LOG_ERR;
    }
  }
  else
  {
    SU_LOG_ERR;
  }

  return rv;
}


/* ----------- Local Functions ----------------------------------- */
static void *su_start_thread(void *startarg)
{
  su_threadstart_t *tstart = (su_threadstart_t *) startarg;
  void *rv = NULL;

  if(tstart)
  {
    /* Register child thread */
    su_thread_init(SU_THREAD_CHILD, tstart->attr, tstart->start_routine);

    /* Start thread routine */
    rv = tstart->start_routine(tstart->arg);

    /* Cleanup */
    if(tstart->attr)
    {
      free(tstart->attr);
      tstart->attr = NULL;
    }
    free(tstart);
    tstart = NULL;
  }
  else
  {
    SU_LOG_ERR;
  }

  return rv;
}


static void su_thread_init(su_threadtype_t threadtype, pthread_attr_t *rattr,
                           void *func_ptr)
{
  struct su_threadinfo_s *threadinfo = NULL;
#ifndef __APPLE__
  pthread_attr_t attr;
#endif
  
  threadinfo = calloc(sizeof(struct su_threadinfo_s), 1);
  if(threadinfo == NULL)
  {
    SU_LOG_WARN;
    return;
  }

  if(threadtype == SU_THREAD_CHILD)
  {
    /* Thread specific (dummy) data */
    pthread_key_t *key_value = calloc(sizeof(pthread_key_t), 1);

    if(key_value)
    {
      pthread_setspecific(key, key_value);

      /* Get requested stack size */
      if(rattr)
      {
        size_t req_size = 0;
        void *req_addr = NULL;
        if(pthread_attr_getstack(rattr, &req_addr, &req_size) == 0)
        {
          threadinfo->stack_req_size = req_size;

          /* If requested stack size 0, the stack uses default size */
          if(threadinfo->stack_req_size == 0)
          {
            size_t stacksize;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            if(pthread_attr_getstacksize(&attr, &stacksize) == 0)
            {
              threadinfo->stack_req_size = stacksize;
            }
            else
            {
              SU_LOG_WARN;
            }
          }
        }
        else
        {
          SU_LOG_WARN;
        }
      }
      else
      {
        /* If not explicitly specified, the stack uses default size */
        size_t stacksize;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if(pthread_attr_getstacksize(&attr, &stacksize) == 0)
        {
          threadinfo->stack_req_size = stacksize;
        }
        else
        {
          SU_LOG_WARN;
        }
      }
    }
    else
    {
      SU_LOG_WARN;
    }
  }
  else if(threadtype == SU_THREAD_MAIN)
  {
    /* For requested stack size in main thread, use default stack size limit */
    struct rlimit rlim;
    if(getrlimit(RLIMIT_STACK, &rlim) == 0)
    {
      threadinfo->stack_req_size = rlim.rlim_cur;
    }
    else
    {
      SU_LOG_WARN;
    }
  }

  /* Store general thread info parameters */
  threadinfo->threadtype = threadtype;
  threadinfo->pthread = pthread_self();
  threadinfo->func_ptr = func_ptr;
#ifdef __linux__
  threadinfo->tid = syscall(SYS_gettid);
#endif
  
  /* Get current/actual stack attributes */
#ifdef __APPLE__
  threadinfo->stack_addr = pthread_get_stackaddr_np(threadinfo->pthread);
  threadinfo->stack_size = pthread_get_stacksize_np(threadinfo->pthread);
  threadinfo->stack_addr -= threadinfo->stack_size;
  threadinfo->guard_size = 0;
#else
  if(pthread_getattr_np(threadinfo->pthread, &attr) == 0)
  {
    size_t stack_size = 0;
    size_t guard_size = 0;
    void *stack_addr = NULL;

    if(pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0)
    {
      threadinfo->stack_addr = stack_addr;
      threadinfo->stack_size = stack_size;
    }
    else
    {
      SU_LOG_WARN;
    }

    if(pthread_attr_getguardsize(&attr, &guard_size) == 0)
    {
      threadinfo->guard_size = guard_size;
    }
    else
    {
      SU_LOG_WARN;
    }
  }
  else
  {
    SU_LOG_WARN;
  }
#endif

  /* Get current stack position (base), and fill unused stack with data */
  {
    char stack_var;
    char *fill_ptr = NULL;

    if(su_get_stack_growth(&stack_var) > 0)
    {
      /* Stack growing upwards / increasing address */

      /* Store end address */
      threadinfo->stack_end = (char *) threadinfo->stack_addr +
        threadinfo->stack_size;
      /* Fill stack starting at offset, for stack that is in use */
      fill_ptr = (&stack_var) + SU_FILL_OFFSET;
      while(fill_ptr < (char *) threadinfo->stack_end)
      {
        *fill_ptr = SU_FILL_BYTE;
        fill_ptr++;
      }
    }
    else
    {
      /* Stack growing downwards / decreasing address */

      /* Guard is included at base of stack, adjust for that */
      threadinfo->stack_addr += threadinfo->guard_size;
      /* Store end address */
      threadinfo->stack_end = (char *) threadinfo->stack_addr +
        threadinfo->stack_size;
      /* Fill stack starting at offset, for stack that is in use */
      fill_ptr = (&stack_var) - SU_FILL_OFFSET;
      while(fill_ptr > (char *) threadinfo->stack_addr)
      {
        *fill_ptr = SU_FILL_BYTE;
        fill_ptr--;
      }
    }
  }

  /* Get current stack usage */
  su_get_stack_usage(threadinfo);

  /* Store start time */
  if(time(&(threadinfo->time_start)) == ((time_t)-1))
  {
    SU_LOG_WARN;
  }

  /* Store thread info in linked list */
  pthread_mutex_lock(&threadinfo_mx);
  threadinfo->next = NULL;
  if(threadinfo_head == NULL)
  {
    threadinfo->id = 0;
    threadinfo_head = threadinfo;
  }
  else
  {
    struct su_threadinfo_s *threadinfo_it = threadinfo_head;
    while(threadinfo_it->next)
    {
      threadinfo_it = threadinfo_it->next;
    }
    threadinfo->id = threadinfo_it->id + 1;
    threadinfo_it->next = threadinfo;
  }
  pthread_mutex_unlock(&threadinfo_mx);
}


static void su_log_stack_usage(void)
{
  struct su_threadinfo_s *threadinfo_it = NULL;
  pthread_mutex_lock(&threadinfo_mx);
  threadinfo_it = threadinfo_head;
  SU_LOG("%s log start -------------------------------------------------\n",
         su_name);
  SU_LOG("  pid  id    tid  requested     actual     maxuse  max%%    dur"
         "  funcP\n");
  while(threadinfo_it)
  {
    int usage_percent = 0;
    if(threadinfo_it->stack_req_size > 0)
    {
      usage_percent =
        (int) (threadinfo_it->stack_max_usage * 100) /
        (int) threadinfo_it->stack_req_size;
    }

    SU_LOG("%5d %3d  %5d  %9d  %9d  %9d   %3d  %5d  %p\n",
           getpid(),
           threadinfo_it->id, 
           threadinfo_it->tid,
           (int) threadinfo_it->stack_req_size,
           (int) threadinfo_it->stack_size,
           (int) threadinfo_it->stack_max_usage,
           (int) usage_percent,
           threadinfo_it->time_duration,
           threadinfo_it->func_ptr
          );

    threadinfo_it = threadinfo_it->next;
  }
  SU_LOG("%s log end ---------------------------------------------------\n",
         su_name);
  pthread_mutex_unlock(&threadinfo_mx);
}


static int __attribute__ ((noinline)) su_get_stack_growth(char *stack_addr)
{
  char new_stack_var;
  return (int)((&new_stack_var) - stack_addr);
}


static void su_get_stack_usage(struct su_threadinfo_s *threadinfo)
{
  char stack_var;
  char *read_ptr = NULL;
  if(su_get_stack_growth(&stack_var) > 0)
  {
    read_ptr = (char *)(threadinfo->stack_end) - 1;
    while((*read_ptr & 0xff) == SU_FILL_BYTE)
    {
      read_ptr--;
    }
    threadinfo->stack_max_usage = read_ptr - (char *)threadinfo->stack_addr;
  }
  else
  {
    read_ptr = (char *)(threadinfo->stack_addr) + 1;
    while((*read_ptr & 0xff) == SU_FILL_BYTE)
    {
      read_ptr++;
    }
    threadinfo->stack_max_usage = (char *)threadinfo->stack_end - read_ptr;
  }
}


static struct su_threadinfo_s *su_get_threadinfo_by_pthread(pthread_t pthread)
{
  struct su_threadinfo_s *threadinfo = NULL;
  pthread_mutex_lock(&threadinfo_mx);
  if(threadinfo_head)
  {
    struct su_threadinfo_s *threadinfo_it = threadinfo_head;
    while(threadinfo_it)
    {
      if(threadinfo_it->pthread == pthread)
      {
        threadinfo = threadinfo_it;
        break;
      }
      threadinfo_it = threadinfo_it->next;
    }
  }
  pthread_mutex_unlock(&threadinfo_mx);
  return threadinfo;
}


static void su_thread_fini(void *key)
{
  /* Find current thread in list */
  struct su_threadinfo_s *threadinfo = NULL;
  threadinfo = su_get_threadinfo_by_pthread(pthread_self());

  pthread_mutex_lock(&threadinfo_mx);

  /* Update its stack usage info */
  su_get_stack_usage(threadinfo);

  /* Store stop time and calculate duration */
  if(time(&(threadinfo->time_stop)) != ((time_t)-1))
  {
    threadinfo->time_duration = threadinfo->time_stop -
      threadinfo->time_start;
  }
  else
  {
    SU_LOG_WARN;
  }

  if(key)
  {
    /* Free key */
    free(key);
    key = NULL;
  }

  pthread_mutex_unlock(&threadinfo_mx);
}


static void su_get_env(void)
{
  if(getenv(SU_ENV_STDERR))
  {
    su_log_stderr = 1;
  }

  if(getenv(SU_ENV_SYSLOG))
  {
    su_log_syslog = 1;
  }
}

