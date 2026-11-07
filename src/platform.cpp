#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

/* ============================================================== threading */
#ifdef _WIN32
Mutex::Mutex() { CRITICAL_SECTION *cs = new CRITICAL_SECTION; InitializeCriticalSection(cs); impl = cs; }
Mutex::~Mutex() { DeleteCriticalSection((CRITICAL_SECTION *)impl); delete (CRITICAL_SECTION *)impl; }
void Mutex::lock() { EnterCriticalSection((CRITICAL_SECTION *)impl); }
void Mutex::unlock() { LeaveCriticalSection((CRITICAL_SECTION *)impl); }

Event::Event(bool initially_set) { impl = CreateEventW(0, TRUE, initially_set ? TRUE : FALSE, 0); }
Event::~Event() { CloseHandle((HANDLE)impl); }
void Event::set() { SetEvent((HANDLE)impl); }
void Event::clear() { ResetEvent((HANDLE)impl); }
bool Event::is_set() { return WaitForSingleObject((HANDLE)impl, 0) == WAIT_OBJECT_0; }
bool Event::wait(int timeout_ms)
{
    return WaitForSingleObject((HANDLE)impl, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) == WAIT_OBJECT_0;
}

Thread::Thread() : handle(0), id(0), fn_(0), arg_(0), done(false) {}
Thread::~Thread()
{
    if (handle)
        CloseHandle((HANDLE)handle);
}
unsigned long __stdcall Thread::trampoline(void *self)
{
    Thread *t = (Thread *)self;
    t->fn_(t->arg_);
    t->done = true;
    return 0;
}
bool Thread::start(ThreadFunc fn, void *arg)
{
    fn_ = fn;
    arg_ = arg;
    done = false;
    DWORD tid = 0;
    handle = CreateThread(0, 1 << 20, (LPTHREAD_START_ROUTINE)trampoline, this, 0, &tid);
    id = tid;
    return handle != 0;
}
bool Thread::join(int timeout_ms)
{
    if (!handle)
        return true;
    return WaitForSingleObject((HANDLE)handle, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) == WAIT_OBJECT_0;
}
bool Thread::is_alive() { return handle && WaitForSingleObject((HANDLE)handle, 0) == WAIT_TIMEOUT; }
bool Thread::is_current() { return handle && GetCurrentThreadId() == id; }

long atomic_get(volatile long *p) { return InterlockedCompareExchange(p, 0, 0); }
void atomic_set(volatile long *p, long v) { InterlockedExchange(p, v); }

void sleep_ms(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }

double mono_time()
{
    static LARGE_INTEGER freq;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = true;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

double wall_time()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    return (double)(t - 116444736000000000ULL) / 1e7;
}
#else
Mutex::Mutex() { pthread_mutex_t *m = new pthread_mutex_t; pthread_mutex_init(m, 0); impl = m; }
Mutex::~Mutex() { pthread_mutex_destroy((pthread_mutex_t *)impl); delete (pthread_mutex_t *)impl; }
void Mutex::lock() { pthread_mutex_lock((pthread_mutex_t *)impl); }
void Mutex::unlock() { pthread_mutex_unlock((pthread_mutex_t *)impl); }

struct PosixEvent {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool set;
};
Event::Event(bool initially_set)
{
    PosixEvent *e = new PosixEvent;
    pthread_mutex_init(&e->m, 0);
    pthread_cond_init(&e->c, 0);
    e->set = initially_set;
    impl = e;
}
Event::~Event()
{
    PosixEvent *e = (PosixEvent *)impl;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    delete e;
}
void Event::set()
{
    PosixEvent *e = (PosixEvent *)impl;
    pthread_mutex_lock(&e->m);
    e->set = true;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}
void Event::clear()
{
    PosixEvent *e = (PosixEvent *)impl;
    pthread_mutex_lock(&e->m);
    e->set = false;
    pthread_mutex_unlock(&e->m);
}
bool Event::is_set()
{
    PosixEvent *e = (PosixEvent *)impl;
    pthread_mutex_lock(&e->m);
    bool s = e->set;
    pthread_mutex_unlock(&e->m);
    return s;
}
bool Event::wait(int timeout_ms)
{
    PosixEvent *e = (PosixEvent *)impl;
    pthread_mutex_lock(&e->m);
    if (timeout_ms < 0) {
        while (!e->set)
            pthread_cond_wait(&e->c, &e->m);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!e->set)
            if (pthread_cond_timedwait(&e->c, &e->m, &ts) != 0)
                break;
    }
    bool s = e->set;
    pthread_mutex_unlock(&e->m);
    return s;
}

