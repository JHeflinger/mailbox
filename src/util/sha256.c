#include "sha256.h"
#include <string.h>

static const uint32_t g_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Sha256Block(Sha256* sha, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = sha->state[0], b = sha->state[1], c = sha->state[2], d = sha->state[3];
    uint32_t e = sha->state[4], f = sha->state[5], g = sha->state[6], h = sha->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + g_sha256_k[i] + w[i];
        uint32_t s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    sha->state[0] += a; sha->state[1] += b; sha->state[2] += c; sha->state[3] += d;
    sha->state[4] += e; sha->state[5] += f; sha->state[6] += g; sha->state[7] += h;
}

void Sha256Begin(Sha256* sha) {
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(sha->state, initial, sizeof(initial));
    sha->length = 0;
    sha->filled = 0;
}

void Sha256Feed(Sha256* sha, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    sha->length += size;
    while (size > 0) {
        size_t take = 64 - sha->filled;
        if (take > size) take = size;
        memcpy(sha->block + sha->filled, bytes, take);
        sha->filled += take;
        bytes += take;
        size -= take;
        if (sha->filled == 64) {
            Sha256Block(sha, sha->block);
            sha->filled = 0;
        }
    }
}

void Sha256Finish(Sha256* sha, uint8_t digest[SHA256_SIZE]) {
    uint64_t bits = sha->length * 8;
    uint8_t pad = 0x80;
    Sha256Feed(sha, &pad, 1);
    pad = 0x00;
    while (sha->filled != 56) Sha256Feed(sha, &pad, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - i * 8));
    Sha256Feed(sha, length, 8);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(sha->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(sha->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(sha->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(sha->state[i]);
    }
}

void Sha256Password(const uint8_t* nonce, size_t noncesize, const char* password, uint8_t digest[SHA256_SIZE]) {
    Sha256 sha;
    Sha256Begin(&sha);
    Sha256Feed(&sha, nonce, noncesize);
    Sha256Feed(&sha, password, strlen(password));
    Sha256Finish(&sha, digest);
}

BOOL DigestsMatch(const uint8_t* a, const uint8_t* b, size_t size) {
    uint8_t diff = 0;
    for (size_t i = 0; i < size; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}
