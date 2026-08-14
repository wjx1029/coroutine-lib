#include <stdio.h>
#include <ucontext.h>
#include <cstdlib>

void func1() {
    puts("In func1");
}

int main() {
    ucontext_t context;
    getcontext(&context);
    context.uc_stack.ss_sp = malloc(8192);
    context.uc_stack.ss_size = 8192;
    context.uc_link = NULL;
    makecontext(&context, func1, 0);
    setcontext(&context);
    puts("This will not be printed");
    puts("Hello World");
    return 0;
}