#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include "fiber.h"
#include "thread.h"

#include <mutex>
#include <vector>

namespace sylar 
{

class Scheduler
{
public:
    //threads指定线程池的线程数量，use_caller指定是否将主线程作为工作线程，name调度器的名称
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name="Scheduler");
    virtual ~Scheduler();//防止出现资源泄露，基类指针删除派生类对象时不完全销毁的问题。

    const std::string& getName() const {return m_name;}//获取调度器的名字

public:
    static Scheduler* GetThis();    // 获取正在运行的调度器

protected:
    void SetThis(); // 设置正在运行的调度器

public:
    // 添加任务到任务队列
 	//  Fiber_or_Cb 调度任务类型，可以是协程对象或函数指针
    template <class Fiber_or_Cb>//这个不需要想那么复杂看成T也行
    void scheduleLock(Fiber_or_Cb fc, int thread = -1)
    {
        bool need_tickle;//用于标记任务队列是否为空，从而判断是否需要唤醒线程。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // empty ->  all thread is idle -> need to be waken up
            need_tickle = m_tasks.empty();
            // 创建Task的任务对象
            ScheduleTask task(fc, thread);
            if (task.fiber || task.cb) // 存在就加入 
            {
                m_tasks.push_back(task);
            }
        }

        if (need_tickle)  // 如果检查出了队列为空，就唤醒线程
        {
            tickle();
        }
    }

    virtual void start();   // 启动线程池，启动调度器
    virtual void stop();    // 关闭线程池，停止调度器，等所有调度任务都执行完后再返回。

protected:
    virtual void tickle();      //唤醒线程
    virtual void run();         // 线程函数
    virtual void idle();        // 空闲协程函数，无任务调度时执行idle协程。
    virtual bool stopping();    // 是否可以关闭
    bool hasIdleThreads() {return m_idleThreadNum > 0;} //返回是否有空闲线程

private:
    struct ScheduleTask // 任务,可以和协程绑定,也可以和函数绑定
    {
        std::shared_ptr<Fiber> fiber;
        std::function<void()> cb;
        int thread;     // 指定任务需要运行的线程id

        ScheduleTask()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }

        ScheduleTask(std::shared_ptr<Fiber> f, int thr)
        {
            fiber = f;
            thread = thr;
        }

        ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        {//将内容转移也就是指针内部的转移和上面的赋值不同，引用计数不会增加
            fiber.swap(*f);
            thread = thr;
        }

        ScheduleTask(std::function<void()> f, int thr)
        {
            cb = f;
            thread = thr;
        }

        ScheduleTask(std::function<void()>* f, int thr)
        {
            cb.swap(*f);//同理
            thread = thr;
        }

        void reset()//重置
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };

private:
    std::string m_name; // 调度器名称
    std::mutex m_mutex; // 互斥锁 -> 保护任务队列
    std::vector<std::shared_ptr<Thread>> m_threads; // 线程池, 存初始化好的线程
    std::vector<int> m_threadIds;   // 工作线程的Id
    size_t m_threadCount = 0;   // 额外创建的线程数
    std::atomic<size_t> m_activeThreadNum = {0}; // 活跃线程数
    std::atomic<size_t> m_idleThreadNum = {0};  // 空闲线程数
    std::vector<ScheduleTask> m_tasks; // 任务队列
    bool m_useCaller;   // 主线程是否用于工作线程
    std::shared_ptr<Fiber> m_scheduleFiber; // 如果主线程用于工作线程, 需要额外创建调度协程
    int m_rootThread = -1;  // 如果主线程用于工作线程, 记录主线程的线程id
    bool is_stopping = false; // 是否正在关闭
};


}

#endif