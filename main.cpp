#define _GUN_SOURCE 1
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <pthread.h>

#define EPOLLMAX 1024

#define BUFSIZE 10240

#define MAXLINE 1024

void *httphandlertask(void *args);


typedef struct task
{
    void *args;
    void *(*run) (void *);
} task_t;


typedef struct Node
{
    task_t task;
    Node *next;
} Node;

typedef struct Queue
{
    Node *head;
    Node *tail;
} Queue;

void queue_init(Queue *queue)
{
    queue->head = (Node *)malloc(sizeof(Node));
    queue->tail = queue->head;
}

void queue_push(Queue *queue, Node *node)
{
    node->next = queue->tail->next;
    queue->tail->next = node;
    queue->tail = node;
}

Node *queue_pop(Queue *queue)
{
    if(queue->head->next==NULL) return NULL;

    Node *node = queue->head->next;
    if(queue->head->next==queue->tail) queue->tail = queue->head;
    queue->head->next = node->next;
    return node;
}

bool queue_empty(Queue *queue)
{
    return queue->head->next == NULL;
}

void queue_destory(Queue *queue)
{
    Node *node = queue->head;
    while(node!=NULL)
    {
        queue->head = node->next;
        free(node);
        node = queue->head;
    }

    queue->head = NULL;
    queue->tail = NULL;
}



typedef struct thread_pool
{
    int num;
    int isrun;
    pthread_t *pts;
    int detach;
    void *(*thread_task)(void *);
    Queue *task_queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} thread_pool_t;

void *thread_task(void *args)
{
    thread_pool_t *t_pool = (thread_pool_t*)args;
    
    Queue *queue = t_pool->task_queue;
    while(t_pool->isrun)
    {
        
        pthread_mutex_lock(&t_pool->mutex);

        while(queue_empty(queue))
            pthread_cond_wait(&t_pool->cond, &t_pool->mutex);

        Node *node = queue_pop(queue);

        pthread_mutex_unlock(&t_pool->mutex);        

        task_t task = node->task;
        task.run(task.args);
        free(node);
    }
}


void thread_pool_init(thread_pool_t *t_pool)
{
    t_pool->pts = (pthread_t*)malloc(sizeof(pthread_t)*t_pool->num);
    t_pool->task_queue = (Queue*)malloc(sizeof(Queue));
    queue_init(t_pool->task_queue);
    pthread_cond_init(&t_pool->cond, NULL);
    pthread_mutex_init(&t_pool->mutex, NULL);
    t_pool->isrun = 1;
    t_pool->thread_task = thread_task;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if(t_pool->detach)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    else
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int i=0;
    for(;i<t_pool->num;i++)
    {
        pthread_create(t_pool->pts+i, &attr, t_pool->thread_task, t_pool);
    }


}

void thread_pool_destory(thread_pool_t *t_pool)
{
    free(t_pool->pts);
    free(t_pool->task_queue);
}

void thread_pool_submit(thread_pool_t *t_pool, task_t task)
{
    Node *node = (Node*)malloc(sizeof(Node));
    node->task = task;

    Queue *queue = t_pool->task_queue;
    pthread_mutex_lock(&t_pool->mutex);

    queue_push(queue, node);

    pthread_cond_signal(&t_pool->cond);
    pthread_mutex_unlock(&t_pool->mutex);

    // free(node);
}

typedef struct httphandler_args
{
    int epollfd;
    int sockcli;
} httphandler_args_t;

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return new_option;
}

class httphandler
{
private:
    int epollfd;

