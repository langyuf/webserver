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
#include "logger.h"

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
    if (lfd < 0) {
        log_error("socket 创建失败: %s", strerror(errno));
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_error("bind 端口 %u 失败: %s", port, strerror(errno));
        perror("bind");
        close(lfd);
        return -1;
    }
    if (listen(lfd, 128) < 0) {
        log_error("listen 失败: %s", strerror(errno));
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

    printf("线程池模式已启动\n");
    printf("  端口      : %u\n", port);
    printf("  工作线程  : %d ~ %d\n", min, max);
    printf("  网站根目录: %s\n", http_root() ? http_root() : "(未设置)");
    printf("按 Ctrl+C 退出\n");
    log_info("服务器启动: 模式=线程池(%d~%d 线程) 端口=%u 根目录=%s",
             min, max, port, http_root() ? http_root() : "(未设置)");

    {
        /* 线程池放在这个作用域里: 出作用域即析构,
         * 析构会等所有任务跑完 -> 通知线程退出 -> join 回收。 */
        ThreadPool pool(min, max);

        while (!g_stop) {
            struct sockaddr_in peer;
            socklen_t peerlen = sizeof(peer);
            int cfd = accept(lfd, reinterpret_cast<sockaddr*>(&peer), &peerlen);
            if (cfd < 0) {
                if (errno == EINTR) continue;   /* 被信号打断 -> 回到 while 判断 g_stop */
                if (g_stop) break;
                log_error("accept 失败: %s", strerror(errno));
                perror("accept");
                break;
            }
            set_socket_timeout(cfd, 5, 10);

            /* 【日志点 1/4 · accept】记录新接入的连接 */
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            log_info("accept fd=%d 来自 %s:%d", cfd, ip, ntohs(peer.sin_port));

            /* ★ 关键一步: 把"一整条连接"作为任务交给线程池。
             *   按值捕获 cfd, 任务的生存期完全由线程池管理。 */
            pool.addTask([cfd] {
                http_serve_connection(cfd);     /* 阻塞式处理完整条连接(含 keep-alive) */
                close(cfd);
            });
        }
    }   /* ← ThreadPool 在这里析构完毕 */

    /* 【日志点 4/4 · 退出】 */
    close(lfd);
    printf("\n收到退出信号, 线程池已关闭\n");
    log_info("收到退出信号, 线程池已关闭, 服务器退出");
    return 0;
}
