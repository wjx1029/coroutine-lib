#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

/*
宏HOOK_FUN（XX)是一个宏展开机制，通过将XX依次应用于宏定义中的每一个函数名称来生成一系列代码。
这种方式可以有效减少重复代码，提高代码的可读性和维护性
*/
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace sylar
{

//使用线程局部变量，每个线程都会判断一下是否启用了钩子
static thread_local bool t_hook_enable = false;//表示当前线程是否启用了钩子功能。初始值为 false，即钩子功能默认关闭。

bool is_hook_enable()//返回当前线程的钩子功能是否启用。
{
    return t_hook_enable;
}

void set_hook_enable(bool flag)//设置当前线程的钩子功能是否启用。
{
    t_hook_enable = flag;
}

void hook_init()
{
    static bool is_inited = false;  ;//通过一个静态变量来确保 hook_init() 只初始化一次，防止重复初始化。
    if (is_inited)
        return;

    is_inited = true;

// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX)
#undef XX
}

// static variable initialisation will run before the main function
struct HookIniter
{
    HookIniter()    //钩子函数
    {
        hook_init();    //初始化hook，让原始调用绑定到宏展开的函数指针中
    }
};

static HookIniter s_hook_initer;    //定义了一个静态的 HookIniter 实例。由于静态变量的初始化发生在 main() 函数之前，所以 hook_init() 会在程序开始时被调用，从而初始化钩子函数。

}   // end namespace sylar


struct timer_info
{   // 用于跟踪定时器的状态。
    // 具体来说，它有一个cancelled成员变量，通常用于表示定时器是否已经被取消。
    int cancelled = 0;  
};

// universal template for read and write function
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args)
{
    if (!sylar::t_hook_enable)  // 如果全局钩子未启用,直接调用原始 I/O 函数
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    //获取与文件描述符 fd 相关联的上下文 ctx。如果上下文不存在，则直接调用原始的 I/O 函数
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx)
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    if (ctx->isClosed())     // 如果文件描述符已经关闭，设置errno为EBADF并返回-1。
    {
        errno = EBADF;       // 表示文件描述符无效或已经关闭
        return -1;
    }

    if(!ctx->isSocket() || ctx->getUserNonblock())  // 如果文件描述符不是一个socket或者用户设置了非阻塞模式，则直接调用原始的I/O操作函数
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取超时设置并初始化timer_info结构体，用于后续的超时管理和取消操作。
    uint64_t timeout = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);  // timer condition

retry:
    // 调用原始的 I/O 函数
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // EINTR -> Operation interrupted by system -> retry
    while(n == -1 && errno == EINTR) // 如果由于系统中断（EINTR）导致操作失败，函数会重试
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    // 0 resource was temporarily unavailable -> retry until ready
    // 如果I/O操作因为资源暂时不可用（EAGAIN）而失败，函数会添加一个事件监听器来等待资源可用。同时，如果有超时设置，还会启动一个条件计时器来取消事件。
    if (n == -1 && EAGAIN)
    {
        sylar::IOManager* iom = sylar::IOManager::GetThis();

        std::shared_ptr<sylar::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 1 timeout has been set -> add a conditional timer for canceling this operation
        // 如果执行的read等函数在Fdmanager管理的Fdctx中fd设置了超时时间，就会走到这里。添加addconditionTimer事件
        if (timeout != (uint64_t)-1)    // 这行代码检查是否设置了超时时间。
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]()
            {
                auto t = winfo.lock();
                if (!t || t->cancelled) // 如果 timer_info 对象已被释放（!t），或者操作已被取消（t->cancelled 非 0），则直接返回。
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;   // 如果超时时间到达并且事件尚未被处理(即cancelled任然是0)；

                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));  // 取消该文件描述符上的事件，并立即触发一次事件（即恢复被挂起的协程）
            }, winfo);
        }

        // 2 add event -> callback is this fiber
        // 这行代码的作用是将 fd（文件描述符）和 event（要监听的事件，如读或写事件）添加到 IOManager 中进行管理。IOManager 会监听这个文件描述符上的事件，当事件触发时，它会调度相应的协程来处理这个事件。
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if (rt == -1)
        {   // 如果 rt 为-1，说明 addEvent 失败。此时，会打印一条调试信息，并且因为添加事件失败所以要取消之前设置的定时器，避免误触发。
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer)   
            {
                timer->cancel();
            }
            return -1;
        }
        else
        {
            // 如果 addEvent 成功（rt 为 0），当前协程会调用 yield() 函数，将自己挂起，等待事件的触发。
            sylar::Fiber::GetThis()->yield();

            // 3 resume either by addEvent or cancelEvent
            // 当协程被恢复时（例如，事件触发后），它会继续执行 yield() 之后的代码。
            // 如果之前设置了定时器（timer 不为 nullptr），则在事件处理完毕后取消该定时器。
            // 取消定时器的原因是，该定时器的唯一目的是在 I/O 操作超时时取消事件。
            // 如果事件已经正常处理完毕，那么定时器就不再需要了。
            if(timer) 
            {
                timer->cancel();
            }
            
            // 接下来检查 tinfo->cancelled 是否等于 ETIMEDOUT。
            // 如果等于，说明该操作因超时而被取消，因此设置 errno 为 ETIMEDOUT 并返回 -1，表示操作失败。
            if(tinfo->cancelled == ETIMEDOUT) 
            {
                errno = tinfo->cancelled;
                return -1;
            }
            // 如果没有超时，则跳转到 retry 标签，重新尝试这个操作。
            goto retry;
        }
    }
    return 0;
}

