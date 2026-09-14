#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if(argc < 2 || argc == 3 || argc > 4)
    {
	fprintf(stderr, "Uso: %s <porta_local> [ip_remoto] [porta_remota]\n", argv[0]);
	return 1;
    }

    int porta_local = atoi(argv[1]);
    char *ip_remoto = argv[2];
    int porta_remota = atoi(argv[3]);

    printf("%d %s %d\n", porta_local, ip_remoto, porta_remota);

    return 0;
}
