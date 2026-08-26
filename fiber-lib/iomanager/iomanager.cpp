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

// ===================================================================
// IOManager::delEvent
// ===================================================================

bool IOManager::delEvent(int fd, Event event)
{
    // attemp to find FdContext
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);//读锁

    if ((int)m_fdContexts.size() > fd)//查找到FdContext如果没查找到代表数组中没这个文件描述符直接，返回false；
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    //找到后添加互斥锁
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if (!(fd_ctx->events & event))//如果事件不相同就返回false，否则就继续
    {
        return false;
    }

    // delete the event
	//因为这里要删除事件，对原有的事件状态取反就是删除原有的状态比如说传入参数是读事件，我们取反就是删除了这个读事件但可能还要写事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op  = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;//这一步是为了在 epoll 事件触发时能够快速找到与该事件相关联的 FdContext 对象。

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    --m_pendingEventCount;//减少了待处理的事件

    // update fdcontext
    fd_ctx->events = new_events;//所以因为要先将fd_ctx的状态放入epevent.data.ptr所以就没先去更新，这也是为什么需要单独写Event new_events

    // update event context
	//重置上下文
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

// ===================================================================
// IOManager::cancelEvent
// ===================================================================

bool IOManager::cancelEvent(int fd, Event event)
{
    // attemp to find FdContext
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);//读锁

    if ((int)m_fdContexts.size() > fd)//查找到FdContext如果没查找到代表数组中没这个文件描述符直接，返回false；
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    //找到后添加互斥锁
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if (!(fd_ctx->events & event))//如果事件不相同就返回false，否则就继续
    {
        return false;
    }

    // delete the event
	//因为这里要删除事件，对原有的事件状态取反就是删除原有的状态比如说传入参数是读事件，我们取反就是删除了这个读事件但可能还要写事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op  = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;//这一步是为了在 epoll 事件触发时能够快速找到与该事件相关联的 FdContext 对象。

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    --m_pendingEventCount;//减少了待处理的事件

    // update fdcontext, event context and trigger
    fd_ctx->triggerEvent(event);//这个代码和上面那个delEvent一致好像就是最后的处理不同一个是重置，一个是调用事件的回调函数
    return true;
}

// ===================================================================
// IOManager::cancelAll
// ===================================================================

bool IOManager::cancelAll(int fd)
{
    // attemp to find FdContext
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);//读锁

    if ((int)m_fdContexts.size() > fd)//查找到FdContext如果没查找到代表数组中没这个文件描述符直接，返回false；
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    //找到后添加互斥锁
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if (!fd_ctx->events)//如果没有事件就返回false，否则就继续
    {
        return false;
    }

    // delete all events
    int op  = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;//这一步是为了在 epoll 事件触发时能够快速找到与该事件相关联的 FdContext 对象。

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // update fdcontext, event context and trigger
    if (fd_ctx->events & READ)
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if (fd_ctx->events & WRITE)
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

// ===================================================================
// IOManager::tickle
// ===================================================================

