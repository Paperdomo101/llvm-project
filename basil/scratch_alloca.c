#include <stdio.h>
#include <stdlib.h>

struct MyStruct {
    void *ptr;
    int size;
};

int main() {
    int size = 21;
    struct MyStruct buffer = { ({ void *p = __builtin_alloca(size); p; }), size };
    printf("%p\n", buffer.ptr);
    return 0;
}
