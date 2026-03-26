/*
 * Minimal pthreads compatibility stub for Windows
 * Provides basic mutex and thread ID functionality for libssh
 */

#include <winsock2.h>
#include <windows.h>
#include <process.h>

/* pthread mutex types */
typedef CRITICAL_SECTION pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef unsigned long pthread_t;

/* pthread mutex functions */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    InitializeCriticalSection(mutex);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    DeleteCriticalSection(mutex);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    EnterCriticalSection(mutex);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    LeaveCriticalSection(mutex);
    return 0;
}

/* pthread thread functions */
pthread_t pthread_self(void)
{
    return (pthread_t)GetCurrentThreadId();
}

/* Stub for if_nametoindex - must be __stdcall to match Winsock import convention */
unsigned int __stdcall if_nametoindex(const char *ifname)
{
    /* Not implemented - return 0 */
    return 0;
}
