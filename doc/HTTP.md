# `http.c` / `http.h` 代码详解

> 面向读者：已经会写 `socket` / `epoll` 基础代码，想搞清楚这个 HTTP 模块
> **每一层为什么这么写**的人。
>
> 阅读顺序建议：第 2 节建立整体结构感 → 第 4.3 节看懂数据结构 → 第 4.8 节
> 看懂主流程 → 再回头按需精读第 4.4 ~ 4.7 节的各层实现 → 第 6 节看设计取舍
> → 第 9 节看线程池模式（`http_serve_connection`）与多线程安全。
>
> 文中行号对应仓库当前版本（`http.h` 69 行，`http.c` 880 行）。
> 源码位于仓库的 `source_code/` 目录（即 `source_code/http.h`、`source_code/http.c`）。

---

## 1. 模块定位

`http.h` / `http.c` 是从网络层里**剥离出来的 HTTP 协议层**，它只依赖一个东西：
一个已经建立、已设为非阻塞、已经用 `EPOLLET` 注册进 `epoll` 的**连接 fd**。

```
┌────────────────────────────────────────────────────────────┐
│ 调用方（server.c）                                          │
│   InitListen / new_connection / epoll_wait                  │
│                       │                                     │
│                       ▼  事件来了                           │
│            http_handle(epfd, curfd, evs[i].events)          │
└───────────────────────┬────────────────────────────────────┘
                        │
┌───────────────────────▼────────────────────────────────────┐
│ http.c 内部（调用方完全不用管）                              │
│                                                            │
│  读取(ET 循环) ──> 解析 ──> 路径安全校验 ──> 定位资源        │
│                                              │             │
│                        ┌─────────────────────┴────────┐    │
│                        ▼                              ▼    │
│                   普通文件                        目录/错误  │
│              sendfile + EPOLLOUT               内存型响应   │
│                        └─────────────┬────────────────┘    │
│                                      ▼                     │
│                    keep-alive ? 复位继续 : epoll_ctl DEL+close
└────────────────────────────────────────────────────────────┘
```

**模块边界**：HTTP 模块**绝不**调用 `accept`，也**绝不**碰监听套接字。
`epoll_wait` 循环、监听、连接建立仍然在 `server.c` 里，职责划分是
"网络层负责让连接可用，协议层负责把连接用完"。

---

## 2. 文件构成与分层

`http.c` 用注释横幅显式分成了 5 层，阅读时可以按层跳转：

| 层 | 行区间 | 内容 | 是否对外可见 |
| --- | --- | --- | --- |
| ① 配置 / 状态 | 27 ~ 69 | 宏、`http_conn_t`、全局变量、连接表锁 | 内部 |
| ② 动态缓冲 | 71 ~ 116 | `buf_t` 与三个操作函数 | 内部 |
| ③ 工具函数 | 118 ~ 311 | 时间、状态码、MIME、转义、URL、请求头解析 | 内部 |
| ④ 连接管理 | 313 ~ 415 | 状态机、读、写、关闭、复位 | 内部 |
| ⑤ 响应构造 | 417 ~ 610 | 响应头拼装、错误页、目录列表、文件发送 | 内部 |
| ⑥ 请求处理 | 612 ~ 731 | `process_request()` 总调度 | 内部 |
| ⑦ 对外接口 | 733 ~ 837 | `http_init` / `http_handle` 等 | **公开** |
| ⑧ 阻塞式接口 | 839 ~ 880 | `http_serve_connection()`（供线程池使用），见第 9 节 | **公开** |

对外只有 5 个函数，其余 29 个函数一律 `static`，链接期不会污染符号表
（可以用 `nm http.o` 验证：只有 `http_init` / `http_handle` / `http_root` /
`http_set_verbose` / `http_serve_connection` 是 `T`，其余是 `t`）。

---

## 3. `http.h` 详解

### 3.1 头文件保护与依赖（第 1 ~ 4 行）

```c
#ifndef HTTP_H
#define HTTP_H
#include <stdint.h>
```

唯一的依赖是 `<stdint.h>`，目的只有一个：`http_handle()` 的第三个参数
`uint32_t events`。**刻意没有包含** `<sys/epoll.h>`——因为调用方本来就是
从 `evs[i].events` 取值，用 `uint32_t` 接住即可。这样头文件极轻，
谁 include 它都不会引入额外依赖。

### 3.2 返回值宏（第 34 ~ 35 行）

```c
#define HTTP_OK   0
#define HTTP_ERR (-1)
```

设计意图：把"连接还活着"和"连接已被我关了"两件事用一个 `int` 表达，
调用方只需 `if (http_handle(...) < 0) { /* 别再用这个 fd 了 */ }`。

之所以不用 `1` 表示成功，是为了让调用方能直接用 `if (ret < 0)` 判断——
和 `recv` / `sendfile` 的错误约定保持一致。

### 3.3 五个对外函数

#### `int http_init(const char *root)` —— 第 39 行

| 项目 | 说明 |
| --- | --- |
| 参数 | `root` 为要发布的本地目录；传 `NULL` 或 `""` 表示"当前工作目录" |
| 返回 | `0` 成功；`-1` 失败（目录不存在/不是目录/`realpath` 失败） |
| 副作用 | 写全局 `g_root` / `g_root_len`；把 `stdout` 设为**行缓冲**；失败时向 `stderr` 打印原因 |
| 调用时机 | 进程启动时调一次；重复调用会覆盖旧值 |

注意它不是"初始化数据结构"意义上的 init——连接表是惰性分配的，
所以这个函数可以被安全地重复调用（`http_handle` 内部就用它做兜底）。

#### `int http_handle(int epfd, int fd, uint32_t events)` —— 第 43 行

整个模块的**唯一入口**。参数 `events` 直接透传 `epoll` 的结果，模块自己
决定该读、该写、还是该关闭。

返回值：
- `HTTP_OK (0)`：连接仍由模块持有，后续可能还有事件；
- `HTTP_ERR (-1)`：连接**已被模块关闭**（`epoll_ctl DEL` + `close` 都做完了），
  调用方不要再对它调用任何函数。

**调用约束**（重要）：
- 同一个 `fd` 不能在多线程里并发调用（模块内部无锁）；
- `fd` 必须是非阻塞的——模块里所有 `recv` / `send` / `sendfile` 都假设
  `EAGAIN` 是正常情况而非错误；
- `fd` 必须已用 `EPOLLET` 注册（模块依赖"一次事件读干净"的语义，
  并且用 `EPOLL_CTL_MOD` 重新武装边沿触发）；
- 调用方**不要**再对该 `fd` 调用 `recv`/`send`/`close`/`epoll_ctl`，
  否则会与模块的内部状态打架。

#### `void http_set_verbose(int on)` —— 第 46 行

访问日志开关，默认开。关掉后 `conn_finish()` 里那行 `printf` 不再执行。

#### `const char *http_root(void)` —— 第 49 行

返回当前生效的根目录**绝对路径**（经过 `realpath` 规范化、去掉末尾 `/`）。
未初始化时返回 `NULL`——`server.c` 正是用这个返回值判断"要不要兜底初始化"。

#### `int http_serve_connection(int fd)` —— 第 59 行

| 项目 | 说明 |
| --- | --- |
| 参数 | `fd`：`accept` 得到的**阻塞**套接字（建议先设 `SO_RCVTIMEO` 防占死线程） |
| 返回 | `0` = 处理已结束；**调用方负责 `close(fd)`** |
| 用途 | 线程池模式下"一个连接一个任务"的入口，详见第 9 节 |
| 与 `http_handle` 的区别 | 内部自循环把整条连接（含 keep-alive 的多个请求）处理完才返回，完全不碰 epoll |