Thread::Thread() : handle(0), id(0), fn_(0), arg_(0), done(false) {}
Thread::~Thread()
{
    if (handle) {
        if (!done)
            pthread_detach(*(pthread_t *)handle);
        delete (pthread_t *)handle;
    }
}
void *Thread::trampoline(void *self)
{
    Thread *t = (Thread *)self;
    t->fn_(t->arg_);
    t->done = true;
    return 0;
}
bool Thread::start(ThreadFunc fn, void *arg)
{
    fn_ = fn;
    arg_ = arg;
    done = false;
    pthread_t *th = new pthread_t;
    if (pthread_create(th, 0, trampoline, this) != 0) {
        delete th;
        return false;
    }
    handle = th;
    return true;
}
bool Thread::join(int timeout_ms)
{
    if (!handle)
        return true;
    double end = mono_time() + timeout_ms / 1000.0;
    while (!done) {
        if (timeout_ms >= 0 && mono_time() > end)
            return false;
        sleep_ms(2);
    }
    return true;
}
bool Thread::is_alive() { return handle && !done; }
bool Thread::is_current() { return handle && pthread_equal(pthread_self(), *(pthread_t *)handle); }

long atomic_get(volatile long *p) { return __sync_fetch_and_add(p, 0); }
void atomic_set(volatile long *p, long v) { __sync_lock_test_and_set(p, v); }

void sleep_ms(int ms) { usleep((useconds_t)(ms < 0 ? 0 : ms) * 1000); }
double mono_time()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
double wall_time()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}
#endif

std::string utc_timestamp()
{
    time_t t = time(0);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &g);
    return buf;
}

/* ================================================================ sockets */
#ifdef _WIN32
typedef SOCKET native_sock;
#define NATIVE(s) ((SOCKET)(s))
static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
typedef int native_sock;
#define NATIVE(s) ((int)(s))
#define closesocket close
static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
#endif

bool net_init()
{
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        done = true;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    return true;
}

void sock_set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(NATIVE(s), FIONBIO, &on);
#else
    int fl = fcntl(NATIVE(s), F_GETFL, 0);
    fcntl(NATIVE(s), F_SETFL, fl | O_NONBLOCK);
#endif
}

static void set_blocking(sock_t s)
{
#ifdef _WIN32
    u_long off = 0;
    ioctlsocket(NATIVE(s), FIONBIO, &off);
#else
    int fl = fcntl(NATIVE(s), F_GETFL, 0);
    fcntl(NATIVE(s), F_SETFL, fl & ~O_NONBLOCK);
#endif
}

static std::string sock_err_text()
{
#ifdef _WIN32
    int e = WSAGetLastError();
    if (e == WSAEADDRINUSE)
        return "[WinError 10048] Only one usage of each socket address (protocol/network address/port) is normally permitted";
    if (e == WSAEACCES)
        return "[WinError 10013] An attempt was made to access a socket in a way forbidden by its access permissions";
    return strf("[WinError %d] socket error", e);
#else
    return strf("[Errno %d] %s", errno, strerror(errno));
#endif
}

sock_t tcp_listen(const char *host, int port, std::string *err)
{
    net_init();
    native_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == (native_sock)-1) {
        if (err)
            *err = sock_err_text();
        return BK_BAD_SOCK;
    }
    int on = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = inet_addr(host);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
        if (err)
            *err = sock_err_text();
        closesocket(s);
        return BK_BAD_SOCK;
    }
    sock_set_nonblocking((sock_t)s);
    return (sock_t)s;
}

sock_t tcp_accept(sock_t s)
{
    native_sock c = accept(NATIVE(s), 0, 0);
    if (c == (native_sock)-1)
        return BK_BAD_SOCK;
    return (sock_t)c;
}

int tcp_bound_port(sock_t s)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (getsockname(NATIVE(s), (struct sockaddr *)&a, &len) != 0)
        return -1;
    return ntohs(a.sin_port);
}

