#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

/*池是一组资源的集合，这组资源在服务器启动之初就被完全创建好并初始化
* 池的本质是空间换时间：当服务器需要相关的资源，可以直接从池中获取，无需动态分配，处理完客户的连接后,把相关的资源放回池中，无需通过系统调用释放资源
* 
* 数据库访问的一般流程：
* 系统需要访问数据库时，先系统创建数据库连接，完成数据库操作，然后系统断开数据库连接
* 若系统需要频繁访问数据库，则需要频繁创建和断开数据库连接，创建数据库连接是很耗时的操作，频繁操作对数据库造成安全隐患
* 在程序初始化的时候，集中创建多个数据库连接，并集中管理供程序使用，可以保证较快的数据库读写速度和高可靠
* 
* 使用单例模式和链表创建数据库连接池
* 工作线程从数据库连接池取得一个连接，访问数据库中的数据，访问完毕后将连接交还连接池。
* 
*/


using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}


/*
* 《Effective C++》（Item 04）: 
* 更优雅的单例模式实现，使用函数内的局部静态对象，这种方法不用加锁和解锁操作
* C++0X以后，要求编译器保证内部静态变量的线程安全性，故C++0x之后该实现是线程安全的
* C++0x是C++11标准成为正式标准之前的草案临时名称
* 
* 经典的线程安全的懒汉模式，使用双判空加锁的方法，实现如下：
	class single{
	 private:
	     //私有静态指针变量指向唯一实例
	     static single *p;
	 
	     //静态锁，是由于静态函数只能访问静态成员
	     static pthread_mutex_t lock;

	     //私有化构造函数
	    single(){
	        pthread_mutex_init(&lock, NULL); //构造函数初始化锁
	    }
	    ~single(){}

	 public:
	    //公有静态方法获取实例
	    static single* getinstance();
	};

	pthread_mutex_t single::lock;
	single* single::p = NULL;

	single* single::getinstance(){ //懒汉模式：有需要时才初始化唯一实例
	    if (NULL == p){ //这里判空，是避免多次调用每次都加锁，p!=NULL直接返回
	        pthread_mutex_lock(&lock); //加锁，否则getinstance多线程重入，会创建多个实例
	        if (NULL == p){ //这个判空不是多余，必须在锁内作为原子操作，否则加锁前切线程，会创建多个实例
	            p = new single;
	        }
	        pthread_mutex_unlock(&lock);
	    }
	    return p;
	}

  饿汉模式：
  类内new该唯一对象，getInstance()只返回对象的指针：
	single* single::p = new single();
	single* single::getinstance(){
	    return p;
	}

  缺陷：不同编译单元中，有可能对象初始化完成之前调用 getInstance() 方法，会返回一个未定义的实例
*/
connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//连接池的构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; i++) //构造多个sql连接并存入链表，形成sql连接对象的资源池
	{
		MYSQL *con = NULL;
		con = mysql_init(con); //初始化一个连接对象

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		//配置该连接使之可用
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		
		connList.push_back(con); //连接对象放入链表
		++m_FreeConn; 			//资源池可用的空闲连接计数++
	}
	//将信号量初始化为最大连接次数，实现是sem_init，见locker.h
	//reserve是connection_pool的sem类成员
	reserve = sem(m_FreeConn); 
	m_MaxConn = m_FreeConn; //最大可用连接
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();  //实现是sem_wait，原子操作：若信号量(即资源计数)大于0，则计数-1并继续；若等于0，则阻塞在此
	
	lock.lock();

	con = connList.front(); //链表中选中一个sql连接对象
	connList.pop_front(); //链表弹出该对象

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con); //连接对象放回资源池
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post(); //sem_post, 释放资源时，信号量计数+1
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}
//RAII机制销毁连接池
connection_pool::~connection_pool()
{
	DestroyPool();
}

/*
* 不直接调用获取和释放连接的接口，将其封装如下成RAII机制，进行获取和释放
*/
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}