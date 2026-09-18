#define _POSIX_C_SOURCE 200809L

#include "superpeer.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct SuperPeer
{
    Node local_node;
    SuperPeerMember *members;
    size_t member_count;
    size_t member_capacity;
    pthread_mutex_t members_mutex;
};

/* Adquire mutex; converte erro de pthread para -1/errno. */
static int lock_members(SuperPeer *superpeer)
{
    int error = pthread_mutex_lock(&superpeer->members_mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}

/* Libera mutex e propaga eventual erro por errno. */
static int unlock_members(SuperPeer *superpeer)
{
    int error = pthread_mutex_unlock(&superpeer->members_mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}

/* Busca linear O(N); exige que o chamador já tenha adquirido o mutex. */
static int find_member_index_locked(const SuperPeer *superpeer, const NodeID *node_id, size_t *index)
{
    size_t i;

    for (i = 0U; i < superpeer->member_count; ++i)
    {
        if (node_id_equal(&superpeer->members[i].node.id, node_id))
        {
            if (index != NULL)
            {
                *index = i;
            }
            return 0;
        }
    }
    return -1;
}

/* Sob mutex, duplica capacidade com verificação de overflow; preserva ponteiro se realloc falhar. */
static int grow_members_locked(SuperPeer *superpeer)
{
    size_t new_capacity;
    SuperPeerMember *new_members;

    if (superpeer->member_count < superpeer->member_capacity)
    {
        return 0;
    }

    if (superpeer->member_capacity == 0U)
    {
        new_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    }
    else
    {
        if (superpeer->member_capacity > SIZE_MAX / 2U)
        {
            errno = ENOMEM;
            return -1;
        }
        new_capacity = superpeer->member_capacity * 2U;
    }

    if (new_capacity > SIZE_MAX / sizeof(*new_members))
    {
        errno = ENOMEM;
        return -1;
    }

    new_members = realloc(superpeer->members, new_capacity * sizeof(*new_members));
    if (new_members == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    superpeer->members = new_members;
    superpeer->member_capacity = new_capacity;
    return 0;
}

/* Copia nó e renova ALIVE/last_seen; não realiza heartbeat. */
static void set_member(SuperPeerMember *member, const Node *node)
{
    member->node = *node;
    member->state = SUPERPEER_MEMBER_ALIVE;
    member->last_seen = time(NULL);
}

/* Configura identidade conhecida e capacidade inicial padrão de 16 membros. */
int superpeer_config_init_with_uuid(SuperPeerConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE])
{
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_init_with_uuid(&config->node, ip, port, uuid) == -1)
    {
        return -1;
    }

    config->initial_member_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    return 0;
}

/* Configura identidade com UUID aleatório e capacidade padrão 16. */
int superpeer_config_init(SuperPeerConfig *config, const char *ip, uint16_t port)
{
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_init(&config->node, ip, port) == -1)
    {
        return -1;
    }

    config->initial_member_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    return 0;
}

/* Aloca tabela/mutex e autorregistra o Super Peer: contagem inicial 1. Desfaz alocações em falha. */
int superpeer_create(const SuperPeerConfig *config, SuperPeer **output)
{
    SuperPeer *superpeer;
    size_t initial_capacity;
    int mutex_error;
    Node local_node;

    if (output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *output = NULL;
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_validate(&config->node) == -1 || node_init(&local_node, &config->node) == -1)
    {
        return -1;
    }
    initial_capacity = config->initial_member_capacity == 0U ? SUPERPEER_DEFAULT_MEMBER_CAPACITY : config->initial_member_capacity;

    if (initial_capacity > SIZE_MAX / sizeof(SuperPeerMember))
    {
        errno = ENOMEM;
        return -1;
    }

    superpeer = calloc(1U, sizeof(*superpeer));
    if (superpeer == NULL)
    {
        return -1;
    }

    superpeer->members = calloc(initial_capacity, sizeof(*superpeer->members));
    if (superpeer->members == NULL)
    {
        free(superpeer);
        return -1;
    }

    mutex_error = pthread_mutex_init(&superpeer->members_mutex, NULL);
    if (mutex_error != 0)
    {
        free(superpeer->members);
        free(superpeer);
        errno = mutex_error;
        return -1;
    }

    local_node.role = NODE_ROLE_SUPERPEER;
    superpeer->local_node = local_node;
    superpeer->member_capacity = initial_capacity;
    set_member(&superpeer->members[0], &superpeer->local_node);
    superpeer->member_count = 1U;
    *output = superpeer;
    return 0;
}

