#include<ucontext.h>
#include<stdio.h>
void func1(void *arg)
{
    puts("1");
    puts("11");
    puts("111");
    puts("1111");
}//此函数用来给makecontext使用
void context_test()
{
    char stack[1024*128];//设置栈的空间
    ucontext_t child,main;//设置两个上下文
    getcontext(&child);//将此时的上下文信息保存到child中
    child.uc_stack.ss_sp=stack;//指定栈空间
    child.uc_stack.ss_size=sizeof(stack);//指定栈空间大小
    child.uc_stack.ss_flags=0;
    child.uc_link=&main;//设置后继上下文
    makecontext(&child,(void(*)(void))func1,0);//修改上下文让其指向func1的函数
    swapcontext(&main,&child);//切换到child上下文，保存当前上下文到main
    puts("main");//如果设置了后继上下文也就是uc_link指向了其他ucontext_t的结构体对象则makecontext中的函数function
                //执行完成后会返回此处打印main，如果指向的为nullptr就直接结束
}
int main()
{
    context_test();
    return 0;
}


/*
具体的分析：
首先我们设计一个 func1 函数，供 makecontext 使用。
当 makecontext 执行后，会将 child 上下文绑定到 func1 函数，只有通过 swapcontext 等函数切换后，func1 才会真正执行。
可以看到，getcontext 和 makecontext 是成对使用的，必须在 makecontext 之前正确设置好 ucontext_t 结构体。
调用 swapcontext 后，会将当前上下文保存到 main，并切换到 child 上下文执行。
由于 child 已经通过 makecontext 绑定了 func1，因此程序会执行 func1 并打印结果。
又因为 uc_link 设置了后继上下文，所以 func1 执行完毕后，会从 swapcontext 的下一行继续执行，打印 main，至此整个流程结束。
所以，通过这套流程：非对称协程的特点得以体现。

协程 1 是 child，主协程是 main。
控制权从主协程（context_test 中的 main 上下文）切换到 child（协程 1），执行完成后，控制权再回到 main 继续执行。
swapcontext + uc_link 共同实现了协程的切换与恢复，完成了main → child → main 的完整协程流转。

*/