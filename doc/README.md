# mini-http —— 基于 epoll 的 Linux HTTP 静态文件服务器

一个用 C 从零实现的轻量 HTTP/1.1 静态文件服务器，对外发布一个本地目录，
浏览器直接访问。HTTP 协议层封装在独立的 `http.c` / `http.h` 模块中，
**一函数接入**，并支持两种运行模型：

| 模型 | 入口 | 说明 |
| --- | --- | --- |
| 单线程 epoll（缺省） | `http_handle()` | 边缘触发 + 非阻塞 + `sendfile`，一个线程扛大量并发连接 |
| 线程池 | `http_serve_connection()` | 主线程 accept，每个连接作为一个任务丢进 `ThreadPool`，工作线程阻塞式处理完整条连接 |

```bash
cd source_code
./s 8989 /var/www/html             # 单线程 epoll
./s 8989 /var/www/html 8           # 线程池模式: 2~8 个工作线程
```

```
GET /            -> 200  默认首页 index.html，没有则生成目录列表
GET /style.css   -> 200  text/css; charset=utf-8
GET /sub         -> 301  Location: /sub/          (目录补斜杠)
GET /big.bin     -> 206  Range: bytes=0-99        (断点续传)
GET /nope        -> 404
POST /           -> 405  Allow: GET, HEAD
```

---

## 1. 功能特性

**网络层**

- `epoll` + 边缘触发（`EPOLLET`），`accept` / `recv` 均循环到 `EAGAIN`；
- 监听与连接套接字全部非阻塞；
- 大响应体通过 `sendfile` 零拷贝发送，遇 `EAGAIN` 自动转 `EPOLLOUT` 续传，
  慢速客户端不会阻塞其它连接；
- 连接状态（收发缓冲、文件偏移）按 `fd` 索引动态增长，无需手工管理；
- 忽略 `SIGPIPE`，单个客户端异常不会拖垮整个服务。

**并发模型（两种可选）**

- 单线程 `epoll`（缺省）：一个线程扛大量并发连接，靠非阻塞 + `sendfile`；
- 线程池（第 5 节）：主线程 `accept`，每个连接作为一个任务交给 `ThreadPool`，
  工作线程阻塞式处理整条连接，线程数按负载自动扩缩容；
- 连接表已加锁，`http_serve_connection()` 可被多个工作线程并发调用
  （每个连接只由一个线程处理，因此不需要 per-connection 锁）。

**HTTP 层**

- 请求方法：`GET` / `HEAD`，其余方法返回 `405`；
- HTTP/1.1 默认 keep-alive，HTTP/1.0 默认关闭，`Connection` 头可覆盖；
- 响应头：`Date`、`Server`、`Content-Type`、`Content-Length`、`Connection`、
  `Last-Modified`、`Accept-Ranges`；
- 单区间 `Range` 请求：`bytes=0-99` / `bytes=100-` / `bytes=-100`
  → `206 Partial Content`，非法区间 → `416`；
- 自动 MIME 识别（html/css/js/json/png/jpg/svg/woff2/mp4/pdf… 共 30 种常见类型）；
- URL 百分号解码，中文 / 空格文件名可正常访问；
- 目录请求：`/dir` → `301`；有 `index.html` / `index.htm` 则返回默认页，
  否则生成带文件大小、可点击的目录列表页（名称做了 URL 编码与 HTML 转义）；
- 安全：拒绝含 `..` 的路径段，并用 `realpath` 校验结果前缀，
  防止目录穿越与符号链接逃逸；拒绝 `%00`；
- 错误页：`400 / 403 / 404 / 405 / 413 / 414 / 416`。

> 请求体（POST）、chunked 编码、HTTPS 未实现。若请求携带请求体，
> 响应后会主动关闭连接，避免 keep-alive 解析错位。

---

## 2. 目录结构

