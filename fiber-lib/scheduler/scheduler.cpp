#include "scheduler.h"

static bool debug = false;

namespace sylar
{

static thread_local Scheduler* t_scheduler = nullptr;

//====================
// 构造函数：
// ====================
/*
在构造函数中，首先完成了对线程池规模（threads）、调用者线程是否参与调度（use_caller）以及调度器名称等基本参数的初始化。
随后，程序会根据 use_caller 的值调整调度架构：
● 如果 use_caller 为 true：意味着创建调度器的当前线程（Caller 线程）也将作为工作线程参与任务调度。因此，线程池实际需要额外创建的新线程数量需减一。
● 协程环境初始化：根据前文所述，在当前线程参与调度前，必须先调用 Fiber::GetThis()。该操作的作用是初始化当前线程的线程局部变量，并将当前运行环境封装为该线程的主协程。
● 调度协程的创建：接着，通过 m_schedulerFiber.reset() 创建一个专门运行 run 方法的调度协程。这个调度协程将负责在当前线程中执行任务分发循环。
● 线程信息记录：最后，记录当前线程（主线程）的 ID 并将其存入工作线程 ID 集合中，同时将计算后需要额外创建的线程总数赋值给 m_threadCount。
*/
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
: m_useCaller(use_caller), m_name(name)
{
    //首先判断线程的数量是否大于0，并且调度器的对象是否是空指针，是就调用setThis()进行设置.
    assert(threads > 0 && t_scheduler == nullptr);

    SetThis(); //设置当前调度器对象

    Thread::SetName(m_name); //设置当前线程的名称为调度器的名称 m_name。

    // 使用主线程当作工作线程，创建协程的主要原因是为了实现更高效的任务调度和管理
    if (use_caller)
    {   //如果user_caller为true，表示当前线程也要作为一个工作线程使用。
        threads--;

        // 创建主协程
        Fiber::GetThis();

        // 创建调度协程  false -> 该调度协程退出后将返回主协程
        m_scheduleFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        Fiber::SetSchedulerFiber(m_scheduleFiber.get());//设置协程的调度器对象
        m_rootThread = Thread::GetThreadId();//获取主线程ID
        m_threadIds.push_back(m_rootThread);
    }

    m_threadCount = threads;//将剩余的线程数量（即总线程数量减去是否使用调用者线程）赋值给 m_threadCount
    if(debug) std::cout << "Scheduler::Scheduler() success\n";
}


// ====================
// 析构函数
// ====================
// 等到Scheduler对象结合后用来释放资源，防止资源不释放占用系统资
Scheduler::~Scheduler()
{
    assert(stopping() == true); //判断调度器是否终止
    if (GetThis() == this)      //获取调度器的对象 
    {
        t_scheduler == nullptr; //将其设置为nullptr防止悬空指针
    }
    if (debug) std::cout << "Scheduler::~Scheduler() success\n";
}

// ====================
// 获得当前调度器
// ====================
Scheduler* Scheduler::GetThis()
{
    return t_scheduler;
}

// ====================
// 设置当前调度器
// ====================
void Scheduler::SetThis()
{
    t_scheduler = this;
}

// ====================
// start() 初始化调度线程池
// ====================
void Scheduler::start()
{
    std::lock_guard<std::mutex> lock(m_mutex); //互斥锁防止共享资源的竞争
    if(is_stopping)//如果调度器退出直接报错打印cerr
	{
		std::cerr << "Scheduler is stopped" << std::endl;
		return;
	}

    assert(m_threads.empty());  //首先线程池数量为空
    m_threads.resize(m_threadCount);//将其线程池的数量多少重置

    for (size_t i = 0; i < m_threadCount; i++)
    {
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }

    if(debug) std::cout << "Scheduler::start() success\n";
}

// ====================
// stop()
// ====================
void Scheduler::stop()
{
    if(debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;

    if (stopping())
        return;

    is_stopping = true;

    if (m_useCaller)
        assert(GetThis() == this);
    else
        assert(GetThis() != this);

    //调用tickle()的目的唤醒空闲线程或协程，防止m_scheduler或其他线程处于永久阻塞在等待任务的状态中
    for (size_t i = 0; i < m_threadCount; i++)
    {
        tickle();//唤醒空闲线程
    }    

    if (m_scheduleFiber)
    {
        tickle();//唤醒可能处于挂起状态，等待下一个任务的调度的协程
    }

    //当只有主线程或调度线程作为工作线程的情况，只能从stop()方法开始任务调度
    if(m_scheduleFiber)
    {
        m_scheduleFiber->resume();//开始任务调度
        if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
    }

    //获取此时的线程, 通过swap不会增加引用计数的方式加入到thrs，方便下面的join保持线程正常退出
    std::vector<std::shared_ptr<Thread>> thrs;
    {
  	std::lock_guard<std::mutex> lock(m_mutex);
        thrs.swap(m_threads);
    }

    for(auto &i : thrs)
    {
        i->join();
    }
    if(debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;

}

// ====================
// tickle()
// ====================
void Scheduler::tickle()
{

}

// ====================
// run()
// 作用：调度器的核心，负责从任务队列中取出任务并通过协程执行
// ====================
void Scheduler::run()
{
    int thread_id = Thread::GetThreadId(); //获取当前线程的ID
    if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;

    // set_hook_enable(true);

    SetThis(); //设置调度器对象

    // 运行在新创建的线程 -> 需要创建主协程
    if (thread_id != m_rootThread)  //如果不是主线程，创建主协程
    {
        Fiber::GetThis();   //分配了线程的主协程和调度协程
    }

    //创建空闲协程，std::make_shared 是 C++11 引入的一个函数，用于创建 std::shared_ptr 对象。相比于直接使用 std::shared_ptr 构造函数，std::make_shared 更高效且更安全，因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作。
    std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));//子协程
    ScheduleTask task;

    while (true)
    {
        task.reset();
        bool tickle_me = false;//是否唤醒了其他线程进行任务调度

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.begin();
            // 1 遍历任务队列
            while (it != m_tasks.end())
            {
                //不能等于当前thread_id,其目的是让人任何的线程都可以执行。
                if (it->thread != -1 && it->thread != thread_id)
                {
                    it++;
                    tickle_me = true;
                    continue;
                }

                // 2 取出任务
                assert(it->fiber||it->cb);
                task = *it;
                m_tasks.erase(it);
                m_activeThreadNum++;
                break;//这里取到任务的线程就直接break所以并没有遍历到队尾
            }
            tickle_me = tickle_me || (it != m_tasks.end());//确保任然存在未处理的任务
        }

        if(tickle_me)//这里虽然写了唤醒但并没有具体的逻辑代码，具体的在io+scheduler
        {
            tickle();
        }

        // 3 执行任务
        if(task.fiber)
        {		//resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程-1；
            {
                std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
                if(task.fiber->getState()!=Fiber::TERM)
                {
                    task.fiber->resume();
                }
            }
            m_activeThreadNum--;//线程完成任务后就不再处于活跃状态，而是进入空闲状态，因此需要将活跃线程计数减一。
            task.reset();
        }
        else if(task.cb)
        {	  //上面解释过对于函数也应该被调度，具体做法就封装成协程加入调度。
            std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
            {
                std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
                cb_fiber->resume();
            }
            m_activeThreadNum--;
            task.reset();
        }
        else    // 4 无任务 -> 执行空闲协程
        {
            // 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
  		    if (idle_fiber->getState() == Fiber::TERM)
            {		//如果调度器没有调度任务，那么idle协程回不断的resume/yield,不会结束进入一个忙等待，如果idele协程结束了
                  //一定是调度器停止了，直到有任务才执行上面的if/else，在这里idle_fiber就是不断的和主协程进行交互的子协程
                if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                break;
            }
            m_idleThreadNum++;
            idle_fiber->resume();
            m_idleThreadNum--;
        }

    }


}

// ====================
// idle()
// ====================
void Scheduler::idle()
{
	while(!stopping())
	{
		if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;	
		sleep(1);//降低空闲协程在无任务时对cpu占用率，避免空转浪费资源
		Fiber::GetThis()->yield();
	}
}


// ====================
// stopping()
// ====================
bool Scheduler::stopping()
{
    // m_tasks，m_activeThreadCount会被多线程竞争所以需要互斥锁来保护资源的访问
    std::lock_guard<std::mutex> lock(m_mutex);
    return is_stopping && m_tasks.empty() && m_activeThreadNum == 0;
}


}