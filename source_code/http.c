/* =====================================================================
 *  http.c - 轻量 HTTP/1.1 静态文件服务器 (epoll 边缘触发)
 *  详见 http.h 中的用法说明。
 * ===================================================================== */
#define _GNU_SOURCE
#include "http.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <pthread.h>

#define HTTP_REQ_MAX   8192   /* 单个请求头的最大长度 */
#define HTTP_HDR_MAX   8192   /* 响应头 + 小响应体的上限(超出则截断保护) */

/* ------------------------------------------------------------------ */
/*  连接状态                                                           */
/* ------------------------------------------------------------------ */
typedef enum { ST_READING = 0, ST_SENDING } http_state_t;

typedef struct {
    int          fd;
    int          epfd;
    http_state_t state;
    int          keep_alive;
    int          head_only;      /* HEAD: 只发头, 不发体 */
    int          status;         /* 日志用 */

    char         method[16];
    char         target[256];

    char         req[HTTP_REQ_MAX];
    size_t       req_len;

    char        *hdr;            /* 待发送的响应头(以及小响应体) */
    size_t       hdr_len;
    size_t       hdr_sent;

    int          file_fd;        /* 待发送的文件(NULL 为 -1) */
    off_t        file_off;
    off_t        file_remain;
} http_conn_t;

/* 以 fd 为下标连接表 */
static http_conn_t **g_conn   = NULL;
static int           g_conn_cap = 0;

static char        g_root[PATH_MAX];
static size_t      g_root_len = 0;
static int         g_verbose  = 1;

/* 连接表锁: 只在"查表/建表项/摘链/扩容"时短暂持有。
 * 每个连接的状态本身只由其所属线程访问(一个连接只交给一个线程),
 * 所以不需要 per-connection 锁。 */
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  小工具: 动态缓冲                                                   */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t len, cap; } buf_t;

static int buf_reserve(buf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 512;
    while (cap < b->len + extra) cap *= 2;
    if (cap > HTTP_HDR_MAX) cap = HTTP_HDR_MAX;
    if (b->len + extra > cap) return -1;
    char *np = realloc(b->p, cap);
    if (!np) return -1;
    b->p = np; b->cap = cap;
    return 0;
}

static int buf_add(buf_t *b, const char *s, size_t n)
{
    if (n == 0) return 0;
    if (buf_reserve(b, n + 1) < 0) return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int buf_puts(buf_t *b, const char *s) { return buf_add(b, s, strlen(s)); }

static int buf_printf(buf_t *b, const char *fmt, ...)
{
    char    tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return buf_add(b, tmp, (size_t)n);
    if (buf_reserve(b, (size_t)n + 1) < 0) return -1;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  小工具: 时间 / 状态码 / MIME / 转义                                 */
/* ------------------------------------------------------------------ */
static void http_date(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } tbl[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "application/javascript; charset=utf-8" },
        { "mjs",  "application/javascript; charset=utf-8" },
        { "json", "application/json; charset=utf-8" },
        { "txt",  "text/plain; charset=utf-8" },
        { "md",   "text/plain; charset=utf-8" },
        { "csv",  "text/csv; charset=utf-8" },
        { "xml",  "application/xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "bmp",  "image/bmp" },
        { "webp", "image/webp" },
        { "svg",  "image/svg+xml" },
        { "ico",  "image/x-icon" },
        { "woff", "font/woff" },
        { "woff2","font/woff2" },
        { "ttf",  "font/ttf" },
        { "otf",  "font/otf" },
        { "pdf",  "application/pdf" },
        { "mp4",  "video/mp4" },
        { "webm", "video/webm" },
        { "mp3",  "audio/mpeg" },
        { "wav",  "audio/wav" },
        { "ogg",  "audio/ogg" },
        { "zip",  "application/zip" },
        { "gz",   "application/gzip" },
        { NULL, NULL }
    };
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; tbl[i].ext; ++i)
        if (strcasecmp(dot + 1, tbl[i].ext) == 0) return tbl[i].type;
    return "application/octet-stream";
}