```
Linux/
├── doc/                文档
│   ├── README.md           本文件（项目说明、编译运行、两种并发模型）
│   └── HTTP.md             http.c / http.h 的逐层代码详解
├── source_code/        源码 + 构建脚本（自包含，可独立编译）
│   ├── Makefile            混合构建：C 用 gcc，C++ 用 g++，统一 g++ 链接
│   ├── main.c              程序入口：解析参数、选择运行模型
│   ├── server.h/.c         网络层：监听、epoll 事件循环、连接管理
│   ├── http.h/.c           HTTP 协议层（epoll 与阻塞式两个入口）
│   ├── pool_server.h/.cpp  线程池接入层（main.c 与 ThreadPool 之间的桥）
│   ├── TaskQueue.h/.cpp    任务队列（std::queue + std::mutex，C++11）
│   ├── threadpool.h/.cpp   线程池（工作线程 + 管理线程，动态扩缩容）
│   ├── client.c            配套 TCP 测试客户端（与 HTTP 模块无关）
│   ├── s                   服务器可执行文件（编译产物）
│   └── c                   客户端可执行文件（编译产物）
├── source/             与本项目无关的 SVG 动画网页（历史文件）
└── package/            未使用的 libevent 源码树（历史文件）
```

> `source_code/` 是自包含的：源码、`Makefile`、编译产物都在里面，
> 可以整个目录拷走单独编译。文档在 `doc/`。
>
> 因此下面**编译相关命令要先 `cd source_code`**；运行服务器可以
> `cd source_code && ./s ...`，也可以直接在仓库根目录写
> `./source_code/s ...`。

---

### 头文件包含约定

每个源文件按 **自身头文件 → 项目内其他头文件 → C++ 标准库 → C/POSIX 系统头**
分组，组间空一行，组内按字母序：

```c
#include "自身头文件.h"        /* 若有: 放最前, 用于验证头文件自洽 */

#include "项目内其他头.h"      /* 无则整组省略 */

#include <C++ 标准库>          /* 如 <vector> <mutex> */

#include <C 标准库 / POSIX>    /* 如 <stdio.h> <sys/socket.h> */
```

两条硬性规则：

- **特性宏必须写在所有 `#include` 之前**。`http.c` 与 `server.c` 顶部的
  `#define _GNU_SOURCE` 就是这个位置——`sendfile`、`MSG_NOSIGNAL`、
  `SO_REUSEPORT` 这些 Linux 扩展只有定义了它才会在系统头里声明；
  在严格标准模式下（如 `-std=c11`）漏掉它就会直接报 `undeclared`。
- **头文件只包含自己需要的头**。`server.h` / `http.h` / `pool_server.h`
  刻意不包含任何系统头（它们的原型只用内建类型），需要系统头的实现文件
  自己包含。这样 `#include "server.h"` 不会连带拖进一堆无关声明。
  验证方法：把每个头单独 include 进一个空文件编译，应当能通过。

```
头文件自洽性测试结果:
  [ok] server.h  (C / C++)     [ok] http.h  (C / C++)
  [ok] pool_server.h (C / C++) [ok] TaskQueue.h (C++)
  [ok] threadpool.h (C++)
```

---

## 3. 快速开始

### 编译

```bash
cd source_code  # 构建脚本与源码在同一目录
make            # 推荐: 产出 source_code/s
make client     # 单独编译 TCP 测试客户端, 产出 source_code/c
make clean      # 清理 .o 与可执行文件
```

等价的**分编译器手工构建**——按扩展名分工即可，不需要任何特殊开关：

| 编译器 | 文件 |
| --- | --- |
| `gcc`（C） | `main.c`、`server.c`、`http.c`、`client.c` |
| `g++`（C++） | `TaskQueue.cpp`、`threadpool.cpp`、`pool_server.cpp` |

```bash
cd source_code
gcc -c -o main.o      main.c
gcc -c -o server.o    server.c
gcc -c -o http.o      http.c
g++ -std=c++11 -c -o TaskQueue.o  TaskQueue.cpp
g++ -std=c++11 -c -o threadpool.o threadpool.cpp
g++ -std=c++11 -c -o pool_server.o pool_server.cpp
g++ -pthread -o s main.o server.o http.o TaskQueue.o threadpool.o pool_server.o
```

