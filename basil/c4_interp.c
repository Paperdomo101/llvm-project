#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Heap-allocated interpolated string builder.
// Callers should free() the result when done, or use strdup() to
// copy to a managed buffer.  We use malloc instead of alloca because
// alloca inside a helper function frees memory on return.
//   persistent := %"`n` is zero"::strdup()
//   %"`n`\n"::printf()        // okay; pointer lives through the call
char *__c4_interp_str(const char *fmt, ...) {
    va_list args, args_copy;

    va_start(args, fmt);
    va_copy(args_copy, args);

    // Compute required length.
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) {
        va_end(args);
        return (char *)"";
    }

    // Heap-allocate the buffer.
    char *buf = (char *)malloc((size_t)len + 1);

    // Format into the buffer.
    vsnprintf(buf, (size_t)len + 1, fmt, args);
    va_end(args);

    return buf;
}
