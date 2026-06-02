#ifndef BATBUG_H
#define BATBUG_H
     /* ___________________________________ */
    //-(______________BATBUG_______________)-//
      /*
        # A single header c debug library

        ## Quick Example
            ```c
            // --- toggle library --- //
            // #define BATBUG_OFF

            // --- explicit names --- //
            // eg. trace_fn() -> BATBUG_TRACE_FN()
            // #define BATBUG_NO_STRIP

            #define BATBUG_IMPLEMENTATION
            #include "batbug.h"

            #include <stdio.h>

            static
            void test( void )
            {
                trace_fn( "" );
                printf( "Hello, testing!\n" );
            }

            int main( int argc, char **argv )
            {
                trace_fn( "%d, %s", argc, *argv );
                test();
                return 0;
            }

            ```

        ### output
            test.c:22	|\_ main(1, ./test)
            test.c:16	|___\_ test()
            Hello, testing!

        ## Revision history
            0.1.0 (2026-05-30) | first release

        ___________________________________ */
    //-(______________v0.1.0_______________)-//


     /* ___________________________________ */
    //-(___________DEPENDENCIES____________)-//

        #include <stdio.h>
     /* ___________________________________ */
    //-(___________________________________)-//


    #define _BATBUG_TRACE(...) do { \
        fprintf( stderr, "%s\033[0;2m(\033[0m", __func__ ); \
        fprintf( stderr, __VA_ARGS__ );                     \
        fprintf( stderr, "\033[2m)\033[0m\n" );             \
    } while (0)

    #ifndef BATBUG_NO_STRIP
        #define trace_fn BATBUG_TRACE_FN
        #define trace_me BATBUG_TRACE_ME
    #endif

    #ifndef BATBUG_OFF

        #define BATBUG_TRACE_ME() \
            fprintf( stderr, "REACHED %s:%d\n", __FILE__, __LINE__)


        extern int _batbug_trace_depth;
        extern void _batbug_decrement_depth( int *unused );

        #define _BATBUG_TRACE_UNDERLINE_STRING          "___" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \
        "___________________________________________________" \

        #define _BATBUG_DEFER_DEPTH \
            __attribute__((cleanup(_batbug_decrement_depth))) int _defer_dummy

            // Your gun.  YES!  no.  YES!  no. YES! n- YES!  *BANG*
        #define BATBUG_TRACE_FN(...) do { \
            fprintf( stderr, "\033[2;36m%s\033[0;36m:%d\t\033[0m", __FILE__, __LINE__ );  \
            fprintf( stderr, "\033[2;30m|%.*s\\\033[2m_\033[0;%dm ",                      \
                _batbug_trace_depth * 3, _BATBUG_TRACE_UNDERLINE_STRING,                  \
                31 + (_batbug_trace_depth % 6)                                            \
            );                                                                            \
            ++_batbug_trace_depth;       \
            _BATBUG_TRACE(__VA_ARGS__); \
        } while (0); \
                    \
        _BATBUG_DEFER_DEPTH;

    #else
        #define BATBUG_TRACE_FN(...)
    #endif


    #ifdef BATBUG_IMPLEMENTATION
     /* ___________________________________ */
    //-(__________IMPLEMENTATION___________)-//

        #ifndef BATBUG_OFF
            int _batbug_trace_depth = 0;

            void _batbug_decrement_depth( int *unused )
            {
                (void)unused;
                _batbug_trace_depth--;
            }
        #endif

     /* ___________________________________ */
    //-(___________________________________)-//
    #endif

#endif
