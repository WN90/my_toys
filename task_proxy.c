#include <stdio.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define REQUESTBUF_SIZE 5100
#define SOCK_PATH  "/tmp/task_proxy"
#define MAX_TASKS  16
#define BACKLOG    32
#define MAX_EVENTS 2+MAX_TASKS

#define ARGV_MAX 16


/*
 * BSD License
 * request must start with CMD, end with \0, parameters separated with DELIM
 * NRET means return immediately, excute background
 * PIPE means receive the output of the task
 * EXEC means receive a return status like that in waitpid
 * don't use spaces unless you know what you are doing
 * for example:
 * "exec#ls#-l#/tmp" \0 is contained
 * "pipe#ls#-l#/tmp" \0 is contained
 * "nret#ls#-l#/tmp" \0 is contained
 * if a status is returned it is put after RETURN_MARK
 * for example:
 * "####\x0\x0\x0\x0"
 **/
#define CMDLEN 4
#define EXEC "exec"
#define PIPE "pipe"
#define NRET "nret"
enum
{
    EXECID,
    PIPEID,
    NRETID,
};

#define DELIM '#'
#define RETURN_MARK "####"

#define error_exist(msg ) do { perror(msg); exit(EXIT_FAILURE); } while (0)

struct taskbuf
{
    int len;
    char buf[REQUESTBUF_SIZE];
};
/* pid is positive, use negtive num to represent a free list */
int pid_count, task_count;
pid_t task_pids[MAX_TASKS];
int   task_socks[MAX_TASKS];
void *task_buf[MAX_TASKS];
int avaliable_list;

int epollfd;

struct taskbuf *task_getbuf(i)
{
    return task_buf[i];
}
struct taskbuf *task_allocbuf(i)
{
#ifndef NDEBUG
    fprintf(stderr, "task alloc\n");
    if(task_buf[i] != NULL)
    {
        fprintf(stderr, "unexpected memory not free\n");
    }
    else
#endif
    {
        task_buf[i] = malloc(REQUESTBUF_SIZE);
    }
    return task_buf[i];
}
void task_freebuf(i)
{
    if(task_buf[i])
    {
        free(task_buf[i]);
        task_buf[i] = NULL;
    }
}

void task_prepare(void)
{
    int i;
    for (i = 0; i < MAX_TASKS; ++i) {
        task_pids[i] = -i-1;
        task_socks[i] = -1;
        task_buf[i] = NULL;
    }
    avaliable_list = 0;
    pid_count = 0;
    task_count = 0;
    signal(SIGPIPE, SIG_IGN);
}

/* to get a free node
   use with task_put in pairs like malloc and free */
int task_get(void)
{
    int i = -avaliable_list;

    if(i == MAX_TASKS)      /* not exist, no free node */
        return -1;

    avaliable_list = task_pids[i];
    task_socks[i] = -1;
    task_count++;
    return i;
}
/* add a node back to the free list */
void task_put(int i)
{
    task_count--;
    task_pids[i] = avaliable_list;
    avaliable_list = -i;
    if(task_socks[i] > 0)
        close(task_socks[i]);
    task_socks[i] = -1;
    if(task_buf[i])
        task_freebuf(i);
}
/* find the node by pid */
int task_find(pid_t pid)
{
    int i;
    for (i = 0; i < MAX_TASKS; ++i) {
        if(task_pids[i] == pid)
            return i;
    }
    fprintf(stderr, "pid %d not found\n", pid);
#ifndef NDEBUG
    for (i = 0; i < MAX_TASKS; ++i) {
        fprintf(stderr, "i %d pid %d socks %d\n", i, task_pids[i], task_socks[i]);
    }
#endif
    return -1;
}


/* split the request string into argv for execvp */
void split_request(char buf[], int l, char *argv[], int argc)
{
    int i, j;
    for (i = 0, j = 0; i < l && j < argc-1; ++i) {
        if(buf[i] == DELIM)
        {
            buf[i] = 0;
            if(i+1 < l)
            {
                argv[j++] = buf+i+1;
            }
        }
    }
    argv[j] = NULL;
#ifndef NDEBUG
    fprintf(stderr, "argc=%d %s:\t", j, buf);
    for(j=0; argv[j]!=NULL; j++)
    {
        fprintf(stderr, "%s ", argv[j]);
    }
    fprintf(stderr, "\n");
#endif
}
void exec_process(int fd, int type, char buf[], int len)
{
    char *argv[ARGV_MAX];

    close(fd);  /* write nothing, so close it immediately */
    split_request(buf, len, argv, ARGV_MAX);
    execvp(argv[0], argv);
    error_exist("exec_process");
}
void pipe_process(int fd, int type, char buf[], int len)
{
    char *argv[ARGV_MAX];

    split_request(buf, len, argv, ARGV_MAX);
    if(dup2(fd, 1) == -1)   /* redirect output to client */
    {
        error_exist("dup2");
    }
    execvp(argv[0], argv);
    error_exist("pipe_process");
}

