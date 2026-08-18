#include "fiber.h"

static bool debug = false;

namespace sylar
{
// 当前线程上的协程控制信息

static thread_local Fiber* t_fiber = nullptr;   // 正在运行的协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;    // 主协程
static thread_local Fiber* t_scheduler_fiber = nullptr;     // 调度协程


static std::atomic<uint64_t> s_fiber_id{0}; // 协程id
static std::atomic<uint64_t> s_fiber_count{0};  // 协程计数器


// 创建主协程。设置状态，初始化上下文，并分配ID;
Fiber::Fiber()
{
    SetThis(this);      // 在getThis中使用了无参的FIber来构造t_fiber
    m_state = RUNNING;  // 设置协程的状态为可运行

    if (getcontext(&m_ctx))
    {
        std::cerr << "Fiber() failed\n";
		pthread_exit(NULL);
    }

    m_id = s_fiber_id++;    // 分配id，协程id从0开始，用完加1
    s_fiber_count++;        // 活跃的协程数量+1；
    if (debug)
        std::cout << "Fiber(): main id = " << m_id << std::endl;
}

// 创建一个新协程，初始化回调函数，栈的大小和状态。分配栈空间，
// 并通过make修改上下文当set或swap激活ucontext_t m_ctx上下文时候会执行make第二个参数的函数。
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
: m_cb(cb), m_runInScheduler(run_in_scheduler)
{
    m_state = READY;    // 初始化状态

    // 分配协程栈空间
	m_stacksize = stacksize ? stacksize : 128000;
	m_stack = malloc(m_stacksize);

    if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		pthread_exit(NULL);
	}

    //这里因为没有设置了后继所以在运行完mainfunc后协程退出，会调用一次yield返回主协程。
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);

    m_id = s_fiber_id++;
    s_fiber_count++;
    if (debug)
        std::cout << "Fiber(): child id = " << m_id << std::endl;

}

Fiber::~Fiber()
{
	if(m_stack)
	{
        // 存在栈是子协程
        assert(m_state == TERM);
		free(m_stack);
	}
    else
    {
        // 没有栈，说明是线程的主协程
        assert(m_state == RUNNING);
        Fiber* cur = t_fiber;     // 此时运行的协程肯定是主协程
        if (cur == this)
        {
            SetThis(nullptr);
        }
    }

    s_fiber_count --;   // 减少活跃协程计数器
	if(debug)
        std::cout << "~Fiber(): id = " << m_id << std::endl;	
}


void Fiber::reset(std::function<void()> cb)//重置协程状态和⼊⼝函数，复⽤栈空间，不重新创建栈
{
    assert(m_stack != nullptr && m_state == TERM);

    m_state = READY;
    m_cb = cb;

    if (getcontext(&m_ctx))
    {
        std::cerr << "reset() failed\n";
        pthread_exit(NULL);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);

}

void Fiber::resume()   //恢复协程执行。
{
    assert(m_state == READY);

    m_state = RUNNING;

    if (m_runInScheduler)//这里的切换就相当于非对称协程函数那个当a执行完成后会将执行权交给b
    {//m_runInScheduler 为 true，则将上下文切换到调度协程；
        SetThis(this);//这里的setthis实际是就是目前工作的协程。
        if (swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_scheduler_fiber failed\n";
            pthread_exit(NULL);
        }
    }
    else
    {// 否则，切换到主线程的协程。
        SetThis(this);
        if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

void Fiber::yield()    //将执行权还给调度协程
{
    assert(m_state == RUNNING || m_state == TERM);

    if (m_state != TERM)    m_state = READY;
    
    if (m_runInScheduler)
    {
        SetThis(t_scheduler_fiber);
        if (swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
        {
            std::cerr << "yield() to t_scheduler_fiber failed\n";
            pthread_exit(NULL);
        }
    }
    else
    {
        SetThis(t_thread_fiber.get());
        if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
        {
            std::cerr << "yield() to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

void Fiber::SetThis(Fiber *f)          //设置当前运行的协程。
{
    t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()//获取当前运行的协程的shared_ptr实例。
{   
    if (t_fiber)
    {
        return t_fiber->shared_from_this();
    }

    // 若当前线程不存在协程则调用 Fiber() 创建主协程
    std::shared_ptr<Fiber> main_fiber(new Fiber());
    t_thread_fiber = main_fiber;
    t_scheduler_fiber = main_fiber.get();   // 除非主动设置 主协程默认为调度协程

    assert(t_fiber == main_fiber.get());    //用于判断，t_fiber是否等于main_fiber
                                            //是继续执行, 否则程序终止。
    return t_fiber->shared_from_this();

}


void Fiber::SetSchedulerFiber(Fiber* f)//设置调度协程，默认主协程
{
    t_scheduler_fiber = f;
}

uint64_t GetFiberId()           //获取当前运行的协程的ID。
{
    if (t_fiber)
        return t_fiber->getId();

    return (uint64_t)-1;
}

void Fiber::MainFunc()                 //协程的主函数，入口点	
{
    std::shared_ptr<Fiber> cur = GetThis(); // GetThis()的shared_from_this()⽅法让引⽤计数加1
    assert(cur != nullptr);

    cur->m_cb();            //真正执行任务的地方
    cur->m_cb = nullptr;    //防止悬空引用
    cur->m_state = TERM;

    // 运行完毕 -> 让出执行权
	auto raw_ptr = cur.get();   //获取原始指针，不增加引用计数
    cur.reset();                //引用计数-1，如果此时为0，协程对象销毁
    raw_ptr->yield();
}

}