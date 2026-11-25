/* A few macros are the whole test framework: a failed CHECK prints where
 * and counts, a suite returns the count, main() maps it to the exit code. */

#ifndef MC_TEST_H
#define MC_TEST_H

#include <stdio.h>
#include <string.h>

extern int mc_test_failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);  \
            mc_test_failures++;                                                       \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                            \
    do {                                                                                          \
        const long long a_ = (long long)(a), b_ = (long long)(b);                                 \
        if (a_ != b_) {                                                                           \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, #a, #b, \
                    a_, b_);                                                                      \
            mc_test_failures++;                                                                   \
        }                                                                                         \
    } while (0)

#define CHECK_STR(a, b)                                                                           \
    do {                                                                                          \
        if (strcmp((a), (b)) != 0) {                                                              \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b));           \
            mc_test_failures++;                                                                   \
        }                                                                                         \
    } while (0)

int mc_test_core(void);

#endif /* MC_TEST_H */
