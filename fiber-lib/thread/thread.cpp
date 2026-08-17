#include "thread.h"

#include <sys/syscall.h>
#include <iostream>
#include <unistd.h>


namespace sylar
{

// 线程信息
/*
thread_local 表示变量是线程局部的，即每个访问该变量的线程都会拥有一个独立副本。
例如每个线程都会独立拥有一个 Thread 指针和当前线程名称，多个线程的副本互不干扰
*/
static thread_local Thread* t_thread          = nullptr;    //当前线程的Thread对象指针
static thread_local std::string t_thread_name = "UNKNOWN";  //当前线程的名称。

// 构造函数
Thread::Thread(std::function<void()> cb, const std::string& name)
: m_cb(cb)
, m_name(name)
{
    // 这里需要注意的是this是传递给run函数进行转换的。
    // pthread_create的核心作用是为线程绑定入口函数，即指定线程创建完成后要执行的任务内容
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);    // 成功返回 0，失败返回错误码
    if (rt != 0)
    {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
        throw std::logic_error("pthread_create error");
    }

    // 等待线程函数完成初始化
    m_semaphore.wait();
}

// 析构函数
Thread::~Thread()
{
    if (m_thread)
    {
        // 使线程进入“分离状态”。该状态下的线程在运行结束后，系统会自动回收其占用的所有资源（如栈空间、TCB 等）。
        // 场景：主线程不需要等待子线程结果（即不需要调用 join），且希望避免因忘记调用 join 而产生的内存泄漏。
        pthread_detach(m_thread);
        m_thread = 0;
    }
}

void Thread::join()
{
    if (m_thread)
    {
        int rt = pthread_join(m_thread, nullptr);
        if (rt)
        {
            std::cerr << "pthread_join failed, rt = " << rt << ", name = " << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

// 获取系统分配的线程id
pid_t Thread::GetThreadId()
{
    //syscall(SYS_gettid)是一个系统调用，用于获取当前线程的唯一ID。SYS_gettid 是 Linux 特定的系统调用编号，用来获取线程ID (TID)。pid_t 是一个数据类型，用于表示进程ID或线程ID
    return syscall(SYS_gettid);
}

// 获取当前所在线程
Thread* Thread::GetThis()
{
    return t_thread;
}

//获取当前线程的名字
const std::string& Thread::GetName()
{
    return t_thread_name;
}

//给当前线程设置名字
void Thread::SetName(const std::string &name)
{
    if (t_thread)
        {
            t_thread->m_name = name;
        }
    t_thread_name = name;
}

// 执行函数
void* Thread::run(void* arg)
{
    Thread* thread = (Thread*)arg;

    t_thread       = thread;
    t_thread_name  = thread->m_name;
    thread->m_id   = GetThreadId();

    // 功能：非标准扩展，用于调试。在 top -H 或 gdb 中可以直接看到线程名
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(thread->m_cb);   // swap -> 可以减少m_cb中只能指针的引用计数

    // 初始化完成
    thread->m_semaphore.signal();

    cb();
    return 0;
}


}