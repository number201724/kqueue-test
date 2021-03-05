//
//  main.c
//  kqueue_test
//
//  Created by number201724 on 2021/3/5.
//

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>

#define MAXEVENTS 64




int running;

typedef struct sizebuf_s
{
    const char *buffername;
    char *data;
    int cursize;
    int maxsize;
}sizebuf_t;

#define FLAG_READ (1 << 0)
#define FLAG_WRITE (1 << 1)

typedef struct client_s
{
    int refcnt;
    int fd;
    int flags;
    sizebuf_t message;
    char message_buf[8192];
}client_t;


void sighandler(int sig){
    running = 0;
    printf("break running\n");
}

void *sz_getspace(sizebuf_t *sz, int len){
    void *data;
    if(sz->cursize + len > sz->maxsize) {
        return NULL;
    }
    
    data = &sz->data[sz->cursize];
    sz->cursize += len;
    
    return data;
}

// replace to fifo
void sz_remove(sizebuf_t *sz, int len) {
    if(sz->cursize < len){
        printf("error\n");
        return;
    }
    
    memmove(sz->data, sz->data+len, sz->cursize - len);
    sz->cursize -= len;
}

void decref_client(client_t *cl) {
    if(--cl->refcnt == 0) {
        close(cl->fd);
        cl->fd = -1;
        
        printf("release client_t:%p\n", cl);
        free(cl);
    }
}

int main(int argc, const char * argv[]) {
    int fd, nfd;
    int eno;
    int nevents;
    struct sockaddr_in addr;
    struct kevent evset;
    struct kevent events[MAXEVENTS];
    struct timespec ts;
    client_t *cl;
    char buf[128];
    int len;
    
    int kqueue_fd = kqueue();
    if( kqueue_fd == -1 ) {
        printf( "kqueue failed err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    fd = socket( AF_INET, SOCK_STREAM,  IPPROTO_TCP );
    if( fd == -1 ) {
        printf( "socket failed err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    int opt = 1;
    eno = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if( eno == -1 ) {
        printf( "socket failed err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(27020);
    
    eno = bind( fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in) );
    if( eno == -1 ) {
        printf( "bind failed err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    eno = listen(fd, SOMAXCONN);
    if( eno == -1 ) {
        printf( "listen failed err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    int flags = 0;
    
    eno = fcntl(fd, F_GETFL, &flags);
    if( eno == -1 ) {
        printf( "fcntl F_GETFL err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    flags |= O_NONBLOCK;
    eno = fcntl(fd, F_SETFL, flags);
    if( eno == -1 ) {
        printf( "fcntl F_SETFL err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    
    EV_SET(&evset, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    eno = kevent(kqueue_fd, &evset, 1, NULL, 0, NULL);
    if( eno == -1 ) {
        printf( "kevent add listen fd failed err:%s\n", strerror(errno) );
        return EXIT_FAILURE;
    }
    
    signal(SIGTERM, sighandler);
    
    running = 1;
    
    while(running)
    {
        ts.tv_nsec = 1000*100;
        ts.tv_sec = 0;
        nevents = kevent(kqueue_fd, NULL, 0, events, MAXEVENTS, &ts);
        
        // printf("idle\n");
        // do idle
        if(nevents == 0) {
            continue;
        }
        
        if(nevents > 0)
        {
            for(int i =0; i < nevents; i++ )
            {
                if(events[i].flags & EV_EOF)
                {
                    assert(events[i].udata != NULL);        // unknown error panic!
                    cl = events[i].udata;
                    decref_client(cl);
                    continue;
                }
                
                if(events[i].filter == EVFILT_READ)
                {
                    if(events[i].ident == fd)
                    {
                        nfd = accept(fd, NULL, 0);
                        if(nfd == -1) {
                            printf("accept err:%s\n", strerror(errno));
                            return EXIT_FAILURE;
                        }
                        
                        printf("accept success fd is %d.\n", nfd );
                        
                        int flags = 0;
                        
                        eno = fcntl(nfd, F_GETFL, &flags);
                        if( eno == -1 ) {
                            printf( "fcntl F_GETFL err:%s\n", strerror(errno) );
                            return EXIT_FAILURE;
                        }
                        
                        flags |= O_NONBLOCK;
                        eno = fcntl(nfd, F_SETFL, flags);
                        if( eno == -1 ) {
                            printf( "fcntl F_SETFL err:%s\n", strerror(errno) );
                            return EXIT_FAILURE;
                        }
                        
                        cl = (client_t *)malloc(sizeof(client_t));
                        if(cl == NULL) {
                            printf("malloc problem\n");
                            return EXIT_FAILURE;
                        }
                        
                        cl->refcnt = 1;
                        cl->fd = nfd;
                        cl->flags = FLAG_READ;
                        cl->message.buffername = "send message";
                        cl->message.cursize = 0;
                        cl->message.data = cl->message_buf;
                        cl->message.maxsize = sizeof(cl->message_buf);
                        
                        EV_SET(&evset, nfd, EVFILT_READ, EV_ADD, 0, 0, cl);
                        eno = kevent(kqueue_fd, &evset, 1, NULL, 0, NULL);
                        
                        if( eno == -1 ) {
                            printf( "kevent add listen fd failed err:%s\n", strerror(errno) );
                            return EXIT_FAILURE;
                        }
                        
                        printf("add to kqueue success client_t:%p.\n", cl);
                        cl = NULL;
                        continue;
                    }
                    
                    cl = events[i].udata;
                    assert(cl != NULL);
                    
                    while((len = (int)recv(cl->fd, buf, sizeof(buf), 0)) != -1)
                    {
                        printf("fd:%d recv: %d bytes\n", cl->fd, len);
                        
                        void *data = sz_getspace(&cl->message, len);
                        
                        if( data == NULL ) {
                            printf("message overflow\n");
                            continue;
                        }
                       
                        memcpy(data, buf, len);
                    }
                    
                    printf("read ok.\n");
                    
                    // enable_write()
                    if(!(cl->flags & FLAG_WRITE)) {
                        EV_SET(&evset, cl->fd, EVFILT_WRITE, EV_ADD, 0, 0, cl);
                        eno = kevent(kqueue_fd, &evset, 1, NULL, 0, NULL);
                        if(eno == -1){
                            printf("EVFILT_WRITE eno:%d\n", eno);
                        }
                        
                        cl->refcnt++;
                        cl->flags |= FLAG_WRITE;
                    }
                }
                
                if(events[i].filter == EVFILT_WRITE) {
                    cl = events[i].udata;
                    assert(cl != NULL);
                    
                    len = (int)send(cl->fd, cl->message.data, cl->message.cursize, 0);
                    if(len == -1) { // eof or error
                        continue;
                    }
                    
                    sz_remove(&cl->message, len);
                    
                    if(cl->message.cursize == 0) {
                        // disable_write()
                        EV_SET(&evset, cl->fd, EVFILT_WRITE, EV_DELETE, 0, 0, cl);
                        eno = kevent(kqueue_fd, &evset, 1, NULL, 0, NULL);
                        if(eno == -1){
                            printf("EVFILT_WRITE eno:%d\n", eno);
                        }
                        
                        cl->flags &= ~FLAG_WRITE;
                        decref_client(cl);
                    }
                }
                
                
            }
            
            continue;
        } // end of for(int i =0; i < nevents; i++ ) {
        

        
        if(nevents < 0) {
            if(errno == EINTR){
                continue;
            }
            
            printf("errno:%d\n",errno);
            break;
        }
    }
    
    close(fd);
    
    return 0;
}
