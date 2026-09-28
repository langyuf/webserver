#include "threadpool.h"

#include <chrono>   /* std::chrono::seconds / milliseconds */
#include <iostream> /* std::cout / std::endl */
#include <mutex>    /* std::mutex / lock_guard / unique_lock */
#include <new>      /* std::nothrow */
#include <sstream>  /* std::ostringstream */
#include <string>   /* std::string / std::to_string */
#include <thread>   /* std::thread / std::this_thread */
#include <vector>   /* std::vector */
using namespace std;

namespace {

/* 日志互斥锁 --------------------------------------------------------------
 * std::cout 的一串 << 并不是原子操作。多线程同时输出时会交错成:
 *     threadExit(): thread threadExit(): thread 123...123... exiting exiting
 * 所以统一走 logLine() 一次性输出整行: 先拼好字符串, 再在锁内一次写出。
 * (线程池的析构日志与工作线程的退出日志会并发, 必须保护。) */
std::mutex g_logMutex;

void logLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << line << std::endl;
}

}  // namespace


ThreadPool::ThreadPool(int min, int max){
    if(min<0 || min>max || max<0){
        logLine("ThreadPool: 参数不合法，要求 0 < minNum <= maxNum");
        return; // 直接返回；析构函数会安全跳过所有清理（见析构函数第一行）
    }

    m_minNum=min;
    m_maxNum=max;
    m_busyNum=0;
    m_aliveNum=0;
    m_exitNum=0;

    //创建任务队列
    //new (std::nothrow) 表示“内存不够时返回 nullptr 而不是抛异常”。
    m_taskQ = new (std::nothrow) TaskQueue;
    if (m_taskQ == nullptr)
    {
        logLine("ThreadPool: 创建任务队列失败");
        return;
    }

    //创建工作线程
    threads.resize(m_maxNum);
    threadRunning.assign(m_maxNum,false);
    for(int i=0;i<m_minNum;i++){
        if (createWorker(i)==false)
        {
            logLine("ThreadPool: 创建第 " + to_string(i) + " 个工作线程失败");
            break; // 创建失败就停止，能创建几个算几个
        }
    }
    //创建管理者线程，动态管理线程池
    //std::thread 构造失败会抛异常（内存不足），这里用 try/catch 捕获。
    try
    {
        managerThread = std::thread(&ThreadPool::manager, this);
        managerStarted = true;
    }
    catch (...)
    {
        logLine("ThreadPool: 创建管理者线程失败");
    }

}
ThreadPool::~ThreadPool(){
     // 0) 如果构造早期就失败（任务队列都没建成功），无需任何清理。
    if (m_taskQ == nullptr)
    {
        return;
    }

    // 1) 第一段：等待所有已提交任务执行完毕（队列为空且忙线程数为 0）。
    //    - 用条件变量等待，替代旧版“每 1ms 轮询”的忙等，更省 CPU、响应更快；
    //    - 带 5 秒超时兜底：若任务无限地往池里加任务（病态用法），
    //      析构不会被永久卡死，超时后丢弃剩余任务并继续关闭。
    {
        unique_lock<mutex> lock(m_mutex);
        if (!m_allDone.wait_for(lock, chrono::seconds(5), [this]() {
                return m_taskQ->taskNumber() == 0 && m_busyNum == 0;
            }))
        {
            logLine("ThreadPool: 等待任务完成超时（5 秒），剩余任务将被丢弃");
        }
    }

    // 2) 第二段：关闭线程池。
    //    - m_shutdown 让所有线程知道该退出了；
    //    - notify_all 一次性唤醒所有等待中的工作线程（相当于 broadcast）；
    //    - notify_one 专门叫醒管理者线程。
    {
        lock_guard<mutex> lock(m_mutex);
        m_shutdown = true;
        m_notEmpty.notify_all();
        m_managerCond.notify_one();
    }

    // 3) 第三段：等待管理者线程退出。
    if (managerStarted && managerThread.joinable())
    {
        managerThread.join();
    }

    // 4) 第四段：收集仍存活的工作线程并 join。
    //    先在锁内“收集”要 join 的线程对象，再到锁外去 join：
    //    - 在锁内读 m_threadRunning / joinable，避免数据竞争；
    //    - 到锁外 join，避免 join 时还握着锁造成死锁。
    vector<thread*> running;
    {
        lock_guard<mutex> lock(m_mutex);
        for (int i = 0; i < m_maxNum; ++i)
        {
            // 收集条件只看 joinable()：正常退出的工作线程保持 joinable，
            // 由这里统一 join；已 detach（缩容退出的）线程 joinable() 为 false，自动跳过。
            // 若改用 m_threadRunning 判断，shutdown 阶段未 detach 的线程会被漏掉，
            // vector<thread> 析构时会对未 join 的线程调用 std::terminate。
            if (threads[i].joinable())
            {
                running.push_back(&threads[i]);
            }
        }
    }

    for (size_t i = 0; i < running.size(); ++i)
    {
        if (running[i]->joinable())
        {
            running[i]->join(); // 等待线程真正结束，回收线程资源
        }
    }

    // 5) 第五段：释放资源。
    delete m_taskQ;
    m_taskQ = nullptr;
}


bool ThreadPool::createWorker(int index){
    if (index < 0 || index >= m_maxNum || threadRunning[index])return false; // 槽位不合法或已被占用

    try
    {
        // move 赋值：先移动构造临时 std::thread，再移动到槽位里。
        threads[index] = thread(&ThreadPool::worker, this);
    }
    catch (...)
    {
        return false; // 创建失败
    }

    threadRunning[index] = true;
    m_aliveNum++; // 存活线程数 +1
    return true;
}