### 3.4 API 设计取舍：为什么只暴露一个 `http_handle`

一个 HTTP 服务器天然有 4 个阶段：读、解析、写、关。天真的设计会暴露 4 个函数，
调用方就得自己在 `epoll` 循环里维护"这个 fd 现在处于哪个阶段"——那等于把
状态机搬到调用方，`epoll` 循环会立刻变得难以维护。

本模块把状态机收进 `http_conn_t.state`，对外只留一个入口，
调用方**零状态**。代价是模块必须自己持有 per-fd 状态（见 4.3.3 的连接表），
以及对 `epoll` 注册项有完全的控制权（所以它需要 `epfd` 参数）。

---

## 4. `http.c` 详解

### 4.1 `_GNU_SOURCE` 与头文件（第 5 ~ 25 行）

```c
#define _GNU_SOURCE      /* 必须在所有 #include 之前 */
```

它带来两个关键能力：

| 用到的东西 | 来自 | 为什么需要 `_GNU_SOURCE` |
| --- | --- | --- |
| `sendfile()` | `<sys/sendfile.h>` | 声明受特性宏保护 |
| `MSG_NOSIGNAL` | `<sys/socket.h>` | 同上 |
| `strcasecmp` / `strdup` | `<strings.h>` / `<string.h>` | 同上 |
| `gmtime_r` | `<time.h>` | POSIX 线程安全版本 |

`MSG_NOSIGNAL` 的作用是让 `send()` 在对方已关闭时不触发 `SIGPIPE`
（模块没有依赖进程级 `signal(SIGPIPE, SIG_IGN)`，更自洽）。

### 4.2 常量（第 27 ~ 28 行）

| 宏 | 值 | 语义 | 超出时的行为 |
| --- | --- | --- | --- |
| `HTTP_REQ_MAX` | 8192 | 请求头缓冲 `c->req` 的容量 | 返回 `413 Content Too Large` |
| `HTTP_HDR_MAX` | 8192 | 响应缓冲（响应头 + 内存型响应体）的容量上限 | 构建函数返回 `-1`，响应可能被截断 |

注意两者的角色不同：前者是**硬性拒绝**（保护服务端内存），
后者是**构建上限**（正常响应头只有约 200 字节，只有目录列表才可能顶到上限）。

### 4.3 数据结构

#### 4.3.1 状态枚举（第 33 行）

```c
typedef enum { ST_READING = 0, ST_SENDING } http_state_t;
```

只有两个状态，这是刻意的简化：

- `ST_READING`：**等着收请求**。可能是新连接，也可能是 keep-alive 复用中的空闲期。
- `ST_SENDING`：**有响应没发完**（响应头在 `hdr` 里，或文件在 `file_fd` 里）。

因为 `input` 永远只可能是"一个请求"，不需要 `ST_PARSING` 这种中间态——
解析是在同一函数调用里一次做完的（`process_request` 是纯计算，不阻塞）。

#### 4.3.2 `http_conn_t`（第 35 ~ 56 行）—— 每个连接一份状态

| 字段 | 类型 | 含义 | 何时写入 |
| --- | --- | --- | --- |
| `fd` | `int` | 本连接的文件描述符，同时是连接表的键 | `conn_get()` 创建时 |
| `epfd` | `int` | 备份的 epoll 实例，供模块内部自行 `MOD` / `DEL` | 每次 `http_handle` 入口 |
| `state` | `http_state_t` | 当前状态机位置 | `resp_commit` / `conn_finish` |
| `keep_alive` | `int` | 本次响应发完后是否复用连接 | `process_request`，`resp_begin` 会读它写进响应头 |
| `head_only` | `int` | 本次是 HEAD：只发头不发体（`Content-Length` 照常填） | `process_request` |
| `status` | `int` | 响应码，仅用于日志 | `resp_begin` |
| `method[16]` | `char[]` | 请求方法副本，供日志与重定向使用 | `process_request` |
| `target[256]` | `char[]` | 请求目标副本（**已截断**，仅供日志和 `Location`） | `process_request` |
| `req[HTTP_REQ_MAX]` | `char[]` | 请求头累积缓冲，始终以 `'\0'` 结尾 | `conn_read` |
| `req_len` | `size_t` | 已收字节数 | `conn_read` |
| `hdr` | `char*` | 待发送的响应头（内存型响应则含响应体） | `resp_commit` |
| `hdr_len` / `hdr_sent` | `size_t` | 响应头总长 / 已发出长度 | `resp_commit` / `conn_flush` |
| `file_fd` | `int` | 待发送的文件；没有则为 `-1` | `serve_file` → `resp_commit` |
| `file_off` | `off_t` | 已发到的文件偏移（`sendfile` 出参） | `serve_file` 初始化，`conn_flush` 推进 |
| `file_remain` | `off_t` | 还没发的字节数 | 同上 |

**两处容易忽略的细节**：

1. `hdr` 是**堆内存**（`buf_t` 移交过来的），`conn_*` 里所有释放点都必须 `free`，
   否则每个请求泄漏一次。
2. `file_off` / `file_remain` 把"发送进度"外化了。有了它们，
   `sendfile` 返回 `EAGAIN` 时**什么都不用记住**，下次事件进来接着调即可——
   这是"无栈式"续传的关键，也是为什么不需要为每个连接分配输出缓冲。

#### 4.3.3 `buf_t` 动态缓冲（第 74 ~ 116 行）

```c
typedef struct { char *p; size_t len, cap; } buf_t;
```

响应是"响应行 + 若干头 + `\r\n\r\n` + 可能的小响应体"拼出来的，
长度事先不可知，所以用"指针 + 长度 + 容量"三件套，按需倍增。

| 函数 | 行 | 行为 | 失败条件 |
| --- | --- | --- | --- |
| `buf_reserve(b, extra)` | 70 | 保证还能塞下 `extra` 字节，容量不足则 `realloc` 倍增 | 超过 `HTTP_HDR_MAX` 或 `realloc` 失败 → `-1` |
| `buf_add(b, s, n)` | 83 | 追加 `n` 字节并补 `'\0'` | 同上 |
| `buf_puts(b, s)` | 93 | `buf_add` 的 `strlen` 包装 | 同上 |
| `buf_printf(b, fmt, ...)` | 95 | `printf` 风格追加 | 同上 |

`buf_printf` 有个小技巧（第 101 ~ 116 行）：**先用栈上的 `tmp[1024]` 试写**，
如果一次装得下就直接 `buf_add`；装不下才第二次 `vsnprintf`，直接写进扩容后的
目标缓冲。这样绝大多数调用（都是一两百字节）不会触发 `realloc`。
需要注意第二次 `vsnprintf` 前要**重新 `va_start`**，因为 `va_list` 已被消耗。

> ⚠️ 已知取舍：`buf_reserve` 在超过 `HTTP_HDR_MAX` 时返回 `-1`，
> 上层 `buf_add` / `buf_puts` 会把 `-1` 一路传上来。但
> `resp_dir_listing()`（第 488 行）**没有检查这些返回值**——
> 目录文件很多导致列表 HTML 超过 8 KB 时，页面会被静默截断
> （`Content-Length` 与截断后的实际长度仍然一致，所以不是协议错误，
> 只是内容不全）。目录很大时应调大 `HTTP_HDR_MAX`，或改为检查返回值后
> 返回 `500`。

#### 4.3.4 连接表（第 59 ~ 69 行 + 766 行）

```c
static http_conn_t **g_conn   = NULL;   /* 以 fd 为下标的指针数组 */
static int           g_conn_cap = 0;    /* 数组容量 */
```

