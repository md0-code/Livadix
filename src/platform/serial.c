/* Host ends of the serial ports (serial.h): Win32 communications and named
 * pipe calls as the Windows API documents them; elsewhere (Linux) a tty
 * through termios and a pipe as a Unix-domain socket (POSIX.1, tty_ioctl(2),
 * unix(7)). */
#include "serial.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
static int fail(char *error, size_t capacity, const char *what, const char *path) {
    snprintf(error, capacity, "%s %s (Windows error %lu)", what, path, (unsigned long)GetLastError());
    return 0;
}
#else
static int fail(char *error, size_t capacity, const char *what, const char *path) {
    snprintf(error, capacity, "%s %s (%s)", what, path, strerror(errno));
    return 0;
}

/* A descriptor that never blocks and is not inherited. */
static int nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static int unix_address(const char *path, struct sockaddr_un *a) {
    if (strlen(path) >= sizeof a->sun_path) { errno = ENAMETOOLONG; return 0; }
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, strlen(path));
    return 1;
}
#endif

#ifdef _WIN32
/* A server end waits for a client without blocking. */
static void listen_pipe(mc_serial_end *e) {
    if (ConnectNamedPipe(e->handle, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) e->connected = 1;
}
#else
/* A server end accepts a waiting client without blocking; one at a time, the
 * next waits in the socket's queue. */
static void listen_pipe(mc_serial_end *e) {
    if (e->listener < 0 || e->fd >= 0) return;
    const int client = accept(e->listener, NULL, NULL);
    if (client < 0) return;
    if (!nonblocking(client)) { close(client); return; }
    e->fd = client;
    e->connected = 1;
}

/* A tty: raw bytes at the speed and framing the port has (set them with
 * stty -F DEVICE, as Windows has its COM port's), modem lines left to the
 * Macintosh's handshake. */
static int open_tty(mc_serial_end *e, const char *path, char *error, size_t capacity) {
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return fail(error, capacity, "Cannot open the serial port", path);
    struct termios t;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || tcgetattr(fd, &t) != 0) {
        const int err = errno;
        close(fd);
        errno = err;
        return fail(error, capacity, "Cannot open the serial port", path);
    }
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(tcflag_t)CRTSCTS;
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        const int err = errno;
        close(fd);
        errno = err;
        return fail(error, capacity, "Cannot set up the serial port", path);
    }
    ioctl(fd, TIOCEXCL);        /* one program at a time, as a COM port is opened */
    e->fd = fd;
    return 1;
}

/* A socket someone else serves is joined; otherwise Livadix serves it (a stale
 * socket file nobody serves is replaced). */
static int open_socket(mc_serial_end *e, const char *path, char *error, size_t capacity) {
    struct sockaddr_un a;
    if (!unix_address(path, &a)) return fail(error, capacity, "Cannot open the pipe", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || !nonblocking(fd)) {
        const int err = errno;
        if (fd >= 0) close(fd);
        errno = err;
        return fail(error, capacity, "Cannot open the pipe", path);
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        e->fd = fd;
        e->connected = 1;
        return 1;
    }
    const int refused = errno == ECONNREFUSED;
    close(fd);
    if (refused) {
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) { errno = EEXIST; return fail(error, capacity, "Cannot open the pipe", path); }
            unlink(path);
        }
    } else if (errno != ENOENT) return fail(error, capacity, "Cannot open the pipe", path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || !nonblocking(fd)) {
        const int err = errno;
        if (fd >= 0) close(fd);
        errno = err;
        return fail(error, capacity, "Cannot open the pipe", path);
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        const int err = errno;
        close(fd);
        errno = err;
        return fail(error, capacity, "Cannot open the pipe", path);
    }
    if (listen(fd, 1) != 0) {
        const int err = errno;
        close(fd);
        unlink(path);
        errno = err;
        return fail(error, capacity, "Cannot open the pipe", path);
    }
    e->listener = fd;
    e->server = 1;
    snprintf(e->socket_path, sizeof e->socket_path, "%s", path);
    listen_pipe(e);
    return 1;
}
#endif

