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
// 
// ===================================================================



}