static void html_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < outsz; ++i) {
        switch (in[i]) {
        case '&': memcpy(out + o, "&amp;", 5);  o += 5; break;
        case '<': memcpy(out + o, "&lt;", 4);   o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4);   o += 4; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        default:  out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static void url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < outsz; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* 把 URL 路径做百分号解码; 返回解码后长度, 失败返回 -1 */
static int url_decode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; in[i]; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            if (!isxdigit((unsigned char)in[i + 1]) || !isxdigit((unsigned char)in[i + 2]))
                return -1;
            char h[3] = { in[i + 1], in[i + 2], '\0' };
            c = (unsigned char)strtol(h, NULL, 16);
            i += 2;
        }
        if (c == '\0') return -1;           /* 拒绝 %00 之类 */
        if (o + 1 >= outsz) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return (int)o;
}

/* 规范化 URL 路径成 "/a/b" 形式, 拒绝任何 ".." */
static int sanitize_rel(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);
        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return -1;
        if (o + n + 2 >= outsz) return -1;
        out[o++] = '/';
        memcpy(out + o, seg, n);
        o += n;
    }
    if (o == 0) {
        if (outsz < 2) return -1;
        out[o++] = '/';
    }
    out[o] = '\0';
    return 0;
}

/* 在请求头里取某个字段的值(大小写不敏感), 找到返回 1 */
static int header_get(const char *req, const char *name, char *out, size_t outsz)
{
    size_t nlen = strlen(name);
    const char *p = strchr(req, '\n');
    if (!p) return 0;
    p++;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen && p[linelen - 1] == '\r') linelen--;
        if (linelen == 0) break;                       /* 空行 = 头部结束 */
        if (linelen > nlen + 1 && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = (size_t)(p + linelen - v);
            while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

/* 请求头是否收全; 返回头部结束的偏移(0=未收全), has_body 表示后面还有字节 */
static size_t find_header_end(const char *buf, size_t len, int *has_body)
{
    if (has_body) *has_body = 0;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            if (has_body) *has_body = (i + 2 < len);
            return i + 2;
        }
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            if (has_body) *has_body = (i + 4 < len);
            return i + 4;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  连接对象管理                                                       */
/* ------------------------------------------------------------------ */
static void conn_want_write(http_conn_t *c)
{
    if (c->epfd < 0) return;             /* 阻塞模式: 不需要 EPOLLOUT */

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = c->fd;
    epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* 摘除连接表项并释放连接状态(不碰套接字 fd) */
static void conn_unlink(http_conn_t *c)
{
    pthread_mutex_lock(&g_conn_lock);
    if (c->fd >= 0 && c->fd < g_conn_cap && g_conn[c->fd] == c)
        g_conn[c->fd] = NULL;
    pthread_mutex_unlock(&g_conn_lock);

    if (c->file_fd >= 0) close(c->file_fd);
    free(c->hdr);
    free(c);
}

static void conn_close(http_conn_t *c)
{
    if (c->epfd >= 0) epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_unlink(c);
}

/* 响应全部发完: keep-alive 则复位复用, 否则返回 -1 让调用方关闭 */
static int conn_finish(http_conn_t *c)
{
    if (c->file_fd >= 0) { close(c->file_fd); c->file_fd = -1; }
    free(c->hdr);
    c->hdr = NULL;
    c->hdr_len = c->hdr_sent = 0;
    c->file_off = c->file_remain = 0;

    if (g_verbose)
        printf("[http] %s %s -> %d%s\n", c->method, c->target, c->status,
               c->keep_alive ? " (keep-alive)" : "");

    if (!c->keep_alive) return -1;

    c->state   = ST_READING;
    c->req_len = 0;
    c->head_only = 0;

    if (c->epfd < 0) return 0;           /* 阻塞模式: 无需重新注册 epoll */

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN | EPOLLET | EPOLLRDHUP;   /* MOD 会重新武装 ET */
    ev.data.fd = c->fd;
    if (epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) return -1;
    return 0;
}

/* 发送待发数据。返回 1=还有剩余(需 EPOLLOUT), 0=全部发完, -1=出错 */
static int conn_flush(http_conn_t *c)
{
    while (c->hdr_sent < c->hdr_len) {
        ssize_t n = send(c->fd, c->hdr + c->hdr_sent, c->hdr_len - c->hdr_sent, MSG_NOSIGNAL);
        if (n > 0) { c->hdr_sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        return -1;
    }
    while (c->file_fd >= 0 && c->file_remain > 0) {
        ssize_t n = sendfile(c->fd, c->file_fd, &c->file_off, (size_t)c->file_remain);
        if (n > 0) { c->file_remain -= n; continue; }
        if (n == 0) break;                                  /* 文件读完了 */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -1;
    }
    return 0;
}

static int conn_read(http_conn_t *c)
{
    for (;;) {
        if (c->req_len >= sizeof(c->req) - 1) return -2;    /* 请求头过大 */
        ssize_t n = recv(c->fd, c->req + c->req_len, sizeof(c->req) - 1 - c->req_len, 0);
        if (n > 0) {
            c->req_len += (size_t)n;
            c->req[c->req_len] = '\0';
            if (find_header_end(c->req, c->req_len, NULL)) return 1;
            continue;
        }
        if (n == 0)                                          /* 对端关闭 */
            return find_header_end(c->req, c->req_len, NULL) ? 1 : -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return find_header_end(c->req, c->req_len, NULL) ? 1 : 0;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/*  响应构造                                                           */
/* ------------------------------------------------------------------ */
static int resp_begin(http_conn_t *c, buf_t *b, int code, off_t clen,
                      const char *ctype, const char *extra)
{
    char date[64];
    http_date(time(NULL), date, sizeof(date));
    c->status = code;

    if (buf_printf(b, "HTTP/1.1 %d %s\r\n", code, status_text(code)) < 0) return -1;
    if (buf_printf(b, "Date: %s\r\n", date) < 0) return -1;
    if (buf_puts(b, "Server: mini-http/1.0\r\n") < 0) return -1;
    if (ctype && buf_printf(b, "Content-Type: %s\r\n", ctype) < 0) return -1;
    if (buf_printf(b, "Content-Length: %lld\r\n", (long long)clen) < 0) return -1;
    if (buf_printf(b, "Connection: %s\r\n", c->keep_alive ? "keep-alive" : "close") < 0) return -1;
    if (extra && buf_puts(b, extra) < 0) return -1;
    return buf_puts(b, "\r\n");
}

static void resp_commit(http_conn_t *c, buf_t *b, int file_fd)
{
    free(c->hdr);
    c->hdr      = b->p;
    c->hdr_len  = b->len;
    c->hdr_sent = 0;
    b->p = NULL; b->len = b->cap = 0;
    c->file_fd  = file_fd;
    c->state    = ST_SENDING;
}

/* 内存型响应(错误页 / 目录列表 / 重定向) */
static void resp_memory(http_conn_t *c, int code, const char *ctype,
                        const char *body, size_t blen, const char *extra)
{
    buf_t b = {0};
    if (resp_begin(c, &b, code, (off_t)blen, ctype, extra) < 0) { free(b.p); return; }
    if (!c->head_only && blen) buf_add(&b, body, blen);
    if (!b.p) { b.p = strdup(""); b.len = 0; b.cap = 0; }   /* 极端兜底 */
    resp_commit(c, &b, -1);
}

static void resp_error(http_conn_t *c, int code, const char *msg, const char *extra)
{
    char body[512];
    int  n = snprintf(body, sizeof(body),
                      "<!DOCTYPE html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                      "<title>%d %s</title></head>\n"
                      "<body style=\"font-family:system-ui,sans-serif;margin:3rem\">"
                      "<h1>%d %s</h1><hr><p>mini-http</p></body></html>\n",
                      code, msg, code, msg);
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(body)) n = (int)sizeof(body);
    resp_memory(c, code, "text/html; charset=utf-8", body, (size_t)n, extra);
}

static void human_size(off_t sz, char *out, size_t outsz)
{
    static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)sz;
    int    i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, outsz, "%lld B", (long long)sz);
    else        snprintf(out, outsz, "%.1f %s", v, u[i]);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void resp_dir_listing(http_conn_t *c, const char *fs_path, const char *rel)
{
    DIR *d = opendir(fs_path);
    if (!d) { resp_error(c, 403, "Forbidden", NULL); return; }

    char  **names = NULL;
    size_t  n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 32;
            char **nn = realloc(names, nc * sizeof(*nn));
            if (!nn) break;
            names = nn; cap = nc;
        }
        if ((names[n] = strdup(de->d_name)) == NULL) break;
        n++;
    }
    closedir(d);
    if (n > 1) qsort(names, n, sizeof(*names), cmp_str);

    char rel_esc[2048];
    html_escape(rel, rel_esc, sizeof(rel_esc));

    buf_t b = {0};
    buf_puts(&b, "<!DOCTYPE html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">");
    buf_puts(&b, "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    buf_printf(&b, "<title>Index of %s</title>", rel_esc);
    buf_puts(&b, "<style>body{font-family:system-ui,-apple-system,'PingFang SC',sans-serif;"
                 "margin:2rem auto;max-width:60rem;padding:0 1rem}"
                 "h1{font-size:1.15rem}table{border-collapse:collapse;width:100%}"
                 "td{padding:.35rem .5rem;border-bottom:1px solid #eee}"
                 "td.size{text-align:right;color:#666;white-space:nowrap}"
                 "a{color:#0366d6;text-decoration:none}a:hover{text-decoration:underline}"
                 "</style></head><body>");
    buf_printf(&b, "<h1>Index of %s</h1><table>", rel_esc);
    if (strcmp(rel, "/") != 0)
        buf_puts(&b, "<tr><td><a href=\"../\">../</a></td><td class=\"size\">-</td></tr>");

    for (size_t i = 0; i < n; ++i) {
        char full[PATH_MAX], disp[1024], href[4096], size_s[32];
        snprintf(full, sizeof(full), "%s/%s", fs_path, names[i]);
        struct stat st;
        int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        if (isdir || stat(full, &st) != 0) snprintf(size_s, sizeof(size_s), "-");
        else                               human_size(st.st_size, size_s, sizeof(size_s));
        html_escape(names[i], disp, sizeof(disp));
        url_encode(names[i], href, sizeof(href));
        buf_printf(&b, "<tr><td><a href=\"%s%s\">%s%s</a></td><td class=\"size\">%s</td></tr>\n",
                   href, isdir ? "/" : "", disp, isdir ? "/" : "", size_s);
    }
    buf_puts(&b, "</table></body></html>\n");

    resp_memory(c, 200, "text/html; charset=utf-8", b.p, b.len, NULL);

    free(b.p);
    for (size_t i = 0; i < n; ++i) free(names[i]);
    free(names);
}

static void serve_file(http_conn_t *c, const char *fs_path,
                       const struct stat *st, const char *range)
{
    int fd = open(fs_path, O_RDONLY);
    if (fd < 0) { resp_error(c, 404, "Not Found", NULL); return; }

    off_t total = st->st_size;
    off_t start = 0, len = total;
    int   partial = 0;
    char  content_range[128] = {0};

    if (range && strncasecmp(range, "bytes=", 6) == 0 &&
        strchr(range + 6, ',') == NULL && total > 0) {
        const char *r    = range + 6;
        const char *dash = strchr(r, '-');
        long long   s = -1, e = -1;
        if (dash == r) {                       /* bytes=-N 最后 N 字节 */
            if (dash[1]) {
                long long k = atoll(dash + 1);
                if (k > 0) { s = total - k; e = total - 1; if (s < 0) s = 0; }
            }
        } else if (dash) {
            s = atoll(r);
            e = dash[1] ? atoll(dash + 1) : total - 1;
        }
        if (s >= 0 && e >= s && s < total) {
            if (e >= total) e = total - 1;
            start   = s;
            len     = e - s + 1;
            partial = 1;
            snprintf(content_range, sizeof(content_range),
                     "Content-Range: bytes %lld-%lld/%lld\r\n",
                     (long long)start, (long long)(start + len - 1), (long long)total);
        } else if (dash) {                     /* 区间非法 */
            char extra[160];
            snprintf(extra, sizeof(extra), "Content-Range: bytes */%lld\r\n", (long long)total);
            close(fd);
            resp_error(c, 416, "Range Not Satisfiable", extra);
            return;
        }
    }

    char lastmod[64];
    http_date(st->st_mtime, lastmod, sizeof(lastmod));

    char extra[352];
    if (partial)
        snprintf(extra, sizeof(extra),
                 "Accept-Ranges: bytes\r\nLast-Modified: %s\r\n%s", lastmod, content_range);
    else
        snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\nLast-Modified: %s\r\n", lastmod);

    buf_t b = {0};
    if (resp_begin(c, &b, partial ? 206 : 200, len, mime_type(fs_path), extra) < 0) {
        close(fd);
        free(b.p);
        return;
    }
    c->file_off    = start;
    c->file_remain = c->head_only ? 0 : len;    /* HEAD: 只发头 */
    resp_commit(c, &b, fd);
}

/* ------------------------------------------------------------------ */
/*  请求处理                                                           */
/* ------------------------------------------------------------------ */
static void process_request(http_conn_t *c)
{
    char method[16] = {0}, target[2048] = {0}, version[16] = {0};

    if (sscanf(c->req, "%15s %2047s %15s", method, target, version) != 3) {
        c->keep_alive = 0;
        resp_error(c, 400, "Bad Request", NULL);
        return;
    }
    snprintf(c->method, sizeof(c->method), "%s", method);
    snprintf(c->target, sizeof(c->target), "%.*s",
             (int)sizeof(c->target) - 1, target);

    int is_get  = (strcmp(method, "GET")  == 0);
    int is_head = (strcmp(method, "HEAD") == 0);
    c->head_only = is_head;

    /* keep-alive: HTTP/1.1 默认保持, 1.0 默认关闭 */
    int  keep = (strcmp(version, "HTTP/1.1") == 0);
    char conn_hdr[64];
    if (header_get(c->req, "Connection", conn_hdr, sizeof(conn_hdr))) {
        if      (strcasecmp(conn_hdr, "close") == 0)      keep = 0;
        else if (strcasecmp(conn_hdr, "keep-alive") == 0) keep = 1;
    }
    int has_body = 0;
    find_header_end(c->req, c->req_len, &has_body);
    if (has_body) keep = 0;     /* 有请求体/流水线: 本次响应后关闭, 避免解析错位 */
    c->keep_alive = keep;

    if (!is_get && !is_head) {
        c->keep_alive = 0;
        resp_error(c, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n");
        return;
    }

    char *q  = strchr(target, '?');
    if (q) *q = '\0';
    char *fr = strchr(target, '#');
    if (fr) *fr = '\0';

    if (target[0] != '/') {
        c->keep_alive = 0;
        resp_error(c, 400, "Bad Request", NULL);
        return;
    }
    size_t tlen     = strlen(target);
    int    want_dir = (tlen > 0 && target[tlen - 1] == '/');

    char decoded[2048];
    if (url_decode(target, decoded, sizeof(decoded)) < 0) {
        c->keep_alive = 0;
        resp_error(c, 400, "Bad Request", NULL);
        return;
    }
    char rel[2048];
    if (sanitize_rel(decoded, rel, sizeof(rel)) < 0) {
        c->keep_alive = 0;
        resp_error(c, 403, "Forbidden", NULL);
        return;
    }

    char fspath[PATH_MAX];
    if (snprintf(fspath, sizeof(fspath), "%s%s", g_root, rel) >= (int)sizeof(fspath)) {
        c->keep_alive = 0;
        resp_error(c, 414, "URI Too Long", NULL);
        return;
    }

    /* realpath 消除符号链接, 防止越出根目录 */
    char real[PATH_MAX];
    struct stat st;
    if (realpath(fspath, real) == NULL || stat(real, &st) < 0) {
        resp_error(c, 404, "Not Found", NULL);
        return;
    }
    if (!(g_root_len == 1 ||
          (strncmp(real, g_root, g_root_len) == 0 &&
           (real[g_root_len] == '/' || real[g_root_len] == '\0')))) {
        resp_error(c, 403, "Forbidden", NULL);
        return;
    }

    char range[128];
    int  has_range = header_get(c->req, "Range", range, sizeof(range));

    if (S_ISDIR(st.st_mode)) {
        if (!want_dir) {                    /* 补上结尾 '/', 保证相对链接正确 */
            char extra[320];
            snprintf(extra, sizeof(extra), "Location: %s/\r\n", c->target);
            const char *body = "<!DOCTYPE html><html><body>301 Moved Permanently</body></html>";
            resp_memory(c, 301, "text/html; charset=utf-8", body, strlen(body), extra);
            return;
        }
        char idx[PATH_MAX + 16];
        const char *sep = (real[strlen(real) - 1] == '/') ? "" : "/";
        snprintf(idx, sizeof(idx), "%s%sindex.html", real, sep);
        struct stat ist;
        if (stat(idx, &ist) == 0 && S_ISREG(ist.st_mode)) {
            serve_file(c, idx, &ist, has_range ? range : NULL);
            return;
        }
        snprintf(idx, sizeof(idx), "%s%sindex.htm", real, sep);
        if (stat(idx, &ist) == 0 && S_ISREG(ist.st_mode)) {
            serve_file(c, idx, &ist, has_range ? range : NULL);
            return;
        }
        resp_dir_listing(c, real, rel);
        return;
    }

    if (S_ISREG(st.st_mode)) {
        serve_file(c, real, &st, has_range ? range : NULL);
        return;
    }

    resp_error(c, 403, "Forbidden", NULL);
}

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */
int http_init(const char *root)
{
    /* 输出重定向到文件时也能实时看到日志 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    char cwd[PATH_MAX];
    if (!root || !*root) {
        if (!getcwd(cwd, sizeof(cwd))) { perror("http_init: getcwd"); return -1; }
        root = cwd;
    }
    char real[PATH_MAX];
    if (!realpath(root, real)) { perror("http_init: realpath"); return -1; }

    size_t n = strlen(real);
    while (n > 1 && real[n - 1] == '/') real[--n] = '\0';

    struct stat st;
    if (stat(real, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "http_init: %s 不是目录\n", real);
        return -1;
    }
    snprintf(g_root, sizeof(g_root), "%s", real);
    g_root_len = strlen(g_root);
    return 0;
}

void http_set_verbose(int on) { g_verbose = on ? 1 : 0; }

const char *http_root(void) { return g_root_len ? g_root : NULL; }

static http_conn_t *conn_get(int fd)
{
    if (fd < 0) return NULL;

    pthread_mutex_lock(&g_conn_lock);

    if (fd >= g_conn_cap) {
        int ncap = g_conn_cap ? g_conn_cap : 256;
        while (ncap <= fd) ncap *= 2;
        http_conn_t **nt = realloc(g_conn, (size_t)ncap * sizeof(*nt));
        if (!nt) { pthread_mutex_unlock(&g_conn_lock); return NULL; }
        memset(nt + g_conn_cap, 0, (size_t)(ncap - g_conn_cap) * sizeof(*nt));
        g_conn = nt;
        g_conn_cap = ncap;
    }
    http_conn_t *c = g_conn[fd];
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c) { pthread_mutex_unlock(&g_conn_lock); return NULL; }
        c->fd      = fd;
        c->file_fd = -1;
        c->state   = ST_READING;
        g_conn[fd] = c;
    }

    pthread_mutex_unlock(&g_conn_lock);
    return c;
}

int http_handle(int epfd, int fd, uint32_t events)
{
    if (g_root_len == 0 && http_init(NULL) < 0) return HTTP_ERR;

    http_conn_t *c = conn_get(fd);
    if (!c) return HTTP_ERR;
    c->epfd = epfd;

    if (events & (EPOLLHUP | EPOLLERR)) { conn_close(c); return HTTP_ERR; }

    /* 1. 上次没发完, 继续发 */
    if (c->state == ST_SENDING) {
        int r = conn_flush(c);
        if (r < 0) { conn_close(c); return HTTP_ERR; }
        if (r == 1) { conn_want_write(c); return HTTP_OK; }
        if (conn_finish(c) < 0) { conn_close(c); return HTTP_ERR; }
        if (!(events & EPOLLIN)) return HTTP_OK;
    }

    /* 2. 读请求 */
    int r = conn_read(c);
    if (r == -2) {                       /* 请求头过大 */
        c->keep_alive = 0;
        snprintf(c->method, sizeof(c->method), "-");
        snprintf(c->target, sizeof(c->target), "-");
        c->req_len = 0;
        resp_error(c, 413, "Content Too Large", NULL);
    } else if (r < 0) {
        conn_close(c);
        return HTTP_ERR;
    } else if (r == 0) {
        return HTTP_OK;                  /* 还没收全, 继续等下一次 EPOLLIN */
    } else {
        process_request(c);
    }

    /* 3. 立刻尝试发送, 发不完就等 EPOLLOUT */
    int f = conn_flush(c);
    if (f < 0) { conn_close(c); return HTTP_ERR; }
    if (f == 1) { conn_want_write(c); return HTTP_OK; }
    if (conn_finish(c) < 0) { conn_close(c); return HTTP_ERR; }
    return HTTP_OK;
}

/* ------------------------------------------------------------------ */
/*  阻塞式整连接处理 —— 供线程池使用                                   */
/* ------------------------------------------------------------------ */
/* 在"一个连接交给一个线程"的模型下不需要 epoll: 工作线程在这个 fd 上
 * 把整条连接(含 keep-alive 的多个请求)处理完再返回。
 *
 *   - fd 应当处于阻塞模式(accept 拿到的默认就是阻塞);
 *   - 建议调用方设置 SO_RCVTIMEO, 否则空闲的 keep-alive 连接会一直占住线程;
 *   - 本函数不关闭 fd, 由调用方负责 close()。
 * 返回 0 表示处理已结束(无论成功与否), 调用方 close(fd) 即可。
 */
int http_serve_connection(int fd)
{
    if (g_root_len == 0 && http_init(NULL) < 0) return -1;

    http_conn_t *c = conn_get(fd);
    if (!c) return -1;
    c->epfd = -1;                       /* -1 = 阻塞模式, 不走 epoll */

    for (;;) {
        int r = conn_read(c);
        if (r == -2) {                  /* 请求头过大 */
            c->keep_alive = 0;
            snprintf(c->method, sizeof(c->method), "-");
            snprintf(c->target, sizeof(c->target), "-");
            c->req_len = 0;
            resp_error(c, 413, "Content Too Large", NULL);
        } else if (r < 0) {
            break;                      /* 对端关闭或出错 */
        } else if (r == 0) {
            break;                      /* 阻塞模式下只可能是读超时 */
        } else {
            process_request(c);
        }

        if (conn_flush(c) < 0) break;   /* 发送失败 */
        if (conn_finish(c) < 0) break;  /* 非 keep-alive: 收工 */
    }

    conn_unlink(c);                     /* 只释放状态, 不关 fd */
    return 0;
}
