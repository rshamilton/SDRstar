/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlcheck: dlopen() every file named on the command line with RTLD_NOW and report the loader's error text on
 * failure.  scripts/smoke_launch.sh uses it to prove that every SDRstar plugin and its whole library chain resolves
 * from inside the app bundle (a missing or mis-addressed dylib shows up as "Library not loaded: ...").
 *
 *   cc -o dlcheck dlcheck.c, then run: dlcheck <each plugin .dylib>
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        void* handle = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            printf("OK   %s\n", argv[i]);
        } else {
            printf("FAIL %s: %s\n", argv[i], dlerror());
            bad++;
        }
    }
    printf("%d of %d loaded\n", argc - 1 - bad, argc - 1);
    return bad ? 1 : 0;
}
