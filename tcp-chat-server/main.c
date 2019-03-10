#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

#define PORT "3490"  // the port users will be connecting to
#define BACKLOG 10     // how many pending connections queue will hold
#define MAXDATASIZE 2000

#define COMMAND_FIELD_WIDTH 20
#define USERNAME_FIELD_WIDTH 20
#define MESSAGE_FIELD_WIDTH 100

typedef struct _client {
    char username[USERNAME_FIELD_WIDTH];
    int fd;
    struct sockaddr_storage addr;
    struct _client *next;
} client_t;

typedef struct _client_list {
    client_t *head;
    pthread_mutex_t mutex;
} client_list_t;

typedef struct _instruction {
    char command[COMMAND_FIELD_WIDTH];
    char username[USERNAME_FIELD_WIDTH];
    char message[MESSAGE_FIELD_WIDTH];
} instruction_t; 

static client_list_t client_list;

void sigchld_handler(int s) {
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void *client_handler(void *_client) { 
    int nbytes;
    client_t* client = (client_t*)_client;
    char buf[MAXDATASIZE];
    
    printf("server: client handler waiting on fd %d\n", client->fd);
    
    while (1) {
        if ((nbytes = recv(client->fd, buf, MAXDATASIZE-1, 0)) == -1) {
            perror("recv");
            close(client->fd);
            pthread_exit((void *)-1);
        } else if (nbytes == 0) {
            printf("server: connection on fd %d gracefully closed\n", client->fd);
            close(client->fd);
            pthread_exit((void *)0);
        }
        
        buf[nbytes] = 0;
        
        printf("server: received '%s'\n", buf);
        
        if (send(client->fd, buf, nbytes, 0) == -1) {
            perror("send");
        } else {
            printf("server: echoed\n");
        }
             
    }
 
    pthread_exit((void *)-1);
    
}


int main(void) {
    int sockfd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sigaction sa;
    int yes = 1;
    int rv;
    char s[INET6_ADDRSTRLEN];
    
    client_list = (client_list_t) {.head = NULL, .mutex = PTHREAD_MUTEX_INITIALIZER };

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }

    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("server: waiting for connections...\n");

    while(1) {  // main accept() loop
        client_t* client = malloc(sizeof(client_t));
        
        socklen_t sin_size = sizeof (client->addr);
        client->fd = accept(sockfd, (struct sockaddr *)&client->addr, &sin_size);
        if (client->fd == -1) {
            perror("accept");
            free(client);
            continue;
        }
        
        //connection successfully accepted
        inet_ntop(client->addr.ss_family,
            get_in_addr((struct sockaddr *)&client->addr),
            s, sizeof s);
        printf("server: connection accepted from %s:%hu on fd %d\n", s, ntohs(((struct sockaddr_in *)(&client->addr))->sin_port), client->fd);
        
        //insert client into client list
        pthread_mutex_lock(&(client_list.mutex));
        client->next = client_list.head;
        client_list.head = client;
        pthread_mutex_unlock(&(client_list.mutex));
        
        //create client handler thread
        pthread_t t;
        pthread_attr_t tattr;
        pthread_attr_init(&tattr);
        pthread_attr_setdetachstate(&tattr,PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &tattr, client_handler, client);
    }

    printf("server: exiting");
    return 0;
}