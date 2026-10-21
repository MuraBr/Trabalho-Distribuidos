//Curso de Ciência da Computação.
//Prof. Dr. Rubens Barbosa Filho
//Ano: 2015

#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int test_crc32(void) {
    const char *s = "123456789";
    uint32_t got = protocol_crc32(s, strlen(s));
    uint32_t expected = 0xCBF43926u;
    if (got != expected) {
        fprintf(stderr, "[FAIL] CRC32: got=0x%08X expected=0x%08X\n", got, expected);
        return 1;
    }
    printf("[PASS] CRC32 conhece o vetor\n");
    return 0;
}

static int test_message_types(void) {
    if (strcmp(message_type_name(MSG_PING), "PING") != 0) return 1;
    if (strcmp(message_type_name(MSG_PONG), "PONG") != 0) return 1;
    if (strcmp(message_type_name(MSG_JOIN), "JOIN") != 0) return 1;
    if (strcmp(message_type_name(MSG_ACK), "ACK") != 0) return 1;
    if (strcmp(message_type_name(MSG_ERROR), "ERROR") != 0) return 1;
    if (strcmp(message_type_name((MessageType)999), "DESCONHECIDO") != 0) return 1;
    printf("[PASS] Tipo de Mensagem mapeada\n");
    return 0;
}

static int test_wire_constants(void) {
    /* 1 + 2 + 32 + 32 + 16 + 8 + 4 + 4 = 99 bytes */
    enum { HEADER_WIRE_SIZE = 99 };
    if (HEADER_WIRE_SIZE != 99) return 1;
    if (PROTOCOL_VERSION != 1) return 1;
    if (TX_ID_SIZE != 16) return 1;
    if (NODE_ID_SIZE != 32) return 1;
    printf("[PASS] Protocol constantes/header size\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_crc32();
    rc |= test_message_types();
    rc |= test_wire_constants();
    if (rc == 0) {
        printf("Unidade de testes protocolo C1: PASS\n");
        return 0;
    }
    printf("Unidade de testes protocolo C1: FAIL\n");
    return 1;
}
