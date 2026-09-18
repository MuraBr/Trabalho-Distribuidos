#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Cria servidor TCP IPv4 em todas as interfaces; configura reutilização de endereço, bind e fila listen. */
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

/* Aceita uma conexão e retorna seu descritor; erros, inclusive EINTR, são tratados pelo chamador. */
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

/* Conecta a um IPv4 numérico e porta; fecha o socket se a validação ou conexão falhar. */
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

/* Repete send para completar o buffer e retoma após EINTR; retorna total enviado ou -1. */
ssize_t network_send_all(int sock, const void *buffer, size_t tam)
{
    const char *data = buffer;
    size_t total_sent = 0;

    while(total_sent < tam)
    {
        ssize_t sent_now = send(sock, data + total_sent, tam - total_sent, 0);
        if(sent_now > 0)
        {
            total_sent += (size_t)sent_now;
            continue;
        }
        if(sent_now == -1)
        {
            if(errno == EINTR)
            {
                continue;
            }
            perror("send");
            return -1;
        }

        fprintf(stderr, "send retornou zero bytes\n");
        return -1;
    }
    return (ssize_t)total_sent;
}

/* Acumula recv até o tamanho pedido; retorna total parcial no fechamento, zero sem dados ou -1 em erro. */
ssize_t network_recv_exact(int sock, void *buffer, size_t tam)
{
    size_t total_received = 0;
    char *data = buffer;

    while(total_received < tam)
    {
        ssize_t received_now = recv(sock, data + total_received, tam - total_received, 0);
        if(received_now > 0)
        {
            total_received += (size_t)received_now;
            continue;
        }
        if(received_now == 0)
        {
            return (ssize_t)total_received;
        }
        if(errno == EINTR)
        {
            continue;
        }
        perror("recv");
        return -1;
    }
    return (ssize_t)total_received;
}

/* Tenta encerrar os dois sentidos e sempre chama close; o retorno reflete o resultado de close. */
int network_shutdown(int sock)
{
    if(shutdown(sock, SHUT_RDWR) == -1)
    {
	    perror("shutdown");
    }
    if(close(sock) == -1)
    {
	    perror("close");
	    return -1;
    }
    return 0;
}
