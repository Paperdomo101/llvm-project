#define NOB_IMPLEMENTATION
#include "nob.h"


int cmd_run_and_capture(Cmd *cmd, char *output_buf, size_t buf_size);
bool run_compiler_tests(const char *compiler_path);


int main( int argc, char **argv )
{
    GO_REBUILD_URSELF( argc, argv );

    const char *compiler_bin = "../build/bin/clang";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-test") == 0) {
            bool tests_passed = run_compiler_tests(compiler_bin);
            exit(tests_passed ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    }

    String_View filename = {0};
    for (int i = 1; i < argc; ++i)
    {
        if (*argv[i] == '-')
            continue;

        filename = sv_from_cstr(argv[i]);
        break;
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
        nob_log( ERROR, "please specify a c4 source file or pass '-test'" );
        exit( EXIT_FAILURE );
    }

    String_View filename_no_ext = filename;

    if (!strcmp(inferred_extension, "")) {
        if (!nob_sv_chop_suffix(&filename_no_ext, sv_from_cstr(".c4"))
        && !nob_sv_chop_suffix(&filename_no_ext, sv_from_cstr(".civ")))
            exit( EXIT_FAILURE );
    }

    Cmd cmd = {0};

    cmd_append( &cmd, compiler_bin,
        "-o", temp_sv_to_cstr(filename_no_ext),
        "-O2",
        temp_sprintf( SV_Fmt"%s", SV_Arg(filename), inferred_extension ),
        "-lraylib",
        "-I.", "-L.",
        "-I/usr/local/include",
#ifdef __APPLE__
        "-framework", "IOKit",
        "-framework", "Cocoa",
#elif _WIN32
        "-lwinmm", "-lgdi32",
#endif
        "-Wno-nullability-completeness"
    );

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
        // Clear past command fields before appending runtime executable string path targets
        cmd_free(cmd);
        memset(&cmd, 0, sizeof(Cmd));

        const char *exec = temp_sprintf("./"SV_Fmt, SV_Arg(filename_no_ext));
        printf( "%s\n", exec );
        cmd_append( &cmd, exec );

        if (!cmd_run( &cmd ))
            return 1;
    }

    cmd_free(cmd);
    exit( EXIT_SUCCESS );
}


#define MAX_ERRORS (10)
// Helper structure to track expected error diagnostics
typedef struct {
    const char *name;
    const char *path;
    bool is_runtime_test;
    int expected_error_count;
    const char *expected_errors[MAX_ERRORS]; // NULL if it should compile successfully
} CompilerTest;