static sock_t connect_impl(const std::string &ip, int port, int timeout_ms)
{
    net_init();
    native_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == (native_sock)-1)
        return BK_BAD_SOCK;
    sock_set_nonblocking((sock_t)s);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = inet_addr(ip.c_str());
    int r = connect(s, (struct sockaddr *)&a, sizeof(a));
    if (r != 0 && !would_block()) {
        closesocket(s);
        return BK_BAD_SOCK;
    }
    if (r != 0) {
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select((int)s + 1, 0, &wf, &ef, &tv);
        if (r <= 0 || FD_ISSET(s, &ef)) {
            closesocket(s);
            return BK_BAD_SOCK;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
        if (soerr != 0) {
            closesocket(s);
            return BK_BAD_SOCK;
        }
    }
    return (sock_t)s;
}

sock_t tcp_connect(const std::string &ip, int port, int timeout_ms) { return connect_impl(ip, port, timeout_ms); }

sock_t tcp_connect_blocking(const std::string &ip, int port, int timeout_ms)
{
    sock_t s = connect_impl(ip, port, timeout_ms);
    if (s == BK_BAD_SOCK)
        return s;
    set_blocking(s);
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(NATIVE(s), SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(NATIVE(s), SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(NATIVE(s), SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(NATIVE(s), SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#endif
    return s;
}

sock_t udp_socket()
{
    net_init();
    native_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == (native_sock)-1)
        return BK_BAD_SOCK;
    sock_set_nonblocking((sock_t)s);
    return (sock_t)s;
}

sock_t udp_bind(const char *host, int port)
{
    sock_t s = udp_socket();
    if (s == BK_BAD_SOCK)
        return s;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = inet_addr(host);
    if (bind(NATIVE(s), (struct sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(NATIVE(s));
        return BK_BAD_SOCK;
    }
    return s;
}

int sock_recv(sock_t s, char *buf, int len)
{
    int n = recv(NATIVE(s), buf, len, 0);
    if (n > 0)
        return n;
    if (n == 0)
        return 0;
    return would_block() ? -1 : -2;
}

int sock_send(sock_t s, const char *buf, int len)
{
#ifdef _WIN32
    int n = send(NATIVE(s), buf, len, 0);
#else
    int n = (int)send(NATIVE(s), buf, (size_t)len, MSG_NOSIGNAL);
#endif
    if (n >= 0)
        return n;
    return would_block() ? -1 : -2;
}

int sock_sendto(sock_t s, const char *buf, int len, const u8 ip[4], int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    memcpy(&a.sin_addr.s_addr, ip, 4);
    int n = sendto(NATIVE(s), buf, len, 0, (struct sockaddr *)&a, sizeof(a));
    return n >= 0 ? n : -2;
}

int sock_recvfrom(sock_t s, char *buf, int len, u8 ip[4], int *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int n = recvfrom(NATIVE(s), buf, len, 0, (struct sockaddr *)&a, &al);
    if (n < 0)
        return would_block() ? -1 : -2;
    memcpy(ip, &a.sin_addr.s_addr, 4);
    if (port)
        *port = ntohs(a.sin_port);
    return n;
}

bool sock_send_all(sock_t s, const Bytes &data)
{
    size_t off = 0;
    while (off < data.size()) {
        int n = send(NATIVE(s), data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

void sock_shutdown_wr(sock_t s)
{
#ifdef _WIN32
    shutdown(NATIVE(s), SD_SEND);
#else
    shutdown(NATIVE(s), SHUT_WR);
#endif
}

void sock_close(sock_t s)
{
    if (s != BK_BAD_SOCK)
        closesocket(NATIVE(s));
}

bool sock_select(const std::vector<sock_t> &rd, const std::vector<sock_t> &wr,
                 std::vector<sock_t> &rd_ready, std::vector<sock_t> &wr_ready, int timeout_ms)
{
    rd_ready.clear();
    wr_ready.clear();
    fd_set rf, wf;
    FD_ZERO(&rf);
    FD_ZERO(&wf);
    int maxfd = 0;
    for (size_t i = 0; i < rd.size(); i++) {
        FD_SET(NATIVE(rd[i]), &rf);
        if ((int)NATIVE(rd[i]) > maxfd)
            maxfd = (int)NATIVE(rd[i]);
    }
    for (size_t i = 0; i < wr.size(); i++) {
        FD_SET(NATIVE(wr[i]), &wf);
        if ((int)NATIVE(wr[i]) > maxfd)
            maxfd = (int)NATIVE(wr[i]);
    }
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(maxfd + 1, rd.empty() ? 0 : &rf, wr.empty() ? 0 : &wf, 0, &tv);
    if (r < 0)
        return false;
    for (size_t i = 0; i < rd.size(); i++)
        if (FD_ISSET(NATIVE(rd[i]), &rf))
            rd_ready.push_back(rd[i]);
    for (size_t i = 0; i < wr.size(); i++)
        if (FD_ISSET(NATIVE(wr[i]), &wf))
            wr_ready.push_back(wr[i]);
    return true;
}

bool resolve_host(const std::string &name, u8 ip[4])
{
    net_init();
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name.c_str(), 0, &hints, &res) != 0 || !res)
        return false;
    struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
    memcpy(ip, &a->sin_addr.s_addr, 4);
    freeaddrinfo(res);
    return true;
}

int free_tcp_port()
{
    net_init();
    native_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;
    int port = -1;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0)
        port = tcp_bound_port((sock_t)s);
    closesocket(s);
    return port;
}

/* ============================================================== processes */
#ifdef _WIN32
static std::wstring widen_s(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring quote_arg(const std::wstring &a)
{
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') {
            i++;
            bs++;
        }
        if (i == a.size()) {
            q.append(bs * 2, L'\\');
            break;
        } else if (a[i] == L'"') {
            q.append(bs * 2 + 1, L'\\');
            q += L'"';
        } else {
            q.append(bs, L'\\');
            q += a[i];
        }
    }
    q += L'"';
    return q;
}
#endif

ChildProcess::ChildProcess() : proc(0), pipe_rd(0), pid(0), fd(-1), status(0), exited(false) {}
ChildProcess::~ChildProcess()
{
    if (running())
        kill();
#ifdef _WIN32
    if (pipe_rd)
        CloseHandle((HANDLE)pipe_rd);
    if (proc)
        CloseHandle((HANDLE)proc);
#else
    if (fd >= 0)
        close(fd);
#endif
}

bool ChildProcess::start(const StrList &argv, const std::vector<std::pair<std::string, std::string> > &extra_env)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = 0;
    HANDLE rd = 0, wr = 0;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16))
        return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i)
            cmd += L" ";
        cmd += quote_arg(widen_s(argv[i]));
    }
    /* environment block: current environment plus overrides */
    std::wstring envblk;
    wchar_t *cur = GetEnvironmentStringsW();
    std::vector<std::wstring> vars;
    for (wchar_t *p = cur; *p; p += wcslen(p) + 1)
        vars.push_back(p);
    FreeEnvironmentStringsW(cur);
    for (size_t k = 0; k < extra_env.size(); k++) {
        std::wstring key = widen_s(extra_env[k].first) + L"=";
        for (size_t i = 0; i < vars.size(); i++)
            if (_wcsnicmp(vars[i].c_str(), key.c_str(), key.size()) == 0) {
                vars.erase(vars.begin() + (long)i);
                break;
            }
        vars.push_back(key + widen_s(extra_env[k].second));
    }
    for (size_t i = 0; i < vars.size(); i++) {
        envblk += vars[i];
        envblk += L'\0';
    }
    envblk += L'\0';
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(0);
    BOOL ok = CreateProcessW(0, &cmdbuf[0], 0, 0, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                             (void *)envblk.c_str(), 0, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    CloseHandle(pi.hThread);
    proc = pi.hProcess;
    pid = (int)pi.dwProcessId;
    pipe_rd = rd;
    return true;
#else
    int p[2];
    if (pipe(p) != 0)
        return false;
    int child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        for (size_t k = 0; k < extra_env.size(); k++)
            setenv(extra_env[k].first.c_str(), extra_env[k].second.c_str(), 1);
        std::vector<char *> av;
        for (size_t i = 0; i < argv.size(); i++)
            av.push_back((char *)argv[i].c_str());
        av.push_back(0);
        execv(av[0], &av[0]);
        _exit(127);
    }
    close(p[1]);
    pid = child;
    fd = p[0];
    return true;