选型理由：`epoll` 事件天然带 `fd`，用 `fd` 直接下标是 **O(1)**，
比哈希表简单得多，也不需要引入任何依赖。代价是数组长度要跟得上
`fd` 取值，所以 `conn_get()`（第 766 行）做了惰性扩容：

```c
int ncap = g_conn_cap ? g_conn_cap : 256;   /* 初值 256 */
while (ncap <= fd) ncap *= 2;                /* 按 2 倍增长到能覆盖 fd */
realloc(...); memset(新区域, 0, ...);        /* 新增部分必须清零 */
```

`memset` 那一步是必须的——`realloc` 出来的新区域是垃圾值，
不清零会把随机地址当成 `http_conn_t*` 用，直接段错误。

**`g_conn_lock`（第 69 行）**：为了支持第 9 节的线程池模式，
连接表加了一把 `pthread_mutex_t`。加锁范围刻意做到最小——**只在
"查表 / 建表项 / 扩容 / 摘链"这几步短暂持有**（`conn_get()` 第 766 行、
`conn_unlink()` 第 328 行）。之所以不需要 per-connection 锁，是因为
**每个连接只交给一个线程处理**，连接状态本身不会跨线程访问。
这把锁保护的是"表"这个数据结构，而不是表里的连接。

`g_root` / `g_root_len` / `g_verbose` 是进程级全局配置，
所以本模块**不支持多目录 / 多实例**；要支持就得把它们塞进一个
`http_server_t` 结构体，并把 `http_handle` 的第一个参数换成它。

### 4.4 工具函数层（第 121 ~ 311 行）

#### `http_date()` 121 · `status_text()` 128 · `mime_type()` 146

- `http_date()` 用 `gmtime_r` + `strftime` 生成 RFC 7231 要求的
  `Sun, 27 Sep 2026 09:28:05 GMT` 格式。**必须用 GMT**，不能用本地时间，
  否则浏览器缓存判断会错；`gmtime_r` 是线程安全版本。
- `status_text()` 是 `switch` 映射，只覆盖本模块会产生的状态码，
  `default` 返回 `"OK"`（正常路径不会走到）。
- `mime_type()`（140 行）用一张 `{扩展名, 类型}` 静态表线性查找，
  先 `strrchr` 取最后一个 `.`，再 `strcasecmp` 比较扩展名。
  30 种常见类型，未命中则 `application/octet-stream`。
  `text/*` 一律带 `; charset=utf-8`，否则中文文件名页面在浏览器里会乱码。

#### 转义三兄弟：`html_escape()` 188 · `url_encode()` 203 · `url_decode()` 221

这三个函数解决的是**两个方向的编码问题**，很容易混淆：

| 函数 | 方向 | 用途 | 关键实现 |
| --- | --- | --- | --- |
| `html_escape` | 数据 → HTML | 把目录里的文件名安全地嵌进列表页 | `& < > "` 转实体，其余原样 |
| `url_encode` | 数据 → URL | 把文件名变成 `<a href>` 里可用的形式 | 保留 `A-Za-z0-9-._~`，其余 `%XX` 大写十六进制 |
| `url_decode` | URL → 数据 | 把浏览器发来的 `%E4%B8%AD` 还原成字节 | 同时拒绝非法转义和 `%00` |

`url_decode()`（215 行）有三处防御，都是必要的：

```c
if (!isxdigit(in[i+1]) || !isxdigit(in[i+2])) return -1;   /* %ZZ / 截断的 %4 */
if (c == '\0') return -1;                                   /* %00 —— 绝不能进路径！ */
if (o + 1 >= outsz) return -1;                              /* 输出溢出 */
```

`%00` 那一行特别关键：如果放任 `\0` 进入路径，后续 `snprintf`/`strlen`
会在那里截断，攻击者就能用 `safe%00/../../etc/passwd` 之类手法绕过检查。

#### `sanitize_rel()` 242 —— 路径规范化

把 URL 路径压成 `/a/b` 形式的相对路径，同时**拒绝任何 `..` 段**：

```c
while (*p == '/') p++;                       /* 折叠 //、/// */
if (n == 1 && seg[0] == '.') continue;       /* 丢弃 "." 段 */
if (n == 2 && seg[0]=='.' && seg[1]=='.') return -1;   /* 拒绝 ".." */
if (o == 0) out[o++] = '/';                  /* 空路径归一为 "/" */
```

它不是简单的 `strstr(path, "..")`——那样会误杀 `..foo.txt` 这种正常文件名。
按段处理才是正确的。

#### `header_get()` 268 —— 取请求头字段

从请求缓冲里按名字取一个头的值，**大小写不敏感**。实现要点：

- 从**第一个 `'\n'` 之后**开始扫（第 271 ~ 273 行的 `strchr(req,'\n')`），
  这样不会把请求行里的内容误当成头；
- 只在**行首**匹配 `name:`，所以 `X-Connection:` 不会命中 `Connection`；
- 逐行处理，遇到空行（去掉 `\r` 后长度为 0）就停止——头部结束；
- 值两端的空格 / 制表符 / `\r` 会被裁掉。

返回值是 `1/0`，值通过出参返回，缓冲区由调用方提供。

#### `find_header_end()` 296 —— 判断"请求头收全了没"

这是 ET 模式下**最核心的一个判断**。因为 `recv` 是流式的，一个请求
可能被拆成多个 TCP 段到达，必须能判断"我手上的字节是否已经构成完整头部"。

```c
for (i...) {
    if (buf[i]=='\n' && buf[i+1]=='\n')                    → 返回 i+2      // 裸 LF 分隔
    if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') → 返回 i+4  // 标准 CRLF
}
return 0;   /* 0 = 还没收全 */
```

同时通过出参 `has_body` 告诉调用方"头部之后还有字节"（即请求体或流水线请求），
返回值 `0` 被复用作"未找到"的哨兵值——因为合法的头部结束位置至少是 2。

### 4.5 连接管理层（第 316 ~ 415 行）

这 5 个函数构成状态机的全部动作。返回值约定贯穿始终：
**`1` = 还需继续，`0` = 完成，`-1` = 出错**（`conn_read` 多一个 `-2`）。

#### `conn_want_write()` 316 —— 切换兴趣到"只关心可写"

```c
ev.events = EPOLLOUT | EPOLLET | EPOLLRDHUP;   /* 注意：没有 EPOLLIN */
epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev);
```

两个设计点：

1. **不含 `EPOLLIN`**：大文件发送期间客户端再发数据我们也不读，
   避免无意义的唤醒（反正响应发完前不会处理下一个请求）。
2. **用 `MOD` 而不是 `ADD`**：`EPOLL_CTL_MOD` 在 ET 模式下会**重新武装**
   边沿触发——这是 ET 编程的标准手法。配合 `conn_finish()` 里再次
   `MOD` 回 `EPOLLIN`，就实现了"读一段 → 写一段 → 读一段"的往返切换。

#### `conn_unlink()` 328 / `conn_close()` 340 —— 资源释放点

释放被拆成了两个函数，公共部分在下层：

```c
/* 摘除连接表项并释放连接状态(不碰套接字 fd) */
static void conn_unlink(http_conn_t *c)
{
    pthread_mutex_lock(&g_conn_lock);                       /* 保护表结构 */
    if (c->fd >= 0 && c->fd < g_conn_cap && g_conn[c->fd] == c)
        g_conn[c->fd] = NULL;                               /* 摘链，防悬垂 */
    pthread_mutex_unlock(&g_conn_lock);

    if (c->file_fd >= 0) close(c->file_fd);                 /* 文件可能还开着 */
    free(c->hdr);
    free(c);
}

static void conn_close(http_conn_t *c)                      /* epoll 模式专用 */
{
    if (c->epfd >= 0) epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_unlink(c);
}
```

