/* Livadix — the host's sockets, threads and clock for src/net (Windows: Winsock
 * 2 and Win32 threads; elsewhere: BSD sockets and POSIX threads).
 *
 * Livadix's own code. */
#ifndef MC_NET_SYS_H
#define MC_NET_SYS_H

#ifdef _WIN32
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024     /* select() over every NAT socket (Winsock's default is 64) */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET mc_socket;
#define MC_BAD_SOCKET INVALID_SOCKET
#define mc_close_socket closesocket
static inline int mc_socket_error(void) { return WSAGetLastError(); }
static inline int mc_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
static inline int mc_set_nonblocking(mc_socket s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
typedef CRITICAL_SECTION mc_mutex;
static inline void mc_mutex_init(mc_mutex *m) { InitializeCriticalSection(m); }
static inline void mc_mutex_free(mc_mutex *m) { DeleteCriticalSection(m); }
static inline void mc_mutex_lock(mc_mutex *m) { EnterCriticalSection(m); }
static inline void mc_mutex_unlock(mc_mutex *m) { LeaveCriticalSection(m); }
typedef CONDITION_VARIABLE mc_cond;
static inline void mc_cond_init(mc_cond *c) { InitializeConditionVariable(c); }
static inline void mc_cond_free(mc_cond *c) { (void)c; }
static inline void mc_cond_wait(mc_cond *c, mc_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void mc_cond_signal(mc_cond *c) { WakeConditionVariable(c); }
static inline void mc_cond_wait_ms(mc_cond *c, mc_mutex *m, int ms) { SleepConditionVariableCS(c, m, (DWORD)ms); }
typedef HANDLE mc_thread;
typedef DWORD(WINAPI *mc_thread_fn)(void *);
#define MC_THREAD_RETURN DWORD WINAPI
static inline int mc_thread_start(mc_thread *t, mc_thread_fn fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t != NULL;
}
static inline void mc_thread_join(mc_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static inline unsigned long long mc_clock_ms(void) { return GetTickCount64(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int mc_socket;
#define MC_BAD_SOCKET (-1)
#define mc_close_socket close
static inline int mc_socket_error(void) { return errno; }
static inline int mc_would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY; }
static inline int mc_set_nonblocking(mc_socket s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
typedef pthread_mutex_t mc_mutex;
static inline void mc_mutex_init(mc_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void mc_mutex_free(mc_mutex *m) { pthread_mutex_destroy(m); }
static inline void mc_mutex_lock(mc_mutex *m) { pthread_mutex_lock(m); }
static inline void mc_mutex_unlock(mc_mutex *m) { pthread_mutex_unlock(m); }
typedef pthread_cond_t mc_cond;
static inline void mc_cond_init(mc_cond *c) { pthread_cond_init(c, NULL); }
static inline void mc_cond_free(mc_cond *c) { pthread_cond_destroy(c); }
static inline void mc_cond_wait(mc_cond *c, mc_mutex *m) { pthread_cond_wait(c, m); }
static inline void mc_cond_signal(mc_cond *c) { pthread_cond_signal(c); }
static inline void mc_cond_wait_ms(mc_cond *c, mc_mutex *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, m, &ts);
}
typedef pthread_t mc_thread;
typedef void *(*mc_thread_fn)(void *);
#define MC_THREAD_RETURN void *
static inline int mc_thread_start(mc_thread *t, mc_thread_fn fn, void *arg) { return pthread_create(t, NULL, fn, arg) == 0; }
static inline void mc_thread_join(mc_thread t) { pthread_join(t, NULL); }
static inline unsigned long long mc_clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}
#endif

#endif