void after_wait(pid_t pid, int status)
{
    pid_count--;
    int i = task_find(pid);
    if(i >= 0)
    {
        if(task_socks[i] > 0)
        {
            char buf[12];
            int32_t s32 = status;
            memcpy(buf, RETURN_MARK, sizeof RETURN_MARK -1);
            memcpy(buf+sizeof RETURN_MARK -1, &s32, sizeof s32);
            int ret = write(task_socks[i], buf, sizeof RETURN_MARK -1 + sizeof s32);
            if(ret < 0)
                perror("after_wait write");
        }
        task_put(i);
    }
}

int client_readbuf(int cl, char buf[], int size)
{
    int l = 0;
    int ret;

    if(size <= 0)
    {
        fprintf(stderr, "request too long\n");
        return -1;
    }

    while(size - l > 0)
    {
        ret = read(cl, buf+l, size-l);
        if(ret < 0)
        {
            if(errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("client_readbuf read");
                return -1;
            }
            break;
        }
        else if(ret == 0)       /* EOF */
            break;
        else
            l += ret;
    }
    if(ret == 0 && l+1 <= size && l-1 >= 0 &&buf[l-1] != 0)
        buf[l++] = 0;    /* must end with 0, append one if not exist */

    if(size-l == 0 && buf[l-1] != 0)
    {
        fprintf(stderr, "request too long\n");
        return -1;
    }
    return l;
}

void client_after_read(int i, char buf[], int l)
{
    int cl = task_socks[i];
    if(l < CMDLEN+1)
    {
        task_put(i);
        return;
    }
   
    /*
    struct epoll_event ev;
    ev.events = EPOLLERR;
    ev.data.fd = cl;
    */
    if(epoll_ctl(epollfd, EPOLL_CTL_DEL, cl, NULL) == -1)
    {
        perror("client_after_read epoll_ctl");
        task_put(i);
        return;
    }
#if 0
    if(shutdown(cl, SHUT_RD) < 0)
    {
        perror("shutdown SHUT_RD");
        task_put(i);
        return;
    }
#endif

    int type;
    if(strncmp(buf, EXEC, CMDLEN) == 0)
        type = EXECID;
    else if(strncmp(buf, NRET, CMDLEN) == 0)
        type = NRETID;
    else if(strncmp(buf, PIPE, CMDLEN) == 0)
        type = PIPEID;
    else
    {
        buf[l-1] = 0;
        task_put(i);
        fprintf(stderr, "wrong request(print without last byte):%s\n", buf);
        return;
    }

    pid_t pid = fork();
    if(pid < 0)
    {
        task_put(i);
        perror("client_after_read fork");
        return;
    }
    else if(pid > 0)    /* parent */
    {
        pid_count++;
        task_freebuf(i);
        task_pids[i] = pid;
        switch(type)
        {
            case EXECID:
                /* close it after wait to write the status of the child to the client */
                break;
            case NRETID:
            case PIPEID:
                close(task_socks[i]);
                task_socks[i] = -1;
        }
        return;
    }
    else                /* child,  exec, no return */
    {
        switch(type)
        {
            case EXECID:
            case NRETID:
                exec_process(cl, type, buf, l);
            case PIPEID:
                pipe_process(cl, type, buf, l);
        }
    }
}
void client_process(int i)
{
    int cl = task_socks[i];
    struct taskbuf *buf = task_getbuf(i);
    if(buf == NULL)     /* mostly, in stack is enough */
    {
        char _buf[REQUESTBUF_SIZE];
        int l = client_readbuf(cl, _buf, REQUESTBUF_SIZE);
        if(l <= 0)
        {
            task_put(i);
            return;
        }
        if(_buf[l-1] != 0)
        {
            struct taskbuf *data = task_allocbuf(i);
            if(data == NULL || REQUESTBUF_SIZE < l)
            {
                task_put(i);
                return;
            }
            memcpy(data->buf, _buf, l);    /* copy from stack to heap */
            data->len = l;
            return;
        }
        return client_after_read(i, _buf, l);
    }
    else
    {
        int l = client_readbuf(cl, &buf->buf[buf->len], REQUESTBUF_SIZE - buf->len);
        if(l < 0)
        {
            task_put(i);
            return;
        }
        if(l == 0 && buf->len < REQUESTBUF_SIZE)
        {
            buf->buf[buf->len] = 0;
            buf->len ++;
        }
        buf->len += l;
        if(buf->buf[buf->len - 1] != 0)
        {
            return;
        }
        return client_after_read(i, buf->buf, buf->len);
    }
}