拆分的理由正是第 9 节的阻塞模式：那边**连接由调用方持有的 fd，模块无权
关闭**，只需要 `conn_unlink()` 把状态回收掉。于是"关不关 fd"这件事被
上移给调用方决定。

三点关键细节：

1. **表项必须置 `NULL`**，否则下一条连接复用同一个 `fd` 号时，`conn_get`
   会拿到已释放的结构体 → use-after-free。判断条件里带了 `g_conn[fd] == c`
   的二次校验，避免误摘别人的槽位。
2. `pthread_mutex_lock` 只包住"摘链"这 3 行，`free` 在锁外做——
   释放自己的结构体不需要保护，缩短临界区。
3. 模块里所有错误路径最终都汇聚到 `conn_unlink()`，所以不存在资源泄漏的分支。

#### `conn_finish()` 348 —— 响应发完后的收尾

```c
if (c->file_fd >= 0) { close(c->file_fd); c->file_fd = -1; }
free(c->hdr); c->hdr = NULL; c->hdr_len = c->hdr_sent = 0;
c->file_off = c->file_remain = 0;

if (g_verbose) printf("[http] %s %s -> %d%s\n", ...);   /* 访问日志 */

if (!c->keep_alive) return -1;      /* 让调用方去 conn_close */

c->state = ST_READING;              /* 复位成"等下一条请求" */
c->req_len = 0;
c->head_only = 0;
ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev);          /* 重新武装 ET */
```

返回值语义：`0` = 已复位可复用；`-1` = 请调用方关闭。
把"关不关"的决定权留给调用方（`http_handle`），是为了让资源释放只有
`conn_close` 一条路径。

日志打印在这里而不是请求处理处，好处是**记录的是实际完成的结果**，
而且天然只打一次。

#### `conn_flush()` 377 —— 非阻塞发送，两段式

```c
while (c->hdr_sent < c->hdr_len) {          /* 第一段：响应头（+小响应体） */
    n = send(c->fd, c->hdr + c->hdr_sent, c->hdr_len - c->hdr_sent, MSG_NOSIGNAL);
    if (n > 0) { c->hdr_sent += n; continue; }
    if (n < 0 && errno == EINTR) continue;                 /* 被信号打断，重试 */
    if (n < 0 && (errno==EAGAIN||errno==EWOULDBLOCK)) return 1;  /* 稍后再发 */
    return -1;                                              /* EPIPE/ECONNRESET… */
}
while (c->file_fd >= 0 && c->file_remain > 0) {             /* 第二段：文件体 */
    n = sendfile(c->fd, c->file_fd, &c->file_off, (size_t)c->file_remain);
    if (n > 0) { c->file_remain -= n; continue; }
    if (n == 0) break;                                      /* 读到 EOF */
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
    return -1;
}
return 0;
```

值得注意的三点：

1. **响应头必须和文件体分开两次系统调用**。HTTP 不允许把响应头写进文件里，
   而 `sendfile` 只能传文件——所以先 `send` 头，再 `sendfile` 体。
2. `sendfile` 的 `count` 直接给 `file_remain`：内核会尽量一次搬完，
   剩下多少由内核通过更新 `file_off` 和返回值告诉我们，**不需要中间缓冲**，
   数据从磁盘页缓存直接进网卡（零拷贝）。
3 . `EINTR` 必须 `continue`，否则信号（比如 `SIGCHLD`）会让一个正常的
   响应莫名中断。

`sendfile` 返回 `0` 却还有 `file_remain` 的情况理论上是"文件被截断了"，
这里直接 `break` 结束（宁可少发也不会死循环）。

#### `conn_read()` 397 —— ET 模式下的读取

```c
for (;;) {
    if (c->req_len >= sizeof(c->req) - 1) return -2;        /* 请求头过大 */
    n = recv(c->fd, c->req + c->req_len, sizeof(c->req) - 1 - c->req_len, 0);
    if (n > 0) {
        c->req_len += n; c->req[c->req_len] = '\0';
        if (find_header_end(c->req, c->req_len, NULL)) return 1;   /* 收全了 */
        continue;                                             /* 继续读，可能还有 */
    }
    if (n == 0)     return 收全了 ? 1 : -1;                   /* 对端关闭 */
    if (EINTR)      continue;
    if (EAGAIN)     return 收全了 ? 1 : 0;                    /* 读干净了 */
    return -1;
}
```

三个关键点：

- **必须循环读到 `EAGAIN`**：ET 模式下如果只 `recv` 一次就返回，
  内核不会再因为"还有剩余数据"给你新事件，请求就永久卡住了。
- **`n == 0` 时先看是否已收全**：客户端可以是"发完请求就 `shutdown(SHUT_WR)`"
  这种合法行为（例如某些 `curl` 用法），这种情况下仍然要正常响应。
- **每读一块都补 `'\0'`**：这样 `sscanf` / `strchr` / `header_get` 才能直接
  按字符串处理，不用到处传长度。
- **`-2` 是自定义的"恶意/异常"信号**，独立于 `-1`，因为调用方要对它
  回 `413` 而不是直接关闭（见 `http_handle` 第 816 行）。

### 4.6 响应构造层（第 420 ~ 610 行）

这一层把"生成 HTTP 响应"拆成两个动作：**先拼进临时 `buf_t`，再移交**

#### `resp_begin()` 420 —— 拼响应头和公共字段

```c
c->status = code;                                    /* 供日志使用 */
HTTP/1.1 <code> <status_text>
Date: <http_date>
Server: mini-http/1.0
Content-Type: <ctype>              /* ctype 可为 NULL，表示不带 */
Content-Length: <clen>
Connection: keep-alive|close        /* 由 c->keep_alive 决定 */
<extra>                             /* 调用方补充的头，如 Content-Range */
（空行）
```

把 `Date` / `Server` / `Content-Length` / `Connection` 统一在这里出，
保证**每个响应都不缺这些字段**，调用方只需关心业务相关的头。

#### `resp_commit()` 437 —— 移交所有权，进入发送态

```c
free(c->hdr);                       /* 释放上一次的（若有） */
c->hdr = b->p; c->hdr_len = b->len; c->hdr_sent = 0;
b->p = NULL; b->len = b->cap = 0;   /* 把 b 掏空，防止调用方误 free */
c->file_fd = file_fd;
c->state   = ST_SENDING;            /* 状态机切换！ */
```

"把 `b` 掏空"这步很重要：`buf_t` 是栈上变量，如果不置 `NULL`，
调用方（比如 `resp_dir_listing`）在 `free(b.p)` 时就会**双重释放**。
这就是为什么所有 `resp_*` 系列的调用方都只 `free(b.p)` 而不会出问题。

#### `resp_memory()` 449 / `resp_error()` 459 —— 内存型响应

```c
size_t send_len = c->head_only ? 0 : blen;      /* HEAD：只发头 */
resp_begin(c, &b, code, (off_t)blen, ctype, extra);   /* Length 用真实长度 */
if (send_len) buf_add(&b, body, send_len);            /* 体可选 */
resp_commit(c, &b, -1);                               /* file_fd = -1 */
```

**HEAD 的语义在这里落地**：`Content-Length` 填**真实**长度（浏览器需要知道
资源大小），但实体一个字节都不发。所有响应都走这个路径，
包括 301 / 404 / 目录列表。

`resp_error()` 只是把状态码包进一段固定的 HTML 模板，再调 `resp_memory`。
模板里的 `%s` 都是编译期常量（如 `"Not Found"`），不存在注入风险。