> 不要把全部源码丢给一条 `g++` 命令：`g++` 会把 `.c` 按 C++ 编译，
> C 代码里 `realloc`/`calloc` 的隐式 `void*` 转换在 C++ 中是非法的，
> 会报一串 `invalid conversion from 'void*'`。

### 运行

```bash
cd source_code
./s <端口> [网站根目录] [工作线程数]

./s 8989                       # 单线程 epoll, 发布当前工作目录
./s 8989 /var/www/html         # 单线程 epoll
./s 8989 /var/www/html 8       # 线程池模式(min=2, max=8)
```

> 想在仓库根目录直接跑，写 `./source_code/s 8989 /var/www/html` 也可以。
> 注意"发布当前工作目录"取的是**进程的 cwd**，所以 `cd /var/www/html && /path/to/s 8989`
> 和 `./s 8989 /var/www/html` 效果相同。

启动后会打印实际生效的根目录：

```
HTTP 服务已就绪, 根目录: /var/www/html, 端口: 8989
```

浏览器打开 `http://<服务器IP>:8989/` 即可。

### 参数说明

| 参数 | 说明 |
| --- | --- |
| `端口` | 必填，监听端口 |
| `网站根目录` | 可选；省略则发布进程当前工作目录 |
| `工作线程数` | 可选；**填了就走线程池模式**（该值即 `max`，`max>=4` 时 `min=2`，否则 `min=1`）。不填则走单线程 epoll |

### 验证

```bash
curl -i  http://127.0.0.1:8989/               # 首页
curl -I  http://127.0.0.1:8989/big.bin        # HEAD，只有响应头
curl -i  -r 0-99 http://127.0.0.1:8989/big.bin # Range，期望 206
curl -i  http://127.0.0.1:8989/sub            # 目录，期望 301
curl -o /dev/null -w '%{http_code}\n' \
        --path-as-is http://127.0.0.1:8989/../../etc/passwd   # 期望 403
```

---

## 4. 在 epoll 事件循环中调用

`http.h` 只暴露一个处理函数，把 epoll 返回的事件原样传进去即可：

```c
#include "http.h"

int main(int argc, char *argv[])
{
    if (http_init("/var/www/html") == -1)   /* 设定发布的本地目录 */
        return 1;

    int lfd  = InitListen(8989);
    int epfd = epoll_create(100);
    /* ... 把 lfd 加入 epfd ... */

    struct epoll_event evs[1024];
    while (1) {
        int num = epoll_wait(epfd, evs, 1024, -1);
        for (int i = 0; i < num; ++i) {
            int curfd = evs[i].data.fd;
            if (curfd == lfd)
                new_connection(curfd, epfd);                 /* 接受新连接 */
            else
                http_handle(epfd, curfd, evs[i].events);     /* 处理 HTTP 请求 */
        }
    }
}
```

`http_handle()` 内部完成整条生命周期：

```
recv 收请求(循环到 EAGAIN) ──> 解析请求行/头 ──> realpath 校验路径
        │                                            │
        │ 未收全，等下次 EPOLLIN                      ▼
        │                                  生成响应头 + 打开文件
        ▼                                            │
   (下次事件再进来)                                  ▼
                                        send/sendfile 发送
                                        ├─ 发完 + keep-alive -> 复位，等下一个请求
                                        └─ 发完 + close      -> epoll_ctl DEL + close
```

因此调用方**不需要**再对该 fd 做 `recv` / `send` / `close` / `epoll_ctl`，
也**不需要**自己注册 `EPOLLOUT`；返回值 `< 0` 表示连接已被模块关闭，
直接丢弃该 fd 即可。

### 接口一览（http.h）

