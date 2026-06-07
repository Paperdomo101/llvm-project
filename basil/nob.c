#define NOB_IMPLEMENTATION
#include <nob.h>

static char *run_and_capture(const char *cmd)
{
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return NULL;
    char *buf = NULL;
    size_t len = 0;
    char tmp[256];
    while (fgets(tmp, sizeof tmp, pipe)) {
        size_t add = strlen(tmp);
        char *newbuf = realloc(buf, len + add + 1);
        if (!newbuf) { free(buf); pclose(pipe); return NULL; }
        buf = newbuf;
        memcpy(buf + len, tmp, add);
        len += add;
        buf[len] = '\0';
    }
    pclose(pipe);    /* strip trailing newline if there is one */
    if (len && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
    return buf;
}

int main( int argc, char **argv )
{
    GO_REBUILD_URSELF( argc, argv );


    String_View filename = {0};
    for (int i = 1; i < argc; ++i)
    {
        if (*argv[i] == '-')
            continue;

        filename = sv_from_cstr(argv[i]);
        break;
        // String_View arg = sv_from_cstr(argv[i]);

        // if (sv_ends_with_cstr(arg, ".c4") || sv_ends_with_cstr(arg, ".civ"))
        // {
        //     filename = arg;
        //     break;
        // }
    }

    const char *inferred_extension = "";

    if (!sv_ends_with_cstr(filename, ".c4") && !sv_ends_with_cstr(filename, ".civ"))
    {
        if (file_exists( temp_sprintf(SV_Fmt".c4", SV_Arg(filename)) ))
            inferred_extension = ".c4";
        else if (file_exists( temp_sprintf(SV_Fmt".civ", SV_Arg(filename)) ))
            inferred_extension = ".civ";
        else {
            filename.data = NULL;
        }
    }


    if (!filename.data) {
        nob_log( ERROR, "please specify a c4 source file" );
        exit( EXIT_FAILURE );
    }

    String_View filename_no_ext = filename;

    if (!strcmp(inferred_extension, "")) {
        if (!nob_sv_chop_suffix(&filename_no_ext, sv_from_cstr(".c4"))
        && !nob_sv_chop_suffix(&filename_no_ext, sv_from_cstr(".civ")))
            exit( EXIT_FAILURE );
    }


    Cmd cmd = {0};

    char *sdk_path = run_and_capture("xcrun --show-sdk-path");
    if (!sdk_path) {
        nob_log( ERROR, "Failed to obtain SDK path" );
        exit( EXIT_FAILURE );
    }

    // -isysroot $(xcrun --show-sdk-path)



    cmd_append( &cmd, "bang",
        "-o", temp_sv_to_cstr(filename_no_ext),
        "-O2",
        temp_sprintf( SV_Fmt"%s", SV_Arg(filename), inferred_extension ),
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
    // da_foreach( const char *, item, &cmd ) {
    //     printf("%s\n", *item);
    // }
    if (!cmd_run( &cmd ))
        return 1;

    bool build_only = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "-b"))
        {
            build_only = true;
        }
    }

    if (!build_only)
    {
        const char *exec = temp_sprintf("./"SV_Fmt, SV_Arg(filename_no_ext));
        printf( "%s\n", exec );
        cmd_append( &cmd, exec );

        if (!cmd_run( &cmd ))
            return 1;
    }

    exit( EXIT_SUCCESS );
}
