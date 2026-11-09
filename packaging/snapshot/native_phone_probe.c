/* Disposable native-runtime qualification host. Android loads the exact
 * shipped .so; Apple links the exact distributed NodeCore archive. Output is
 * evidence, never a PASS verdict: the caller must verify the returned base,
 * tip and accumulator against an independent selected-chain observer.
 * Usage: probe [library on Android] version
 *        probe [library on Android] run <private-datadir> <config.json> <seconds>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef PHONE_PROBE_DYNAMIC
#include <dlfcn.h>
#else
extern int32_t nodecore_start(const char *, const char *);
extern int32_t nodecore_stop(void);
extern char *nodecore_rpc_call(const char *, const char *);
extern void nodecore_free_string(char *);
extern const char *nodecore_version(void);
#endif

int main(int argc, char **argv) {
    int32_t (*start)(const char *, const char *);
    int32_t (*stop)(void);
    char *(*rpc)(const char *, const char *);
    void (*release)(char *);
    const char *(*version)(void);
#ifdef PHONE_PROBE_DYNAMIC
    if (argc < 3) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    start = (int32_t (*)(const char *, const char *))dlsym(library, "nodecore_start");
    stop = (int32_t (*)(void))dlsym(library, "nodecore_stop");
    rpc = (char *(*)(const char *, const char *))dlsym(library, "nodecore_rpc_call");
    release = (void (*)(char *))dlsym(library, "nodecore_free_string");
    version = (const char *(*)(void))dlsym(library, "nodecore_version");
    --argc; ++argv;
#else
    start = nodecore_start; stop = nodecore_stop; rpc = nodecore_rpc_call;
    release = nodecore_free_string; version = nodecore_version;
#endif
    if (!start || !stop || !rpc || !release || !version || argc < 2) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("NCPROBE\tVERSION\t%s\n", version());
    if (argc == 2 && strcmp(argv[1], "version") == 0) return 0;
    if (argc == 2 && strcmp(argv[1], "home") == 0) {
        const char *home = getenv("HOME");
        if (!home) return 2;
        printf("NCPROBE\tHOME\t%s\n", home);
        return 0;
    }
    if (argc != 5 || strcmp(argv[1], "run") != 0) return 2;
    char *end = NULL;
    long duration = strtol(argv[4], &end, 10);
    if (!end || *end || duration < 1 || duration > 3600) return 2;
    FILE *input = fopen(argv[3], "rb");
    if (!input) return 2;
    char config[65537]; size_t size = fread(config, 1, sizeof(config)-1, input);
    if (ferror(input) || !feof(input)) { fclose(input); return 2; }
    fclose(input); config[size] = '\0';
    int32_t result = start(argv[2], config);
    printf("NCPROBE\tSTART\t%d\n", result);
    if (result != 0) return 1;
    const char *methods[] = {"getsnapshotbootstrapstatus", "getbackgroundvalidationprogress",
        "getblockcount", "getbestblockhash", "blockchain.getutreexocommitment"};
    struct timespec pause = {2, 0};
    for (long elapsed = 0; elapsed < duration; elapsed += 2) {
        for (size_t i = 0; i < sizeof(methods)/sizeof(methods[0]); ++i) {
            char *reply = rpc(methods[i], "[]");
            printf("NCPROBE\t%s\t%s\n", methods[i], reply ? reply : "null");
            if (reply) release(reply);
        }
        nanosleep(&pause, NULL);
    }
    result = stop(); printf("NCPROBE\tSTOP\t%d\n", result);
    return result == 0 ? 0 : 1;
}