| 函数 | 说明 |
| --- | --- |
| `int http_init(const char *root)` | 设定网站根目录（`NULL` = 当前工作目录），成功返回 `0` |
| `int http_handle(int epfd, int fd, uint32_t events)` | 处理一次连接事件；`HTTP_ERR` 表示连接已关闭 |
| `void http_set_verbose(int on)` | 访问日志开关，默认开 |
| `const char *http_root(void)` | 返回当前生效的根目录绝对路径 |
| `int http_serve_connection(int fd)` | **阻塞式**处理整条连接（线程池模式入口，见第 5 节）；返回 `0` 后由调用方 `close(fd)` |

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `HTTP_OK` | `0` | 连接仍在使用 |
| `HTTP_ERR` | `-1` | 连接已关闭 |

---

## 5. 线程池模式

### 5.1 为什么不把 epoll 搬进线程池

`http_handle()` 是"一个 epoll 事件驱动一次"的状态机，驱动源是**内核事件**；
而线程池的调度单位是**一次性任务**。把 `epoll_wait` 放进任务里，任务永不
返回 → 工作线程被永久占住 → 排队/扩容/缩容全部失效，`waitAllDone()` 和
析构也会永远卡住。**两者语义不匹配。**

### 5.2 以"一整条连接"为任务

```
主线程                     线程池                              工作线程
  │                         │                                   │
  ├─ accept() ─────────────►│                                   │
  │   cfd                   ├─ addTask([cfd]{ ... }) ──────────►│
  │                         │                     http_serve_connection(cfd)
  │                         │                     阻塞式处理完整条连接
  │                         │                     (含 keep-alive 的多个请求)
  │                         │                     处理完 close(cfd) 回到池中
  │  连接积压 → 队列变长 → 扩容到 max；流量低谷 → 缩容回 min
```

### 5.3 代码（`pool_server.cpp`）

整个接入只有三行：

```cpp
int cfd = accept(lfd, nullptr, nullptr);
set_socket_timeout(cfd, 5, 10);        /* ★ 必须，见 5.4 */
pool.addTask([cfd] {                   /* ★ 一整条连接 = 一个任务 */
    http_serve_connection(cfd);        /* 阻塞式处理完整条连接 */
    close(cfd);
});
```

对外只暴露一个 C 接口（`pool_server.h`，用 `extern "C"` 消除 C++ 名字修饰）：

```c
int pool_run(unsigned short port, int min, int max);
```

### 5.4 必须设置超时（不是可选项）

这个模型下一个连接**独占一个线程**。若不设超时，一个只建立连接、不发数据
的客户端（Slowloris）就能永久占死一个工作线程，几个就足以让线程池瘫痪。
所以每个连接都设了 `SO_RCVTIMEO=5s` / `SO_SNDTIMEO=10s`；空闲的 keep-alive
连接最多占住线程 5 秒。

### 5.5 两个入口怎么选

| | `http_handle()`（epoll 模式） | `http_serve_connection()`（线程池模式） |
| --- | --- | --- |
| 套接字要求 | 非阻塞 + `EPOLLET` | **阻塞** |
| 是否碰 epoll | 会 `MOD` / `DEL` | 不碰 |
| 是否关 fd | 关 | 不关，由调用方 `close()` |
| keep-alive 靠什么 | `EPOLL_CTL_MOD` 重新武装 ET | 循环 + `SO_RCVTIMEO` 超时 |
| 并发上限 | 受 fd 上限，与线程数无关 | 受线程数限制 |
| 慢连接影响 | 不影响其它连接 | 占住一个工作线程 |

静态文件服务器的瓶颈一般不在这里——`epoll` + 非阻塞 + `sendfile` 已经让
单线程能扛住大量并发连接。线程池适合"任务里有阻塞操作"的场景；若目标是
提高并发连接数，应走 **one-loop-per-thread**（每线程一个 epoll +
`SO_REUSEPORT`），而不是加大线程池。**两条路线不要混用。**

---

## 6. 访问日志

```
[http] GET /                     -> 200 (keep-alive)
[http] GET /style.css            -> 200 (keep-alive)
[http] GET /big.bin              -> 206 (keep-alive)
[http] GET /../../secret.txt     -> 403
```

日志做了行缓冲，即使 `./s ... > server.log` 重定向到文件也能实时看到。

---

## 7. 可调参数

