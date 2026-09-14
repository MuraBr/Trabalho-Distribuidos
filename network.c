#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

int network_create_server(uint16_t, int);
int network_accept_client(int);
int network_connect(const char *, uint16_t);
ssize_t network_send_all(int, const char *, size_t);
void network_recv_exact();
void network_shutdown();

int network_create_server(uint16_t porta, int backlog)
{
    int sock;
    struct sockaddr_in address;
    int opt = 1;

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
	perror("socket failed");
        return -1;
    }
    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("setsockopt");
	close(sock);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(porta);

    if(bind(sock, (struct sockaddr*)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
	close(sock);
	return -1;
    }
    if(listen(sock, backlog) < 0)
    {
        perror("listen");
	close(sock);
	return -1;
    }

    return sock;
}

int network_accept_client(int server_fd)
{
    int client_fd;
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    memset(&client, 0, sizeof(client));

    if((client_fd = accept(server_fd, (struct sockaddr*)&client, &client_len)) < 0)
    {
        perror("accept");
        return -1;
    }

    return client_fd;
}

int network_connect(const char *ip, uint16_t porta)
{
    int sock;
    struct sockaddr_in server;

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
	perror("socket failed");
        return -1;
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(porta);

    switch(inet_pton(AF_INET, ip, &server.sin_addr))
    {
    case 1:
    {
	break;
    }
    case 0:
    {
	fprintf(stderr, "O endereco IP nao eh valido!\n");
	close(sock);
	return -1;
    }
    case -1:
    {
	perror("inet_pton");
	close(sock);
	return -1;
    }
    }

    if(connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
	perror("connect");
	close(sock);
	return -1;
    }

    return sock;
}

ssize_t network_send_all(int sock, const char *buffer, size_t tam)
{
    const char *data = buffer;
    size_t total_sent = 0;

    while(total_sent < tam)
    {
	size_t sent_now = send(sock, data + total_sent, tam - total_sent, 0);
	total_sent += sent_now;

	if(sent_now > 0)
	{
	    total_sent += sent_now;
	}
	else if(sent_now == -1 && errno == EINTR)
	{
	    sent_now = send(sock, data + total_sent, tam - total_sent, 0);
	}
    }
    return (ssize_t)total_sent;
}

void network_recv_exact()
{
}

void network_shutdown()
{
}