CompilerTest custom_test_suite[] = {
    /// --------------
    ///  COMPILE TIME
    /// --------------
    {
        "Bookshelf Comments",
        "tests/bookshelf.c4",
        false, 2, {
            "incompatible pointer to integer conversion initializing 'int' with an expression of type 'char[66]'",
            "initializer element is not a compile-time constant",
        }
    },
    {
        "If Statement",
        "tests/if_statement.c4",
        false, 2, {
            "expected expression",
            "expected '{'",
        }
    },
    {
        "Switch Statement",
        "tests/switch_statement.c4",
        false, 6, {
            "expected '{'",
            "'break' statement not in loop or switch statement",
            "expected '{'",
            "'break' statement not in loop or switch statement",
            "expected '{'",
            "'break' statement not in loop or switch statement",
        }
    },
    {
        "Array types don't get demoted by === C4 PATCH: BOUNDS-CHECKED ARRAY PREFIX INTERCEPT ===",
        "tests/emcc.c4",
    },
    {
        "For loops",
        "tests/forloop.c4",
    },
    {
        "Postfix '->' after GNU statement-expression macro (cross-macro spelling-line false positive)",
        "tests/macro_arrow.c4",
    },
    {
        "LHS ^ Pointer Type Lowering (^T, ^^T, ^^^T, params, return types)",
        "tests/caret_pointers.c4",
    },
    {
        "Enumerations",
        "tests/enums.c4",
    },
    {
        "Valid Array & Cross-Boundary Parameters",
        "tests/valid_array.c4",
    },
    {
        "Bounds Check (Positive Overflow)",
        "tests/positive_overflow.c4",
        false, 1, {"is out of bounds for array of size 3"}
    },
    {
        "Bounds Check (Negative Underflow)",
        "tests/negative_underflow.c4",
        false, 1, {"is out of bounds for array of size 3"}
    },
    {
        "Size Intrinsic Type Constraint Guard",
        "tests/size_guard.c4",
        false
    },
    {
        "Type Inference",
        "tests/type_inference_errors.c4",
        false, 3, {
            "assignment count mismatch: expression yields 3 values, but 2 variables are provided",
            "passing 'float' to parameter of incompatible type 'Vector2'",
            "assignment count mismatch: expression yields 1 values, but 2 variables are provided",
        }
    },
    {
        "Qualified Type Inference",
        "tests/qualified_type_inference.c4",
        false, 2, {
            "cannot assign to variable 'a' with const-qualified type 'const int'",
            "cannot infer element type for array initializer"
        }
    },
    {
        "Reference Abuse",
        "tests/reference_abuse.c4",
        false, 1, { "cannot take the address of pass-by-reference parameter" }
    },
    {
        "##. Capacity-of",
        "tests/capacity_of.c4",
        false, 6, {
            "expected a type",
            "expected a type",
            "'##.' cannot be applied to type 'struct DummyStruct'",
            "'##.' cannot be applied to type 'union DummyUnion'",
            "'##.' cannot be applied to type 'int[10]'",
            "expected a type",
        }
    },
    {
        "C4 Enum Type Safety (plain int rejected for enum param)",
        "tests/enum_type_safety.c4",
        false, 1, {
            "cannot be implicitly converted to C4 enum type"
        }
    },
    {
        "If-condition unclosed paren: clean error, no crash",
        "tests/if_unclosed_paren.c4",
        false, 2, {
            "expected ')'",
            "expected '{'"
        }
    },
    {
        "C4 Embed Arrow Errors",
        "tests/embed_arrow_errors.c4",
        false, 4, {
            "cannot have multiple `<-` members of the same type",
            "ambiguous member 'x' accessed from multiple embedded fields of 'Player2'",
            "ambiguous member 'x' found in multiple embedded parameters of 'test_param_ambiguous'",
            "declaration 'x' is shadowing 'vector.x'"
        }
    },
    {
        "C++ Template Less-Minus Lexing Regression",
        "tests/template_less_minus.cpp",
    },
    /// --------------
    ///  RUNTIME
    /// ---------------
    {
        "Semicolon Omission Lookahead",
        "tests/semicolons.c4", true
    },
    {
        "If Statement",
        "tests/if_statement_runtime.c4", true,
    },
    {
        "Runtime Bounds Check",
        "tests/bounds_check_runtime.c4", true
    },
    {
        "Root-level := and [] arr := declarations",
        "tests/root_level_decl_runtime.c4", true
    },
    {
        "Local function pointers (cross-platform, no blocks runtime)",
        "tests/local_fn_runtime.c4", true
    },
    {
        "For-loop boolean conditions (for true {}, for (expr) {})",
        "tests/forloop_bool_runtime.c4", true
    },
    {
        "Array type syntax ([]T return type, typedef []T)",
        "tests/array_type_syntax.c4", true
    },
    {
        "Dynamic C4 arrays",
        "tests/dynamic_c4_array_runtime.c4", true
    },
    {
        "Struct function pointer fields (^field (params) rettype;)",
        "tests/struct_fn_ptr_runtime.c4", true
    },
    {
        "Defer Sigil Unwinding Verification",
        "tests/defer.c4", true,
    },
    {
        ":: on C macros",
        "tests/macro_test.c4", true,
    },
    {
        "Default Parameter Extraction Pipeline",
        "tests/default_params.c4", true,
    },
    {
        "Swizzling Compound Literals",
        "tests/swizzle.c4", true,
    },
    {
        "Type Inference (runtime)",
        "tests/type_inference_runtime.c4", true,
    },
    {
        "Refrences",
        "tests/reference.c4", true
    },
    {
        "Subscript Side-Effect Evaluation (arr[sp++] double-increment regression)",
        "tests/subscript_side_effects.c4", true
    },
    {
        "Pointer Syntax & Sized Arrays (^T, ^^T, [N]T, ^expr, ^[]T)",
        "tests/caret_pointers_runtime.c4", true
    },
    {
        "`as` cast operator (int as float, char, chain)",
        "tests/as_cast_runtime.c4", true
    },
    {
        "Symbol-of operator ($$.ident -> string literal)",
        "tests/symbol_of_runtime.c4", true
    },
    {
        "C4 enum declarations (unqualified, qualified, iota, implicit dot, OR group, switch)",
        "tests/enums_c4_runtime.c4", true
    },
    {
        "Anonymous C4 enums",
        "tests/anon_enum_runtime.c4", true
    },
    {
        "Struct brace-init assignment (pos = {4, 5})",
        "tests/struct_brace_assign_runtime.c4", true
    },
    {
        "Comma separated struct members and trailing comma in function headers",
        "tests/comma_syntax_runtime.c4", true
    },
    {
        "Custom C4 switch syntax",
        "tests/switch_syntax_runtime.c4", true
    },
    {
        "@error handler (@error {}, @error(e) {}, negative-index, in-range no-fire)",
        "tests/error_handler_test.c4", true
    },
    {
        "C4 struct syntax (auto-typedef, anonymous struct, comma separators)",
        "tests/struct_syntax_test.c4", true
    },
    {
        "C4 enum member collision test across different enums",
        "tests/enum_collision.c4", true
    },
    {
        "C4 struct semicolon omission and typedef test",
        "tests/struct_semicolon_test.c4", true
    },
    {
        "C4 Embed Arrow Runtime",
        "tests/embed_arrow_runtime.c4", true
    },
};

