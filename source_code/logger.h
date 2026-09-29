#ifndef LOGGER_H
#define LOGGER_H

/* =====================================================================
 *  简单日志系统
 *
 *  - 单例 Logger, 内部用 std::mutex 保护两个 std::ofstream;
 *  - 两个文件: 访问日志(info) 与 错误日志(warn / error);
 *  - 每行格式:  2026-09-29 10:23:45.123 [INFO] [140234567890] 消息
 *                          └── 时间 ──┘  └级别┘ └── 线程 id ──┘
 *
 *  C++ 用法:
 *      Logger::instance().open("access.log", "error.log");
 *      Logger::instance().info("accept fd=5");
 *
 *  C 用法(http.c / server.c 是 C 代码, 只能用下面这组包装):
 *      log_init("access.log", "error.log");
 *      log_info("accept fd=%d", cfd);
 *      log_warn("响应 %d %s", 404, "Not Found");
 *      log_error("发送失败: %s", strerror(errno));
 * ===================================================================== */

/* ---------------------------------------------------------------------
 *  第一部分: C 代码也能用的接口(printf 风格)
 * ------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* 打开日志文件; 传 NULL 表示用默认路径(access.log / error.log, 位于进程 cwd)。
 * 返回 0 成功, -1 失败。也可以完全不调用: 首次写日志时会自动用默认路径打开。 */
int  log_init(const char *accessPath, const char *errorPath);

void log_info (const char *fmt, ...);   /* 写访问日志 */
void log_warn (const char *fmt, ...);   /* 写错误日志 */
void log_error(const char *fmt, ...);   /* 写错误日志 */

/* 冲刷并关闭两个文件(进程退出前调用, 保证日志落盘) */
void log_close(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* ---------------------------------------------------------------------
 *  第二部分: C++ 类(只有 C++ 源文件可见)
 * ------------------------------------------------------------------- */
#ifdef __cplusplus

#include <fstream>
#include <mutex>
#include <string>

class Logger
{
public:
    /* 单例。C++11 起"函数内 static 局部变量"的初始化是线程安全的,
     * 所以这里不需要双检锁(DCLP)那套写法。 */
    static Logger &instance();

    /* 单例不允许复制 */
    Logger(const Logger &)            = delete;
    Logger &operator=(const Logger &) = delete;

    /* 打开日志文件(追加模式)。返回 false 表示至少有一个打不开。 */
    bool open(const std::string &accessPath = "access.log",
              const std::string &errorPath  = "error.log");
    void close();

    /* 三个级别: info 写访问日志, warn / error 写错误日志 */
    void info (const std::string &msg);
    void warn (const std::string &msg);
    void error(const std::string &msg);

    /* printf 风格, 便于带变量(内部先格式化再交给上面三个) */
    void infof (const char *fmt, ...);
    void warnf (const char *fmt, ...);
    void errorf(const char *fmt, ...);

private:
    Logger()  = default;
    ~Logger();

    enum class Level { INFO = 0, WARN, ERROR };

    void           write(Level lv, const std::string &msg);
    std::string    formatLine(Level lv, const std::string &msg) const;
    std::ofstream &streamFor(Level lv);

    std::mutex    m_mutex;                       /* 保护下面两个 ofstream */
    std::ofstream m_access;                      /* 访问日志 */
    std::ofstream m_error;                       /* 错误日志 */
    bool          m_opened = false;
    std::string   m_accessPath{"access.log"};
    std::string   m_errorPath {"error.log"};
};

#endif /* __cplusplus */

#endif /* LOGGER_H */
