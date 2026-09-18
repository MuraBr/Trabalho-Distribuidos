#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Cria servidor TCP IPv4 em todas as interfaces; configura reutilização de endereço, bind e fila listen. */
int network_create_server(uint16_t porta, int backlog);
/* Aceita uma conexão e retorna seu descritor; erros, inclusive EINTR, são tratados pelo chamador. */
int network_accept_client(int server_fd);
/* Conecta a um IPv4 numérico e porta; fecha o socket se a validação ou conexão falhar. */
int network_connect(const char *ip, uint16_t porta);
/* Repete send para completar o buffer e retoma após EINTR; retorna total enviado ou -1. */
ssize_t network_send_all(int sock, const void *buffer, size_t tam);
/* Acumula recv até o tamanho pedido; retorna total parcial no fechamento, zero sem dados ou -1 em erro. */
ssize_t network_recv_exact(int sock, void *buffer, size_t tam);
/* Tenta encerrar os dois sentidos e sempre chama close; o retorno reflete o resultado de close. */
int network_shutdown(int sock);

#endif /* NETWORK_H */
