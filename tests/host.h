/* Livadix - what the tests need of the host beyond the C library: making a
 * directory and the process id under the names the Windows C runtime gives
 * them, and the host's own spelling of a serial port, a pipe and a file name.
 *
 * Livadix's own code. */
#ifndef MC_TEST_HOST_H
#define MC_TEST_HOST_H

#ifdef _WIN32
#include <direct.h>
#include <process.h>
/* A serial port and a pipe as the configuration names them. */
#define TEST_SERIAL "COM3"
#define TEST_SERIAL_REFUSED "COM"       /* what the message for a serial device that is not one names */
/* The same image under another spelling: the same file where the file system ignores case. */
#define TEST_IMAGE_RESPELLED "A.IMG"
#define TEST_PIPE "\\\\.\\pipe\\livadix-b"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define _mkdir(path) mkdir((path), 0777)
#define _getpid getpid
#define TEST_SERIAL "/dev/ttyUSB3"
#define TEST_SERIAL_REFUSED "/dev/"
#define TEST_IMAGE_RESPELLED "a.img"
#define TEST_PIPE "/tmp/livadix-b.sock"
#endif

#endif