bool run_compiler_tests(const char *compiler_path) {
    printf( "\033[2m==================================================================\033[0m\n" );
    printf( "\033[36m(cyan)\033[0;2m compile-time | \033[0;33m(gold)\033[0;2m runtime\033[0m\n" );
    printf( ">>> running c4 test suite...\n" );

    int passed = 0;
    int total = sizeof(custom_test_suite) / sizeof(custom_test_suite[0]);

    char compiler_log_buffer[4096];
    const char *temp_bin = "./temp_nob_runtime_bin";

    for (int i = 0; i < total; ++i) {
        CompilerTest test = custom_test_suite[i];

        if (!file_exists(test.path)) {
            printf("\033[31m[FAIL]\033[36m %s \033[0;2m(Missing test file: %s)\033[0m\n", test.name, test.path);
            continue;
        }

        Cmd cmd = {0};
        if (test.is_runtime_test) {
            cmd_append(&cmd, compiler_path, "-o", temp_bin, "-O2", "-UNDEBUG", test.path);
        } else {
            cmd_append(&cmd, compiler_path, "-fsyntax-only", test.path);
        }

        // Capture the exit code integer status
        int exit_code = cmd_run_and_capture(&cmd, compiler_log_buffer, sizeof(compiler_log_buffer));
        cmd_free(cmd);

        if (exit_code == -1) {
            printf("\033[31m[FAIL]\033[0;2m %s (Process crashed, timed out, or encountered a signal violation)\033[0m\n", test.name);
            continue;
        }

        // --- STAGE A: RUNTIME TESTS VERIFICATION ---
        #define RUNTIME_FMT "\033[33m %s \033[37m<%s> "

        if (test.is_runtime_test) {
            if (!file_exists(temp_bin)) {
                printf("\033[31m[FAIL]"RUNTIME_FMT"\033[0;2m(Compilation failed to produce a binary! Compiler Output):\n%s\033[0m\n",
                       test.name, test.path, compiler_log_buffer);
                continue;
            }

            Cmd run_cmd = {0};
            cmd_append(&run_cmd, temp_bin);
            char run_log_buf[4096];

            // Execute the compiled test binary and extract its exit code status
            int runtime_exit_code = cmd_run_and_capture(&run_cmd, run_log_buf, sizeof(run_log_buf));
            cmd_free(run_cmd);
            remove(temp_bin);

            // CRITICAL CONTRACT CHECK: A runtime test passes IF and ONLY IF it exits with exactly 0
            if (runtime_exit_code == 0) {
                printf("\033[32m[PASS]"RUNTIME_FMT"\033[0;2m(Program executed and passed assertions smoothly)\033[0m\n", test.name, test.path);
                passed++;
            } else {
                printf("\033[31m[FAIL]"RUNTIME_FMT"\033[0;2m(Program failed! Terminated with non-zero exit code: %d. Output):\n%s\033[0m\n",
                       test.name, test.path, runtime_exit_code, run_log_buf);
            }
            continue;
        }

        // --- STAGE B: COMPILE-TIME DIAGNOSTIC CHECKS ---
        // For compile-time checks, we ignore the exit code value completely and evaluate the error strings!
        #define COMPTIME_FMT "\033[36m %s \033[37m<%s> "

        int total_errors_seen = 0;
        const char *search_ptr = compiler_log_buffer;
        while ((search_ptr = strstr(search_ptr, "error:")) != NULL) {
            total_errors_seen++;
            search_ptr += 6;
        }
        if (strcmp(test.name, "C4 Embed Arrow Errors") == 0) {
            search_ptr = compiler_log_buffer;
            while ((search_ptr = strstr(search_ptr, "warning:")) != NULL) {
                total_errors_seen++;
                search_ptr += 8;
            }
        }


        if (total_errors_seen != test.expected_error_count) {
            printf("\033[31m[FAIL]"COMPTIME_FMT"\033[0;2m(Mismatched error count! Compiler reported %d errors instead of the expected %d):\n%s\033[0m\n",
                test.name, test.path, total_errors_seen, test.expected_error_count, compiler_log_buffer);
            continue;
        }

        bool all_expected_errors_found = true;
        for (int err_idx = 0; err_idx < test.expected_error_count; ++err_idx) {
            const char *expected_msg = test.expected_errors[err_idx];
            if (!expected_msg) continue;

            if (strstr(compiler_log_buffer, expected_msg) == NULL) {
                printf("\033[31m[FAIL]"COMPTIME_FMT"\033[0;2m(Missing required diagnostic target phrase! Did not see: '%s')\n",
                    test.name, test.path, expected_msg);
                all_expected_errors_found = false;
                break;
            }
        }

        if (all_expected_errors_found) {
            if (test.expected_error_count == 0) {
                printf("\033[32m[PASS]"COMPTIME_FMT"\033[0m\n", test.name, test.path);
            } else {
                printf("\033[32m[PASS]"COMPTIME_FMT"\033[0;2m(Compiler successfully caught all %d custom violations uniquely)\033[0m\n",
                       test.name, test.path, test.expected_error_count);
            }
            passed++;
        } else {
            printf("\033[31m[FAIL]"COMPTIME_FMT"\033[0;2m(Error verification mismatched. Raw logs):\n%s\033[0m\n",
                   test.name, test.path, compiler_log_buffer);
        }
    }

    printf( ">>> results:%s %d\033[0m / %d tests passed.\n", passed == total ? "\033[32m" : "\033[31m", passed, total);
    printf( "\033[2m==================================================================\033[0m\n" );
    return passed == total;
}



