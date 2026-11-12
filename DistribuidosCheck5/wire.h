#ifndef WIRE_H
#define WIRE_H
#include <stdint.h>
#include <stddef.h>
static inline void wire_put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static inline uint16_t wire_get_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

static inline void wire_put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static inline uint32_t wire_get_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) | ((uint32_t)source[2] << 8) | source[3];
}

static inline void wire_put_u64(uint8_t *destination, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        destination[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static inline uint64_t wire_get_u64(const uint8_t *source)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        value = (value << 8) | source[index];
    }
    return value;
}

#endif
