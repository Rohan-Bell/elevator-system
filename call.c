#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <errno.h>

//Ensure we are using same ports as the assignment Port 3000 local ip of 127.0.0.1
#define CONTROLLER_PORT 3000
#define CONTROLLER_IP "127.0.0.1"

#define MAX_FLOOR 999 //regular
#define MIN_FLOOR 99 //basement

void recv_looped(int fd, void *buf, size_t sz) {
    char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t received = read(fd, ptr, remain);
        //error determined by -1
        if (received == -1) {
            perror("read()");
            exit(1);
        }
        if (received == 0) { //The connection is closed by the other end
            fprintf(stderr, "Connection closed unexpectedly\n");
            exit(1);
        }
        //We can move the pointer forwad and decrease remainng bytes needed 
        ptr += received; 
        remain -= received;
    }
}

void send_looped(int fd, const void *buf, size_t sz) {
    const char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t sent = write(fd, ptr, remain);
        if (sent == -1) {
            //Error when writing 
            perror("write()");
            exit(1);
        }
        ptr += sent;
        remain -= sent;
    }
}

char *receive_msg(int fd) {
    uint16_t nlen;
    recv_looped(fd, &nlen, sizeof(nlen));
    uint16_t len = ntohs(nlen); // To work out how much mem we need
    char *buf = malloc(len + 1); //Allocate the necessary memorey ( 1 is NT)
    if (buf == NULL) {
        perror("malloc()"); //We know it is malloc error because should be at least malloc(1)
        exit(1);
    }
    buf[len] = '\0'; //End the string (null terminate)
    recv_looped(fd, buf,len);
    return buf;
}

void send_message(int fd, const char *buf) {
    //Get length of string and convert big-endian (host to network)
    uint16_t len = htons(strlen(buf));
    //Lets send a 2-byte length prefix
    send_looped(fd, &len, sizeof(len));
    //sending the message
    send_looped(fd, buf, strlen(buf));
    
}


/*
Considerations: 
B1,2,3,4,5 (increase lower)
Floor can go up to 999
but as low as B99
no ground floor ... B2, B1, 1, 2 ...
*/
int validate_floor(const char* floor) {
    if (!floor || strlen(floor) == 0 || strlen(floor) > 3) {
        return 0; 
    }

    //Handle basement floors
    if (floor[0] == 'B') {
        //Check if remaining cahracters are digits
        for (int i = 1; floor[i] != '\0'; i++) {
            if(floor[i] < '0' || floor[i] > '9'){
                return 0; //Maybe we want to return something else
            }
        }

        //Convert to num
        int basement_num = atoi(floor + 1);
        return basement_num >= 1 && basement_num <= MIN_FLOOR; //Basedment is 1 to 99
    } else {
        //regular floor
        for(int i = 0; floor[i] != '\0'; i++) {
            if(floor[i] < '0' || floor[i] > '9'){
                //It is a character
                return 0;
            }
        }
        int floor_num = atoi(floor);
        return floor_num >= 1 && floor_num <= MAX_FLOOR;
    }
}

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "Invalid format");
        exit(1);
    }

    const char* source_floor = argv[1];
    const char* destination_floor = argv[2];
    //Check if floors are the same
    if(strcmp(source_floor, destination_floor) == 0) {
        printf("You are already on that floor!\n");
        exit(1);
    }

    //Validate teh floors
    if(!validate_floor(source_floor) || !validate_floor(destination_floor)) {
        printf("Invalid floor(s) specified.\n");
        exit(1);
    }
    //Create a socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("Unable to connect to elevator system.\n");
        exit(1);
    }
    //setup the server address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; //IPV4 not IPV6 as 127.0.0.1
    addr.sin_port = htons(CONTROLLER_PORT);
    const char *ip_address = CONTROLLER_IP;
    if (inet_pton(AF_INET, ip_address, &addr.sin_addr) == -1) {
        printf("Unable to connect to elevator system.\n");
        close (sockfd);
        exit(1);
    }

    //Connect to the controller
    if (connect(sockfd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        printf("Unable to connect to elevator system.\n");
        close(sockfd);
        exit(1);
    }

    //prepare to send CALL message
    char call_message[256];
    snprintf(call_message, sizeof(call_message), "CALL %s %s", source_floor, destination_floor);
    send_message(sockfd, call_message);
    
    //Receive the response
    char *response = receive_msg(sockfd);

    //Process the resposne 
    if(strncmp(response, "CAR ", 4) == 0) {
        //print tje server response
        printf("Car %s is arriving.\n", response + 4);
    } else if(strcmp(response,"UNAVAILABLE") == 0) {
        printf("Sorry, no car is available to take this request.\n");
    } else {
        printf("Unable to connect to elevator system.\n");
    }
    free(response); //free the memeory up
    if(close(sockfd) == -1) {
        perror("close()");
        exit(1);
    }
    return 0;


}