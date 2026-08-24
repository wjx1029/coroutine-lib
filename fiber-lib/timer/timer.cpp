#include "timer.h"

namespace sylar
{
// ================================================
// --------------------- Timer --------------------
// ================================================

// =============================================
// Timer::cancel() 取消一个定时器
// =============================================

bool Timer::cancel()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);//写锁互斥锁

    if (m_cb == nullptr)//这里就是将回调函数如果存在设置为nullptr
    {
        return false;
    }
    else
    {
        m_cb = nullptr;
    }

    auto it = m_manager->m_timers.find(shared_from_this());//从定时管理器中找到需要删除的定时器
    if (it != m_manager->m_timers.end())
    {
        m_manager->m_timers.erase(it);//删除定时器
    }
    return true;
}

// =============================================
// Timer::refresh() 刷新定时器超时时间
// =============================================

bool Timer::refresh()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if (m_cb == nullptr)//这里就是将回调函数如果存在设置为nullptr
    {
        return false;
    }

    auto it = m_manager->m_timers.find(shared_from_this());//在定时器集合中查找当前定时器
    if (it == m_manager->m_timers.end())//检查定时器是否存在
    {
        return false;
    }

    //删除当前定时器并更新超时时间
    m_manager->m_timers.erase(it);
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
    m_manager->m_timers.insert(shared_from_this());//并且将新的定时器加入到定时器管理类中
    return true;
}

// =============================================
// Timer::reset 重置定时器的超时时间，可以选择从当前时间或上次起点开始计算超时时间
// =============================================

bool Timer::reset(uint64_t ms, bool from_now)
{
    if(ms == m_ms && !from_now)//检查是否要重置
    {
        return true;//代表不需要重置
    }

    //如果不满足上面的条件需要重置，删除当前的定时器然后重新计算超时时间并重新插入定时器
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

        if(!m_cb)//如果为空，说明该定时器已经被取消或未初始化，因此无法重置
        {
            return false;
        }
		  //否则就是定时器已经初始化了
        auto it = m_manager->m_timers.find(shared_from_this());//寻找定时器
        if(it==m_manager->m_timers.end())//没找到定时器
        {
            return false;
        }
        m_manager->m_timers.erase(it);//找到删除
    }

    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);//如果为true则重新计算超时时间，为false就需要上一次的起点开始
    m_ms = ms;
    m_next = start + std::chrono::milliseconds(m_ms);
    m_manager->addTimer(shared_from_this());
    return true;
}

// =============================================
// Timer::Timer() 构造函数
// =============================================

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager)
: m_recurring(recurring)
, m_ms(ms)
, m_cb(cb)
, m_manager(manager)
{
    auto now = std::chrono::system_clock::now();//记录当前时间
    m_next = now + std::chrono::milliseconds(m_ms);//下一次超时时间
}

// =============================================
// Timer::Comparator Timer对象的比较函数
// =============================================

bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const
{
    assert(lhs != nullptr && rhs != nullptr);
    return lhs->m_next < rhs->m_next;
}


// =======================================================
// --------------------- TimerManager --------------------
// =======================================================


// =============================================
// 构造函数,析构函数
// =============================================

TimerManager::TimerManager()
{
    m_previouseTime = std::chrono::system_clock::now();//初始化当前系统事件，为后续检查系统时间错误时进行校对。
}

TimerManager::~TimerManager()
{
    
}

// =============================================
// TimerManager::addTimer 将一个新定时器，添加到定时器管理器中
// =============================================

std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
{
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    addTimer(timer);
    return timer;
}

void TimerManager::addTimer(std::shared_ptr<Timer> timer)
{
    bool at_front = false;//标识插入的是最早超时的定时器

    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);

        auto it = m_timers.insert(timer).first;//将定时器插入到 m_timers 集合中。由于 m_timers 是一个 std::set，插入时会自动按定时器的超时时间排序。
        at_front = (it == m_timers.begin()) && !m_tickled;//判断插入的定时器是否是集合超时时间中最早的定时器

        // only tickle once till one thread wakes up and runs getNextTime()
        if(at_front)//标识有一个新的最早定时器被插入了，防止重复唤醒。
        {
            m_tickled = true;
        }
    }

    if (at_front)
    {
        // wake up
        onTimerInsertedAtFront();//虚函数具体执行在ioscheduler
    }
}

