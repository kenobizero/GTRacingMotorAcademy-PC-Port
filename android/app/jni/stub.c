/* stub.c -- libmain.so of the 64-bit build (arm64-v8a): loads the game, libgame.so, below 4 GB.

   The port shares its memory with the 32-bit game one to one (host/core/lowmem.c), so on a 64-bit phone the
   host's own code and data have to sit in the process's lowest 4 GB too.  Android's linker puts a library where
   it likes (high), but android_dlopen_ext can load one into a range the caller reserved: this library, which
   Java loads (SDLActivity, "main"), reserves a range below 4 GB, loads libgame.so (the host, SDL and the
   generated game code) into it, and hands the rest over:
   - JNI_OnLoad: SDL's, in libgame.so (it registers SDL's native methods with Java);
   - SDL_main: SDL's Java calls it in "libmain.so" (getMainSharedObject); it is the game's main.
   If the range cannot be had, or the library would not fit in it, the error goes to the log (logcat "GTRacing")
   and on screen through Java (the main entry returns at once). */
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef JNIEXPORT
#define JNIEXPORT __attribute__((visibility("default")))
#endif
#ifndef JNICALL
#define JNICALL
#endif

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "GTRacing", __VA_ARGS__)

static void *game;
static char failure[512];

/* a free range of n bytes between 256 MB and 4 GB (/proc/self/maps), reserved: its address or NULL */
static void *reserve_low(size_t n)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return NULL;
    char line[512];
    uintptr_t prev = 0x10000000u, found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
            continue;
        if (lo > prev && lo - prev >= n && prev + n <= 0xFFFF0000u)
            found = prev;
        if (hi > prev)
            prev = hi;
    }
    fclose(f);
    if (!found && prev + n <= 0xFFFF0000u)
        found = prev;
    if (!found)
        return NULL;
    void *p = mmap((void *)found, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == (void *)found)
        return p;
    if (p != MAP_FAILED)
        munmap(p, n);
    return NULL;
}

static int load_game(void)
{
    /* the library's whole size is not known before it is read: 256 MB, then more if the linker says it does not
       fit (unused pages of the range cost nothing) */
    static const size_t sizes[] = {256u << 20, 768u << 20};
    for (int i = 0; i < 2; i++) {
        for (int tries = 0; tries < 4; tries++) {
            void *at = reserve_low(sizes[i]);
            if (!at) {
                snprintf(failure, sizeof failure, "no room below 4 GB for the game (%zu MB)", sizes[i] >> 20);
                continue;
            }
            android_dlextinfo info;
            memset(&info, 0, sizeof info);
            info.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
            info.reserved_addr = at;
            info.reserved_size = sizes[i];
            game = android_dlopen_ext("libgame.so", RTLD_NOW | RTLD_GLOBAL, &info);
            if (game) {
                LOG("[stub] libgame.so loaded below 4 GB at %p (a %zu MB range)", at, sizes[i] >> 20);
                return 1;
            }
            const char *e = dlerror();
            snprintf(failure, sizeof failure, "libgame.so: %s", e ? e : "not loaded");
            munmap(at, sizes[i]);
            if (e && strstr(e, "reserved"))
                break;                       /* too small: the bigger range */
        }
    }
    LOG("[stub] %s", failure);
    return 0;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    if (!load_game())
        return JNI_ERR;                      /* Java's loadLibrary fails: SDLActivity shows the error */
    jint (*on_load)(JavaVM *, void *) = (jint (*)(JavaVM *, void *))dlsym(game, "JNI_OnLoad");
    return on_load ? on_load(vm, reserved) : JNI_VERSION_1_4;
}

JNIEXPORT int SDL_main(int argc, char *argv[])
{
    int (*m)(int, char **) = game ? (int (*)(int, char **))dlsym(game, "SDL_main") : NULL;
    if (!m) {
        LOG("[stub] the game could not start: %s", failure[0] ? failure : "no SDL_main in libgame.so");
        return 1;
    }
    return m(argc, argv);
}