#### `human_size()` 473 / `cmp_str()` 483

- `human_size`：字节数格式化成 `1.0 KB` / `2.3 MB`，仅用于目录列表展示。
- `cmp_str`：`qsort` 的比较器，让目录列表按名称排序（注：纯 `strcmp`，
  所以大写字母排在小写前；追求"文件管理器式"排序需要自己写比较逻辑）。

#### `resp_dir_listing()` 488 —— 目录列表页

流程：`opendir` → `readdir` 收集名字到 `char**`（跳过 `.` 和 `..`）
→ `qsort` → 逐项 `stat` 取类型/大小 → 拼 HTML → `resp_memory` → 释放。

几个细节：

- 每项都做 `html_escape`（防 XSS，文件名可能含 `<script>`）
  和 `url_encode`（文件名可能含空格、`#`、`?`、中文）；
- 目录项链接末尾补 `/`，并给一个 `../` 返回上级（根目录时不显示）；
- 循环里 `stat` 了两次（先在条件里判目录、再取大小）——这是可以合并的
  小冗余，但换来判断逻辑直白，属于可接受的取舍；
- 所有 `buf_puts` / `buf_printf` 的返回值**未检查**，即 4.3.3 提到的截断风险。

#### `serve_file()` 549 —— 文件响应（含 Range）

这是最"重"的一个函数，步骤是：`open` → 解析 Range → 拼响应头 → 移交。

Range 解析（第 560 ~ 586 行）只在三个前提同时成立时才生效：

```c
range 非空  &&  strncasecmp(range, "bytes=", 6) == 0
           &&  没有逗号（多区间不支持）
           &&  total > 0（空文件不参与）
```

三种合法写法：

| 请求 | 解析结果 | 含义 |
| --- | --- | --- |
| `bytes=100-199` | `start=100, end=199` | 闭区间 |
| `bytes=100-` | `start=100, end=total-1` | 到文件末尾 |
| `bytes=-100` | `start=total-100, end=total-1` | 最后 100 字节 |

判定代码：

```c
if (dash == r) { /* "-N" 形式 */ }
else if (dash) { s = atoll(r); e = dash[1] ? atoll(dash+1) : total-1; }

if (s >= 0 && e >= s && s < total) { end = min(e, total-1); partial = 1; ... }
else if (dash) { /* 语法有横杠但数值非法 → 416 + Content-Range: bytes */total */ }
```

注意 `else if (dash)` 这个条件：`bytes=abc`（没有横杠）会被**忽略**并按
`200` 整文件返回，符合"不认识就忽略"的容错原则；而 `bytes=99999999-`
（有横杠但越界）才回 `416`，并按要求带上 `Content-Range: bytes */<total>`。

拼头部时按是否 `partial` 追加两个额外字段：

```c
if (partial) "Accept-Ranges: bytes\r\nLast-Modified: <date>\r\nContent-Range: bytes s-e/total\r\n"
else         "Accept-Ranges: bytes\r\nLast-Modified: <date>\r\n"
```

最后：

```c
c->file_off    = start;
c->file_remain = c->head_only ? 0 : len;    /* HEAD 时长度为 0，头部却已是 len */
resp_commit(c, &b, fd);                     /* fd 交给模块，由 conn_flush 发 */
```

**注意 `open` 后的 `fd` 所有权**：从此以后由 `http_conn_t.file_fd` 持有，
`conn_finish` / `conn_close` 负责关闭；任何错误分支里都必须在 `resp_commit`
之前 `close(fd)`（第 585 行的 416 分支、第 603 行的 `resp_begin` 失败分支就是这么做的）。

### 4.7 请求处理层 `process_request()` 615

这是"决策中心"，不涉及任何 I/O，纯计算 + 构造响应。执行顺序：

```
1  解析请求行    sscanf(req, "%15s %2047s %15s")   ← 失败 → 400
2  记录 method/target（供日志与 301 使用）
3  判定 keep-alive（版本默认值 + Connection 头覆盖 + 请求体强制关闭）
4  方法白名单    非 GET/HEAD → 405 (Allow: GET, HEAD)
5  丢弃 ?query 与 #fragment
6  校验以 '/' 开头                               → 否则 400
7  url_decode                                   → 失败 400
8  sanitize_rel（拒绝 ..）                       → 失败 403
9  拼绝对路径，长度检查                            → 超长 414
10 realpath + stat                               → 失败 404
11 校验 real 落在 g_root 之内                     → 否则 403
12 取 Range 头
13 分派：目录（301 / index.html / 目录列表）| 普通文件 | 其它 → 403
```

其中几个值得展开的点：

**第 3 步的三个层次**

```c
int keep = (strcmp(version, "HTTP/1.1") == 0);   /* ① 版本默认 */
if (header_get(..., "Connection", ...)) { close/keep-alive 覆盖 }   /* ② 显式头 */
if (has_body) keep = 0;                          /* ③ 有体就关 */
```

第 ③ 条是本实现的**关键简化**：因为模块不解析请求体（`Content-Length`
指定的那部分字节），如果在同一连接上继续等下一条请求，缓冲里残留的请求体
会被当成新请求的开头解析 → 请求走私 / 解析错位。与其写一套请求体消费逻辑，
不如直接关连接，语义上完全合法（`Connection: close` 是标准做法）。

**第 10 ~ 11 步是安全防线**（三重防护的最后一环）：

```c
if (realpath(fspath, real) == NULL || stat(real, &st) < 0) → 404;
if (!(g_root_len == 1 ||                              /* 根目录为 "/" 的特例 */
      (strncmp(real, g_root, g_root_len) == 0 &&
       (real[g_root_len] == '/' || real[g_root_len] == '\0')))) → 403;
```

只比较前缀是不够的——`/var/www2` 也以 `/var/www` 开头，所以必须要求
第 `g_root_len` 个字符是 `/` 或字符串结尾。`g_root_len == 1`
（根目录就是 `/`）单独放行，否则前缀比较会误杀一切。

`realpath` 的作用是**消解符号链接**：如果 web 目录里有个软链接指向
`/etc/passwd`，`..` 检查是拦不住的，只有 `realpath` + 前缀校验能拦住。

**第 13 步的目录分支**：

```c
if (S_ISDIR) {
    if (!want_dir) → 301  Location: <原target>/        /* 补斜杠 */
    找 index.html → 有则 serve_file
    找 index.htm  → 有则 serve_file
    否则 → resp_dir_listing
}
```

301 那一支必须用 `c->target`（**原始 URL**，含查询串）而不是拼出来的
文件路径，否则重定向会带上磁盘路径，既错又泄露信息。

### 4.8 出口层：`http_init()` 736 与 `http_handle()` 795

#### `http_init()` 736

```c
setvbuf(stdout, NULL, _IOLBF, 0);          /* ① 日志行缓冲 */
if (!root || !*root) root = getcwd(...);   /* ② 空参数 → 当前目录 */
realpath(root, real);                       /* ③ 符号链接归一化 */
while (n > 1 && real[n-1] == '/') real[--n] = '\0';   /* ④ 去掉末尾 '/' */
stat + S_ISDIR 校验                          /* ⑤ 必须是目录 */
```

第 ① 步容易被忽视但很实用：默认 `stdout` 重定向到文件时是**全缓冲**，
`printf` 的日志会一直攒着不落盘，`kill` 之后什么都看不到。设为 `_IOLBF`
后每行都及时写出去。

第 ④ 步去掉末尾 `/` 是为了让后面 `snprintf("%s%s", g_root, rel)` 不会
拼出 `//index.html`（虽然 Linux 能容忍，但 `g_root_len` 前缀比较会算错）。