// =============================================
// TimerManager::addConditionTimer
// 添加一个条件定时器，并在定时器触发的时候执行的cb的会触发Ontimer，在Ontimer中会真正触发任务。
// =============================================

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
{
    std::shared_ptr<void> tmp = weak_cond.lock();//确保当前条件的的对象任然存在
    if (tmp)
    {
        cb();
    }
}

std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring)
{
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

// =============================================
// TimerManager::getNextTimer 获取定时器管理器中下一个定时器的超时时间。
// =============================================

uint64_t TimerManager::getNextTimer()
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);//读锁

    // reset m_tickled
    m_tickled = false;

    if (m_timers.empty())
    {   
        return ~0ull;   // 返回最大值
    }

    auto now = std::chrono::system_clock::now();//获取当前系统时间
    auto first_over_time = (*m_timers.begin())->m_next;//获取最小时间堆中的第一个超时定时器判断超时

    if (now >= first_over_time)//判断当前时间是否已经超过了下一个定时器的超时时间
    {
        // 已经有timer超时
        return 0;
    }
    else
    {
        //计算从当前时间到下一个定时器超时时间的时间差，结果是一个 std::chrono::milliseconds 对象。
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(first_over_time - now);
        return static_cast<uint64_t>(duration.count()); //将时间差转换为毫秒，并返回这个值。
    }
}

// =============================================
// TimerManager::listExpiredCb 提取所有已超时的定时器
/*
执行流程（逻辑润色）：
获取当前时间：首先获取当前的系统绝对时间。
加锁保护：由于涉及对定时器集合（最小堆）的修改，使用写锁（std::unique_lock）来确保线程安全。
时钟回滚检测：调用 detectClockRollover() 检查是否发生了系统时钟回退。

提取超时任务：
在满足以下条件之一时，持续从时间堆中取出堆顶元素：
发生回滚：如果系统时间发生了回退，为了安全起见，通常认为堆中所有定时器都已失效并立即触发（或根据策略清理）。
任务超时：堆不为空，且堆顶定时器的绝对超时时间点小于或等于当前时间。

处理任务与循环逻辑：
将到期定时器从时间堆（m_timers）中移除。
将该定时器的回调函数（m_cb）推入结果向量 cbs 中。
循环定时器处理：如果该定时器被标记为循环触发（m_recurring == true），则根据其设定的时间间隔重新计算下一次超时时间，并重新插入时间堆中。
单次定时器处理：如果是单次任务，则在提取回调后，将定时器内部的 m_cb 置空以释放资源。
*/
// =============================================

void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs)
{
    auto now = std::chrono::system_clock::now();

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    bool rollover = detectClockRollover();//判断是否出现系统时间错误

    while(!m_timers.empty() && (rollover || (*m_timers.begin())->m_next <= now))
    {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());

        cbs.push_back(temp->m_cb);

        //如果定时器是循环的,m_next 属性设置为当前时间加上定时器的间隔（m_ms），然后重新插入到定时器集合中。
        if (temp->m_recurring)
        {   // 重新加入时间堆
            temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        }
        else
        {    // 清理cb
            temp->m_cb = nullptr;
        }
    }
}

// =============================================
// TimerManager::detectClockRollover 检测系统时间是否发生了回滚(即时间是否倒退)
// =============================================

bool TimerManager::detectClockRollover()
{
    bool rollover = false;

    // 当前时间 now 与上次记录的时间 m_previouseTime 减去一个小时的时间量 (60 * 60 * 1000 毫秒)。
    // 当前时间 now 小于这个时间值，说明系统时间回滚了，因此将 rollover 设置为 true。
    auto now = std::chrono::system_clock::now();
    if (now < m_previouseTime - std::chrono::milliseconds(60*60*1000))
    {
        rollover = true;
    }
    m_previouseTime = now;
    return rollover;
}

// =============================================
// TimerManager::hasTimer 这里是为了查看超时时间堆是否为空。
// =============================================

bool TimerManager::hasTimer()
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return !m_timers.empty();
}

}