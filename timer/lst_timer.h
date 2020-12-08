#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

/*
* 将连接资源、定时事件和超时时间封装为定时器类
* 连接资源包括客户端套接字地址、文件描述符和定时器
* 定时事件为回调函数，实现删除非活动socket上的注册事件，并关闭连接
* 定时器超时时间 = 浏览器和服务器连接时刻 + 固定时间(TIMESLOT)
* 
* 
* 
* 
* 
*/

class util_timer;

//连接资源
struct client_data
{
    sockaddr_in address; //客户端socket地址
    int sockfd;         //socket文件描述符
    util_timer *timer; //定时器
};

//定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire; //超时时间
    void (* cb_func)(client_data *); //回调函数，超时调用
    client_data *user_data; //连接资源
    util_timer *prev; //前向定时器
    util_timer *next; //后继定时器
};

/*定时器容器类
* 带头尾结点的升序双向链表，为每个连接创建一个定时器，并添加到链表中，按照超时时间升序排列，执行定时任务时，将超时的定时器从链表中删除。
* 升序双向链表主要操作：
* 1.创建头尾节点，其中头尾节点没有意义，仅仅统一方便调整
* 2.add_timer函数，将目标定时器添加到链表中，添加时按照升序添加
* 3.adjust_timer函数，当定时任务发生变化,调整对应定时器在链表中的位置
* 4.del_timer函数将超时的定时器从链表中删除
* 
*/
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