void IOManager::tickle()
{
    //这个函数在scheduler检查当前是否有线程处于空闲状态。如果没有空闲线程，函数直接返回，不执行后续操作。
    if (!hasIdleThreads())
    {
        return;
    }
    //如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符 "T"。这个写操作的目的是向等待在 m_tickleFds[0]（管道的另一端）的线程发送一个信号，通知它有新任务可以处理了。
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

// ===================================================================
// IOManager::tickle
// ===================================================================

bool IOManager::stopping()
{
    uint64_t timeout = getNextTimer();
    // no timers left and no pending events left with the Scheduler::stopping()
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

// ===================================================================
// IOManager::idle
// ===================================================================

void IOManager::idle()
{
    // 第一步：初始化事件存储空间
    // 定义 epoll_wait 单次能处理的最大事件数 MAX_EVENTS（通常设为 256）。
    // 利用 std::unique_ptr<epoll_event[]> 在堆上动态分配内存，用于存储就绪事件数组，确保资源在函数退出时能自动释放。
    static const uint64_t MAX_EVENTS = 256;
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

    // 第二步：进入主循环与阻塞监听
    // 整个逻辑运行在一个 while(true) 循环中。
    // 首先检查 stopping() 状态以决定是否退出。
    // 随后进入 epoll_wait 阻塞调用。注意其超时机制：通过 getNextTimer() 获取定时器堆中最近的超时剩余时间，并与系统默认的 MAX_TIMEOUT（如 5000ms）取最小值，作为 epoll_wait 的超时参数。这保证了定时器能准时触发。
    while (true)
    {
        if(debug) std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl;

        if (stopping())
        {
            if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            break;
        }

        int rt = 0;
        while (true)    // blocked at epoll_wait
        {
            static const uint64_t MAX_TIMEOUT = 5000;   //定义了最大超时时间为 5000 毫秒。
            uint64_t next_timeout = getNextTimer();     //获取下一个超时的定时器
            next_timeout = std::min(MAX_TIMEOUT, next_timeout); //获取下一个定时器的超时时间，并将其与 MAX_TIMEOUT 取较小值，避免等待时间过长。

            rt = epoll_wait(m_epfd, events.get(), MAX_EVENTS, (int)next_timeout);//epoll_wait陷入阻塞，等待tickle信号的唤醒，并且使用了定时器堆中最早超时的定时器作为epoll_wait超时时间。
            
            if (rt < 0 && errno == EINTR)   // rt小于0代表无限阻塞，errno是EINTR(表示信号中断)
                continue;
            else
                break;
        }   // end epoll_wait

        // 第三步：处理超时定时器
        // 当 epoll_wait 返回后（无论是超时还是事件触发），首先调用 listExpiredCb(cbs)。
        // 该函数会收集所有已到期的定时器回调，并将它们一次性推入调度器的任务队列中，等待后续执行。
        std::vector<std::function<void()>> cbs; //用于存储超时的回调函数。
        listExpiredCb(cbs);     //用来获取所有超时的定时器回调，并将它们添加到 cbs 向量中。
        if (!cbs.empty())
        {
            for (const auto& cb : cbs)
            {
                scheduleLock(cb);
            }
            cbs.clear();
        }

        // 第四步：处理 Tickle 唤醒信号
        // 遍历 epoll_wait 返回的就绪事件数组。
        // 若发现就绪的是 m_tickleFds[0]（管道读端），说明有其他线程通过 tickle() 唤醒了当前线程。
        // 此时通过一个 while 循环将管道中的数据彻底读完（直到返回 -1 且 errno 为 EAGAIN），从而清除唤醒标志。
        for (int i = 0; i < rt; ++i)    // 遍历所有的rt，代表有多少个事件准备了。
        {
            epoll_event& event = events[i]; // 获取第 i 个 epoll_event，用于处理该事件。

            if (event.data.fd == m_tickleFds[0])    // 检查当前事件是否是 tickle 事件（即用于唤醒空闲线程的事件）
            {
                uint8_t dummy[256];
                // edge triggered -> exhaust
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            // 第五步：事件映射与归类
            // 对于其他的 IO 就绪事件，通过 event.data.ptr 获取绑定的文件描述符上下文 fd_ctx。
            // 由于抽象层只对外暴露 READ 和 WRITE 事件，因此需要对 epoll 的原生事件进行转换：如果发生 EPOLLERR（错误）或 EPOLLHUP（挂起），将其映射为该 fd 已注册的 READ 或 WRITE 事件。
            // 这样可以确保即使发生错误，相关的协程也能被唤醒去处理异常（例如执行 read 并发现返回 0 或错误）。
            FdContext *fd_ctx = (FdContext*)event.data.ptr; //通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fd_ctx，该指针包含了与文件描述符相关的上下文信息。
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            if (event.events & (EPOLLERR | EPOLLHUP))   //如果当前事件是错误或挂起（EPOLLERR 或 EPOLLHUP），则将其转换为可读或可写事件（EPOLLIN 或 EPOLLOUT），以便后续处理。
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }

            // 确定实际发生的事件类型（读取、写入或两者）
            int real_events = NONE;     
            if (event.events & EPOLLIN)
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT)
            {
                real_events |= WRITE;
            }
            if ((fd_ctx->events & real_events) == NONE)
            {
                continue;
            }

/*
            第六步：更新状态与触发调度
            根据实际发生的 real_events（读、写或两者组合），计算该 fd 剩余的关注事件 left_events = (fd_ctx->events & ~real_events)。
                ○ 根据是否还有剩余事件，调用 epoll_ctl 执行 MOD（修改）或 DEL（删除）操作。
                ○ 调用 triggerEvent()：将触发的读/写回调函数或协程推入任务队列。这一步是 IO 任务转为普通调度任务的关键。
*/
            int left_events = (fd_ctx->events & ~real_events);  // 这里进行取反就是计算剩余未发送的的事件
            int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events    = EPOLLET | left_events;    // 如果left_event没有事件了那么就只剩下边缘触发了events设置了

            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);    // 根据之前计算的操作（op），调用 epoll_ctl 更新或删除 epoll 监听，如果失败，打印错误并继续处理下一个事件。
            if (rt2)
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // 触发事件，事件的执行
            if (real_events & READ)
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) 
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        }
        // 第七步：协程让出（Yield）
        // 在处理完本次所有的就绪事件后，idle 协程主动调用 yield() 让出 CPU 执行权。此时，调度器会立即切入刚才由定时器或 IO 事件产生的新任务。等到所有任务再次执行完毕，调度器会重新回到 idle 协程开始下一轮监听。
        Fiber::GetThis()->yield();
    }   // end while(true)
}



}