extern "C"
{

// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name ## _fun name ## _f = nullptr;
	HOOK_FUN(XX)
#undef XX


// ==========================================================================================================================================================
// sleep函数钩子    /*具体的三个sleep的函数实现的过程都是类似，实现的目标都是为了将时间转换成毫秒，添加到超时时间堆中。然后，暂停协程，方便其他任务执行。*/
// ==========================================================================================================================================================


// 通过钩子机制拦截sleep的调用，并将其改为使用协程来实现非阻塞的休眠。
unsigned int sleep(unsigned int seconds)
{
    if (!sylar::t_hook_enable)  //如果钩子未启用，则调用原始的系统调用
    {
        return sleep_f(seconds);
    }

    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();  // 获取当前正在执行的协程（Fiber），并将其保存到 fiber 变量中。

    sylar::IOManager* iom = sylar::IOManager::GetThis();    // 获得协程调度器

    iom->addTimer(seconds*1000, [fiber, iom](){iom->scheduleLock(fiber, -1);}); // add a timer to reschedule this fiber

    fiber->yield(); // 挂起当前协程的执行，将控制权交还给调度器。

    return 0;
}

// useconds_t一个无符号整数类型，通常用于表示微秒数。在这个函数中，usec表示延时的微秒数，将其转换为毫秒数(usec/1000)后用于定时器。
int usleep(useconds_t usec)
{
    if(!sylar::t_hook_enable)
    {
        return usleep_f(usec);
    }
    
    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();

    iom->addTimer(usec/1000, [fiber, iom](){iom->scheduleLock(fiber);});    // add a timer to reschedule this fiber
    fiber->yield();     // wait for the next resume

    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
    if(!sylar::t_hook_enable)
    {
        return nanosleep_f(req, rem);
    }
    
    int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000;     //timeout_ms 将 tv_sec 转换为毫秒，并将 tv_nsec 转换为毫秒，然后两者相加得到总的超时毫秒数。所以从这里看出实现的也是一个毫秒级的操作。    

    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();

    iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber);});    // add a timer to reschedule this fiber
    fiber->yield();     // wait for the next resume

    return 0;
}


// ==========================================================================================================================================================
// socket钩子函数
// ==========================================================================================================================================================


// 用于在连接超时情况下处理非阻塞套接字连接的实现。它首先尝试使用钩子功能来捕获并管理连接请求的行为，然后使用IOManager和TImer来管理超时机制
int socket(int domain, int type, int protocol)
{
    if(!sylar::t_hook_enable)
    {
        return socket_f(domain, type, protocol);
    }
    
    int fd = socket_f(domain, type, protocol);  // 如果钩子启用了，则通过调用原始的 socket 函数创建套接字，并将返回的文件描述符存储在 fd 变量中。
    if(fd==-1)  // fd是无效的情况
    {
        std::cerr << "socket() failed:" << strerror(errno) << std::endl;
        return fd;
    }
    // 如果socket创建成功会利用Fdmanager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中，如果不在扩展存储文件描述数组大小，并且利用FDctx进行初始化判断是是不是套接字，是不是系统非阻塞模式。
    sylar::FdMgr::GetInstance()->get(fd, true);

    return fd;
}

