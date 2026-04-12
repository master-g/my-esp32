#ifndef SERVICE_BITCOIN_SLOT_RIPEMD160_H
#define SERVICE_BITCOIN_SLOT_RIPEMD160_H

#include <stddef.h>
#include <stdint.h>

int slot_ripemd160(const uint8_t *input, size_t ilen, uint8_t output[20]);

#endif