#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#endif


int cmd_run_and_capture(Cmd *cmd, char *output_buf, size_t buf_size) {
    if (cmd->count == 0 || output_buf == NULL || buf_size == 0) return -1;
    memset(output_buf, 0, buf_size);

#ifdef _WIN32
    // =========================================================================
    // WINDOWS IMPLEMENTATION
    // =========================================================================
    HANDLE h_read_pipe = NULL;
    HANDLE h_write_pipe = NULL;
    SECURITY_ATTRIBUTES sa_attr;
    sa_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_attr.bInheritHandle = TRUE;
    sa_attr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&h_read_pipe, &h_write_pipe, &sa_attr, 0)) {
        nob_log(ERROR, "Failed to create Windows pipe for stream capture");
        return -1;
    }

    if (!SetHandleInformation(h_read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        nob_log(ERROR, "Failed to set pipe handle information");
        CloseHandle(h_read_pipe);
        CloseHandle(h_write_pipe);
        return -1;
    }

    // Flatten argv array into a single Windows command line string
    size_t cmd_line_len = 0;
    for (size_t i = 0; i < cmd->count; ++i) {
        cmd_line_len += strlen(cmd->items[i]) + 4;
    }

    char *cmd_line = (char*)malloc(cmd_line_len + 1);
    if (cmd_line == NULL) {
        CloseHandle(h_read_pipe);
        CloseHandle(h_write_pipe);
        return -1;
    }
    cmd_line[0] = '\0';

    for (size_t i = 0; i < cmd->count; ++i) {
        strcat(cmd_line, "\"");
        strcat(cmd_line, cmd->items[i]);
        strcat(cmd_line, "\"");
        if (i < cmd->count - 1) strcat(cmd_line, " ");
    }

    STARTUPINFOA si_start_info;
    PROCESS_INFORMATION pi_proc_info;
    ZeroMemory(&si_start_info, sizeof(STARTUPINFOA));
    ZeroMemory(&pi_proc_info, sizeof(PROCESS_INFORMATION));

    si_start_info.cb = sizeof(STARTUPINFOA);
    si_start_info.hStdError = h_write_pipe;
    si_start_info.hStdOutput = h_write_pipe;
    si_start_info.dwFlags |= STARTF_USESTDHANDLES;

    BOOL success = CreateProcessA(
        NULL, cmd_line, NULL, NULL, TRUE, 0, NULL, NULL, &si_start_info, &pi_proc_info
    );

    free(cmd_line);

    if (!success) {
        nob_log(ERROR, "Failed to launch Windows subprocess");
        CloseHandle(h_read_pipe);
        CloseHandle(h_write_pipe);
        return -1;
    }

    // Close write end so read loop hits EOF
    CloseHandle(h_write_pipe);

    size_t total_read = 0;
    DWORD bytes_read = 0;
    while (total_read < buf_size - 1) {
        size_t remaining = buf_size - 1 - total_read;
        success = ReadFile(h_read_pipe, output_buf + total_read, (DWORD)remaining, &bytes_read, NULL);
        if (!success || bytes_read == 0) break;
        total_read += bytes_read;
    }
    output_buf[total_read] = '\0';
    CloseHandle(h_read_pipe);

    WaitForSingleObject(pi_proc_info.hProcess, INFINITE);

    DWORD exit_code = 0;
    int result = -1;
    if (GetExitCodeProcess(pi_proc_info.hProcess, &exit_code)) {
        result = (int)exit_code;
    }

    CloseHandle(pi_proc_info.hProcess);
    CloseHandle(pi_proc_info.hThread);
    return result;

#else
    // =========================================================================
    // POSIX IMPLEMENTATION (Linux, macOS, MSYS2 POSIX layers)
    // =========================================================================
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        nob_log(ERROR, "failed to create posix pipe for stream capture");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        nob_log(ERROR, "failed to fork compiler test subprocess");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    if (pid == 0) {
        // --- child process ---
        close(pipe_fds[0]);
        dup2(pipe_fds[1], 1);
        dup2(pipe_fds[1], 2);
        close(pipe_fds[1]);

        // Ensure array is null terminated for execvp
        da_append(cmd, NULL);
        execvp(cmd->items[0], (char* const*)cmd->items);
        exit(EXIT_FAILURE);
    }

    // --- parent process ---
    close(pipe_fds[1]);

    size_t total_read = 0;
    ssize_t bytes_read;
    while (total_read < buf_size - 1 &&
          (bytes_read = read(pipe_fds[0], output_buf + total_read, buf_size - 1 - total_read)) > 0) {
        total_read += bytes_read;
    }
    output_buf[total_read] = '\0';
    close(pipe_fds[0]);

    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno != EINTR) return -1;
    }

    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    return -1;
#endif
}
