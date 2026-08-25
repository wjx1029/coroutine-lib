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
//  OManager::FdContext::getEventContext
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

// ===================================================================
// IOManager::addEvent
// ===================================================================
int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    //查找FdContext对象
    // attemp to find FdContext
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);//使用读写锁
    if ((int)m_fdContexts.size() > fd)//如果说传入的fd在数组里面则查找然后初始化FdContext的对象
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else//不存在则重新分配数组的size来初始化FdContext的对象
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);                                            					
        fd_ctx = m_fdContexts[fd];
    }

    //一旦找到或者创建Fdcontextt的对象后，加上互斥锁，确保Fdcontext的状态不会被其他线程修改
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event has already been added
     if(fd_ctx->events & event)//判断事件是否存在存在？是就返回-1，因为相同的事件不能重复添加
     {
         return -1;
     }

    // add new event
 	//所以这里就很好判断了如果已经存在就fd_ctx->events本身已经有读或写，就是修改已经有事件，如果不存在就是none事件的情况，就添加事件。
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events   = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx; 
    //函数将事件添加到 epoll 中。如果添加失败，打印错误信息并返回 -1。
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    ++m_pendingEventCount;//原子计数器，待处理的事件++；

    // update fdcontext
    fd_ctx->events = (Event)(fd_ctx->events | event);//更新 FdContext 的 events 成员，记录当前的所有事件。注意events可以监听读和写的组合，如果fd_ctx->events为none,就相当于直接是fd_ctx->events = event

    // update event context
    //设置事件上下文
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);//确保 EventContext 中没有其他正在执行的调度器、协程或回调函数。
    event_ctx.scheduler = Scheduler::GetThis();//设置调度器为当前的调度器实例（Scheduler::GetThis()）。
    //如果提供了回调函数 cb，则将其保存到 EventContext 中；否则，将当前正在运行的协程保存到 EventContext 中，并确保协程的状态是正在运行。
    if (cb)
    {
        event_ctx.cb.swap(cb);
    }    
    else
    {
        event_ctx.fiber = Fiber::GetThis();//需要确保存在主协程
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0;

}



}