#pragma once

#ifndef TASKQUEUE_H
#define TASKQUEUE_H

// 头文件保护：防止同一个头文件被重复包含。
#include <functional>  /* std::function */
#include <mutex>       /* std::mutex / std::lock_guard */
#include <queue>       /* std::queue */
#include <utility>     /* std::move */

// 任务类型：std::function<void()> 可以保存 lambda、普通函数等。
// 相比旧的“函数指针 + void* 参数”，它更安全，也更容易理解。
/*using：C++11 引入的别名声明语法，功能等同于传统的 typedef，但可读性更好，尤其适用于模板别名。
std::function<void()>：C++11 标准库提供的通用函数包装器。
模板参数 void() 表示“返回值类型为 void，且参数列表为空”。
*/
using TaskFunction = std::function<void()>;

// 新任务结构体，任务本身就是一个可执行对象。
struct Task
{
    TaskFunction function;

    Task() : function(nullptr)
    {
    }

    explicit Task(TaskFunction f) : function(std::move(f))
    {
    }
};



// 任务队列：负责保存任务，并用互斥锁保证多线程访问安全。
class TaskQueue
{
public:
    TaskQueue() = default; // 使用编译器自动生成的默认构造/析构（std::mutex 自己会初始化/释放）
    ~TaskQueue() = default;

    // 添加一个任务（旧接口保留，内部会复制 Task）。
    void addTask(Task& task);
    // 新接口：直接接收 std::function，调用更方便。
    void addTask(TaskFunction func);

    // 取出一个任务。队列为空时返回空的 Task（function == nullptr）。
    Task takeTask();

    // 获取当前队列中任务个数。
    int taskNumber();

private:
    std::mutex m_mutex;         // 保护 m_queue 的互斥锁（C++11 标准库）
    std::queue<Task> m_queue;   // 任务队列
};

#endif // TASKQUEUE_H