在 `http.c` 顶部：

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `HTTP_REQ_MAX` | `8192` | 单个请求头的最大长度，超出返回 `413` |
| `HTTP_HDR_MAX` | `8192` | 响应头 + 内存型响应体的上限（目录列表也在内） |

其它固定值：`listen` 队列 128、`epoll_wait` 事件数组 1024、
单次 `sendfile` 以剩余字节数为准（不限制分片大小）。

---

## 8. 实测结论

以下为本机 `curl` 实跑结果（Linux x86-64, gcc 13）：

- 默认页 `200`、`HEAD` 无 body 且 `Content-Length` 正确；
- `Range: bytes=0-99` → `206` + `Content-Range: bytes 0-99/1048576`；
  非法区间 → `416` + `Content-Range: bytes */1048576`；
- `/sub` → `301 Location: /sub/`，`/sub/` 返回目录列表；
- CSS 返回 `text/css; charset=utf-8`；中文 / 空格文件名 `200`；
- 目录穿越（`/../../secret.txt` 与 `%2e%2e` 双重编码）均返回 `403`；
- `POST` 返回 `405`；
- **大文件完整性**：1MB 文件整取与 `--limit-rate 200k` 慢速读取，
  md5 均与源文件一致（即 `EPOLLOUT` 分片续传路径正确）；
- **keep-alive**：同一条 TCP 连接内连续请求 `/` 与 `/style.css`，
  客户端报告连接被复用（`left intact`）。- **线程池模式**：20 个并发慢速下载（各 1MB、限速 100KB/s，`min=2 max=8`），
  线程数自动 `4 → 6 → 8` 扩容，20 份下载 md5 全部与源文件一致；
  空闲后自动 `8 → 6 → 4` 缩容，最终回落到 `min=2`。
- **日志并发安全**：`threadExit()` 等日志已改为"整行拼好 + 加锁一次输出"，
  多线程缩容时不再出现 `threadExit(): thread threadExit(): thread 123...123... exiting exiting` 这种交错。


---

## 9. 已知限制与后续可做

- 缺省的单线程 epoll 模型未使用多线程；线程池模式（第 5 节）是另一种可选模型；
- **线程池模式下并发连接数受线程数限制**，且慢连接会占住工作线程，
  必须保留 `SO_RCVTIMEO` / `SO_SNDTIMEO`（`pool_server.cpp` 的 `set_socket_timeout`）；
- 项目是 C / C++ 混编：`.c` 用 `gcc`、`.cpp` 用 `g++`，最后统一用 `g++` 链接
  （`make` 已处理）；若把 `.c` 交给 `g++`，会在 `realloc`/`calloc` 处报
  `invalid conversion from 'void*'`；
- **内存型响应体（含目录列表）超过 `HTTP_HDR_MAX`(8 KB) 会被截断**，
  目录内文件较多时请调大该宏（请求头与响应体是两个独立的宏）；
- 无请求体解析（POST/表单/上传）、无 chunked、无 gzip 压缩、无 HTTPS；
- 目录列表会列出目录下全部文件，对外发布时请确认目录内容安全；
- 未实现 `If-Modified-Since` / `ETag` 协商缓存（已有 `Last-Modified`）；
- 未实现多区间 `Range`（`bytes=0-99,200-299`）；当前会忽略该头正常返回 200。

代码清理建议：

- ~~`server.c` 中的 `Communication()` / `is_http_request()` 死代码~~ —— 已删除；
- ~~`server.h` 里 `static` 声明导致的编译警告~~ —— 已删除；
- `server.c` 的 `epollRun()` 里还有一句 `printf("num = %d\n", num)` 调试输出，
  每轮 `epoll_wait` 都会打印一次，需要时可去掉。

---

## 10. 关于 `client.c` / `c`

`client.c` 是配套的 TCP 回显测试客户端（硬编码 `192.168.19.128:8989`），
与本 HTTP 模块无关（服务端已不含回显逻辑）；
若服务器已切换为 HTTP 模式，客户端可改用 `curl` 或浏览器测试。