    typedef struct 
    {
        int rio_fd;
        int rio_cnt;            // the length of rio_buf
        char *rio_buf_ptr;      // point unread pos
        char rio_buf[BUFSIZE];
    } rio_t;
    typedef enum 
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    } REQUEST_METHOD;

    typedef struct
    {
        REQUEST_METHOD method;
        
        int keep_alive;
        int content_length;
        char *uri;
        int uri_dir;    // 请求的uri是目录还是文件
        char path[30];
    } request_t;

    typedef struct
    {
        char *content;
        int content_length;
        int status_code;
        char *msg;
    } response_t;

    typedef enum
    {
        PROCESS_READ = 0,
        PROCESS_PARSE_HEADERS,
        PROCESS_PARSE_BODY,
        PROCESS_END,
        PROCESS_ERR
    } PROCESS;

    void format_size(char *buf, struct stat *stat)
    {
        if (S_ISDIR(stat->st_mode))
        {
            sprintf(buf, "%s", "[DIR]");
        }
        else
        {
            off_t size = stat->st_size;
            if (size < 1024)
            {
                sprintf(buf, "%luB", size);
            }
            else if (size < 1024 * 1024)
            {
                sprintf(buf, "%.1fK", (double)size / 1024);
            }
            else if (size < 1024 * 1024 * 1024)
            {
                sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
            }
            else
            {
                sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
            }
        }
    }

    int check_uri(request_t *request, response_t *response)
    {
        
        sprintf(request->path, ".%s", request->uri);

        struct stat filestat;
        if(stat(request->path, &filestat)==-1)
        {
            response->status_code = 404;
            return 1;
        }

        int path_fd = open(request->path, O_RDONLY, 0);

        response->status_code = 200;
        // 文件下载
        if(!S_ISDIR(filestat.st_mode))
        {
            response->content_length = filestat.st_size;
            request->uri_dir = 0;
        }else{
            response->content = (char *)malloc(BUFSIZE);
            memset(response->content, 0, BUFSIZE);

            request->uri_dir = 1;
            strcat(response->content, "<html><head><style>"
                                    "body{font-family: monospace; font-size: 13px;}"
                                    "td {padding: 1.5px 6px;}"
                                    "</style></head><body><table>\n");
            char m_time[32], size[16], content_buf[MAXLINE];

            DIR *d = fdopendir(path_fd);
            struct dirent *dp;
            int ffd;
            while((dp=readdir(d))!=NULL)
            {
                if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;
                if((ffd=openat(path_fd, dp->d_name, O_RDONLY))==-1)
                {
                    perror(dp->d_name);
                    continue;
                }
                fstat(ffd, &filestat);
                strftime(m_time, sizeof(m_time), "%Y-%m-%d %H:%M", localtime(&filestat.st_mtime));
                format_size(size, &filestat);
                if(S_ISREG(filestat.st_mode) || S_ISDIR(filestat.st_mode))
                {
                    const char *d = S_ISDIR(filestat.st_mode) ? "/" : "";
                    sprintf(content_buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n", dp->d_name, d, dp->d_name, d, m_time, size);
                    strcat(response->content, content_buf);

                }
                close(ffd);
            }
            closedir(d);
            strcat(response->content, "</table></body></html>");
            response->content_length = strlen(response->content);
        }

        close(path_fd);
        return 1;
    }

    void noblocksleep(int us)
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = us;
        select(0, NULL, NULL, NULL, &tv);
    }
    
    PROCESS rio_read(rio_t *rio)
    {
        int read_size;
        char *p = rio->rio_buf_ptr;
        int count = 0;
        while(1)
        {   
            count ++;
            read_size=read(rio->rio_fd, p, sizeof(rio->rio_buf)-(p-rio->rio_buf));
            if(read_size<=0)
            {
                if(read_size==0)
                {
                    return PROCESS_ERR;
                }
                else if(errno==EAGAIN)
                {
                    
                    noblocksleep(count*100);
                    if(count==100) {
                        printf("try to read %d times\n", count);
                        return PROCESS_ERR;
                    }
                    continue;
                }
                return PROCESS_ERR;
            }else{
                while(*p!='\n' && *p!=0) ++p;
                
                if(*p==0) continue;
                else break;
            }
        }
        return PROCESS_PARSE_HEADERS;
    }

    void rio_init(int fd, rio_t *rio)
    {
        rio->rio_buf_ptr = rio->rio_buf;
        rio->rio_cnt = 0;
        rio->rio_fd = fd;
        memset(rio->rio_buf, 0, BUFSIZE);
    }

    int parse_requestline(rio_t *rio, request_t *request)
    {
        
        if(strncasecmp(rio->rio_buf_ptr, "GET", 3)==0)
        {
            request->method = GET;
        }else if(strncasecmp(rio->rio_buf_ptr, "POST", 4)==0)
        {
            request->method = POST;
        }else{
            return 1;
        }

        while(*rio->rio_buf_ptr++!=' ');

        char *p = rio->rio_buf_ptr;
        while(*p++!=' ');
        *(p-1) = 0;
        int sizeofpath = strlen(rio->rio_buf_ptr);
        request->uri = (char *)malloc(sizeofpath+1);
        
        strcpy(request->uri, rio->rio_buf_ptr);
        rio->rio_buf_ptr = p;
        
        rio->rio_buf_ptr = strpbrk(rio->rio_buf_ptr, "\r\n");
        rio->rio_buf_ptr += 2;
        return 3;
    }

    PROCESS parse_requestheader(rio_t *rio, request_t *request)
    {
        char *p;
        while(1)
        {
            if(*rio->rio_buf_ptr=='\r') return PROCESS_PARSE_BODY;
            else if(strncasecmp(rio->rio_buf_ptr, "GET", 3)==0 || strncasecmp(rio->rio_buf_ptr, "POST", 4)==0)
            {
                if(strncasecmp(rio->rio_buf_ptr, "GET", 3)==0) request->method = GET;
                else if(strncasecmp(rio->rio_buf_ptr, "POST", 4)==0) request->method = POST;
                else return PROCESS_ERR;
            }
            else if(strncasecmp(rio->rio_buf_ptr, "Connection:", 11)==0)
            {
                rio->rio_buf_ptr += 11;
                while(*rio->rio_buf_ptr==' ') rio->rio_buf_ptr++;
                if(strncasecmp(rio->rio_buf_ptr, "keep-alive", 10)==0) request->keep_alive = 1;
                else request->keep_alive = 0;
            }else if(strncasecmp(rio->rio_buf_ptr, "Content-Length", 15)==0)
            {
                rio->rio_buf_ptr += 15;
                while(*rio->rio_buf_ptr==' ') rio->rio_buf_ptr++;
                p = strpbrk(rio->rio_buf_ptr, "\r\n");
                if(p==NULL) return PROCESS_ERR;
                *p = 0;
                request->content_length = atoi(rio->rio_buf_ptr);
                *p = '\r';
            }
            p = strpbrk(rio->rio_buf_ptr, "\r\n");
            if(p==NULL) return PROCESS_READ;
            rio->rio_buf_ptr = p + 2;
        }
    }

    PROCESS parse_requestcontent(rio_t *rio, request_t *request)
    {
        if(request->method==GET)
            return PROCESS_END;
    }

    int builder(request_t *request, response_t *response, rio_t *rio)
    {
        char header_line[MAXLINE];
        memset(header_line, 0, MAXLINE);

        if(response->status_code==200)
        {
            sprintf(header_line, "HTTP/1.1 %d %s\r\n", response->status_code, "OK");
        }else if(response->status_code==404)
        {
            sprintf(header_line, "HTTP/1.1 %d %s\r\n", response->status_code, "FILE NOT FOUND!");
        }else if(response->status_code==500)
        {
            sprintf(header_line, "HTTP/1.1 %d %s\r\n", response->status_code, "bad request");
        }
        strcat(rio->rio_buf, header_line);

        sprintf(header_line, "Content-Length: %d\r\n", response->content_length);
        strcat(rio->rio_buf, header_line);

        if(request->keep_alive)
        {
            sprintf(header_line, "Connection: Keep-Alive\r\n");
        }else{
            sprintf(header_line, "Connection: close\r\n");
        }
        strcat(rio->rio_buf, header_line);

        if(request->uri_dir)
        {
            sprintf(header_line, "Content-Type: text/html\r\n");
        }else{
            sprintf(header_line, "Content-Type: application/octet-stream\r\n");
        }
        strcat(rio->rio_buf, header_line);

        strcat(rio->rio_buf, "\r\n");
    }

    int write_all(request_t *request, response_t *response, rio_t *rio)
    {
        rio->rio_cnt = strlen(rio->rio_buf);
        rio->rio_buf_ptr = rio->rio_buf;
        int writed_size;
        while(rio->rio_cnt>0)
        {
            if((writed_size=write(rio->rio_fd, rio->rio_buf_ptr, rio->rio_cnt))<0)
            {
                if(errno==EAGAIN)
                {
                    continue;
                }
                return 0;
            }
            rio->rio_cnt -= writed_size;
            rio->rio_buf_ptr += writed_size;
        }

        int cnt=0;
        if(request->uri_dir)
        {
            while(response->content_length>0)
            {
                if((writed_size=write(rio->rio_fd, response->content+cnt, response->content_length))<0)
                {
                    if(errno==EAGAIN)
                    {
                        continue;
                    }
                    return 0;
                }
                cnt += writed_size;

                response->content_length -= writed_size;
            }
        }else{
            int path_fd = open(request->path, O_RDONLY, 0);

            char *src_addr = (char *)mmap(NULL, response->content_length, PROT_READ, MAP_PRIVATE, path_fd, 0);

            while(response->content_length>0)
            {
                if((writed_size=write(rio->rio_fd, src_addr+cnt, response->content_length))<0)
                {
                    if(errno==EAGAIN)
                    {
                        continue;
                    }
                    break;
                }
                cnt += writed_size;
                response->content_length -= writed_size;
            }

            munmap(src_addr, response->content_length);
            close(path_fd);
        }

        return 1;
    }

    int resource_free(request_t *request, response_t *response)
    {
        if(response->content!=NULL) free(response->content);
        if(request->uri!=NULL) free(request->uri);
    }