/* Libera recursos; o chamador deve encerrar threads usuárias antes de destruir. */
void superpeer_destroy(SuperPeer *superpeer)
{
    if (superpeer == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&superpeer->members_mutex);
    free(superpeer->members);
    free(superpeer);
}

/* Copia o nó local, imutável após criação. */
int superpeer_get_node(const SuperPeer *superpeer, Node *output)
{
    if (superpeer == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *output = superpeer->local_node;
    return 0;
}

/* Valida nó; sob mutex, adiciona ou atualiza por ID. Duplicata não aumenta contagem. */
SuperPeerRegistrationResult superpeer_register_node(SuperPeer *superpeer, const Node *node)
{
    size_t index;
    int result;

    if (superpeer == NULL || node == NULL)
    {
        errno = EINVAL;
        return SUPERPEER_REGISTER_ERROR;
    }
    if (node_validate(node) == -1)
    {
        return SUPERPEER_REGISTER_ERROR;
    }

    if (lock_members(superpeer) == -1)
    {
        return SUPERPEER_REGISTER_ERROR;
    }

    if (find_member_index_locked(superpeer, &node->id, &index) == 0)
    {
        set_member(&superpeer->members[index], node);
        result = SUPERPEER_MEMBER_UPDATED;
    }
    else if (grow_members_locked(superpeer) == -1)
    {
        result = SUPERPEER_REGISTER_ERROR;
    }
    else
    {
        set_member(&superpeer->members[superpeer->member_count], node);
        ++superpeer->member_count;
        result = SUPERPEER_MEMBER_ADDED;
    }

    if (unlock_members(superpeer) == -1)
    {
        return SUPERPEER_REGISTER_ERROR;
    }
    return (SuperPeerRegistrationResult)result;
}

/* Protege nó local e compacta vetor com memmove. LEAVE em peer.c ainda não chama esta API. */
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id)
{
    size_t index;
    size_t remaining;

    if (superpeer == NULL || node_id == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_id_equal(&superpeer->local_node.id, node_id))
    {
        errno = EPERM;
        return -1;
    }
    if (lock_members(superpeer) == -1)
    {
        return -1;
    }

    if (find_member_index_locked(superpeer, node_id, &index) == -1)
    {
        unlock_members(superpeer);
        errno = ENOENT;
        return -1;
    }

    remaining = superpeer->member_count - index - 1U;
    if (remaining > 0U)
    {
        memmove(&superpeer->members[index], &superpeer->members[index + 1U], remaining * sizeof(*superpeer->members));
    }
    --superpeer->member_count;

    if (unlock_members(superpeer) == -1)
    {
        return -1;
    }
    return 0;
}

/* Busca sob mutex e retorna cópia: nenhum ponteiro interno sobrevive a realloc. */
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output)
{
    SuperPeer *mutable_superpeer;
    size_t index;

    if (superpeer == NULL || node_id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return -1;
    }

    if (find_member_index_locked(superpeer, node_id, &index) == -1)
    {
        unlock_members(mutable_superpeer);
        errno = ENOENT;
        return -1;
    }

    *output = superpeer->members[index];
    if (unlock_members(mutable_superpeer) == -1)
    {
        return -1;
    }
    return 0;
}

/* Consulta contagem sob mutex, incluindo o nó local; zero também pode sinalizar erro. */
size_t superpeer_member_count(const SuperPeer *superpeer)
{
    SuperPeer *mutable_superpeer;
    size_t count;

    if (superpeer == NULL)
    {
        errno = EINVAL;
        return 0U;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return 0U;
    }
    count = superpeer->member_count;
    unlock_members(mutable_superpeer);
    return count;
}

/* Consulta existência sob mutex; zero significa ausência ou erro. */
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id)
{
    SuperPeer *mutable_superpeer;
    int registered;

    if (superpeer == NULL || node_id == NULL)
    {
        errno = EINVAL;
        return 0;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return 0;
    }
    registered = find_member_index_locked(superpeer, node_id, NULL) == 0;
    unlock_members(mutable_superpeer);
    return registered;
}
