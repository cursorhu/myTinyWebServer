#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

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
		//连接对象放入链表
		connList.push_back(con);
		++m_FreeConn; //资源池可用的连接计数++
	}
	//sem_init，sem定义在locker.h
	//m_FreeConn作为信号量的计数，即可用资源的个数，reserve是connection_pool的sem类成员
	reserve = sem(m_FreeConn); 

	m_MaxConn = m_FreeConn; //最大可用连接
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();  //sem_wait，若信号量(即资源计数)大于0，则计数-1并继续；若等于0，则阻塞在此
	
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

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}