void server_sock_process(int fd)
{
    int i;

    if((i = task_get()) < 0)
        return;

    int cl = accept(fd, NULL, NULL);
    if(cl < 0)
    {
        perror("accept");
        task_put(i);
        return;
    }
    task_socks[i] = cl;

    int flags;
    flags = fcntl(cl,F_GETFL,0);
    if(flags == -1 || fcntl(cl, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl");
        task_put(i);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = -i;    /* fd is saved in task_socks[i], use negative num to not conflict with normal fd */
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, cl, &ev) == -1) {
        perror("server_sock_process epoll_ctl");
        task_put(i);
        return;
    }
}

void sfd_process(int sfd)
{
    struct signalfd_siginfo fdsi;
    ssize_t s;
    s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
    if (s != sizeof(struct signalfd_siginfo))
    {
        perror("signal read");
        return;
    }

    if (fdsi.ssi_signo != SIGCHLD) {
        fprintf(stderr, "unexpected signal %d\n", fdsi.ssi_signo);
    }

    while(1)
    {
        int status;
        pid_t pid;
        pid = waitpid(-1, &status, WNOHANG);
        if(pid > 0)
        {
            after_wait(pid, status);
        }
        else if(pid <= 0)
        {
            if(pid < 0 && errno != ECHILD)
                perror("sfd_process wait");
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    sigset_t mask;
    int sfd;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        error_exist("sigprocmask");

    sfd = signalfd(-1, &mask, 0);
    if (sfd == -1)
        error_exist("signalfd");


    int server_sock;
    struct sockaddr_un server_sockaddr;
    char buf[256];
    memset(&server_sockaddr, 0, sizeof(struct sockaddr_un));
    memset(buf, 0, 256);                

    server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_sock == -1){
        error_exist("socket");
    }

    server_sockaddr.sun_family = AF_UNIX;
    unlink(SOCK_PATH);
    strncpy(server_sockaddr.sun_path, SOCK_PATH, sizeof(server_sockaddr.sun_path)-1);
    if(bind(server_sock, (struct sockaddr*)&server_sockaddr, sizeof(server_sockaddr)) == -1)
        error_exist("bind");
    if(listen(server_sock, BACKLOG) == -1)
        error_exist("listen");

    struct epoll_event ev, events[MAX_EVENTS];

    epollfd = epoll_create1(0);
    if(epollfd == -1)
        error_exist("epoll_create1");

    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, sfd, &ev) == -1)
        error_exist("epoll_ctl");

    ev.events = EPOLLIN;
    ev.data.fd = server_sock;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, server_sock, &ev) == -1)
        error_exist("epoll_ctl");

    task_prepare();

    while(1)
    {
        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if(nfds == -1)
        {
            perror("epoll_wait");
            sleep(1);
            continue;
        }
        if(nfds == 1 && events[0].data.fd == server_sock && avaliable_list == -MAX_TASKS)
        {
            usleep(500000);
            continue;
        }

        int i;
        for (i = 0; i < nfds; ++i) {
            if(events[i].data.fd == sfd)
                sfd_process(sfd);
            else if(events[i].data.fd == server_sock)
                server_sock_process(server_sock);
            else
            {
                int cl_i = -events[i].data.fd;
                if(cl_i >= 0)
                {
                    if(events[i].events & EPOLLERR)
                        task_put(cl_i);
                    else if(task_pids[cl_i] <= 0) 
                        client_process(cl_i);
                }
                else
                    fprintf(stderr, "unexpected index, %d\n", cl_i);

            }
        }
    }
    return 0;
}
