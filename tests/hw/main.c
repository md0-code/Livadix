/* mc_hw_tests <suite>: bus, via, rtc, adb-via, dafb. */
#include "hw_test.h"

#include <string.h>

int main(int argc, char **argv) {
    static const struct { const char *name; int (*run)(void); } suites[] = {
        {"bus", mc_test_bus}, {"via", mc_test_via}, {"rtc", mc_test_rtc}, {"adb-via", mc_test_adb}, {"dafb", mc_test_dafb},
    };
    for (size_t i = 0; argc == 2 && i < sizeof suites / sizeof *suites; i++)
        if (!strcmp(argv[1], suites[i].name)) {
            suites[i].run();
            printf("%s: %d failure(s)\n", argv[1], mc_test_failures);
            return mc_test_failures ? 1 : 0;
        }
    fprintf(stderr, "usage: mc_hw_tests bus|via|rtc|adb-via|dafb\n");
    return 2;
}