#ifdef _WIN32
static int open_end(mc_serial_end *e, const mc_port_config *c, char *error, size_t capacity) {
    memset(e, 0, sizeof *e);
    e->type = c->type;
    e->dcd = e->cts = -1;
    if (c->type == MC_PORT_OUTPUT) {
        e->file = fopen(c->path, "ab");
        return e->file ? 1 : fail(error, capacity, "Cannot open the port output", c->path);
    }
    if (c->type == MC_PORT_SERIAL) {
        char name[32];
        snprintf(name, sizeof name, "\\\\.\\%s", c->path);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) return fail(error, capacity, "Cannot open the serial port", c->path);
        /* Reads return what is there at once; writes give up after 100 ms. */
        COMMTIMEOUTS t = {MAXDWORD, 0, 0, 0, 100};
        SetCommTimeouts(h, &t);
        e->handle = h;
        return 1;
    }
    if (c->type == MC_PORT_PIPE) {
        /* A pipe someone else serves is joined; otherwise Livadix serves it. */
        HANDLE h = CreateFileA(c->path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
            SetNamedPipeHandleState(h, &mode, NULL, NULL);
            e->handle = h;
            e->connected = 1;
            return 1;
        }
        h = CreateNamedPipeA(c->path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1,
                             MC_SERIAL_BUFFER, MC_SERIAL_BUFFER, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) return fail(error, capacity, "Cannot open the pipe", c->path);
        e->handle = h;
        e->server = 1;
        listen_pipe(e);
        return 1;
    }
    return 1;      /* none or disabled */
}
#else
static int open_end(mc_serial_end *e, const mc_port_config *c, char *error, size_t capacity) {
    memset(e, 0, sizeof *e);
    e->type = c->type;
    e->dcd = e->cts = -1;
    e->fd = e->listener = -1;
    if (c->type == MC_PORT_OUTPUT) {
        e->file = fopen(c->path, "ab");
        return e->file ? 1 : fail(error, capacity, "Cannot open the port output", c->path);
    }
    if (c->type == MC_PORT_SERIAL) return open_tty(e, c->path, error, capacity);
    if (c->type == MC_PORT_PIPE) return open_socket(e, c->path, error, capacity);
    return 1;      /* none or disabled */
}
#endif

int mc_host_serial_open(mc_host_serial *s, const mc_port_config ports[2], char *error, size_t capacity) {
    memset(s, 0, sizeof *s);
    for (int p = 0; p < 2; p++)
        if (!open_end(&s->port[p], &ports[p], error, capacity)) {
            mc_host_serial_close(s);
            return 0;
        }
    return 1;
}

static void transmit(void *ctx, unsigned port, uint8_t byte) {
    mc_host_serial *s = ctx;
    if (port > 1) return;
    mc_serial_end *e = &s->port[port];
    if (e->type == MC_PORT_DEFAULT || e->type == MC_PORT_DISABLED) return;
    if (e->out_count == sizeof e->out) mc_host_serial_flush(e);
    if (e->out_count < sizeof e->out) e->out[e->out_count++] = byte;      /* else the line overran: lost */
}

static void outputs(void *ctx, unsigned port, int dtr) {
    mc_host_serial *s = ctx;
    if (port > 1) return;
    mc_serial_end *e = &s->port[port];
    e->dtr = dtr;
#ifdef _WIN32
    if (e->type == MC_PORT_SERIAL) EscapeCommFunction(e->handle, dtr ? SETDTR : CLRDTR);
#else
    if (e->type == MC_PORT_SERIAL) {
        const int line = TIOCM_DTR;
        ioctl(e->fd, dtr ? TIOCMBIS : TIOCMBIC, &line);
    }
#endif
}

mc_serial_host mc_host_serial_callbacks(mc_host_serial *s) {
    mc_serial_host h = {s, transmit, outputs};
    return h;
}

/* A pipe whose other end went away: a server listens again; a client is
 * done (the pipe is gone with its server). */
static void pipe_broken(mc_serial_end *e) {
    e->connected = 0;
    e->out_count = 0;
#ifdef _WIN32
    if (e->server) {
        DisconnectNamedPipe(e->handle);
        listen_pipe(e);
    }
#else
    if (e->fd >= 0) close(e->fd);
    e->fd = -1;
    if (e->server) listen_pipe(e);
#endif
}

void mc_host_serial_flush(mc_serial_end *e) {
    if (!e->out_count) return;
    if (e->type == MC_PORT_OUTPUT) {
        fwrite(e->out, 1, e->out_count, e->file);
        fflush(e->file);
        e->out_count = 0;
        return;
    }
    if (e->type == MC_PORT_PIPE && !e->connected) { e->out_count = 0; return; }   /* nobody listens */
    if (e->type != MC_PORT_SERIAL && e->type != MC_PORT_PIPE) { e->out_count = 0; return; }
#ifdef _WIN32
    DWORD wrote = 0;
    if (!WriteFile(e->handle, e->out, (DWORD)e->out_count, &wrote, NULL)) {
        if (e->type == MC_PORT_PIPE) pipe_broken(e);
        else e->out_count = 0;
        return;
    }
    memmove(e->out, e->out + wrote, e->out_count - wrote);
    e->out_count -= wrote;
#else
    /* A busy line keeps what is queued for the next pump. */
    const ssize_t wrote = e->type == MC_PORT_PIPE ? send(e->fd, e->out, e->out_count, MSG_NOSIGNAL)
                                                  : write(e->fd, e->out, e->out_count);
    if (wrote < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        if (e->type == MC_PORT_PIPE) pipe_broken(e);
        else e->out_count = 0;
        return;
    }
    memmove(e->out, e->out + wrote, e->out_count - (size_t)wrote);
    e->out_count -= (size_t)wrote;
#endif
}

