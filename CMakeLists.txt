# =====================================================================
#  mini-http —— CMake 构建脚本
#
#  用法(推荐 out-of-source, 不污染源码目录):
#      cmake -S . -B build              # 配置
#      cmake --build build -j           # 编译, 产出 build/s 与 build/c
#      cmake --build build --target s   # 只编译 HTTP 服务器
#      cmake --build build --target c   # 只编译 TCP 测试客户端
#
#  与 source_code/Makefile 完全等价:
#      C   源码(main.c server.c http.c client.c) 交给 gcc
#      C++ 源码(TaskQueue/threadpool/pool_server/logger) 交给 g++
#      最终用 C++ 链接器链接(C++ 运行时 + -pthread)
#
#  注意: 不要把全部源码塞进一个 target 用同一种语言编译 —— g++ 会把 .c
#  当 C++ 编译, C 代码里 realloc/calloc 的隐式 void* 转换在 C++ 中非法。
#  CMake 按扩展名自动分派编译器, 正好避免这个问题。
# =====================================================================

cmake_minimum_required(VERSION 3.10)

project(mini-http LANGUAGES C CXX)

# 缺省 Release(-O2/-O3); 需要调试: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HTTP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source_code)

find_package(Threads REQUIRED)

# ---------- HTTP 服务器: C 与 C++ 混编, 由 C++ 链接器收尾 ----------
add_executable(s
    ${HTTP_SRC}/main.c          # C   入口: 解析参数、选择运行模型
    ${HTTP_SRC}/server.c        # C   epoll 网络层
    ${HTTP_SRC}/http.c          # C   HTTP 协议层
    ${HTTP_SRC}/TaskQueue.cpp   # C++ 任务队列
    ${HTTP_SRC}/threadpool.cpp  # C++ 线程池
    ${HTTP_SRC}/pool_server.cpp # C++ 线程池接入层
    ${HTTP_SRC}/logger.cpp      # C++ 日志系统
)
target_include_directories(s PRIVATE ${HTTP_SRC})
target_compile_options(s PRIVATE -Wall)
target_link_libraries(s PRIVATE Threads::Threads)

# ---------- TCP 测试客户端: 纯 C, 与 server 无关 ----------
add_executable(c ${HTTP_SRC}/client.c)
target_include_directories(c PRIVATE ${HTTP_SRC})
target_compile_options(c PRIVATE -Wall)
