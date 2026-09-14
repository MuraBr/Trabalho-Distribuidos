#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "node.h"
#include "superpeer.h"

static void test_node_id_is_deterministic(void)
{
    static const uint8_t uuid[NODE_UUID_SIZE] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    NodeConfig config;
    NodeID node_id;
    char node_id_hex[NODE_ID_HEX_SIZE];

    assert(node_config_init_with_uuid(&config, "127.0.0.1", 8080, uuid) == 0);
    assert(node_compute_id(&config, &node_id) == 0);
    assert(node_id_to_hex(&node_id, node_id_hex, sizeof(node_id_hex)) == 0);
    assert(strcmp(node_id_hex,
                  "9bc987770f04725d8afac342e3eaa1d2"
                  "ef4bc1f4587841f8dd4b8b0fca39f84f") == 0);
}

static void test_node_records_process_and_round_trips_id(void)
{
    static const uint8_t uuid[NODE_UUID_SIZE] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x00
    };
    NodeConfig config;
    Node node;
    NodeID parsed_id;
    char node_id_hex[NODE_ID_HEX_SIZE];
    char too_small[NODE_ID_HEX_SIZE - 1U];

    assert(node_config_init_with_uuid(&config, "::1", 9000, uuid) == 0);
    assert(node_init(&node, &config) == 0);
    assert(node_get_process_id(&node) == getpid());
    assert(node.role == NODE_ROLE_PEER);
    assert(node_validate(&node) == 0);
    assert(node_id_to_hex(&node.id, node_id_hex, sizeof(node_id_hex)) == 0);
    assert(node_id_from_hex(&parsed_id, node_id_hex) == 0);
    assert(node_id_equal(&node.id, &parsed_id));

    errno = 0;
    assert(node_id_to_hex(&node.id, too_small, sizeof(too_small)) == -1);
    assert(errno == ENOSPC);
}

static void test_node_rejects_invalid_configuration(void)
{
    static const uint8_t uuid[NODE_UUID_SIZE] = {0};
    NodeConfig config;
    NodeConfig malformed_config;

    assert(node_config_init_with_uuid(&config, "not-an-ip", 9000, uuid) == -1);
    assert(node_config_init_with_uuid(&config, "127.0.0.1", 0, uuid) == -1);
    assert(node_config_init(&config, "127.0.0.1", 9001) == 0);
    assert(node_config_validate(&config) == 0);

    memset(&malformed_config, 'x', sizeof(malformed_config));
    malformed_config.port = 9002U;
    assert(node_config_validate(&malformed_config) == -1);
}

static void test_superpeer_registers_and_finds_members(void)
{
    static const uint8_t local_uuid[NODE_UUID_SIZE] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
    };
    static const uint8_t member_uuid[NODE_UUID_SIZE] = {
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };
    SuperPeerConfig config;
    SuperPeer *superpeer = NULL;
    Node member;
    Node local_node;
    SuperPeerMember found_member;
    NodeConfig member_config;

    assert(superpeer_config_init_with_uuid(&config,
                                           "127.0.0.1",
                                           7000,
                                           local_uuid) == 0);
    config.initial_member_capacity = 1U;
    assert(superpeer_create(&config, &superpeer) == 0);
    assert(superpeer_get_node(superpeer, &local_node) == 0);
    assert(local_node.role == NODE_ROLE_SUPERPEER);
    assert(node_validate(&local_node) == 0);
    assert(superpeer_member_count(superpeer) == 1U);
    assert(superpeer_find_member(superpeer,
                                 &local_node.id,
                                 &found_member) == 0);
    assert(found_member.state == SUPERPEER_MEMBER_ALIVE);

    assert(node_config_init_with_uuid(&member_config,
                                      "127.0.0.2",
                                      7001,
                                      member_uuid) == 0);
    assert(node_init(&member, &member_config) == 0);
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_MEMBER_ADDED);
    assert(superpeer_member_count(superpeer) == 2U);
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_MEMBER_UPDATED);
    assert(superpeer_member_count(superpeer) == 2U);
    assert(superpeer_find_member(superpeer, &member.id, &found_member) == 0);
    assert(node_id_equal(&found_member.node.id, &member.id));
    assert(found_member.node.config.port == 7001U);

    superpeer_destroy(superpeer);
}

