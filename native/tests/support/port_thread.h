/* 测试线程垫片：MSVC 无 pthread.h。x64 Windows 只有一种调用约定，且
 * 本仓库测试线程均返回 NULL、返回值不被检查，_beginthreadex 即可满足
 * pthread_create/join 语义（join 后即关句柄，等价分离）。 */
#ifndef TOPOS_PORT_THREAD_H
#define TOPOS_PORT_THREAD_H

#if defined(_MSC_VER)
#include <windows.h>
#include <process.h>

typedef uintptr_t pthread_t;

static int pthread_create(pthread_t* t, const void* attr,
                          void* (*fn)(void*), void* arg)
{
    (void)attr;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0u,
                                      (unsigned(__stdcall*)(void*))fn, arg,
                                      0u, NULL);
    if (h == NULL) { return -1; }
    *t = (pthread_t)h;
    return 0;
}

static int pthread_join(pthread_t t, void** retval)
{
    (void)retval;
    if (WaitForSingleObject((HANDLE)t, INFINITE) != WAIT_OBJECT_0) { return -1; }
    CloseHandle((HANDLE)t);
    return 0;
}
#else
#include <pthread.h>
#endif

#endif /* TOPOS_PORT_THREAD_H */
