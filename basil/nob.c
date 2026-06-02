#define NOB_IMPLEMENTATION
#include <nob.h>

static char *run_and_capture(const char *cmd){    FILE *pipe = popen(cmd, "r");    if (!pipe) return NULL;
    char *buf = NULL;    size_t len = 0;    char tmp[256];
    while (fgets(tmp, sizeof tmp, pipe)) {        size_t add = strlen(tmp);        char *newbuf = realloc(buf, len + add + 1);        if (!newbuf) { free(buf); pclose(pipe); return NULL; }        buf = newbuf;        memcpy(buf + len, tmp, add);        len += add;        buf[len] = '\0';    }
    pclose(pipe);    /* strip trailing newline if there is one */    if (len && buf[len - 1] == '\n')        buf[len - 1] = '\0';    return buf;}

int main( int argc, char **argv )
{
    GO_REBUILD_URSELF( argc, argv );

    Cmd cmd = {0};

    char *sdk_path = run_and_capture("xcrun --show-sdk-path");    if (!sdk_path) {        fprintf(stderr, "Failed to obtain SDK path\n");        return 1;    }

    cmd_append( &cmd, "../build/bin/clang",
        "-o", "test",
        "test.c",
        "-lraylib",
        "-I.",
        "-L.",
        "-I/usr/local/include",
        "-isysroot", sdk_path,
        "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib",
        "-framework", "IOKit",
        "-framework", "Cocoa",
        "-Wno-nullability-completeness"
    );
    if (!cmd_run( &cmd ))
        return 1;

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "-r"))
        {
            cmd_append( &cmd, "./test" );

            if (!cmd_run( &cmd ))
                return 1;
        }
    }

    return 0;
}