#### `http_handle()` 795 —— 三阶段处理

```c
if (g_root_len == 0 && http_init(NULL) < 0) return HTTP_ERR;   /* 兜底 */
http_conn_t *c = conn_get(fd);          /* 取/建 per-fd 状态 */
c->epfd = epfd;
if (events & (EPOLLHUP|EPOLLERR)) → conn_close;                 /* ① 异常 */

if (c->state == ST_SENDING) {                                   /* ② 续传 */
    r = conn_flush(c);
    if (r < 0) → close;  if (r == 1) → want_write 后返回;         /* 还没发完 */
    if (conn_finish(c) < 0) → close;                             /* 发完了 */
    if (!(events & EPOLLIN)) return HTTP_OK;   /* 没有新可读事件就结束 */
}

r = conn_read(c);                                               /* ③ 读请求 */
if (r == -2)       → 413（并清 req_len、日志占位）
else if (r < 0)    → close
else if (r == 0)   → return（还没收全，等下次事件）
else               → process_request(c)      /* 状态变为 ST_SENDING */

f = conn_flush(c);                                              /* ④ 立刻发 */
if (f < 0) → close;  if (f == 1) → want_write 后返回;
if (conn_finish(c) < 0) → close;
```

几个精妙之处：

- **阶段 ② 里"发完但这次事件同时可读"**（第 811 行 `if (!(events & EPOLLIN))`）：
  一次 `epoll_wait` 可能同时报告 `EPOLLOUT|EPOLLIN`（比如客户端在
  keep-alive 连接上已经流水线发了下一个请求）。发完后**不 return**，
  继续往下走读流程，避免多等一轮事件。
- **阶段 ④ "立刻尝试发送"**：小响应（比如 404 页）通常一次 `send` 就发完了，
  这时根本不需要 `EPOLLOUT`，`conn_finish` 直接复位连接。
  **只有 `conn_flush` 返回 `1` 时才切到 `EPOLLOUT`**——绝大部分请求
  因此少一次 `epoll_ctl` 系统调用。
- **`-2` 分支里把 `method`/`target` 置成 `"-"`**（第 818 ~ 819 行）：
  否则日志里会打印上一次请求的数据（结构体在 keep-alive 上是复用的），
  看起来像是"日志串了"。
- **兜底初始化**（第 797 行）：调用方忘了 `http_init` 也不会崩，
  只是默认发布 cwd。

---

## 5. 一次 `GET /big.bin` 的完整时序

```
调用方                          http.c
  │
  │ epoll_wait 返回 EPOLLIN
  ├── http_handle(epfd, fd, EPOLLIN) ──►│  状态 = ST_READING
  │                                     ├─ conn_get(fd)       创建/取出状态
  │                                     ├─ 无 HUP/ERR
  │                                     ├─ 跳过发送阶段（state != ST_SENDING）
  │                                     ├─ conn_read():
  │                                     │    recv → "GET /big.bin HTTP/1.1\r\n..."
  │                                     │    find_header_end() 找到 \r\n\r\n → 返回 1
  │                                     ├─ process_request():
  │                                     │    解析 → keep-alive=1（无请求体）
  │                                     │    decode/sanitize → /big.bin
  │                                     │    realpath + 前缀校验 OK
  │                                     │    stat → 普通文件 1 MB
  │                                     │    无 Range → serve_file()
  │                                     │      open() → fd
  │                                     │      resp_begin(200, 1048576, ...)
  │                                     │      c->file_off=0, file_remain=1048576
  │                                     │      resp_commit() → state = ST_SENDING
  │                                     ├─ conn_flush():
  │                                     │    send() 把响应头发完
  │                                     │    sendfile() 可能只搬了一部分 → EAGAIN → 返回 1
  │                                     ├─ conn_want_write() → MOD 为 EPOLLOUT|ET
  │◄── return HTTP_OK ───────────────────┤
  │
  │ epoll_wait 返回 EPOLLOUT
  ├── http_handle(epfd, fd, EPOLLOUT) ─►│  状态 = ST_SENDING
  │                                     ├─ conn_flush() → 搬完剩余字节 → 返回 0
  │                                     ├─ conn_finish():
  │                                     │    close(文件fd)、free(hdr)、清计数
  │                                     │    printf("[http] GET /big.bin -> 200 (keep-alive)")
  │                                     │    keep_alive → 复位为 ST_READING、req_len=0
  │                                     │    MOD 回 EPOLLIN|ET（重新武装）
  │◄── return HTTP_OK ───────────────────┤
  │
  │  （连接保持，等待同一个 fd 上的下一个请求）
```

---

## 6. 关键设计点与易踩的坑

### 6.1 为什么必须用 `EPOLL_CTL_MOD` 反复重新武装

ET（边沿触发）只在新数据到达时报告一次。模块在发送期间把注册项改成
`EPOLLOUT`（丢掉了 `EPOLLIN`），发完后必须用 `MOD` **重新注册** `EPOLLIN`。
如果只是"什么都没做"，注册项仍停留在 `EPOLLOUT`，客户端的新请求
永远不会触发事件 → 连接假死。

反之，`MOD` 本身会重置 ET 的内部状态：即使数据在 `MOD` 之前就已经到达
（比如客户端在 pipelining 场景下早发了下一个请求），`MOD` 之后也会**补报**
一次 `EPOLLIN`。这是本模块能同时支持 keep-alive 和流水线数据的基础。

### 6.2 为什么不用"给每个连接分配输出缓冲"

朴素做法是：把整个响应（含文件内容）拼进一块大内存，然后循环 `send`。
1 MB 文件 × 100 个并发连接 = 100 MB 常驻内存，而且要做大量 `memcpy`。

本模块改成**只持有文件 fd + 偏移 + 剩余长度**（`file_fd` / `file_off` /
`file_remain`），靠 `sendfile` 让内核直接从页缓存搬到套接字。
内存占用与文件大小**无关**，只有响应头那几百字节是堆内存。

### 6.3 `EPOLLRDHUP` 注册了但没显式判断

`conn_want_write` 和 `conn_finish` 都注册了 `EPOLLRDHUP`，但代码里
没有任何 `if (events & EPOLLRDHUP)` 分支。这是有意的：

- 对端半关闭时，`recv` 会返回 `0`，`conn_read` 已经能正确处理；
- 如果对端只是 `shutdown(SHUT_WR)` 但仍然在下载文件，
  此时**不应该**关闭连接，否则大文件会下载中断。

所以让 `recv` 去决定，比用 `EPOLLRDHUP` 提前判断更安全。
代价是：**纯粹的对端关闭**会多经历一次 `recv` 系统调用才被发现。

### 6.4 已知边界与限制（诚实清单）

| 限制 | 位置 | 影响 | 建议 |
| --- | --- | --- | --- |
| 不解析请求体 | `process_request` 第 641 行 | POST/上传不支持，带体请求后连接关闭 | 需要时按 `Content-Length` 消费字节后再决定 keep-alive |
| 多区间 Range 被忽略 | `serve_file` 第 561 行 `strchr(...,',')` | `bytes=0-9,20-29` 返回 200 整文件 | 需要时实现 `multipart/byteranges` |
| 目录列表超 8 KB 截断 | `resp_dir_listing` 未检查缓冲返回值 | 页面内容不全 | 调大 `HTTP_HDR_MAX` 或在溢出时返回 `500` |
| `EPOLLHUP\|EPOLLERR` 直接关闭 | `http_handle` 第 803 行 | 极端情况下已缓冲的数据未发完 | 如需"优雅收尾"，改成"先 flush 再关" |
| 全局单例（`g_root` 等） | 第 59 ~ 69 行 | 一个进程只能发布一个目录 | 封装成 `http_server_t` |
| 无超时 | 全靠调用方 | 慢连接可能长期占用 fd | 在调用方加 `EPOLL` 超时 / `timerfd` 踢掉空闲连接 |
| 无 per-connection 锁 | 全局状态 | 同一个连接不能被两个线程同时处理 | 靠"一个连接只交给一个线程"的约束保证（见第 9 节） |
| 阻塞模式下连接独占线程 | `http_serve_connection` 第 850 行 | 并发连接数受线程数限制 | 提高线程上限，或改用 one-loop-per-thread |
| 阻塞模式必须设超时 | `pool_server.cpp` 的 `set_socket_timeout` | 未设超时时空闲 keep-alive 会永久占住线程 | 已设 SO_RCVTIMEO=5s / SO_SNDTIMEO=10s |

