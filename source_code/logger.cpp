/* =====================================================================
 *  logger.cpp —— 简单日志系统的实现
 *
 *  设计要点:
 *    1) 单例: 用函数内 static 局部变量实现, C++11 保证初始化线程安全;
 *    2) 用一把 std::mutex 保护两个 std::ofstream, 保证多线程写不串行化;
 *    3) 写文件失败(磁盘满/无权限)只静静跳过, 绝不因为日志把服务器搞崩;
 *    4) 每行都 flush, 便于 tail -f 实时观察; 代价是高频写盘会慢,
 *       生产环境可以改成攒够一定条数再 flush。
 * ===================================================================== */
#include "logger.h"

#include <chrono>     /* std::chrono::system_clock / milliseconds */
#include <cstdarg>    /* va_list / va_start / va_end */
#include <cstdio>     /* std::vsnprintf */
#include <ctime>      /* std::localtime_r / std::tm */
#include <iomanip>    /* std::put_time / setw / setfill */
#include <sstream>    /* std::ostringstream */
#include <thread>     /* std::this_thread::get_id */

namespace {

/* 把 printf 风格的参数格式化成 std::string。
 * 先用 vsnprintf(nullptr, 0, ...) 试探长度, 再按需分配 —— 不设固定上限。 */
std::string vformat(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);

    std::string out;
    int n = std::vsnprintf(nullptr, 0, fmt, ap);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        std::vsnprintf(&out[0], static_cast<size_t>(n) + 1, fmt, ap2);
    }
    va_end(ap2);
    return out;
}

}  // namespace

/* ------------------------------------------------------------------ */
/*  单例                                                               */
/* ------------------------------------------------------------------ */
Logger &Logger::instance()
{
    /* 函数内 static: 首次调用时构造, 进程退出时析构。
     * C++11 起标准保证这一步是线程安全的(编译器会插入一次性同步)。 */
    static Logger s_instance;
    return s_instance;
}

Logger::~Logger()
{
    /* 正常路径: main 里已经调过 log_close()。
     * 这里兜底冲刷一次, 防止还有没落盘的内容。 */
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_access.is_open()) m_access.close();
    if (m_error.is_open())  m_error.close();
}

/* ------------------------------------------------------------------ */
/*  打开 / 关闭                                                        */
/* ------------------------------------------------------------------ */
bool Logger::open(const std::string &accessPath, const std::string &errorPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_access.is_open()) m_access.close();
    if (m_error.is_open())  m_error.close();

    m_accessPath = accessPath;
    m_errorPath  = errorPath;

    m_access.open(accessPath, std::ios::app);   /* 追加: 重启不覆盖旧日志 */
    m_error .open(errorPath,  std::ios::app);
    m_opened = true;

    return m_access.is_open() && m_error.is_open();
}

void Logger::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_access.is_open()) { m_access.flush(); m_access.close(); }
    if (m_error .is_open()) { m_error .flush(); m_error .close(); }
    m_opened = false;
}

/* ------------------------------------------------------------------ */
/*  格式化与写入                                                       */
/* ------------------------------------------------------------------ */
std::string Logger::formatLine(Level lv, const std::string &msg) const
{
    static const char *names[] = { "INFO", "WARN", "ERROR" };

    /* 时间: 本地时间 + 毫秒 */
    auto now = std::chrono::system_clock::now();
    auto sec = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;

    std::tm tm{};
    localtime_r(&sec, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms
        << " ["   << names[static_cast<int>(lv)] << ']'
        << " ["   << std::this_thread::get_id() << ']'
        << ' '    << msg;

    return oss.str();
}

std::ofstream &Logger::streamFor(Level lv)
{
    return (lv == Level::INFO) ? m_access : m_error;
}

void Logger::write(Level lv, const std::string &msg)
{
    std::string line = formatLine(lv, msg);   /* 先在锁外把字符串拼好 */

    std::lock_guard<std::mutex> lock(m_mutex);

    /* 懒打开: 调用方忘了 log_init() 也能用, 保持"日志不该拖累主流程" */
    if (!m_opened) {
        m_access.open(m_accessPath, std::ios::app);
        m_error .open(m_errorPath,  std::ios::app);
        m_opened = true;
    }

    std::ofstream &os = streamFor(lv);
    if (!os.is_open()) return;                /* 打不开就算了, 不能崩 */

    os << line << std::endl;                  /* endl: 立刻 flush */
}

void Logger::info (const std::string &msg) { write(Level::INFO,  msg); }
void Logger::warn (const std::string &msg) { write(Level::WARN,  msg); }
void Logger::error(const std::string &msg) { write(Level::ERROR, msg); }

/* ------------------------------------------------------------------ */
/*  printf 风格                                                        */
/* ------------------------------------------------------------------ */
void Logger::infof(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    info(s);
}

void Logger::warnf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    warn(s);
}

void Logger::errorf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    error(s);
}

/* ------------------------------------------------------------------ */
/*  给 C 代码用的包装                                                  */
/* ------------------------------------------------------------------ */
extern "C" int log_init(const char *accessPath, const char *errorPath)
{
    return Logger::instance().open(accessPath ? accessPath : "access.log",
                                   errorPath  ? errorPath  : "error.log") ? 0 : -1;
}

extern "C" void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    Logger::instance().info(vformat(fmt, ap));
    va_end(ap);
}

extern "C" void log_warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    Logger::instance().warn(vformat(fmt, ap));
    va_end(ap);
}

extern "C" void log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    Logger::instance().error(vformat(fmt, ap));
    va_end(ap);
}

extern "C" void log_close(void)
{
    Logger::instance().close();
}
