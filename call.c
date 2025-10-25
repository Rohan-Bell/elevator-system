#include "shared.h"



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

    // Initialize SSL context
    SSL_CTX *ssl_ctx = init_ssl_client_context();
    if (!ssl_ctx) {
        printf("Unable to initialize SSL.\n");
        exit(1);
    }

    //Create a socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("Unable to connect to elevator system.\n");
        SSL_CTX_free(ssl_ctx);
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
        SSL_CTX_free(ssl_ctx);
        exit(1);
    }

    //Connect to the controller
    if (connect(sockfd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        printf("Unable to connect to elevator system.\n");
        close(sockfd);
        SSL_CTX_free(ssl_ctx);
        exit(1);
    }

    // Create SSL and connect
    SSL *ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        printf("Unable to create SSL.\n");
        close(sockfd);
        SSL_CTX_free(ssl_ctx);
        exit(1);
    }
    SSL_set_fd(ssl, sockfd);
    if (SSL_connect(ssl) <= 0) {
        printf("Unable to establish SSL connection.\n");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(sockfd);
        SSL_CTX_free(ssl_ctx);
        exit(1);
    }

    //prepare to send CALL message
    char call_message[256];
    snprintf(call_message, sizeof(call_message), "CALL %s %s", source_floor, destination_floor);
    send_message_ssl(ssl, call_message);
    
    //Receive the response
    char *response = receive_msg_ssl(ssl);

    //Process the resposne 
    if(response != NULL && strncmp(response, "CAR ", 4) == 0) {
        //print tje server response
        printf("Car %s is arriving.\n", response + 4);
    } else if(response != NULL && strcmp(response,"UNAVAILABLE") == 0) {
        printf("Sorry, no car is available to take this request.\n");
    } else {
        printf("Unable to connect to elevator system.\n");
    }
    if (response) free(response); //free the memeory up
    SSL_free(ssl);
    if(close(sockfd) == -1) {
        perror("close()");
        SSL_CTX_free(ssl_ctx);
        exit(1);
    }
    SSL_CTX_free(ssl_ctx);
    return 0;


}