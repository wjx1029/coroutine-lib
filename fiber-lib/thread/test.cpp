#include "thread.h"
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>  

using namespace sylar;

void func()
{
    std::cout << "id: " << Thread::GetThreadId() << ", name: " << Thread::GetName();
    std::cout << ", this id: " << Thread::GetThis()->getId() << ", this name: " << Thread::GetThis()->getName() << std::endl;

    sleep(120);
}

int main() {
    std::vector<std::shared_ptr<Thread>> threads;

    for(int i=0;i<5;i++)
    {
        std::shared_ptr<Thread> thread = std::make_shared<Thread>(&func, "thread_"+std::to_string(i));
        threads.push_back(thread);
    }

    for(int i=0;i<5;i++)
    {
        threads[i]->join();
    }

    return 0;
}

/*
编译
g++ *.cpp -std=c++17 -o test

运行
./test

测试
1 查看进程号 
ps uax | grep <name>
2 查看该进程号下所有线程信息
ps -eLf | grep <pid>
*/