int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms)
{
    if(!sylar::t_hook_enable)
    {
        return connect_f(fd, addr, addrlen);
    }

    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd); //获取文件描述符 fd 的上下文信息 FdCtx
    if(!ctx || ctx->isClosed())//检查文件描述符上下文是否存在或是否已关闭。
    {
        errno = EBADF;//EBAD表示一个无效的文件描述符
        return -1;
    }
    //如果不是一个套接字调用原始的
    if(!ctx->isSocket())                                                 
    {
        return connect_f(fd, addr, addrlen);
    }
    //检查用户是否设置了非阻塞模式。如果是非阻塞模式，调用原始的
    if(ctx->getUserNonblock())
    {

        return connect_f(fd, addr, addrlen);
    }

    // 尝试进行 connect 操作，返回值存储在 n 中。
    int n = connect_f(fd, addr, addrlen);

    if(n == 0)
    {
        return 0;
    }
    else if(n != -1 || errno != EINPROGRESS) // 说明连接请求未处于等待状态，直接返回结果。
    {
        return n;
    }

    // wait for write event is ready -> connect succeeds
    sylar::IOManager* iom = sylar::IOManager::GetThis();// 获取当前线程的 IOManager 实例。
    std::shared_ptr<sylar::Timer> timer;                // 声明一个定时器对象。
    std::shared_ptr<timer_info> tinfo(new timer_info);  // 创建追踪定时器是否取消的对象
    std::weak_ptr<timer_info> winfo(tinfo);             // 判断追踪定时器对象是否存在

    if(timeout_ms != (uint64_t)-1)  // 检查是否设置了超时时间。如果 timeout_ms 不等于 -1，则创建一个定时器。
    {	// 添加一个定时器，当超时时间到达时，取消事件监听并设置 cancelled 状态。
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]()
            {
                auto t = winfo.lock();//判断追踪定时器对象是否存在或者追踪定时器的成员变量是否大于0.大于0就意味着取消了
                if(!t || t->cancelled)
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;//如果超时了但时间任然未处理
                iom->cancelEvent(fd, sylar::IOManager::WRITE);//将指定的fd的事件触发将事件处理。
            }, winfo);
    }

    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);//为文件描述符 fd 添加一个写事件监听器。这样的目的是为了上面的回调函数处理指定文件描述符
    if(rt == 0)//代表添加事件成功
    {
        sylar::Fiber::GetThis()->yield();

        // resume either by addEvent or cancelEvent
        if(timer)//如果有定时器，取消定时器。
        {
            timer->cancel();
        }

        if(tinfo->cancelled)//发生超时错误或者用户取消
        {
            errno = tinfo->cancelled;//赋值给errno通过其查看具体错误原因。
            return -1;
        }
    }
    else    //添加事件失败
    {
        if(timer)
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // check out if the connection socket established 
    int error = 0;
    socklen_t len = sizeof(int);
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))//通过getsocketopt检查套接字实际错误状态
        //来判断是否成功或失败。
    {
        return -1;
    }
    if(!error)//如果没有错误，返回 0 表示连接成功。
    {
        return 0;
    }
    else//如果有错误，设置 errno 并返回错误。
    {
        errno = error;
        return -1;
    }
}

// s_connect_timeout 是一个 static 变量，表示默认的连接超时时间，类型为 uint64_t，可以存储 64 位无符号整数。
// -1 通常用于表示一个无效或未设置的值。
// 由于它是无符号整数，-1 实际上会被解释为 UINT64_MAX，表示没有超时限制。
static uint64_t s_connect_timeout = -1;   

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);  // 调用hook启用后的connect_with_timeout函数
}

// accept
// 用于处理套接字接收连接的操作，同时支持超时连接控制
// 实现了非阻塞accpet的操作，并且如果成功接收了一个新的连接，则将新的文件描述符fd添加到文件描述符管理器(FdManager)中进行跟踪管理
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd = do_io(sockfd, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);	
	if(fd >= 0)
	{
		sylar::FdMgr::GetInstance()->get(fd, true); // 添加到文件描述符管理器FdManager中
    }
    return fd;
}

// 将所有文件描述符的事件处理了调用IOManager的callAll函数将fd上的读写事件全部处理，最后从FdManger文件描述符管理中移除该fd。
int close(int fd)
{
    if(!sylar::t_hook_enable)
    {
        return close_f(fd);
    }

    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

    if(ctx)
    {
        auto iom = sylar::IOManager::GetThis();
        if(iom)
        {
            iom->cancelAll(fd);
        }
        // del fdctx
        sylar::FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);//处理完后调用原始系统调用
}


}