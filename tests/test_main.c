/* mc_tests <suite>: core. */

#include "test.h"

int mc_test_failures;

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "core")) {
        fprintf(stderr, "usage: mc_tests core\n");
        return 2;
    }
    mc_test_core();
    printf("%s: %d failure(s)\n", argv[1], mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
