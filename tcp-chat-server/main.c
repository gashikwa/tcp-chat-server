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

#define COMMAND_FIELD_WIDTH 1
#define USERNAME_FIELD_WIDTH 20
#define MESSAGE_FIELD_WIDTH 1000

#define MAX_USERS MESSAGE_FIELD_WIDTH / USERNAME_FIELD_WIDTH
#define MAXDATASIZE COMMAND_FIELD_WIDTH + USERNAME_FIELD_WIDTH + MESSAGE_FIELD_WIDTH

typedef enum _command {
    UNICAST = 1,
    BROADCAST,
    LIST,
    EXIT,
    JOIN,
    ERROR
} command_t;

typedef struct _client {
    char username[USERNAME_FIELD_WIDTH];
    int fd;
    struct sockaddr_storage addr;
    struct _client *next, *prev;
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

int send_instruction(int fd, char* buf, size_t len) {
    if (send(fd, buf, MAXDATASIZE, 0) == -1) {
        perror("send");
    }
    
}

void *client_handler(void *_client) { 
    int nbytes;
    client_t* client = (client_t*)_client;
    char buf[MAXDATASIZE];
    int rv = 0;
    int tmpfd;
    
    printf("server: client handler waiting on fd %d\n", client->fd);
    
    while (1) {
        if ((nbytes = recv(client->fd, buf, MAXDATASIZE, 0)) == -1) {
            perror("recv");
            rv = -1;
            break;
        } else if (nbytes == 0) {
            printf("server: connection on fd %d gracefully closed\n", client->fd);
            break;
        }
        
        buf[nbytes] = 0;
        printf("server: received '%s' on fd%d\n", buf, client->fd);
        
        instruction_t *instruction = (instruction_t *)buf;
        client_t *p = client_list.head;
        char *m = instruction->message;

        switch(instruction->command[0]) {
            case UNICAST:
                while (p) {
                    if (memcmp(p->username, instruction->username, USERNAME_FIELD_WIDTH)) {
                        memcpy(instruction->username, client->username, USERNAME_FIELD_WIDTH);
                        if (send(p->fd, buf, MAXDATASIZE, 0) == -1) {
                            perror("send");
                        }
                        goto done;
                    }
                    p = p->next;
                }
                //user not found
                instruction->command[0] = ERROR;
                strcpy(instruction->message, "recipient not found");
                if (send(client->fd, buf, MAXDATASIZE, 0) == -1) {
                    perror("send");
                }
                break;
                
            case BROADCAST:
                memcpy(instruction->username, client->username, USERNAME_FIELD_WIDTH);
                while (p) {
                    if (send(p->fd, buf, MAXDATASIZE, 0)) {
                        perror("send");
                    }
                }
                break;

            case LIST:
                while (p) {
                    memcpy(m, instruction->username, USERNAME_FIELD_WIDTH);
                    p = p->next;
                    m += USERNAME_FIELD_WIDTH;
                }
                if (send(client->fd, buf, MAXDATASIZE, 0) == -1) {
                    perror("send");
                }
                break;
                
            case JOIN:
                //client has already joined
                if (client->username[0]) {
                    instruction->command[0] = ERROR;
                    strcpy(instruction->message, "you have already joined the server");
                    if (send(client->fd, buf, MAXDATASIZE, 0) == -1) {
                        perror("send");
                    }
                    goto done;
                }
                while (p) {
                    if (memcmp(p->username, client->username, USERNAME_FIELD_WIDTH)) {
                        instruction->command[0] = ERROR;
                        strcpy(instruction->message, "username taken");
                        if (send(client->fd, buf, MAXDATASIZE, 0) == -1) {
                            perror("send");
                        }
                        goto close_connection;
                    }                            
                }
                memcpy(client->username, instruction->username, USERNAME_FIELD_WIDTH);
                p = client_list.head;
                while (p) {
                    if (send(p->fd, buf, MAXDATASIZE, 0)) {
                        perror("send");
                    }
                }
                break;
                
            case EXIT:
                goto close_connection;
                break;
                
            default:
                printf("server: ill-formed instruction from fd %d\n", client->fd);
                break;
        }
        
        done:
        
        continue;
        
        
    }
    
    close_connection:
    
    //remove client from client_list
    pthread_mutex_lock(&(client_list.mutex));
    if (client->next) client->next->prev = client->prev;
    if (client->prev) client->prev->next = client->next;
    if (client_list.head == client) client_list.head = client->next;
    pthread_mutex_unlock(&(client_list.mutex));
    
    //close fd
    close((tmpfd = client->fd));
    
    printf("server: client handler on fd %d returned %d", tmpfd, rv);
    pthread_exit((void *)(long)rv);
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

    while (1) {  // main accept() loop
        client_t *client = malloc(sizeof(client_t));
        *client = (client_t) {.next = NULL, .prev = NULL, .username = '\0'};        

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
        if (client_list.head) {
            client->next = client_list.head;
            client_list.head->prev = client;
        }
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