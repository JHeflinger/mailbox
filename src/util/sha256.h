#ifndef SHA256_H
#define SHA256_H

#include <easybool.h>
#include <stddef.h>
#include <stdint.h>

#define SHA256_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t filled;
} Sha256;

void Sha256Begin(Sha256* sha);

void Sha256Feed(Sha256* sha, const void* data, size_t size);

void Sha256Finish(Sha256* sha, uint8_t digest[SHA256_SIZE]);

void Sha256Password(const uint8_t* nonce, size_t noncesize, const char* password, uint8_t digest[SHA256_SIZE]);

BOOL DigestsMatch(const uint8_t* a, const uint8_t* b, size_t size);

#endif
