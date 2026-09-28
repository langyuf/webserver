#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* C/C++ 混编保护: 本头文件被 C++ 源文件(如 pool_server.cpp)包含时,
 * 必须让这些函数保持 C 链接(不做名字修饰), 否则链接时会找不到
 * 由 gcc 编译出来的 http.o 里的同名符号。 */
#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 *  轻量 HTTP/1.1 静态文件服务器模块 (配合 epoll 边缘触发使用)
 *
 *  最小用法:
 *      http_init("/home/me/www");                     // 设置网站根目录
 *      // epoll_wait 返回事件后, 对每个客户端 fd 调用:
 *      http_handle(epfd, fd, evs[i].events);
 *
 *  http_handle() 内部完成整条生命周期:
 *      收全请求 -> 解析 -> 生成响应 -> 发送 -> (keep-alive 复用 | 关闭)
 *  因此调用方对该 fd 不需要再 recv / send / close / epoll_ctl,
 *  也不需要自己处理 EPOLLOUT。返回值 < 0 表示该连接已被模块关闭。
 *
 *  支持的请求: GET / HEAD
 *  支持的功能: 静态文件、目录列表、index.html 默认页、URL 解码、
 *              MIME 识别、Last-Modified / Content-Length、
 *              单区间 Range(206/416)、keep-alive、301 目录重定向、
 *              40x/41x 错误页、防目录穿越。
 * ===================================================================== */

/* 返回值 */
#define HTTP_OK   0
#define HTTP_ERR (-1)   /* 连接已关闭, 调用方不要再使用该 fd */

/* 设置网站根目录(root 为 NULL 或空串时取当前工作目录)。
 * 返回 0 成功, -1 失败(目录不存在等)。 */
int  http_init(const char *root);

/* 处理一个客户端连接事件。events 直接传 epoll 返回的 evs[i].events。
 * 返回 HTTP_ERR(-1) 表示连接已关闭。 */
int  http_handle(int epfd, int fd, uint32_t events);

/* 是否打印访问日志(默认开) */
void http_set_verbose(int on);

/* 当前生效的网站根目录(绝对路径), 未初始化时返回 NULL */
const char *http_root(void);

/* ---------------------------------------------------------------------
 *  阻塞式接口: 在工作线程里把"整条连接"处理完(含 keep-alive 的多个请求)。
 *  专为"线程池 + 每连接一个任务"的模型设计, 内部不使用 epoll。
 *      fd      accept 得到的阻塞套接字(建议先设 SO_RCVTIMEO 防线程被占死)
 *      返回    0 = 处理结束; 调用方负责 close(fd)
 *  注意: 与 http_init() 共享同一个网站根目录; 连接表内部已加锁,
 *        可被多个工作线程并发调用。
 * ------------------------------------------------------------------- */
int http_serve_connection(int fd);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* HTTP_H */