public:
    httphandler(int epollfd)
    {
        this->epollfd = epollfd;
    }
    
    ~httphandler()
    {

    }

    int process_request(int sockcli)
    {
        rio_t rio;

        request_t request;
        request.keep_alive = 0;
        request.uri = NULL;
        response_t response;
        response.content = NULL;

        rio_init(sockcli, &rio);

        PROCESS process_state = PROCESS_READ;

        int requestlineok = 0;
        while(process_state!=PROCESS_END)
        {   
            switch (process_state)
            {
            // 读
            case PROCESS_READ:
                process_state = rio_read(&rio);
                break;
            // 解析
            case PROCESS_PARSE_HEADERS:
            {
                if(!requestlineok) {
                    parse_requestline(&rio, &request);
                    requestlineok = 1;
                }
                else process_state = parse_requestheader(&rio, &request);
                break;
            }

            // 解析body
            case PROCESS_PARSE_BODY:
                process_state = parse_requestcontent(&rio, &request);
                break;
            case PROCESS_ERR:
            {
                response.status_code = 500;
                response.content_length = 0;
                process_state = PROCESS_END;
                goto res;
            }
                break;
            default:
                break;
            }
        }

        check_uri(&request, &response);

    res:
        rio_init(sockcli, &rio);

        builder(&request, &response, &rio);

        write_all(&request, &response, &rio);

        resource_free(&request, &response);

        return request.keep_alive;
    }

};


