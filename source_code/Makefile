# =====================================================================
#  混合构建: C 源码用 gcc, C++ 源码用 g++, 最终用 g++ 链接
#
#  本文件与源码同在 source_code/ 目录下, 用法:
#      cd source_code
#      make            # 产出 source_code/s   (HTTP 服务器)
#      make client     # 产出 source_code/c   (TCP 测试客户端)
#      make clean      # 清理 .o 与可执行文件
#
#  按扩展名分工, 不需要任何特殊开关:
#      C   源码: main.c  server.c  http.c  client.c
#      C++ 源码: TaskQueue.cpp  threadpool.cpp  pool_server.cpp
#
#  注意: 不要把全部源码交给一条 g++ 命令 —— g++ 会把 .c 当 C++ 编译,
#  C 代码里 realloc/calloc 的隐式 void* 转换在 C++ 中是非法的,
#  http.c 顶部的 _GNU_SOURCE 也会与 g++ 命令行预定义的重定义冲突。
# =====================================================================

CC       = gcc
CXX      = g++
CFLAGS   = -Wall -O2
CXXFLAGS = -std=c++11 -Wall -O2
LDFLAGS  = -pthread

C_OBJS   = main.o server.o http.o
CXX_OBJS = TaskQueue.o threadpool.o pool_server.o
TARGET   = s

HEADERS  = $(wildcard *.h)

all: $(TARGET)

# 头文件一变, 所有目标文件都重编(小项目最省心的做法)
$(C_OBJS) $(CXX_OBJS): $(HEADERS)

$(TARGET): $(C_OBJS) $(CXX_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

# --- C 源码 ---
$(C_OBJS): %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- C++ 源码 ---
$(CXX_OBJS): %.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 顺带构建 TCP 测试客户端(纯 C)
client: client.c
	$(CC) $(CFLAGS) -o c $<

clean:
	rm -f $(C_OBJS) $(CXX_OBJS) $(TARGET) c

.PHONY: all clean client
