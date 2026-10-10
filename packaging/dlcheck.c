/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlcheck: dlopen() every file named on the command line with RTLD_NOW and report the loader's error text on
 * failure.  scripts/smoke_launch.sh uses it to prove that every SDRstar plugin and its whole library chain resolves
 * from inside the app bundle (a missing or mis-addressed dylib shows up as "Library not loaded: ...").  It then checks
 * the five entry points SDR++'s module manager demands of a plugin (core/src/module.cpp).
 *
 *   cc -o dlcheck dlcheck.c, then run: dlcheck <each plugin .dylib>
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        void* handle = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            printf("FAIL %s: %s\n", argv[i], dlerror());
            bad++;
            continue;
        }
        static const char* const entry[] = {"_INFO_", "_INIT_", "_CREATE_INSTANCE_", "_DELETE_INSTANCE_", "_END_"};
        const char* missing = NULL;
        for (unsigned k = 0; k < sizeof entry / sizeof entry[0]; k++) {
            if (!dlsym(handle, entry[k])) {
                missing = entry[k];
                break;
            }
        }
        if (missing) {
            printf("FAIL %s: loads, but the SDR++ entry point %s is missing\n", argv[i], missing);
            bad++;
        } else {
            printf("OK   %s\n", argv[i]);
        }
    }
    printf("%d of %d loaded\n", argc - 1 - bad, argc - 1);
    return bad ? 1 : 0;
}
