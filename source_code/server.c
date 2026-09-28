/* SO_REUSEPORT 是 Linux 扩展(不属于 POSIX), 需要 _GNU_SOURCE 才会在
 * <sys/socket.h> 中声明。该宏必须在任何 #include 之前定义。 */
#define _GNU_SOURCE

#include "server.h"
#include "http.h"

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
        int cfd = accept(lfd, NULL, NULL);
        if (cfd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;/* 没有更多连接，正常退出循环 */
            if (errno == EINTR)continue; /* EINTR：被信号打断，重试 */
            perror("accept");
            break;
        }
        if(set_nonblocking(cfd)==-1){
            perror("set_nonblocking cfd");
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
        printf("新客户端接入: fd=%d\n", cfd);
    }
}

int epollRun(unsigned short port){
    int lfd=InitListen(port);

    // 兜底: 若 main() 未指定根目录, 则用当前工作目录
    if(http_root() == NULL && http_init(NULL) == -1){
        fprintf(stderr, "http_init 失败\n");
        exit(0);
    }
    printf("HTTP 服务已就绪, 根目录: %s, 端口: %u\n", http_root(), port);
    //创建epoll模型
    int epfd = epoll_create(100);
    if(epfd == -1)
    {
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
        perror("epoll_ctl");
        exit(0);
    }

    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(evs[0]);
    //不停的委托内核检测epoll模型中的文件描述符状态
    while (1) 
    {
        int num = epoll_wait(epfd, evs, size, -1);
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

}