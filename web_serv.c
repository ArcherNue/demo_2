#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/tyoes.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include "pub.h"

#define EPOLLSIZE 2048

//回复请求的头
//void send_header(文件描述符, 状态码, 消息, 文件类型, 大小)
void send_header(int cfd, int code, char* MSG, char* filetype, int lenght)
{
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    int len = sprintf(buf, "HTTP/1.1 %d %s\r\n", code, MSG);
    send(cfd, buf, len, 0);
    len = sprintf(buf, "Content-Type:%s\r\n", filetype);
    send(cfd, buf, len, 0);

    if(lenght > 0)
    {
        len = sprintf(buf, "Content-Lenght:%d\r\n", lenght);
        send(cfd, buf, len, 0);
    }
    send(cfd, "\r\n", 2, 0);

}

// 回复请求的文件
//void send_file(文件描述符, 文件名, 树的文件描述符, 时间监听结构体, 是否断开连接)
void send_file(int cfd, char* filename, int epfd, struct epoll_event* ev, int flag)
{
    int fd = open(filename, O_RDWR);
    if(fd < 0)return;
    char buf[1024];
    while (1)
    {
        //读取文件内容
        int res = read(fd, buf, sizeof(buf));
        if(res == 0)break;
        //发送给浏览器
        send(cfd, buf, res, 0);
    }
    close(fd);
    //是否断开连接
    if(flag)
    {
        close(cfd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, ev);
    }
}

//函数：业务处理 do_work
void do_work(int cfd, char* filename, int epfd, struct epoll_event* ev, int flag)
{
    char buf[1024];
    char temp[1024];
    int res = read(cfd, buf, sizeof(buf));

    //将剩余数据读完
    while((res = read(cfd, temp, sizeof(temp)) >0));

    //测试打印数据
    //Write(STDOUT_FILENO, buf, res);
    //请求方式  请求文件
    char method[256];
    char data[256];
    memset(method, 0, sizeof(method));
    memset(data, 0, sizeof(data));

    //格式化获取字符串内容
    sscanf(buf, "%[^ ] %[^ ]", method, data);

    //判断客户端请求
    if(!strcasecmp(method, "GET"))
    {
        //1、 data 数据为（/）：/   index.html
        char* strfile = data + 1;
        //将url的编码格式转成linux编码格式  解决中文乱码
        strdecode(strfile, strfile);
        if(*strfile == "\0")
        {
            strfile = "index.html";
        }
        //获取文件状态
        struct stat st;
        int ret = stat(strfile, &st);
        if(ret == -1)
        {
            struct stat errst;
            stat("error.html", &errst);
            // 回复 404 NO FOUND
            send_header(cfd, 404, "NO FOUND", get_mime_type("error.html"), errst.st_size);
            // 回复 error.html
            send_file(cfd, "error.html", epfd, ev, 1);
        }
        //文件存在
        else
        {
            if(S_ISREG(st.st_mode))
            {
                //2、 data 数据为（文件）：/xxxx.html
                //回复 200 OK
                send_header(cfd, 200, "OK", get_min_type(strfile), st.st_size);
                //回复 具体文件
                send_file(cfd, strfile, epfd, ev, 1);

            }
            else if(S_ISDIR(st.st_mode))
            {
                //3、 data 数据为（目录）：/pic
                 /*
                  1、打开目录  opendir
                  2、读取目录内容 readdir  d_type  文件类型 d_name  文件名
                  3、将dir_header.html发送给浏览器
                  4、组装成网页文件列表格式 发送给浏览器
                   5、将dir_tail.html发送给浏览器
                */
                //发送HTPP协议报文
                send_header(cfd, 200, "OK", get_mime_type("*.html"), 0);
                DIR* dir = opendir(strfile);
                if(dir = NULL)
                {
                    perr_exit("opendir error");
                }
                //发送网页头部
                send_file(cfd, "dir_header.html", epfd, ev, 0);

                struct dirent* d;
                char buf[1024];
                int len;
                while(1)
                {
                    //循环读取目录内容
                    d = readdir(dir);
                    if(d == NULL)break;

                    if(d->d_type == 8)
                    {
                        len = sprintf(buf, "<li><a href ='%s'>%s</a></li>", d->d_name, d->d_name);
                        send(cfd, buf, len, 0);
                    }
                    else if(d->d_type == 4)
                    {
                        len = sprintf(buf, "<li><a href='%s/'>%s</a></li>",d->d_name, d->d_name);
                        send(cfd, buf, len, 0);
                    }
                }
                //发送网页尾部
                send_file(cfd, "dir_tail.html", epfd, ev, 1);
                //关闭打开目录
                closedir(dir);
            }
        }
    }
    else
    {
        //其他情况 暂缓处理
        close(cfd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, ev);
    }

}

int main(int argc, char* argv[])
{
    chdir("/home/anolgame/......");

   //1、创建lfd文件描述符 struct sockaddr结构体 绑定 监听
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(1234);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(lfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(lfd, 128);

    //1.1 监听事件的返回集合
    struct epoll_event eparr[EPOLLSIZE];

    //2、创建epoll树的文件描述符 epoll_create
    int epfd = epoll_create(EPOLLSIZE);

    //2.1 定义事件结构体
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;

    //3、将lfd添加上树
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    //4、循环监听事件
    while (1) {

        //4.1 阻塞监听事件触发
        int ready = epoll_wait(epfd, eparr, EPOLLSIZE, -1);

        //4.2 循环处理事件
        for(int i = 0; i < ready; i++)
        {
            //4.2.1 判断是否是连接请求
            if(eparr[i].data.fd == lfd && eparr[i].events & EPOLLIN)
            {
                //4.2.1.1 创建链接
                struct sockaddr_in clie_addr;
                socklen_t clie_len = sizeof(clie_addr);
                int cfd = accpet(lfd, (struct sockaddr*)&clie_addr, &clie_len);

                //4.2.1.2 设置cfd的边缘触发
                int flag = fcntl(cfd, F_GETEL);

                //4.2.1.3 设置为非阻塞
                flag |= O_NONBLOCK;
                fcntl(cfd, F_SETEL, flag);

                //4.2.1.4 将cfd设置事件并上树
                ev.data.fd = cfd;
                ev.events = EPOLLN | EPOLLET;

                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

            }
            else
            {
                //4.2.2 业务处理
                do_work(epfd, eparr[i].data.fd, &eparr[i]);
            }

        }
    }

    return 0;
}