int socket_init(int port, char *host)
{
    int socksev;
    struct sockaddr_in addrsev;
    memset(&addrsev, '\0', sizeof(addrsev));

    addrsev.sin_family = AF_INET;
    addrsev.sin_port = htons(port);
    addrsev.sin_addr.s_addr = inet_addr(host);

    socksev = socket(AF_INET, SOCK_STREAM, 0);
    if(socksev==-1){
        printf("create socket fail\n");
        exit(1);
    }
    int reuse = 1;
    if(setsockopt(socksev, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int))<0)
    {
        perror("setsocketopt");
        exit(0);
    }
    if(bind(socksev, (struct sockaddr*)&addrsev, sizeof(addrsev))<0){
        printf("bind error!\n");
        exit(1);
    }

    if(listen(socksev, 5)<0){
        printf("listen fail\n");
        exit(0);
    }

    return socksev;
}

void *sockaccepttask(void *args)
{
    int sockcli;
    httphandler_args_t *http_h_args = (httphandler_args_t*)args;
    int epollfd = http_h_args->epollfd;
    int socksev = http_h_args->sockcli;
    struct sockaddr_in addrcli;
    socklen_t addrlen = sizeof(addrcli);
    struct epoll_event ev;
    while( (sockcli=accept(socksev, (struct sockaddr*)&addrcli, &addrlen))>0)
    {
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
        ev.data.fd = sockcli;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, sockcli, &ev);
        setnonblocking(sockcli);
        printf("socket %d connected\n", sockcli);
    }
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = socksev;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, socksev, &ev);
    printf("reset socksev %d\n", socksev);
    free(http_h_args);
}

