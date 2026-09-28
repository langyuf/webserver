#include <arpa/inet.h>  /* inet_pton / sockaddr_in */
#include <errno.h>      /* errno */
#include <stdio.h>      /* printf / perror */
#include <stdlib.h>     /* exit */
#include <string.h>     /* memset */
#include <unistd.h>     /* close / sleep */

#define SERVER_IP "192.168.19.128" 
#define SERVER_PORT 8989 
#define BUF_SIZE 1024

int main()
{
    // 1. 创建通信的套接字
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if(cfd == -1){
        perror("socket");
        exit(0);
    }
    
	// 2. 设置服务器地址
    struct sockaddr_in addr; 
    memset(&addr, 0, sizeof(addr)); 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(SERVER_PORT); 
    // 将字符串 IP 转换成网络字节序 
    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) <= 0) 
    { 
        perror("inet_pton"); 
        close(cfd); 
        exit(EXIT_FAILURE); 
    }
    
    // 3. 连接服务器 
    if (connect(cfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) 
    { 
        perror("connect");
        close(cfd); 
        exit(EXIT_FAILURE); 
    }
    
    printf("连接服务器成功: %s:%d\n", SERVER_IP, SERVER_PORT);
    
    // 4. 通信
    int num = 0;
    while(1){
        // 发送数据
        char buf[BUF_SIZE];
        // 用 snprintf 而不是 sprintf: 它最多写 sizeof(buf) 字节, 只会截断不会溢出。
        // 返回值是"本来想写多少字节", 所以 (size_t)ret >= sizeof(buf) 能真正检测到截断;
        // sprintf 做不到这点 —— 溢出在返回值可用之前就已经发生, 后面那句检查是死代码。
        int ret = snprintf(buf, sizeof(buf), "hello, world, %d,......", num++);
        if (ret < 0) { perror("snprintf"); break; }
        if ((size_t)ret >= sizeof(buf)) { fprintf(stderr, "发送数据过长\n"); break; }
        //fgets(buf,sizeof(buf),stdin); 客户端手动输入
        send(cfd, buf, strlen(buf)+1, 0);
        
        // 接收数据
        memset(buf, 0, sizeof(buf));
        int len = recv(cfd, buf, sizeof(buf), 0);
        if(len == 0){
            printf("服务器已经断开了连接...\n");
            break;
        }else if (len > 0){
            printf("recv buf: %s\n", buf);
        }else{
            perror("recv");
            break;
        }
        sleep(1);

    }

    // 4. 断开连接
    close(cfd);
}