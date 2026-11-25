/* Host ends of the Macintosh's serial ports (`ports.a`, `ports.b`,
 * docs/NOTES.md): a Windows COM port (Linux: a tty device), a named
 * pipe (Linux: a Unix-domain socket) or an output file. Bytes the Macintosh
 * sends are queued by the machine's transmit callback and written out by
 * mc_host_serial_pump, which also offers the bytes that arrived to the
 * machine and passes on the modem inputs. */
#ifndef MC_HOST_SERIAL_H
#define MC_HOST_SERIAL_H

#include "../core/config.h"
#include "../core/machine.h"

#include <stddef.h>
#include <stdint.h>

#define MC_SERIAL_BUFFER 4096

typedef struct mc_serial_end {
    unsigned type;              /* MC_PORT_* */
#ifdef _WIN32
    void *handle;               /* COM port or pipe (a Windows HANDLE) */
#else
    int fd;                     /* the tty, or the pipe's connected socket (-1 none) */
    int listener;               /* the listening socket of a pipe served here (-1 none) */
    char socket_path[108];      /* its socket file, removed at close (a sockaddr_un path) */
#endif
    void *file;                 /* output: FILE * */
    int server;                 /* the pipe was created here (waits for a client) */
    int connected;              /* pipe: a client is (or the server was) there */
    int dtr;                    /* the Macintosh's handshake output */
    int dcd, cts;               /* the modem inputs last passed on (-1 none yet) */
    uint8_t out[MC_SERIAL_BUFFER];
    size_t out_count;
    uint8_t in[MC_SERIAL_BUFFER];
    size_t in_count;
} mc_serial_end;

typedef struct mc_host_serial {
    mc_serial_end port[2];      /* 0 modem (ports.a), 1 printer (ports.b) */
} mc_host_serial;

/* Opens the configured ends; 0 with a message when one cannot open. */
int mc_host_serial_open(mc_host_serial *serial, const mc_port_config ports[2], char *error, size_t capacity);
/* The machine's callbacks for these ends (mc_machine_set_serial). */
mc_serial_host mc_host_serial_callbacks(mc_host_serial *serial);
/* Writes what the Macintosh sent, reads what arrived and gives it to the
 * machine, updates the modem inputs. After each run slice. */
void mc_host_serial_pump(mc_host_serial *serial, mc_machine *machine);
void mc_host_serial_close(mc_host_serial *serial);

/* The pieces of the pump without a machine (tests): flush the queued
 * output; read what arrived into the port's input buffer. */
void mc_host_serial_flush(mc_serial_end *end);
void mc_host_serial_read(mc_serial_end *end);

#endif