void ThreadPool::manager(){
    const int NUMBER = 2; // 每次最多增减 2 个线程

    while (true)
    {
        unique_lock<mutex> lock(m_mutex);

        // 要么等满 5 秒（返回 false），要么被唤醒且 m_shutdown 为真（返回 true）。
        bool closed = m_managerCond.wait_for(lock, chrono::seconds(5),
                                             [this]() { return m_shutdown; });

        if (closed) // 线程池已关闭，管理者线程结束
        {
            break;
        }

        // 采样当前状态。
        int queueSize = m_taskQ->taskNumber();
        int liveNum = m_aliveNum;
        int busyNum = m_busyNum;

        // —— 扩容：任务多、线程不够用，就新建线程 ——
        if (queueSize > liveNum && liveNum < m_maxNum)
        {
            int num = 0;
            for (int i = 0; i < m_maxNum && num < NUMBER
                 && m_aliveNum < m_maxNum; ++i)
            {
                if (!threadRunning[i]) // 该槽位空闲，可以放新线程
                {
                    if (createWorker(i))
                    {
                        num++; // 创建成功才计数
                    }
                }
            }
        }

        // —— 缩容：空闲线程太多，就通知一部分线程退出 ——
        if (busyNum * 2 < liveNum && liveNum > m_minNum)
        {
            m_exitNum = NUMBER; // 挂起“让 NUMBER 个线程退出”的请求
            m_notEmpty.notify_all(); // 唤醒工作线程去检查退出请求
        }
    }
}
void ThreadPool::worker(){
    while(true){
        unique_lock<std::mutex> lock(m_mutex);

        // 1) 队列为空且线程池未关闭 → 阻塞等待。如果没有m_shutdown，且没有任务，会一直阻塞
        m_notEmpty.wait(lock, [this]() {
            return m_shutdown|| m_exitNum > 0 || m_taskQ->taskNumber() > 0;
        });

        if(m_shutdown)return;
        
        if (m_exitNum > 0 && m_aliveNum > m_minNum)
        {
            m_exitNum--;
            m_aliveNum--;
            lock.unlock();
            threadExit(); // 清理自己的槽位（函数内部会自己加锁）
            return;       // 正常结束本线程
        }

        //取任务
        Task task = m_taskQ->takeTask();

        m_busyNum++;
        lock.unlock();

        //在锁外执行任务（避免长时间占着锁，别的线程只能干等）。
        if (task.function) // 空任务（function 为空）直接跳过
        {
            task.function();
        }

        //任务执行完，正在执行任务的线程数 -1。
        lock_guard<mutex> busyLock(m_mutex);
        m_busyNum--;
        // 队列已空且没有线程在忙 → 最后一批任务执行完，
        // 唤醒 waitAllDone() / 析构中等待的线程。
        if (m_busyNum == 0 && m_taskQ->taskNumber() == 0)
        {
            m_allDone.notify_all();
        }
    }   
}

void ThreadPool::threadExit(){
    thread::id self = this_thread::get_id();
    bool flag = false; // 是否在槽位中找到了自己

    {
        lock_guard<mutex> lock(m_mutex);
        for (int i = 0; i < m_maxNum; ++i)
        {
            if (threads[i].get_id() == self)
            {
                if (!m_shutdown)
                {
                    // 正常缩容：detach 解除对这个线程对象的所有权，槽位可复用，
                    // 之后 createWorker 才能安全地放入新线程
                    // （否则把一个 joinable 的 std::thread 覆盖赋值会直接 terminate）。
                    // 注意：只有在“线程池尚未关闭”时才 detach；
                    // 若析构已在进行（m_shutdown 为真），保持 joinable，
                    // 由析构函数统一 join，避免“join 与 detach 同抢一个线程”的竞争。
                    threads[i].detach();
                }
                threadRunning[i] = false; // 标记该线程已退出，槽位可复用
                flag = true;
                break;
            }
        }
    }

    // 打印移到锁外：持锁做 I/O 会阻塞其他线程。
    if (flag)
    {
        // 先拼成一条完整的字符串(含自增计数), 再一次性输出, 避免多线程交错
        ostringstream oss;
        oss << "threadExit(): thread " << self << " exiting";
        logLine(oss.str());
    }
}

// 添加任务
void ThreadPool::addTask(TaskFunction func)
{
    // 在同一个锁内完成“关闭检查 + 入队 + 唤醒”，消除竞态窗口：
    // 若析构已置 m_shutdown，这里直接返回；否则任务必然入队成功，
    // 不会再出现“检查通过后任务却被 worker 丢弃”的中间态。
    lock_guard<mutex> lock(m_mutex);
    if (m_shutdown)
    {
        return;
    }

    // 任务入队（任务队列内部有自己的锁，锁顺序固定为 m_mutex → 队列锁）。
    m_taskQ->addTask(std::move(func));

    // 唤醒一个等待中的工作线程去取任务。
    m_notEmpty.notify_one();
}

// 等待所有已提交任务执行完毕：队列为空且忙线程数为 0 时返回。
// 线程池保持打开状态，之后仍可继续 addTask()。
void ThreadPool::waitAllDone()
{
    unique_lock<mutex> lock(m_mutex);
    m_allDone.wait(lock, [this]() {
        return m_shutdown || (m_taskQ->taskNumber() == 0 && m_busyNum == 0);
    });
}

int ThreadPool::getAliveNumber()
{
    lock_guard<mutex> lock(m_mutex);
    return m_aliveNum;
}

int ThreadPool::getBusyNumber()
{
    lock_guard<mutex> lock(m_mutex);
    return m_busyNum;
}