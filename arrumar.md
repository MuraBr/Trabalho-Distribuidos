# Network.c

Melhorias opcionais
- repetir accept quando o erro for EINTR;
- validar porta e backlog;
- usar const void * em network_send_all, para aceitar qualquer buffer binário;
- usar getaddrinfo se quiser aceitar nomes como localhost, pois atualmente network_connect aceita apenas IP numérico IPv4.


## network_send_all

Mais adiante, considere usar MSG_NOSIGNAL ou tratar SIGPIPE, porque o processo pode ser encerrado pelo sistema se tentar enviar para um peer que já fechou a conexão.

## network_recv_exact

retorno == tam → recebeu tudo
retorno == 0   → conexão fechada antes de receber dados
0 < retorno < tam → conexão fechada no meio da mensagem
retorno == -1  → erro

## network_shutdown

Defina se uma falha em shutdown deve fazer a função retornar erro. Em muitos casos, o close é a operação essencial, mas isso deve ficar documentado.
