#include "lst_timer.h"
#include "../http/http_conn.h"

/* 基础知识
* 非活跃，是指客户端（这里是浏览器）与服务器端建立连接后，长时间不交换数据，一直占用服务器端的文件描述符，导致连接资源的浪费
* 定时事件，是指固定一段时间之后触发某段代码，由该段代码处理一个事件，如从内核事件表删除事件，并关闭文件描述符，释放连接资源
* 定时器，是指利用结构体或其他形式，将多种定时事件进行封装起来。具体的，这里只涉及一种定时事件，即定期检测非活跃连接，这里将该定时事件与连接资源封装为一个结构体定时器
* 定时器容器，是指使用某种容器类数据结构，将上述多个定时器组合起来，便于对定时事件统一管理。具体的，项目中使用升序链表将所有定时器串联组织起来
* 
* 本项目中，服务器主循环为每一个连接创建一个定时器，并对每个连接进行定时,
* 另外，利用升序时间链表容器将所有定时器串联起来，若主循环接收到定时通知，则在链表中依次执行定时任务
* 
* Linux下提供了三种定时的方法
* socket选项SO_RECVTIMEO和SO_SNDTIMEO
* SIGALRM信号
* I/O复用系统调用的超时参数
* 
* 本项目使用SIGALRM信号：利用alarm函数周期性地触发SIGALRM信号，信号处理函数利用管道通知主循环，
* 主循环接收到该信号后对升序链表上所有定时器进行处理，若该段时间内没有交换数据，则将该连接关闭，释放所占用的资源。
* 
* 重要数据结构：
*   struct sigaction {
        void (*sa_handler)(int); //函数指针，指向信号处理函数
        void (*sa_sigaction)(int, siginfo_t *, void *); //信号处理函数，有三个参数，可以获得关于信号更详细的信息
        sigset_t sa_mask;        //指定在信号处理函数执行期间需要被屏蔽的信号
        int sa_flags;           //指定信号处理的行为
        void (*sa_restorer)(void);
    }

*   int sigfillset(sigset_t *set); 
    //将参数set信号集初始化，然后把所有的信号加入到此信号集里
*   unsigned int alarm(unsigned int seconds); 
    //信号传送闹钟，即用来设置信号SIGALRM在经过参数seconds秒数后发送给目前的进程。如果未设置信号SIGALRM的处理函数，那么alarm()默认处理终止进程
*   int socketpair(int domain, int type, int protocol, int sv[2]); 
    //使用socketpair函数能够创建一对套接字进行通信，一个读端，一个写端，实现管道通信
*   ssize_t send(int sockfd, const void *buf, size_t len, int flags); 
    //当套接字发送缓冲区变满时，send会阻塞，除非套接字设置为非阻塞模式，当缓冲区变满时，返回EAGAIN或者EWOULDBLOCK错误，此时可以调用select函数来监视何时可以发送数据
* 
* 信号通知流程:
    信号的本质是中断，Linux信号采用异步处理机制，信号处理函数和当前进程是两条不同的执行路线。
    具体的，当进程收到信号时，操作系统会中断进程当前的正常流程，转而进入信号处理函数执行操作，完成后再返回中断的地方继续执行
    为避免信号竞态现象发生，信号处理期间系统不会再次触发它。所以，为确保该信号不被屏蔽太久，信号处理函数需要尽可能快地执行完毕
    本项目为减少信号处理时间，信号处理函数仅发送信号通知程序主循环，将信号对应的处理逻辑放在程序主循环中，由主循环执行信号对应的逻辑代码
* 统一事件源：
    统一事件源，是指将信号事件与其他事件一样被处理。
    信号处理函数使用管道将信号传递给主循环，信号处理函数往管道的写端写入信号值，主循环则从管道的读端读出信号值，
    使用I/O复用系统调用来监听管道读端的可读事件，这样信号事件与其他文件描述符都可以通过epoll来监测，从而实现统一处理。

* 信号处理机制
    0.进程之中都存着一个信号表，里面是每种信号所代表的含义，内核通过设置表项中每一个位来标识对应的信号类型
    1.接收信号的任务是由内核代理的，当内核接收到信号后，会将其放到对应进程的信号队列中，同时向进程发送一个中断，使其陷入内核态。
    注意，此时信号还只是在队列中，对进程来说暂时是不知道有信号到来的
    2.进程在内核态中，从睡眠状态被唤醒的时候进行信号检测
    3.检测到信号后，进程返回到用户态中，在用户态中执行相应的信号处理函数
    4.信号处理函数执行完成后，还需要返回内核态，检查是否还有其它信号未处理。
    5.如果所有信号都处理完成，就会将内核栈恢复，和中断前的运行位置，回到用户态继续执行原进程
*/

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

//常规的销毁链表
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    //如果新的定时器超时时间小于当前头部结点，直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //否则插入timer,并调整内部结点
    add_timer(timer, head); //重载的私有方法add_timer实现子操作
}

//调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    //被调整的定时器在链表尾部，或定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    //被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    //被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
//删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    //若链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //若被删除的定时器为头结点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //若被删除的定时器为尾结点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }

    //若被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

/*定时任务处理函数
* 使用统一事件源，SIGALRM信号每次被触发，主循环中调用一次定时任务处理函数，处理链表容器中到期的定时器。
* 1.遍历定时器升序链表容器，从头结点开始依次处理每个定时器，直到遇到尚未到期的定时器
* 2.若当前时间小于定时器超时时间，跳出循环，即未找到到期的定时器
* 3.若当前时间大于定时器超时时间，即找到了到期的定时器，执行回调函数，然后将它从链表中删除，然后继续遍历
* 
*/
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    //获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    //遍历定时器链表
    while (tmp)
    {
        //链表容器为升序排列，当前时间小于定时器的超时时间，后面的定时器也没有到期
        if (cur < tmp->expire)
        {
            break;
        }
        //当前定时器到期，则调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);
        //将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
//私有成员，被公有成员add_timer和adjust_time调用
//用于调整链表内部结点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    //遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    //空链表，插入节点放到尾结点处
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性: 再次执行该函数，环境变量与之前执行的无关，不会有数据覆盖或丢失
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0); //将信号值从管道写端写入，字符类型
    errno = save_errno; //将原来的errno赋值为当前的errno
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa; //创建sigaction变量
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler; //注册信号处理函数handler到sigaction，信号发生时回调
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask); //将所有信号添加到信号集中
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
//定时器回调函数
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    //关闭文件描述符
    close(user_data->sockfd);
    //更新连接数
    http_conn::m_user_count--;
}
