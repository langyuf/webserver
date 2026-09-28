/* =====================================================================
 *  pool_server.cpp —— ThreadPool 接入 webserver 的桥接实现
 *
 *  ── 为什么用这个模型 ────────────────────────────────────────────────
 *  http.c 里的 http_handle() 是"一个 epoll 事件驱动一次"的状态机,
 *  它的驱动源是内核事件, 而线程池的调度单位是"一次性任务", 两者语义
 *  并不匹配: 把 epoll_wait 放进任务里会永久占住工作线程, 线程池的
 *  排队/扩容/缩容也就全部失去意义。
 *
 *  真正能发挥线程池价值的用法是: 把"处理一整条连接"作为任务。
 *    主线程 accept ──► addTask(处理该连接) ──► 工作线程阻塞式处理完 ──► 回收
 *
 *  这样线程池的调度逻辑才真正生效:
 *    连接积压  → 队列变长      → 管理线程扩容到 max
 *    流量低谷  → 忙线程数下降  → 管理线程缩容回 min
 *
 *  ── 代价与防护 ──────────────────────────────────────────────────────
 *  并发连接数受线程数限制, 且慢连接会占住工作线程, 所以必须给套接字
 *  设置收发超时(set_socket_timeout), 否则空闲的 keep-alive 连接会把
 *  线程池的线程全部占死。
 *
 *  ── 线程安全 ────────────────────────────────────────────────────────
 *  http.c 的连接表已加锁(g_conn_lock), 且每个连接只交给一个线程处理,
 *  因此 http_serve_connection() 可以被多个工作线程并发调用。
 * ===================================================================== */
#include "pool_server.h"
#include "http.h"
#include "threadpool.h"

/* C++ 标准库 */
#include <atomic>   /* std::atomic */
#include <cerrno>   /* errno */
#include <csignal>  /* signal / sigaction */
#include <cstdio>   /* printf / perror */
#include <cstring>  /* memset */

/* POSIX / Linux */
#include <arpa/inet.h>   /* htons / INADDR_ANY */
#include <netinet/in.h>  /* sockaddr_in */
#include <sys/socket.h>  /* socket / bind / listen / accept */
#include <sys/time.h>    /* timeval / SO_RCVTIMEO */
#include <unistd.h>      /* close */

namespace {

std::atomic<bool> g_stop{false};

/* 信号处理函数: 只置标志。真正的清理放在主循环里做(异步信号安全)。 */
void on_signal(int) { g_stop = true; }

/* 创建阻塞式监听套接字(accept 会挂起等待, 不忙等) */
int listen_blocking(unsigned short port)
{
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(lfd);
        return -1;
    }
    if (listen(lfd, 128) < 0) {
        perror("listen");
        close(lfd);
        return -1;
    }
    return lfd;   /* 保持阻塞模式 */
}

/* 收发超时: 防止慢客户端/空闲 keep-alive 长期占住工作线程 */
void set_socket_timeout(int fd, int recv_sec, int send_sec)
{
    timeval tv;
    tv.tv_sec = recv_sec;  tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = send_sec;  tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

}  // namespace

int pool_run(unsigned short port, int min, int max)
{
    if (min < 1) min = 1;
    if (max < min) max = min;

    int lfd = listen_blocking(port);
    if (lfd < 0) return -1;

    /* 用 sigaction 且不设 SA_RESTART: 否则 accept() 被信号打断后会被
     * 自动重启, 主循环永远看不到 g_stop, Ctrl+C 也就退不出来。 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    ThreadPool pool(min, max);

    printf("线程池模式已启动\n");
    printf("  端口      : %u\n", port);
    printf("  工作线程  : %d ~ %d\n", min, max);
    printf("  网站根目录: %s\n", http_root() ? http_root() : "(未设置)");
    printf("按 Ctrl+C 退出\n");

    while (!g_stop) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;   /* 被信号打断 -> 回到 while 判断 g_stop */
            if (g_stop) break;
            perror("accept");
            break;
        }
        set_socket_timeout(cfd, 5, 10);

        /* ★ 关键一步: 把"一整条连接"作为任务交给线程池。
         *   按值捕获 cfd, 任务的生存期完全由线程池管理。 */
        pool.addTask([cfd] {
            http_serve_connection(cfd);     /* 阻塞式处理完整条连接(含 keep-alive) */
            close(cfd);
        });
    }

    close(lfd);
    printf("\n收到退出信号, 等待剩余任务完成后关闭线程池...\n");
    return 0;   /* pool 析构: 等任务做完 -> 通知所有线程退出 -> join 回收 */
}
