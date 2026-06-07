#include <stdio.h>

int bob(int a, int b, int c)
{
    return a + b * c;
}

void print_int(int value)
{
    printf("%d\n", value);
}

int main(void)
{
    print_int( bob( bob(1, 2, 3), 4, 5 ) );
    return 0;
}
