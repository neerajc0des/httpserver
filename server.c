#include <stdio.h>  // standard i/o
#include <stdlib.h> // mem alloc, exit()
#include <unistd.h> // close, fork, read, write
#include <errno.h>  // to set error codes
#include <string.h> // string mem, like memset, strlen
#include <sys/types.h>  // for system types like size_t
#include <sys/socket.h> // socket, bind, listen, accept, send, recv
#include <netinet/in.h> // for structures like sockaddr_in, or sockaddr_in6
#include <netdb.h>  // for getaddrinfo() and dns resolution
#include <arpa/inet.h>  // ip conversion inet_pton or inet_ntop
#include <sys/wait.h>   // wait for child processes
#include <signal.h> // for singnal(), and SIGCHLD


#define PORT "8080" // port where seerver will listen
#define BACKLOG 10  // num of connection reqs in the queue
#define MAXDATASIZE 4096 // size for the buffer to accept req

// FLOW
// recv the http request->parse the req line->decide the response->send the http response->close


// function to handle zombie processes,
// send SIGCHLD(signal number) to the parent process.
void sigchild_handler(int s){
    (void) s;   // intentionally ignoring s, to avoid compiler warnings
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG)>0);
    //waitpid cleans up reaps(terminated child processes)
    //-1 means wait for any child process
    //NULL means, dont care about child's status
    //Without this loop zombie processes might accumulate

    errno = saved_errno;    //restoring the errno that might have changed in the loop above
}

// to get socket addr, we are returning the pointer to the ip address
void *get_in_addr(struct sockaddr *sa){
    if(sa->sa_family==AF_INET){
        return &(((struct sockaddr_in*) sa) -> sin_addr);
    }
    else{
        return &(((struct sockaddr_in6*) sa) -> sin6_addr);
    }
}


struct httpres{
    int status_code;    
    const char *status_text;
    const char *content_type;  // for text/img
    size_t content_len; // length of the content
    unsigned char *body;// body to send in the response can contain img
    int close_connection;
};

int make_headers(char *header_buf, size_t header_size, struct httpres *res){
    return snprintf(header_buf, header_size,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Connection: %s\r\n"      // sending one response at a time and closing the connection after it
                    "Content-Length: %zu\r\n" // %z for the dtype of size_t and %u stands for unsigned int
                    "\r\n",
                    res->status_code,
                    res->status_text,
                    res->content_type,
                    res->close_connection?"close":"keep-alive",  
                    res->content_len
                );
}

const char *get_content_type(const char *f_name){
    const char *lastDot = strrchr(f_name, '.');

    if(strcmp(lastDot, ".html")==0)
        return "text/html";
    if(strcmp(lastDot, ".jpg")==0 || strcmp(lastDot, ".jpeg")==0)
        return "image/jpeg";
    if(strcmp(lastDot, ".png")==0)
        return "image/png";
}

