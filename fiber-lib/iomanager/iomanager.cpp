#include "iomanager.h"

#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

static bool debug = true;

namespace sylar
{
// ===================================================================
// IOManager::GetThis()
// ===================================================================

IOManager* IOManager::GetThis()
{
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

// ===================================================================
//  OManager::FdContext::getEventContext(Event event)
// ===================================================================

IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event)
{
    assert(event == READ || event == WRITE);    //判断事件要么是读事件，或者写事件
    switch(event)
    {
        case READ:
            return read;
        case WRITE:
            return write;
    }
    throw std::invalid_argument("Unsupported event type");
}

// ===================================================================
// IOManager::FdContext::resetEventContext
// ===================================================================

void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}


// ===================================================================
// IOManager::FdContext::triggerEvent
// ===================================================================

void IOManager::FdContext::triggerEvent(IOManager::Event event)
{
    assert(events & event);//确保event是中有指定的事件，否则程序中断。

    // delete event
    // 清理该事件，表示不再关注，也就是说，注册IO事件是一次性的，
    //如果想持续关注某个Socket fd的读写事件，那么每次触发事件后都要重新添加
    events = (Event)(events & ~event);//因为不是使用了十六进制位，所以对标志位取反就是相当于将event从events中删除

    // trigge
    EventContext& ctx = getEventContext(event);
    if (ctx.cb)//这个过程就相当于scheduler文件中的main.cc测试一样，把真正要执行的函数放入到任务队列中等线程取出后任务后，协程执行，执行完成后返回主协程继续，执行run方法取任务执行任务(不过可能是不同的线程的协程执行了)。
    {
        // call ScheduleTask(std::function<void()>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.cb);
    }
    else
    {
        // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }

    // reset event context
    resetEventContext(ctx);
    return;

}


// ===================================================================
// IOManager的构造函数
// ===================================================================

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
: Scheduler(threads, use_caller, name), TimerManager()
{
    // create epoll fd
    m_epfd = epoll_create(5000);//5000，epoll_create 的参数实际上在现代 Linux 内核中已经被忽略，最早版本的 Linux 中，这个参数用于指定 epoll 内部使用的事件表的大小
    assert(m_epfd > 0);//错误就终止程序

    // create pipe
    int rt = pipe(m_tickleFds);//创建管道的函数规定了m_tickleFds[0]是读端，1是写端
    assert(!rt);//错误就终止程序

    //将管道的监听注册到epoll上
    epoll_event event;
    event.events  = EPOLLIN | EPOLLET; // Edge Triggered，设置标志位，并且采用边缘触发和读事件。
    event.data.fd = m_tickleFds[0];

    // non-blocked
    //修改管道文件描述符以非阻塞的方式，配合边缘触发。
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);//每次需要判断rt是否成功

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);//将 m_tickleFds[0];作为读事件放入到event监听集合中
    assert(!rt);

    contextResize(32);//初始化了一个包含 32 个文件描述符上下文的数组

    start();//启动 Scheduler，开启线程池，准备处理任务。
}

// ===================================================================
// IOManager的析构函数
// ===================================================================

IOManager::~IOManager()
{
    stop();//关闭scheduler类中的线程池，让任务全部执行完后线程安全退出
    close(m_epfd);//关闭epoll的句柄
    close(m_tickleFds[0]);close(m_tickleFds[1]);//关闭管道读端写端
    //将fdcontext文件描述符一个个关闭
    for (size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if (m_fdContexts[i])
        {
            delete m_fdContexts[i];
        }
    }
}

// ===================================================================
// IOManager::ContextResize()
// ===================================================================

void IOManager::contextResize(size_t size)
{
    m_fdContexts.resize(size);//调整m_fdContexts的大小
    // 遍历 m_fdContexts 向量，初始化尚未初始化的 FdContext 对象
    for (size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if (m_fdContexts[i] == nullptr)
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i;// 将文件描述符的编号赋值给 fd
        }
    }
}



}