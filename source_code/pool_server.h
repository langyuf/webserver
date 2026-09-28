#ifndef POOL_SERVER_H
#define POOL_SERVER_H

/* =====================================================================
 *  把 ThreadPool 接进 webserver 的桥接接口。
 *
 *  模型: 主线程只做 accept; 每来一个连接就打包成一个任务丢进线程池,
 *        工作线程在连接上做阻塞式处理, 处理完整条连接后回收。
 *
 *  main.c 是 C, pool_server.cpp 是 C++, 所以这里用 extern "C" 消除
 *  C++ 的名字修饰, 让 C 代码能直接调用。
 *
 *  用法:
 *      ./s 8989 4096 /var/www/html 8      // 线程池模式: 2~8 个线程
 *      ./s 8989 4096 /var/www/html        // 缺省仍是单线程 epoll 模式
 * ===================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

/* 以"线程池 + 阻塞式整连接处理"的方式运行服务器。
 *   port  监听端口
 *   min   线程池最小线程数(<=0 时按 1 处理)
 *   max   线程池最大线程数(<=min 时按 min 处理)
 * 返回 0 表示正常退出(收到 SIGINT/SIGTERM), -1 表示启动失败。
 * 本函数会阻塞, 直到收到退出信号。 */
int pool_run(unsigned short port, int min, int max);

#ifdef __cplusplus
}
#endif

#endif /* POOL_SERVER_H */
