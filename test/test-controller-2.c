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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <signal.h>

// Declarations
void recv_looped(int fd, void *buf, size_t sz);
void send_looped(int fd, const void *buf, size_t sz);
char *receive_msg(int fd);
void send_message(int fd, const char *buf);
int validate_floor(const char* floor);
int floor_to_int(const char *floor_str);
void int_to_floor(int floor_int, char *floor_str, size_t size);
void msg(const char *string);
SSL_CTX *init_ssl_client_context(void);
void send_looped_ssl(SSL *ssl, const void *buf, size_t sz);
char *receive_msg_ssl(SSL *ssl);
void send_message_ssl(SSL *ssl, const char *buf);

// Tester for controller (multiple cars)

/*
    Alpha  Beta   Gamma
  5               -----
  4 -----          | |
  3  | |           | |
  2  | |          -----
  1 -----  -----
 B1         | |
 B2         | |
 B3        -----

*/

#define DELAY 50000 // 50ms
#define MILLISECOND 1000 // 1ms

pid_t controller(void);
SSL *connect_to_controller(SSL_CTX *ssl_ctx);
void test_call(SSL_CTX *ssl_ctx, const char *, const char *);
void test_recv(SSL *ssl, const char *);
void cleanup(pid_t);

int main()
{
  SSL_CTX *ssl_ctx = init_ssl_client_context();
  if (!ssl_ctx) {
    fprintf(stderr, "Failed to initialize SSL client context\n");
    exit(1);
  }

  pid_t p;
  p = controller();
  usleep(DELAY);

  // Register a car that can take floors 1 to 4
  SSL *alpha = connect_to_controller(ssl_ctx);
  send_message_ssl(alpha, "CAR Alpha 1 4");
  send_message_ssl(alpha, "STATUS Closed 1 1");

  // Register a car that can take floors B3 to 1
  SSL *beta = connect_to_controller(ssl_ctx);
  send_message_ssl(beta, "CAR Beta B3 1");
  send_message_ssl(beta, "STATUS Closed B3 B3");

  // Register a car that can take floors 2 to 5
  SSL *gamma = connect_to_controller(ssl_ctx);
  send_message_ssl(gamma, "CAR Gamma 2 5");
  send_message_ssl(gamma, "STATUS Closed 2 2");

  usleep(DELAY);

  // Call an elevator to go from 1 to 3. Only Alpha can do this
  test_call(ssl_ctx, "CALL 1 3", "CAR Alpha");
  test_recv(alpha, "FLOOR 1");

  // Call an elevator to go from 1 to B2. Only Beta can do this
  test_call(ssl_ctx, "CALL 1 B2", "CAR Beta");
  test_recv(beta, "FLOOR 1");

  // Call an elevator to go from 3 to 5. Only Gamma can do this
  test_call(ssl_ctx, "CALL 3 5", "CAR Gamma");
  test_recv(gamma, "FLOOR 3");

  // Call an elevator to go from 1 to 5. No car can do this
  test_call(ssl_ctx, "CALL 1 5", "UNAVAILABLE");

  // Call an elevator to go from B3 to 3. No car can do this
  test_call(ssl_ctx, "CALL B3 3", "UNAVAILABLE");

  cleanup(p);

  SSL_free(alpha);
  SSL_free(beta);
  SSL_free(gamma);
  SSL_CTX_free(ssl_ctx);
  
  printf("\nTests completed.\n");
}

void test_call(SSL_CTX *ssl_ctx, const char *sendmsg, const char *expectedreply)
{
  SSL *fd = connect_to_controller(ssl_ctx);
  send_message_ssl(fd, sendmsg);
  char *reply = receive_msg_ssl(fd);
  msg(expectedreply);
  printf("%s\n", reply);
  free(reply);
  SSL_free(fd);
}

void test_recv(SSL *ssl, const char *t)
{
  char *m = receive_msg_ssl(ssl);
  msg(t);
  printf("RECV: %s\n", m);
  free(m);
}

SSL *connect_to_controller(SSL_CTX *ssl_ctx)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sockaddr;
  memset(&sockaddr, 0, sizeof(sockaddr));
  sockaddr.sin_family = AF_INET;
  sockaddr.sin_port = htons(3000);
  sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1)
  {
    perror("connect()");
    exit(1);
  }

  SSL *ssl = SSL_new(ssl_ctx);
  if (!ssl) {
    close(fd);
    exit(1);
  }
  SSL_set_fd(ssl, fd);
  if (SSL_connect(ssl) <= 0) {
    ERR_print_errors_fp(stderr);
    SSL_free(ssl);
    close(fd);
    exit(1);
  }
  return ssl;
}

void cleanup(pid_t p)
{
  // Terminate with SIGINT to allow server to clean up
  kill(p, SIGINT);
}

pid_t controller(void)
{
  pid_t pid = fork();
  if (pid == 0) {
    chdir("..");
    execlp("./controller", "./controller", NULL);
  }

  return pid;
}
