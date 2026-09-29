#include "server.h"
#include "http.h"
#include "pool_server.h"
#include "logger.h"

#include <stdio.h>   /* printf / fprintf */
#include <stdlib.h>  /* atoi / exit */

int main(int argc, char* argv[])
{
    // ./s port [网站根目录] [工作线程数]
    if (argc < 2)
    {
        printf("./s port [网站根目录] [工作线程数]\n");
        printf("   例: ./s 8989                    单线程 epoll, 发布当前目录\n");
        printf("   例: ./s 8989 /var/www/html      单线程 epoll, 指定目录\n");
        printf("   例: ./s 8989 /var/www/html 8    线程池模式(2~8 个线程)\n");
        exit(0);
    }
    // 启动服务器
    unsigned short port = atoi(argv[1]);	//字符串转整型

    // 初始化 HTTP 模块: 设定要对外发布的本地目录(缺省为当前工作目录)
    if (http_init(argc >= 3 ? argv[2] : NULL) == -1)
    {
        fprintf(stderr, "http_init 失败: 请检查目录是否存在\n");
        exit(EXIT_FAILURE);
    }

    /* 初始化日志系统: NULL 表示用默认路径(access.log / error.log, 在进程 cwd)。
     * 打开失败也不退出 —— 日志不该成为服务起不来的原因。 */
    if (log_init(NULL, NULL) == -1)
        fprintf(stderr, "警告: 日志文件打开失败, 本次运行不会有日志\n");

    int threads = (argc >= 4) ? atoi(argv[3]) : 0;
    if (threads > 0)
    {
        // 线程池模式: 主线程 accept, 每个连接作为一个任务交给工作线程
        int maxThreads = threads;
        int minThreads = (maxThreads >= 4) ? 2 : 1;
        pool_run(port, minThreads, maxThreads);
    }
    else
    {
        // 缺省: 单线程 epoll 事件驱动
        epollRun(port);
    }

    log_close();   /* 冲刷并关闭日志文件 */
    return 0;
}