#endif
}

bool ChildProcess::read_some(std::string &out, int timeout_ms)
{
#ifdef _WIN32
    if (!pipe_rd)
        return false;
    double end = mono_time() + timeout_ms / 1000.0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)pipe_rd, 0, 0, 0, &avail, 0))
            return false; /* broken pipe: writer gone and drained */
        if (avail) {
            char buf[65536];
            DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD got = 0;
            if (!ReadFile((HANDLE)pipe_rd, buf, want, &got, 0))
                return false;
            out.append(buf, got);
            return true;
        }
        if (!running()) {
            DWORD avail2 = 0;
            if (!PeekNamedPipe((HANDLE)pipe_rd, 0, 0, 0, &avail2, 0) || !avail2)
                return false;
            continue;
        }
        if (mono_time() >= end)
            return true;
        sleep_ms(1);
    }
#else
    if (fd < 0)
        return false;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(fd + 1, &rf, 0, 0, &tv);
    if (r <= 0)
        return true;
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
        return false;
    out.append(buf, (size_t)n);
    return true;
#endif
}

bool ChildProcess::running()
{
    if (exited)
        return false;
#ifdef _WIN32
    if (!proc)
        return false;
    if (WaitForSingleObject((HANDLE)proc, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess((HANDLE)proc, &code);
        status = (int)code;
        exited = true;
        return false;
    }
    return true;
#else
    if (pid <= 0)
        return false;
    int st = 0;
    int r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
        status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        exited = true;
        return false;
    }
    return true;
#endif
}

