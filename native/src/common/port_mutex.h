/* 平台互斥锁垫片：MSVC 无 pthread.h，用 SRWLOCK；POSIX/MinGW 保持 pthread
 * 路径不变（MinGW 的 winpthreads 已在 macOS/Linux/MinGW 构建中验证）。
 * 本垫片只覆盖库内的静态初始化互斥锁场景，不替代 tpool 的线程池抽象。 */
#ifndef TOPOS_PORT_MUTEX_H
#define TOPOS_PORT_MUTEX_H

#if defined(_MSC_VER)
#include <windows.h>

typedef SRWLOCK topos_mutex;
#define TOPOS_MUTEX_INIT SRWLOCK_INIT

static inline void topos_mutex_lock(topos_mutex* m)
{
    AcquireSRWLockExclusive(m);
}

static inline void topos_mutex_unlock(topos_mutex* m)
{
    ReleaseSRWLockExclusive(m);
}
#else
#include <pthread.h>

typedef pthread_mutex_t topos_mutex;
#define TOPOS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER

static inline void topos_mutex_lock(topos_mutex* m) { pthread_mutex_lock(m); }
static inline void topos_mutex_unlock(topos_mutex* m)
{
    pthread_mutex_unlock(m);
}
#endif

#endif /* TOPOS_PORT_MUTEX_H */
