/* Entrada exclusiva do servidor; membership.c contém a API de membros. */
int superpeer_run(int argc, char **argv);

int main(int argc, char **argv)
{
    return superpeer_run(argc, argv);
}
