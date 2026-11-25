/* Livadix - the few host differences the sources share beyond the platform
 * layers: the MSVC names of the case-insensitive string comparisons on
 * other compilers, and the comparison of file names.
 *
 * Livadix's own code. */
#ifndef MC_PORTABLE_H
#define MC_PORTABLE_H

#ifdef _WIN32
/* File names compare without case, as Windows' file systems do. */
#define mc_path_cmp _stricmp
#define mc_path_ncmp _strnicmp
#else
#include <string.h>
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
/* File names compare exactly, as the Linux file systems do. */
#define mc_path_cmp strcmp
#define mc_path_ncmp strncmp
#endif

#endif