int ChildProcess::exit_code()
{
    running();
    return status;
}

void ChildProcess::kill()
{
#ifdef _WIN32
    if (proc && running())
        TerminateProcess((HANDLE)proc, 1);
#else
    if (pid > 0 && running())
        ::kill(pid, SIGKILL);
#endif
}

void ChildProcess::wait(int timeout_ms)
{
#ifdef _WIN32
    if (proc && !exited) {
        WaitForSingleObject((HANDLE)proc, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        running();
    }
#else
    double end = mono_time() + timeout_ms / 1000.0;
    while (running() && mono_time() < end)
        sleep_ms(2);
#endif
}

std::string self_exe_path()
{
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(0, buf, 4096);
    std::wstring w(buf, n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], m, 0, 0);
    return s;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    return std::string(buf, (size_t)n);
#endif
}

std::string current_dir()
{
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetCurrentDirectoryW(4096, buf);
    std::wstring w(buf, n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], m, 0, 0);
    return s;
#else
    char buf[4096];
    if (!getcwd(buf, sizeof(buf)))
        return std::string();
    return buf;
#endif
}

bool ensure_bundled_firmwares(std::string &out_root)
{
    std::string exe = self_exe_path();
    if (exe.empty())
        return false;
    FILE *f = fopen(exe.c_str(), "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long fsize = ftell(f);
    if (fsize < 16 || fseek(f, fsize - 16, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    u8 footer[16];
    if (fread(footer, 1, 16, f) != 16 || memcmp(footer + 8, "BKSFWB01", 8) != 0) {
        fclose(f);
        return false;
    }
    u32 count = rd32le(footer);
    u32 total = rd32le(footer + 4);
    if (count == 0 || count > 256 || (long)total + 16 > fsize
        || fseek(f, fsize - 16 - (long)total, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    std::string root = path_join(temp_dir(), "bks_release_bundle");
    std::string fw_dir = path_join(root, "firmwares");
    make_dirs(fw_dir);
    for (u32 i = 0; i < count; i++) {
        u8 hdr[4];
        if (fread(hdr, 1, 4, f) != 4) {
            fclose(f);
            return false;
        }
        u32 nlen = rd32le(hdr);
        if (nlen == 0 || nlen > 512) {
            fclose(f);
            return false;
        }
        std::string name((size_t)nlen, '\0');
        if (fread(&name[0], 1, nlen, f) != nlen || fread(hdr, 1, 4, f) != 4) {
            fclose(f);
            return false;
        }
        u32 dlen = rd32le(hdr);
        if (dlen > 32 * 1024 * 1024) {
            fclose(f);
            return false;
        }
        std::string dst = path_join(fw_dir, path_basename(name));
        Bytes cur;
        if (file_exists(dst) && read_file(dst, cur) && cur.size() == (size_t)dlen) {
            if (fseek(f, (long)dlen, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
            continue;
        }
        Bytes data((size_t)dlen, '\0');
        if (dlen > 0 && fread(&data[0], 1, dlen, f) != dlen) {
            fclose(f);
            return false;
        }
        if (!write_file(dst, data)) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    out_root = root;
    return true;
}
