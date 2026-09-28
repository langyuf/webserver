#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "TaskQueue.h"

#include <condition_variable>  /* std::condition_variable */
#include <mutex>                /* std::mutex / std::unique_lock */
#include <thread>               /* std::thread */
#include <vector>               /* std::vector */

class ThreadPool
{
public:
    ThreadPool(int min, int max);
    ~ThreadPool();

    // 禁止复制：线程池里有线程和锁，复制会导致严重的资源问题。
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 添加任务。使用 std::function 后，lambda 可以直接传入。
    void addTask(TaskFunction task);
    // 阻塞等待所有已提交任务执行完毕（队列为空且忙线程数为 0 时返回）。
    // 与析构不同：等待完成后线程池仍保持打开，之后可以继续 addTask()。
    void waitAllDone();
    // 获取忙线程的个数。
    int getBusyNumber();
    // 获取活着的线程个数。
    int getAliveNumber();

private:
    bool createWorker(int index);
    void worker();
    void manager();
    void threadExit();
private:
    int m_minNum = 0;   // 最小线程数
    int m_maxNum = 0;   // 最大线程数
    int m_busyNum = 0;  // 正在执行任务的线程数
    int m_aliveNum = 0; // 存活的线程数
    int m_exitNum = 0;  // 等待退出的线程数
    bool m_shutdown = false; // 线程池是否已关闭

    std::vector<std::thread> threads; //工作线程-n
    std::vector<bool> threadRunning;  //运行中的线程
    std::thread managerThread;      //管理者线程-1
    bool managerStarted=false;      //管理者线程状态

    std::mutex m_mutex; 
    std::condition_variable m_notEmpty;    // 队列非空或线程池关闭时唤醒工作线程
    std::condition_variable m_managerCond; // 唤醒管理者线程（销毁时及时停止它）
    std::condition_variable m_allDone;     // 队列为空且忙线程数为 0 时唤醒等待者

    TaskQueue* m_taskQ = nullptr;// 任务队列（初值为空，便于析构时安全判断）
};

#endif