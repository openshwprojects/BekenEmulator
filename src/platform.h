/* Operating-system services the emulator needs, behind one small interface:
 * threads and locks, clocks, BSD-style sockets, and child processes with a
 * captured stdout (the self-test harness). Win32 and POSIX implementations.
 */
#ifndef BK_PLATFORM_H
#define BK_PLATFORM_H

#include "common.h"

/* ------------------------------------------------------------- threading */
class Mutex {
public:
    Mutex();
    ~Mutex();
    void lock();
    void unlock();
private:
    void *impl;
    Mutex(const Mutex &);
    Mutex &operator=(const Mutex &);
};

class ScopedLock {
public:
    explicit ScopedLock(Mutex &m) : m_(m) { m_.lock(); }
    ~ScopedLock() { m_.unlock(); }
private:
    Mutex &m_;
    ScopedLock(const ScopedLock &);
    ScopedLock &operator=(const ScopedLock &);
};

/* A manual-reset event, like threading.Event */
class Event {
public:
    Event(bool initially_set = false);
    ~Event();
    void set();
    void clear();
    bool is_set();
    bool wait(int timeout_ms = -1); /* true if set */
private:
    void *impl;
    Event(const Event &);
    Event &operator=(const Event &);
};

typedef void (*ThreadFunc)(void *arg);

class Thread {
public:
    Thread();
    ~Thread();
    bool start(ThreadFunc fn, void *arg);
    bool join(int timeout_ms = -1); /* true once the thread has finished */
    bool is_alive();
    bool started() const { return handle != 0; }
    bool is_current();
private:
    void *handle;
    unsigned long id;
    ThreadFunc fn_;
    void *arg_;
    volatile bool done;
    static
#ifdef _WIN32
    unsigned long __stdcall
#else
    void *
#endif
    trampoline(void *self);
    Thread(const Thread &);
    Thread &operator=(const Thread &);
};

/* Interlocked integer helpers for flags shared between threads */
long atomic_get(volatile long *p);
void atomic_set(volatile long *p, long v);

void sleep_ms(int ms);
double mono_time();  /* seconds, monotonic */
double wall_time();  /* seconds since the Unix epoch, sub-second precision */
std::string utc_timestamp(); /* "YYYY-MM-DD HH:MM UTC" */

/* --------------------------------------------------------------- sockets */
typedef long long sock_t;
#define BK_BAD_SOCK ((sock_t)-1)

bool net_init();
sock_t tcp_listen(const char *host, int port, std::string *err); /* exclusive, non-blocking */
sock_t tcp_accept(sock_t s);                                     /* -1 if none ready */
int tcp_bound_port(sock_t s);
/* blocking connect with a timeout; returns a NON-blocking socket or -1 */
sock_t tcp_connect(const std::string &ip, int port, int timeout_ms);
/* blocking connect for clients (harness/UI) with recv timeout; blocking socket */
sock_t tcp_connect_blocking(const std::string &ip, int port, int timeout_ms);
sock_t udp_socket();
sock_t udp_bind(const char *host, int port);   /* bound, non-blocking; port 0 = any (see tcp_bound_port) */
/* >0 bytes, 0 = orderly close, -1 = would block, -2 = error */
int sock_recv(sock_t s, char *buf, int len);
int sock_send(sock_t s, const char *buf, int len);
int sock_sendto(sock_t s, const char *buf, int len, const u8 ip[4], int port);
int sock_recvfrom(sock_t s, char *buf, int len, u8 ip[4], int *port);
bool sock_send_all(sock_t s, const Bytes &data); /* blocking socket */
void sock_shutdown_wr(sock_t s);
void sock_close(sock_t s);
void sock_set_nonblocking(sock_t s);
/* select() over read/write sets; returns false on error */
bool sock_select(const std::vector<sock_t> &rd, const std::vector<sock_t> &wr,
                 std::vector<sock_t> &rd_ready, std::vector<sock_t> &wr_ready, int timeout_ms);
bool resolve_host(const std::string &name, u8 ip[4]);
int free_tcp_port();

/* ------------------------------------------------------------- processes */
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();
    /* argv[0] is the program path; stdout+stderr merged into one pipe */
    bool start(const StrList &argv, const std::vector<std::pair<std::string, std::string> > &extra_env);
    /* read whatever is available; returns false at EOF (pipe closed) */
    bool read_some(std::string &out, int timeout_ms);
    bool running();
    int exit_code();
    void kill();
    void wait(int timeout_ms);
private:
    void *proc;
    void *pipe_rd;
    int pid;
    int fd;
    int status;
    bool exited;
    ChildProcess(const ChildProcess &);
    ChildProcess &operator=(const ChildProcess &);
};

std::string self_exe_path();
std::string current_dir();
bool ensure_bundled_firmwares(std::string &out_root);

#endif
