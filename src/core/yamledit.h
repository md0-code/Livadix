/* Editing livadix.yaml in place (the configuration window's Save): the file is kept as its
 * lines, and an edit changes only the lines of the key it touches, so
 * comments, blank lines, the order of sections and keys, quoting and every
 * other profile stay as they were. Livadix's own code for the block style the
 * template uses (indented mappings and sequences, flow collections on one
 * or more lines, comments); a flow mapping that an edit reaches into is
 * rewritten in block style. Livadix reads the result with libyaml as usual
 * (config.c). */

#ifndef MC_YAMLEDIT_H
#define MC_YAMLEDIT_H

#include <stddef.h>

typedef struct mc_ydoc {
    char **lines;              /* without line ends */
    size_t count, capacity;
    int crlf;                  /* the file's lines end in CR LF */
} mc_ydoc;

/* The text as lines (any of LF, CR LF). 0 when out of memory. */
int mc_ydoc_parse(mc_ydoc *doc, const char *text, size_t length);
int mc_ydoc_load(mc_ydoc *doc, const char *path);
/* The document as text (malloc'd, NUL-terminated); *length without the NUL. */
char *mc_ydoc_text(const mc_ydoc *doc, size_t *length);
/* Writes the file through a temporary file in its folder, replaced at once. */
int mc_ydoc_save(const mc_ydoc *doc, const char *path);
void mc_ydoc_free(mc_ydoc *doc);
int mc_ydoc_copy(mc_ydoc *to, const mc_ydoc *from);

/* A path is a list of mapping keys from the top (unquoted, as libyaml reads
 * them). */
int mc_ydoc_has(const mc_ydoc *doc, const char *const *path, size_t depth);
/* The scalar value of the key, unquoted; 0 when the key is missing or its
 * value is not a scalar on its line. */
int mc_ydoc_get(const mc_ydoc *doc, const char *const *path, size_t depth, char *out, size_t capacity);
/* Sets the key to a value written in YAML on its line (a scalar, quoted as
 * needed by the caller with mc_ydoc_quote, or a flow collection); creates the
 * key and the mappings above it when missing, uncommenting the template's
 * commented-out line for the key ("# key: ...") when there is one. */
int mc_ydoc_set(mc_ydoc *doc, const char *const *path, size_t depth, const char *value);
/* Sets the key to a block value: lines indented relative to the key's
 * children ("- image: a.img", "  readonly: true"). */
int mc_ydoc_set_block(mc_ydoc *doc, const char *const *path, size_t depth, const char *const *lines, size_t count);
/* Rewrites the mapping at path, and every mapping inside it, in block
 * style (depth 0: every top-level mapping but profiles), so that each of
 * their keys has a line of its own. */
int mc_ydoc_expand(mc_ydoc *doc, const char *const *path, size_t depth);
/* Removes the key and its value; 1 when it was not there either. */
int mc_ydoc_remove(mc_ydoc *doc, const char *const *path, size_t depth);
/* Renames the last key of path (written with mc_ydoc_quote_key). */
int mc_ydoc_rename(mc_ydoc *doc, const char *const *path, size_t depth, const char *key);
/* Copies the key and its value after it, under another key. */
int mc_ydoc_duplicate(mc_ydoc *doc, const char *const *path, size_t depth, const char *key);
/* The keys of the mapping at path (depth 0: the top), in order: up to max
 * names of up to 128 bytes. Returns how many. */
size_t mc_ydoc_keys(const mc_ydoc *doc, const char *const *path, size_t depth, char keys[][128], size_t max);

/* A string as a YAML scalar: plain when that reads back as the same string,
 * else in double quotes. */
const char *mc_ydoc_quote(const char *s, char *buffer, size_t capacity);
/* A path as a YAML scalar: single quotes keep backslashes as they are. */
const char *mc_ydoc_quote_path(const char *s, char *buffer, size_t capacity);

#endif /* MC_YAMLEDIT_H */