### 6.5 安全三重防线小结

| 防线 | 位置 | 拦住的攻击 |
| --- | --- | --- |
| 拒绝 `%00` | `url_decode` 第 233 行 | NUL 截断绕过后续所有字符串检查 |
| 按段拒绝 `..` | `sanitize_rel` 第 253 行 | `../../etc/passwd`、`a/../../b` |
| `realpath` + 前缀校验 | `process_request` 第 686 ~ 693 行 | 指向外部的**符号链接**（前两道拦不住） |
| HTML 转义 | `resp_dir_listing` | 恶意文件名造成的目录列表 XSS |
| `MSG_NOSIGNAL` | `conn_flush` | 对端异常关闭导致进程被 `SIGPIPE` 杀死 |

---

## 7. 内存与资源生命周期

| 资源 | 分配点 | 释放点 | 备注 |
| --- | --- | --- | --- |
| `http_conn_t` | `conn_get` 第 783 行 `calloc` | `conn_unlink` 第 337 行 | 每连接一次，keep-alive 期间复用 |
| `g_conn` 数组 | `conn_get` 第 775 行 `realloc` | 进程退出 | 只增不减，增长到最大 fd 所需的规模 |
| `g_conn_lock` | 静态初始化（第 69 行） | 进程退出 | 只在查表/建项/摘链时短暂持有，见 9.4 |
| 响应缓冲 `c->hdr` | `resp_commit` 第 439 行（`buf_t` 移交） | `conn_finish` / `conn_close` 的 `free` | **每个请求一次**，是唯一的高频堆分配 |
| 响应文件 `c->file_fd` | `serve_file` 第 552 行 `open` | `conn_finish` / `conn_close` | 必须在 `resp_commit` 之前 `close` 掉失败分支的 fd |
| 目录列表 `names[]` | `resp_dir_listing` `strdup` | 同函数末尾逐个 `free` + `free(names)` | 即使中途 `break`，也用 `n` 计数保证只释放已分配的项 |
| `reachable` 临时 `buf_t b` | 各 `resp_*` 栈上 | `resp_commit` 掏空 / 失败时 `free(b.p)` | 见 4.6 关于"掏空"的说明 |

**建议的验证方法**：跑一轮压测后用 `valgrind --leak-check=full ./s 8989 4096 <dir>`
或 `gcc -fsanitize=address,undefined` 重编译，逐个 `curl` 各类资源
（200 / 206 / 301 / 404 / 目录列表 / 大文件），确认无泄漏、无越界。

---

## 8. 扩展指南

**加一种 MIME 类型**：在 `mime_type()`（第 146 行）的表里加一行即可，
表必须保持以 `{NULL, NULL}` 结尾。

**支持新方法（如 `DELETE`）**：在 `process_request` 第 628 ~ 629 行附近
增加判定分支，并在第 644 行的白名单处放行；注意同步修改 405 响应里的
`Allow` 头。

**加 `ETag` / `If-Modified-Since` 协商缓存**：在 `serve_file` 里，
用 `st.st_ino + st.st_size + st.st_mtime` 拼一个弱 `ETag`，
先检查请求头的 `If-None-Match` / `If-Modified-Since`，命中则回 `304`
（`304` 不能带实体，且 `Content-Length` 应为 0 或不带）。
需要在 `status_text()` 里补 `304` 的文案。

**加 gzip 预压缩**：在 `serve_file` 里检查 `Accept-Encoding: gzip` 且
同目录存在 `xxx.gz` 时，改发那个文件并加 `Content-Encoding: gzip`。
注意 `Content-Length` 要换成压缩后的大小，`Content-Type` 保持原文件的。

**改成多线程**：本模块的全局状态（`g_conn` / `g_root` / `g_verbose`）
不可跨线程共享。推荐模型是 **one-loop-per-thread**：每个线程
`epoll_create` 一个实例、调用一次 `http_init`（只读配置其实是安全的），
并且只用 `SO_REUSEPORT` 让多线程各自 `accept`——此时连接集合天然按线程隔离。

---

## 9. 线程池模式（`http_serve_connection`）

### 9.1 为什么不把 `epoll` 搬进线程池

`http_handle()` 的驱动源是**内核事件**——它是"一个 epoll 事件驱动一次"的
状态机；而线程池的调度单位是**一次性任务**。如果把 `epoll_wait` 放进任务：

```c
pool.addTask([]{ while (1) epoll_wait(...); });   /* ← 不要这样写 */
```

任务永不返回 → 工作线程被永久占住 → 线程池的排队/扩容/缩容全部失效，
`waitAllDone()` 和析构也会永远卡住。**两者语义不匹配**。

### 9.2 正确的切分：以"一整条连接"为任务

```
主线程                     线程池                          工作线程
  │                         │                                │
  ├─ accept() ─────────────►│                                │
  │   cfd                   ├─ addTask([cfd]{ ... }) ───────►│
  │                         │                     http_serve_connection(cfd)
  │                         │                     阻塞式处理完整条连接
  │                         │                     (含 keep-alive 的多个请求)
  │                         │                     处理完 close(cfd) 回到池中
  │                         │                                │
  │  连接积压 → 队列变长 → 扩容到 max；流量低谷 → 缩容回 min  ← 线程池真正在干活
```

这样线程池的排队/扩容/缩容才真正有意义，而"并行粒度"也正好落在
静态文件服务器最值得并行的单位上。

### 9.3 两个入口的对比

| | `http_handle()` 第 795 行 | `http_serve_connection()` 第 850 行 |
| --- | --- | --- |
| 适用模型 | 单线程 Reactor / 多 Reactor | 线程池 + 每连接一个任务 |
| 驱动方式 | 由 epoll 事件驱动，一次处理一段 | 内部自循环，处理完整条连接才返回 |
| 套接字要求 | 非阻塞 + `EPOLLET` | **阻塞**（`accept` 拿到的默认就是阻塞） |
| 是否碰 epoll | 会 `MOD` / `DEL` | 不碰（内部把 `c->epfd` 置为 `-1`） |
| 是否关 fd | 关（经 `conn_close`） | 不关（只 `conn_unlink`），由调用方 `close` |
| keep-alive | 靠 `EPOLL_CTL_MOD` 重新武装 ET | 靠循环 + `SO_RCVTIMEO` 超时 |
| 并发上限 | 受 fd 上限，与线程数无关 | 受线程数限制 |
| 慢连接影响 | 不影响其它连接 | 会占住一个工作线程 |

### 9.4 为支持阻塞模式做的三处改造