static void test_superpeer_rejects_inconsistent_nodes_and_unregisters(void)
{
    static const uint8_t local_uuid[NODE_UUID_SIZE] = {1};
    static const uint8_t member_uuid[NODE_UUID_SIZE] = {2};
    SuperPeerConfig config;
    SuperPeer *superpeer = NULL;
    Node member;
    Node local_node;
    NodeConfig member_config;
    NodeID unknown_id = {{0}};

    assert(superpeer_config_init_with_uuid(&config,
                                           "127.0.0.1",
                                           7100,
                                           local_uuid) == 0);
    assert(superpeer_create(&config, &superpeer) == 0);
    assert(superpeer_get_node(superpeer, &local_node) == 0);
    errno = 0;
    assert(superpeer_unregister_node(superpeer, &local_node.id) == -1);
    assert(errno == EPERM);
    assert(superpeer_member_count(superpeer) == 1U);
    assert(node_config_init_with_uuid(&member_config,
                                      "127.0.0.2",
                                      7101,
                                      member_uuid) == 0);
    assert(node_init(&member, &member_config) == 0);

    member.id.bytes[0] ^= 0xffU;
    assert(superpeer_register_node(superpeer, &member) ==
           SUPERPEER_REGISTER_ERROR);
    assert(superpeer_member_count(superpeer) == 1U);

    assert(node_init(&member, &member_config) == 0);
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_MEMBER_ADDED);
    assert(superpeer_is_registered(superpeer, &member.id));
    assert(superpeer_unregister_node(superpeer, &member.id) == 0);
    assert(!superpeer_is_registered(superpeer, &member.id));
    assert(superpeer_member_count(superpeer) == 1U);
    assert(superpeer_unregister_node(superpeer, &unknown_id) == -1);

    superpeer_destroy(superpeer);
}

enum
{
    REGISTRATION_THREAD_COUNT = 4,
    REGISTRATIONS_PER_THREAD = 25
};

typedef struct
{
    SuperPeer *superpeer;
    unsigned int thread_number;
    int failed;
} RegistrationContext;

static void *register_members(void *argument)
{
    RegistrationContext *context = argument;
    unsigned int i;

    for (i = 0U; i < REGISTRATIONS_PER_THREAD; ++i)
    {
        uint8_t uuid[NODE_UUID_SIZE] = {0};
        NodeConfig node_config;
        Node node;
        uint16_t port = (uint16_t)(7200U +
                                   context->thread_number * 100U + i);

        uuid[0] = (uint8_t)context->thread_number;
        uuid[1] = (uint8_t)i;
        if (node_config_init_with_uuid(&node_config,
                                       "127.0.0.1",
                                       port,
                                       uuid) == -1 ||
            node_init(&node, &node_config) == -1 ||
            superpeer_register_node(context->superpeer, &node) !=
                SUPERPEER_MEMBER_ADDED)
        {
            context->failed = 1;
            return NULL;
        }
    }
    return NULL;
}

static void test_superpeer_members_are_thread_safe(void)
{
    static const uint8_t local_uuid[NODE_UUID_SIZE] = {3};
    SuperPeerConfig config;
    SuperPeer *superpeer = NULL;
    RegistrationContext contexts[REGISTRATION_THREAD_COUNT];
    pthread_t threads[REGISTRATION_THREAD_COUNT];
    unsigned int i;

    assert(superpeer_config_init_with_uuid(&config,
                                           "127.0.0.1",
                                           7300,
                                           local_uuid) == 0);
    config.initial_member_capacity = 1U;
    assert(superpeer_create(&config, &superpeer) == 0);

    for (i = 0U; i < REGISTRATION_THREAD_COUNT; ++i)
    {
        contexts[i].superpeer = superpeer;
        contexts[i].thread_number = i;
        contexts[i].failed = 0;
        assert(pthread_create(&threads[i], NULL, register_members, &contexts[i]) == 0);
    }
    for (i = 0U; i < REGISTRATION_THREAD_COUNT; ++i)
    {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(contexts[i].failed == 0);
    }

    assert(superpeer_member_count(superpeer) ==
           1U + REGISTRATION_THREAD_COUNT * REGISTRATIONS_PER_THREAD);
    superpeer_destroy(superpeer);
}

int main(void)
{
    test_node_id_is_deterministic();
    test_node_records_process_and_round_trips_id();
    test_node_rejects_invalid_configuration();
    test_superpeer_registers_and_finds_members();
    test_superpeer_rejects_inconsistent_nodes_and_unregisters();
    test_superpeer_members_are_thread_safe();
    puts("node/superpeer tests: ok");
    return 0;
}