//listen on socketfd, and all new connections on newfd;
// getaddrinfo -> socket -> bind -> listen -> accept
int main(){
    int sockfd, newfd;
    struct addrinfo hints, *servinfo, *p;  
    //hints to store the nodes, servinfo pointing the head,
    // p is to iterate over the hints linked list
    struct sockaddr_storage client_addr; //clients addr info
    socklen_t sin_size;   //size of socket structure
    struct sigaction sa;    //struct for signal handling
    int yes=1;  // used with setsockopt()
    char s[INET6_ADDRSTRLEN];   // taking size of v6(largest)
    int rv;     // return value of getaddrinfo
    char req_buf[MAXDATASIZE];
    int bytes;

    memset(&hints, 0, sizeof hints);    //empltying the hints linked list
    hints.ai_family = AF_INET;  //IPv4
    hints.ai_socktype = SOCK_STREAM;    //TCP stream
    hints.ai_flags = AI_PASSIVE;    //using machine ip to act as a server

    // checking for errors
    if((rv = getaddrinfo(NULL, PORT, &hints, &servinfo))!=0){   //if its not 0 then there is an err
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // if there are no errors, then we loop through the results and bind the first one
    for(p=servinfo; p!=NULL; p=p->ai_next){
        //creating a socket
        if((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol))==-1){
            perror("server: socket");
            continue;
        }

        // configuring socket options
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
            perror("setsockopt");
            exit(1);
        }

        //binding the connections
        if(bind(sockfd, p->ai_addr, p->ai_addrlen)==-1){
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;  // break as soon as we connects to first available ip
    }
    freeaddrinfo(servinfo);

    if(p==NULL){
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }
    //listen call
    if(listen(sockfd, BACKLOG)==-1){
        perror("listen");
        exit(1);
    }

    // signal call to handle zombie processes
    sa.sa_handler = sigchild_handler;   // to reap all zombie processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGCHLD, &sa, NULL)==-1){
        perror("sigaction");
        exit(1);
    }

    printf("server: waiting for connections...\n");

    // main loop to accept connections
    while(1){
        sin_size = sizeof client_addr;
        newfd = accept(sockfd, (struct sockaddr*)&client_addr, &sin_size);
        if(newfd==-1){
            perror("accept");
            exit(1);
        }

        inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), s, sizeof s);
        printf("server: got connection from %s\n", s);

        // creating child processes for each connection
        if(!fork()){    //here we go with the child baby!
            // closing the parent socket so that there
            // isnt creating any interference between sockfd and newfd
            close(sockfd);

            // recieving the request from the client
            if((bytes = recv(newfd, req_buf, sizeof(req_buf)-1, 0))==-1){
                perror("recv");
            }
            req_buf[bytes] = '\0';
            if(bytes>=0){
                printf("request recieved:\n%s\n", req_buf);
            }

            // stripping the recieved request
            char method[8];
            char path[256];
            char version[16];

            // read text from the request and store them in the above buffers
            // %s read until space, %255s read upto 255 characters, %15s read upto 15characters
            sscanf(req_buf, "%s %255s %15s", method, path, version);
            //     read_from, format      , buffers to store in ...

            char *f_path;
            if(strcmp(path, "/")==0){
                f_path = "index.html";
            }
            else{
                f_path = path + 1;  // to ignore the '/'in the start
            }

            // declaring our response buffer;
            // char res[BUFSIZ];
            char header_buf[BUFSIZ];

            // creating a file descriptor
            FILE *fp = fopen(f_path, "rb");
            if(!fp){
                perror("fopen");
                return 1;
            }

            // getting that filesize
            fseek(fp, 0, SEEK_END); // moving cursor to eof
            size_t f_size = ftell(fp);  // getting the position
            fseek(fp, 0, SEEK_SET); // rewinding to the start

            // declaring our file buffer
            char *httpbody = malloc(f_size);

            // checking if memory allocation failed, man!
            if (!httpbody) {
                perror("malloc");
                fclose(fp);
                return 1;
            }

            // reading the file into our file buffer
            if(fread(httpbody, 1, f_size, fp) != f_size){   //each item is 1byte long
                // error handling
                perror("fread");
                free(httpbody);
                fclose(fp);
                return 1;
            }
            fclose(fp);
            
            // structuring the http response 
            struct httpres response;
            response.status_code = 200;
            response.status_text = "OK";
            response.content_type = get_content_type(f_path);
            response.content_len = f_size;
            response.close_connection = 1;
            response.body = (unsigned char *)httpbody;

            // making headers
            int header_len = make_headers(header_buf, sizeof(header_buf), &response);

            // sending the http headers separately
            if(send(newfd, header_buf, header_len, 0)==-1){
                perror("header send");
            }
            // sending the http body
            if(send(newfd, response.body, response.content_len, 0)==-1){
                perror("body send");
            }
            
            free(httpbody);
            close(newfd);
            exit(0);
        }
        close(newfd);
    }


    return 0;


}