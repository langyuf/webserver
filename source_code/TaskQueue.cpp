#include "TaskQueue.h"

#include <mutex>    /* std::lock_guard */
#include <utility>  /* std::move */

// 说明：构造函数和析构函数没有单独写。
// 因为 m_mutex 是 std::mutex 类型的成员，构造时它自己会初始化，析构时它自己会释放，
// 不需要（也不能）像 pthread 那样手动 init/destroy，这就是 RAII（资源获取即初始化）。

void TaskQueue::addTask(Task& task)
{
    // std::lock_guard：进入作用域时自动加锁，离开作用域时自动解锁。
    // 即使中间发生异常，锁也会被正确释放，比手动 lock/unlock 更安全。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push(task);
}

void TaskQueue::addTask(TaskFunction func)
{
    // 使用 std::function 创建任务，函数对象会被移动到队列中。
    // 这样就不再需要手动 delete void*，也不会出现重复释放的问题。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push(Task(std::move(func)));
}



Task TaskQueue::takeTask()
{
    // 先创建一个空 Task，如果队列为空就返回它。
    Task t;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_queue.empty())
    {
        t = std::move(m_queue.front()); // 直接移动，避免复制 std::function
        m_queue.pop();
    }
    return t;
}

int TaskQueue::taskNumber()
{
    // 读取队列大小也要加锁，否则可能与 addTask/takeTask 同时发生。
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}