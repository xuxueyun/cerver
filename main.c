#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "hashmap.h"

#define KEY_MAX_LENGTH (256)
#define KEY_PREFIX ("bladerunner")
#define KEY_COUNT (1024*1024)

#define CHUNK_SIZE 512

void *connection_handler(void *);

SSL_CTX *ctx;
HMAP_PTR routeMap;

off_t fsize(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return -1;
}

void init_openssl()
{
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

SSL_CTX *create_context()
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = SSLv23_server_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) {
	perror("Unable to create SSL context");
	ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    return ctx;
}

void configure_context(SSL_CTX *ctx)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);

    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }
}

void construct_routes() {
  routeMap = hmap_create(0,1.0);
  hmap_set(routeMap, "/", "./build/main.html");
  hmap_set(routeMap, "/main.js", "./build/main.js");
  //add routes...
}

void intHandler() {
    hmap_free(routeMap, 0);
    SSL_CTX_free(ctx);
    EVP_cleanup();
}

int main(int argc, char *argv[])
{
    signal(SIGINT, intHandler);
    int socket_desc, client_sock, c, *new_sock;
    struct sockaddr_in server, client;

    init_openssl();
    ctx = create_context();
    configure_context(ctx);

    /* set the local certificate from CertFile */
    SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM);
    /* set the private key from KeyFile */
    SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM);
    /* verify private key */
    if (!SSL_CTX_check_private_key(ctx))
    {
        abort();
    }

    construct_routes();

    socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_desc == -1)
    {
        printf("Could not create socket");
    }

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr("0.0.0.0");
    server.sin_port = htons(443);

    if (bind(socket_desc, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        perror("bind failed. Error");
        return 1;
    }
    listen(socket_desc, 3);

    c = sizeof(struct sockaddr_in);
    while ((client_sock = accept(socket_desc, (struct sockaddr *)&client, (socklen_t *)&c)))
    {
        pthread_t thread;
        new_sock = malloc(1);
        *new_sock = client_sock;

        if (pthread_create(&thread, NULL, connection_handler, (void *)new_sock) < 0)
        {
            perror("could not create thread");
            return 1;
        }
    }

    SSL_CTX_free(ctx);
    EVP_cleanup();

    if (client_sock < 0)
    {
        perror("accept failed");
        return 1;
    }
    return 0;
}

void *connection_handler(void *socket_desc)
{
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, *(int *)socket_desc);
    int sock = *(int *)socket_desc;
    int n;
    char rbuff[10000], sbuff[10000];
    memset(rbuff, '\0', sizeof(rbuff));
    memset(sbuff, '\0', sizeof(sbuff));

    if (SSL_accept(ssl) <= 0)
    {
        ERR_print_errors_fp(stderr);
    }
    else
    {
    }

    while ((n = SSL_read(ssl, rbuff, sizeof(rbuff))) > 0)
    {
        printf("%s\n", rbuff);
        int len = strstr(rbuff, "HTTP") - strstr(rbuff, "/") - 1;
        char reqRoute[len + 1];
        memset(reqRoute, '\0', sizeof(reqRoute));
        strncpy(reqRoute, &rbuff[strcspn(rbuff, " ") + 1], len);
        printf("%s %i\n", reqRoute, strlen(reqRoute));
        char *fileName = (char *)malloc(1000 * sizeof(char));
        memset(fileName, '\0', sizeof(fileName));
        char *tmp = hmap_get(routeMap, reqRoute);
        if(tmp == NULL) {
          strcat(sbuff, "HTTP/1.1 400 Not Found\r\n");
          strcat(sbuff, "Connection: Closed\r\n");
          strcat(sbuff, "Content-Length: 35\r\n\r\n");
          strcat(sbuff, "<html><body>Not Found</body></html>");
          printf("Bad Request\n");
        } else {
          strcpy(fileName, tmp);
          int fileSize = fsize(fileName);
          printf("Here2 %i %s\n", fileSize, fileName);
          char fileContent[fileSize];
          memset(fileContent, '\0', sizeof(fileContent));
          printf("%s %i\n", (char *)fileName, fileSize);
          FILE *fp = fopen(fileName, "r");
          fread(fileContent, 1, fileSize, fp);
          fclose(fp);

          strcat(sbuff, "HTTP/1.1 200 OK\r\n");
          strcat(sbuff, "Content-Length: ");
          sprintf(sbuff, "%s%i", sbuff, fileSize);
          strcat(sbuff, "\r\nConnection: Closed\r\n\r\n");
          strcat(sbuff, fileContent);
        }
        SSL_write(ssl, sbuff, strlen(sbuff));
        memset(rbuff, '\0', sizeof(rbuff));
        memset(sbuff, '\0', sizeof(sbuff));
    }

    SSL_free(ssl);
    close(sock);

    if (n == 0)
    {
        puts("Client Disconnected");
    }
    else
    {
        perror("recv failed");
    }
    return 0;
}