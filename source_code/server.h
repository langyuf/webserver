#ifndef SERVER_H
#define SERVER_H

/* =====================================================================
 *  网络层对外接口
 *
 *  本头文件刻意不包含任何系统头: 下面三个原型只用内建类型
 *  (int / unsigned short), 不需要 <sys/socket.h>、<sys/epoll.h> 之类。
 *  真正需要它们的实现文件 server.c 自己包含。
 *
 *  好处: 任何 include 本文件的文件(如 main.c)不会被连带拖进一堆
 *  无关声明, 头文件的自洽性也更容易验证。
 * ===================================================================== */

int  InitListen(unsigned short port);     /* 建监听套接字(非阻塞) */
void new_connection(int lfd, int epfd);   /* 接受新连接并注册进 epoll */
int  epollRun(unsigned short port);       /* 事件循环主入口 */

#endif /* SERVER_H */
