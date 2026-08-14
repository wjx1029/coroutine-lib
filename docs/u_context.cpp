#include<stdio.h>
#include<ucontext.h>
#include<unistd.h>
int main(int argc,const char *argv[]){
ucontext_t context;//创建结构体对象
getcontext(&context);//获取上下文
puts("Hello world");
sleep(1);
setcontext(&context);//恢复getcontext指向的上下文
return 0;
}

//运行结果
//通过
// gcc example.c -o example
// 会无限制的打印hello world.