void mc_host_serial_read(mc_serial_end *e) {
    if (e->type != MC_PORT_SERIAL && e->type != MC_PORT_PIPE) return;
    if (e->type == MC_PORT_PIPE && !e->connected) {
        if (e->server) listen_pipe(e);
        if (!e->connected) return;
    }
    const size_t room = sizeof e->in - e->in_count;
    if (!room) return;
#ifdef _WIN32
    DWORD avail = (DWORD)room;
    if (e->type == MC_PORT_PIPE) {
        if (!PeekNamedPipe(e->handle, NULL, 0, NULL, &avail, NULL)) { pipe_broken(e); return; }
        if (!avail) return;
        if (avail > room) avail = (DWORD)room;
    }
    DWORD got = 0;
    if (!ReadFile(e->handle, e->in + e->in_count, avail, &got, NULL)) {
        if (e->type == MC_PORT_PIPE && GetLastError() != ERROR_NO_DATA) pipe_broken(e);
        return;
    }
    e->in_count += got;
#else
    const ssize_t got = read(e->fd, e->in + e->in_count, room);
    if (got < 0) {
        if (e->type == MC_PORT_PIPE && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) pipe_broken(e);
        return;
    }
    if (!got) {
        if (e->type == MC_PORT_PIPE) pipe_broken(e);      /* the other end closed */
        return;
    }
    e->in_count += (size_t)got;
#endif
}

/* The modem inputs: the COM port's DCD and CTS; a pipe's are its
 * connection; an output file is always ready. */
static void modem(mc_serial_end *e, unsigned port, mc_machine *m) {
    int dcd, cts;
    if (e->type == MC_PORT_SERIAL) {
#ifdef _WIN32
        DWORD st = 0;
        if (!GetCommModemStatus(e->handle, &st)) return;
        dcd = (st & MS_RLSD_ON) != 0;
        cts = (st & MS_CTS_ON) != 0;
#else
        int lines = 0;
        if (ioctl(e->fd, TIOCMGET, &lines) != 0) return;
        dcd = (lines & TIOCM_CD) != 0;
        cts = (lines & TIOCM_CTS) != 0;
#endif
    } else if (e->type == MC_PORT_PIPE) dcd = cts = e->connected;
    else if (e->type == MC_PORT_OUTPUT) dcd = cts = 1;
    else return;
    if (dcd != e->dcd || cts != e->cts) {
        e->dcd = dcd;
        e->cts = cts;
        mc_machine_serial_modem(m, port, dcd, cts);
    }
}

void mc_host_serial_pump(mc_host_serial *s, mc_machine *m) {
    for (unsigned p = 0; p < 2; p++) {
        mc_serial_end *e = &s->port[p];
        if (e->type == MC_PORT_DEFAULT || e->type == MC_PORT_DISABLED) continue;
        mc_host_serial_flush(e);
        mc_host_serial_read(e);
        if (e->in_count) {
            const size_t took = mc_machine_serial_receive(m, p, e->in, e->in_count);
            memmove(e->in, e->in + took, e->in_count - took);
            e->in_count -= took;
        }
        modem(e, p, m);
    }
}

void mc_host_serial_close(mc_host_serial *s) {
    for (int p = 0; p < 2; p++) {
        mc_serial_end *e = &s->port[p];
        if (e->file) {
            if (e->out_count) fwrite(e->out, 1, e->out_count, e->file);
            fclose(e->file);
        }
#ifdef _WIN32
        if (e->handle) {
            if (e->out_count) mc_host_serial_flush(e);
            CloseHandle(e->handle);
        }
#else
        if (e->type == MC_PORT_SERIAL || e->type == MC_PORT_PIPE) {
            if (e->fd >= 0) {
                if (e->out_count) mc_host_serial_flush(e);
                close(e->fd);
            }
            if (e->listener >= 0) close(e->listener);
            if (e->socket_path[0]) unlink(e->socket_path);
        }
#endif
        memset(e, 0, sizeof *e);
    }
}
