/* SO_REUSEPORT 是 Linux 扩展(不属于 POSIX), 需要 _GNU_SOURCE 才会在
 * <sys/socket.h> 中声明。该宏必须在任何 #include 之前定义。 */
#define _GNU_SOURCE

#include "server.h"
#include "http.h"
#include "logger.h"

#include <stdio.h>       /* printf / fprintf / perror */
#include <stdlib.h>      /* exit */
#include <string.h>      /* memset */
#include <unistd.h>      /* close */
#include <signal.h>      /* signal */
#include <errno.h>       /* errno */
#include <fcntl.h>       /* fcntl */
#include <arpa/inet.h>   /* sockaddr_in / htons / INADDR_ANY */
#include <sys/socket.h>  /* socket / bind / listen / accept */
#include <sys/epoll.h>   /* epoll_create / epoll_ctl / epoll_wait */

/* =====================================================================
 *  退出信号处理
 *  信号处理函数里只置一个标志, 不做任何 I/O —— 因为 Logger 内部用了
 *  互斥锁和 ofstream, 都不是"异步信号安全"的, 在信号上下文里调用可能死锁。
 * ===================================================================== */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int set_nonblocking(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int InitListen(unsigned short port){
    //如果客户端已经关闭连接，服务器继续 send() 可能触发 SIGPIPE。忽略它，避免服务器进程直接退出。
    signal(SIGPIPE, SIG_IGN);
    //1、创建监听套接字
    int lfd=socket(AF_INET,SOCK_STREAM,0);
    if(lfd==-1){
        perror("socket");
        exit(0);
    }
    //设置端口复用
	int optval = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    //2、绑定IP和端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;   // ipv4
    addr.sin_port = htons(port);     // 网络字节序
    addr.sin_addr.s_addr = INADDR_ANY; // 0地址不需要转换
    int ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
    if(ret == -1){
        perror("bind");
        exit(0);
    }
    // 3. 设置监听
    ret = listen(lfd, 128);
    if(ret == -1){
        perror("listen");
        exit(0);
    }
    //设置为非阻塞模式
    if(set_nonblocking(lfd)==-1){
        perror("set_nonblocking");
        exit(0);
    }


    return lfd;
}
void new_connection(int lfd,int epfd){
    while(1){
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr*)&peer, &peerlen);
        if (cfd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;/* 没有更多连接，正常退出循环 */
            if (errno == EINTR)continue; /* EINTR：被信号打断，重试 */
            log_error("accept 失败: %s", strerror(errno));
            break;
        }
        if(set_nonblocking(cfd)==-1){
            log_error("set_nonblocking 失败 fd=%d: %s", cfd, strerror(errno));
            close(cfd);
            continue;
        }
                    
        // cfd 添加到检测的原始集合中
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP; //EPOLLRDHUP 远端关闭了 TCP 连接的写端。
        ev.data.fd = cfd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) == -1) {
            perror("epoll_ctl cfd");
            close(cfd);
            continue;
        }
        /* 【日志点 1/4 · accept】记录新接入的连接 */
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        log_info("accept fd=%d 来自 %s:%d", cfd, ip, ntohs(peer.sin_port));
    }
}

int epollRun(unsigned short port){
    int lfd=InitListen(port);

    // 兜底: 若 main() 未指定根目录, 则用当前工作目录
    if(http_root() == NULL && http_init(NULL) == -1){
        log_error("http_init 失败: 根目录未设置");
        fprintf(stderr, "http_init 失败\n");
        exit(0);
    }
    printf("HTTP 服务已就绪, 根目录: %s, 端口: %u\n", http_root(), port);
    log_info("服务器启动: 模式=epoll(单线程) 端口=%u 根目录=%s", port, http_root());

    /* 安装退出信号处理。不设 SA_RESTART: 这样 epoll_wait 会返回 EINTR,
     * 主循环才能看到 g_stop 并优雅收尾。 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    //创建epoll模型
    int epfd = epoll_create(100);
    if(epfd == -1)
    {
        log_error("epoll_create 失败: %s", strerror(errno));
        perror("epoll_create");
        exit(0);
    }
    //将要检测的节点添加到epoll模型中
    struct epoll_event ev;
    ev.events = EPOLLIN;          // 检测lfd的读缓冲区
    ev.data.fd = lfd;             // 要检测的文件描述符
    int re = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if(re == -1)
    {
        log_error("epoll_ctl 失败: %s", strerror(errno));
        perror("epoll_ctl");
        exit(0);
    }

    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(evs[0]);
    //不停的委托内核检测epoll模型中的文件描述符状态
    while (!g_stop)
    {
        int num = epoll_wait(epfd, evs, size, -1);
        if (num < 0) {
            if (errno == EINTR) continue;   /* 被信号打断 -> 回到 while 判断 g_stop */
            log_error("epoll_wait 失败: %s", strerror(errno));
            break;
        }
        printf("num = %d\n", num);
        // 遍历evs数组，个数就返回值

        for(int i=0; i<num; ++i)
        {
            // 取出数组元素中的文件描述符
            int curfd = evs[i].data.fd;
            if(curfd == lfd){
                //建立新连接
                new_connection(curfd,epfd);
            }else{
                // HTTP 请求处理: 收请求 -> 解析 -> 响应 -> 发送
                // (内部负责该 fd 的 epoll 增删改与 close, 返回值 <0 表示连接已关闭)
                http_handle(epfd, curfd, evs[i].events);
            }
        }
    }

    /* 【日志点 4/4 · 退出】 */
    close(lfd);
    close(epfd);
    log_info("收到退出信号, 服务器已退出");
    return 0;
}