void init(int port, char *host)
{
    int socksev, sockcli;
    struct sockaddr_in addrcli;
    socklen_t addrlen;
    
    socksev = socket_init(port, host);

    for(int i=0;i<2;i++)
        fork();

    int epollfd = epoll_create(1024);

    thread_pool_t t_pool;

    t_pool.num = 4;

    thread_pool_init(&t_pool);

    setnonblocking(socksev);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = socksev;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, socksev, &ev);
    struct epoll_event events[EPOLLMAX];
    int count = 0;
    while(1)
    {
        int num = epoll_wait(epollfd, events, EPOLLMAX, -1);
        int i;
        for(i=0;i<num;++i)
        {
            if(events[i].data.fd==socksev)
            {   
                httphandler_args_t *args = (httphandler_args_t*)malloc(sizeof(httphandler_args_t));
                args->epollfd = epollfd;
                args->sockcli = socksev;
                task_t task;
                task.args = args;
                task.run = sockaccepttask;
                thread_pool_submit(&t_pool, task);
            }else if(events[i].events & EPOLLRDHUP)
            {
                epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                close(events[i].data.fd);
                printf("socket %d disconnected\n", events[i].data.fd);
            }else if(events[i].events & EPOLLIN)
            {
                printf("socket %d event response, the %d request\n", events[i].data.fd, count++);
                // int keep_alive = process_request(events[i].data.fd);
                // pthread_t p;
                // pthread_attr_t attr;
                // pthread_attr_init(&attr);
                // pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                httphandler_args_t *args = (httphandler_args_t*)malloc(sizeof(httphandler_args_t));
                args->epollfd = epollfd;
                args->sockcli = events[i].data.fd;
                task_t task;
                task.args = (void *)args;
                task.run = httphandlertask;
                thread_pool_submit(&t_pool, task);

                // pthread_create(&p, &attr, task, args);
                // printf("socket %d response OK\n", events[i].data.fd);

                // if(keep_alive==0)
                // {
                //     epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                //     close(events[i].data.fd);
                //     printf("socket %d disconnected\n", events[i].data.fd);
                // }
            }else {
                printf("unknown event\n");
            }
        }
    }
    t_pool.isrun = 0;
    thread_pool_destory(&t_pool);
    close(socksev);
}


void *httphandlertask(void *args)
{
    httphandler_args_t *httphandler_args = (httphandler_args_t*)args;
    int epollfd = httphandler_args->epollfd;
    int connfd = httphandler_args->sockcli;
    httphandler handler(epollfd);
    int keep_alive = handler.process_request(connfd);

    if(keep_alive==0)
    {
        close(connfd);
        epoll_ctl(epollfd, EPOLL_CTL_MOD, connfd, NULL);
        printf("socket %d disconnected\n", connfd);
    }else
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
        ev.data.fd = connfd;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, connfd, &ev);
        printf("reset socket %d oneonshut\n", connfd);
    }

    free(httphandler_args);
    printf("socket %d response OK\n", connfd);
}

void *print(void *args)
{
    int i = (int)(long)args;
    printf("Hello world %d\n", i);

}

int main(int argc, char const *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    init(9999, "127.0.0.1");
    // thread_pool_t t_pool;
    // t_pool.num = 1;

    // thread_pool_init(&t_pool);
    // task_t task;
    // task.args = (void *)89;
    // task.run = print;
    // thread_pool_submit(&t_pool, task);

    // sleep(2);

    // thread_pool_submit(&t_pool, task);

    // while(1);

    return 0;
}
