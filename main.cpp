#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "123456";
    string databasename = "cppwebserver";

    //解析命令行配置
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化(配置写入WebServer对象)
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    

    //日志初始化
    server.log_init();

    //数据库初始化和用户数据获取
    server.sql_pool();

    //线程池初始化
    server.thread_pool();

    //触发模式配置
    server.trig_mode();

    //初始化socket及epoll
    server.eventListen();

    //运行事件监听：epoll_wait
    server.eventLoop();

    return 0;
}