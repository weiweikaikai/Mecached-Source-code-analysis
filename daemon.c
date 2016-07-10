/*************************************************************************
	> File Name: Daemon.c
	> Author: wk
	> Mail: 18402927708@163.com
	> Created Time: Fri 08 Jul 2016 05:28:31 PM CST
 ************************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "memcached.h"

/**
 *
 * 编写守护进程的步骤：
 *（1）在父进程中执行fork并exit推出；
 *（2）在子进程中调用setsid函数创建新的会话；
 *（3）在子进程中调用chdir函数，让根目录 ”/” 成为子进程的工作目录；
 *（4）在子进程中调用umask函数，设置进程的umask为0；
 *（5）在子进程中关闭任何不需要的文件描述符 0 1 2
 * 
 * http://blog.csdn.net/tankles/article/details/7050340
 */
int daemonize(int nochdir, int noclose)
{
    int fd;

    
    switch (fork())//首先fork一次
	{
    case -1://fork失败，程序结束
        return (-1);
    case 0: //子进程执行下面的流程
        break;
    default:
        _exit(EXIT_SUCCESS);//父进程安全退出
    }
 //setsid调用成功之后，返回新的会话的ID，
 //调用setsid函数的进程成为新的会话的领头进程，
 //并与其父进程的会话组和进程组脱离 
    if (setsid() == -1)
        return (-1);

    if (nochdir == 0) 
	{
	//进程的当前目录切换到根目录下，根目录是一直存在的，其他的目录就不保证  
        if(chdir("/") != 0) {
            perror("chdir");
            return (-1);
        }
    }

    if (noclose == 0 && (fd = open("/dev/null", O_RDWR, 0)) != -1) 
		{
        if(dup2(fd, STDIN_FILENO) < 0) {//将标准输入重定向到/dev/null下  
            perror("dup2 stdin");
            return (-1);
        }
        if(dup2(fd, STDOUT_FILENO) < 0) {//将标准输出重定向到/dev/null下  
            perror("dup2 stdout");
            return (-1);
        }
        if(dup2(fd, STDERR_FILENO) < 0) {//将标准错误重定向到/dev/null下 
            perror("dup2 stderr");
            return (-1);
        }

        if (fd > STDERR_FILENO) {//大于2的描述符都可以关闭 
            if(close(fd) < 0) {
                perror("close");
                return (-1);
            }
        }
    }
    return (0);
}

