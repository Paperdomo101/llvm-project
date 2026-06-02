#include <stdio.h>

int bob(int a, b, c)
{
    return a + b * c;
}

void print_int(int value)
{
    printf("%d\n", value);
}

int main(void)
{
    bob(1, 2, 3)::bob(4, 5)::print_int();
    return 0;
}