| 改造 | 位置 | 原因 |
| --- | --- | --- |
| 连接表加锁 `g_conn_lock` | 第 69 行；`conn_get` 第 766 行、`conn_unlink` 第 328 行 | `g_conn` 是全局表，多线程会并发 `realloc`/建表项/摘链。只在"表结构"层面加锁；每个连接只由一个线程碰，所以**不需要 per-connection 锁** |
| 拆出 `conn_unlink()` | 第 328 行 | 阻塞模式下模块"只回收状态、不关 fd、不碰 epoll"，与 `conn_close()`（epoll 模式）分离 |
| `c->epfd < 0` 守卫 | `conn_want_write` 第 316 行、`conn_finish` 第 348 行 | 阻塞模式没有 epoll 实例，必须跳过 `epoll_ctl` |

> 另外 `http.h` 加了 **`extern "C"` 保护**（第 9 ~ 11 行 / 第 61 ~ 63 行）。
> 这不是可选项：C++ 源文件（如 `pool_server.cpp`）include `http.h` 时，
> 函数名会被 C++ 规则修饰，链接期找不到 gcc 编出来的 `http_root` /
> `http_serve_connection` 符号，会直接报 `undefined reference`。

### 9.5 使用方式

桥接层在 `pool_server.cpp`，对外只暴露一个 C 接口：

```c
/* pool_server.h */
int pool_run(unsigned short port, int min, int max);   /* extern "C" */
```

`pool_run()` 的核心就三行：

```cpp
int cfd = accept(lfd, nullptr, nullptr);
set_socket_timeout(cfd, 5, 10);                 /* ★ 超时保护，见 9.6 */
pool.addTask([cfd]{                             /* ★ 一整条连接 = 一个任务 */
    http_serve_connection(cfd);
    close(cfd);
});
```

命令行通过第 4 个参数切换模型：

```bash
./s 8989 4096 /var/www/html        # 缺省: 单线程 epoll 模式 (http_handle)
./s 8989 4096 /var/www/html 8      # 线程池模式: min=2, max=8
```

实测（20 个并发慢速下载，每个 1MB 限速 100KB/s，`min=2 max=8`）：

```
启动后线程数 4 (= 主线程 + 管理线程 + 2 工作线程)
  t=4s → 6    (扩容)
  t=6s → 8    (扩容到 max)
20 份下载 md5 全部与源文件一致
空闲 8s → 6, 空闲 14s → 4   (缩容, 每 5 秒回收 2 个, 最终回落到 min)
```

### 9.6 必须设置超时（不是可选项）

阻塞模式下一个连接**独占一个线程**。如果不设超时，一个只建立连接、
不发送任何数据的客户端就能永久占死一个工作线程；几个这样的连接
（Slowloris）足以让整个线程池瘫痪。所以 `pool_server.cpp` 里对每个
连接都调用了：

```cpp
set_socket_timeout(cfd, 5, 10);   /* SO_RCVTIMEO=5s, SO_SNDTIMEO=10s */
```

对应地，`conn_read()`（第 397 行）在阻塞套接字上读到超时会拿到
`EAGAIN`，于是返回 `0`，`http_serve_connection()` 的循环据此退出并关闭连接——
空闲的 keep-alive 连接最多占住线程 5 秒。

### 9.7 什么时候不该用这个模式

静态文件服务器的瓶颈通常不在这里：`epoll` + 非阻塞 + `sendfile` 已经让
单线程能扛住大量并发连接。线程池模式的价值在于**教学**和"任务里有阻塞
操作"的场景（比如查询数据库、读慢速后端）。如果目标是提高并发连接数，
应该走 **one-loop-per-thread**（每线程一个 epoll + `SO_REUSEPORT`），
而不是加大线程池——那时线程池的"任务队列"语义就不适用了。**两条路线不要混用。**

---

## 附录 A：函数索引

| 行号 | 函数 | 职责 | 返回值约定 |
| --- | --- | --- | --- |
| 76 | `buf_reserve` | 保证缓冲容量 | `0` / `-1` |
| 89 | `buf_add` | 追加字节 | `0` / `-1` |
| 99 | `buf_puts` | 追加字符串 | `0` / `-1` |
| 101 | `buf_printf` | 格式化追加 | `0` / `-1` |
| 121 | `http_date` | GMT 时间串 | — |
| 128 | `status_text` | 状态码 → 短语 | — |
| 146 | `mime_type` | 扩展名 → MIME | — |
| 188 | `html_escape` | HTML 实体转义 | — |
| 203 | `url_encode` | 路径段百分号编码 | — |
| 221 | `url_decode` | 百分号解码 | 长度 / `-1` |
| 242 | `sanitize_rel` | 路径规范化 + 拒 `..` | `0` / `-1` |
| 268 | `header_get` | 取请求头字段 | `1` / `0` |
| 296 | `find_header_end` | 头部是否收全 | 偏移（`0`=未全） |
| 316 | `conn_want_write` | 切到 `EPOLLOUT`（阻塞模式空实现） | — |
| 328 | `conn_unlink` | 摘链 + 释放连接状态（不碰 fd） | — |
| 340 | `conn_close` | `conn_unlink` + epoll `DEL` + `close(fd)` | — |
| 348 | `conn_finish` | 响应收尾 / keep-alive 复位 | `0` / `-1` |
| 377 | `conn_flush` | 非阻塞发送头 + 文件 | `1` 未完 / `0` 完成 / `-1` 错 |
| 397 | `conn_read` | ET 循环收请求 | `1`/`0`/`-1`/`-2` |
| 420 | `resp_begin` | 拼响应行与公共头 | `0` / `-1` |
| 437 | `resp_commit` | 移交缓冲 → 进入发送态 | — |
| 449 | `resp_memory` | 内存型响应 | — |
| 459 | `resp_error` | 错误页 | — |
| 473 | `human_size` | 大小美化 | — |
| 483 | `cmp_str` | `qsort` 比较器 | — |
| 488 | `resp_dir_listing` | 目录列表页 | — |
| 549 | `serve_file` | 静态文件 + Range | — |
| 615 | `process_request` | 请求解析与分派 | — |
| 736 | **`http_init`** | 设定根目录 | `0` / `-1` |
| 762 | **`http_set_verbose`** | 日志开关 | — |
| 764 | **`http_root`** | 查询根目录 | 路径 / `NULL` |
| 766 | `conn_get` | 取/建连接状态（内部加锁） | 指针 / `NULL` |
| 795 | **`http_handle`** | epoll 模式入口：一次事件处理一段 | `HTTP_OK` / `HTTP_ERR` |
| 850 | **`http_serve_connection`** | 阻塞模式入口：处理完整条连接 | `0`（调用方负责 `close`） |

（加粗为对外接口，共 5 个；其余 29 个均为 `static`。`http.h` 中的声明位置：
`http_init` 第 39 行、`http_handle` 第 43 行、`http_set_verbose` 第 46 行、
`http_root` 第 49 行、`http_serve_connection` 第 59 行。）

## 附录 B：响应头字段出现顺序

```
HTTP/1.1 <状态码> <短语>\r\n     ← resp_begin
Date: <GMT>\r\n                  ← resp_begin
Server: mini-http/1.0\r\n        ← resp_begin
Content-Type: <MIME>\r\n         ← resp_begin（ctype 为 NULL 时省略）
Content-Length: <字节数>\r\n      ← resp_begin
Connection: keep-alive|close\r\n ← resp_begin
Accept-Ranges: bytes\r\n         ← serve_file 的 extra
Last-Modified: <GMT>\r\n         ← serve_file 的 extra
Content-Range: bytes s-e/total\r\n ← serve_file 的 extra（仅 206）
Location: <url>/\r\n             ← 301 的 extra
Allow: GET, HEAD\r\n             ← 405 的 extra
\r\n                             ← resp_begin 结尾
```

字段顺序在 HTTP 里不敏感，但保持"通用字段在前、语义字段在后"的顺序，
抓包和调试时更好读。
