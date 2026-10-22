#ifndef PSYZ_TEST_SPU_MIX_SCENE_H
#define PSYZ_TEST_SPU_MIX_SCENE_H

// A deterministic, busy SPU scene rendered offline: 24 voices on three ADPCM
// samples at different pitches, volumes and envelopes, staggered key-ons and
// key-offs, reverb sends and a reverb preset, the CD input enabled. It uses
// only the public register API, so the same file builds against a PSY-Z
// without mixer buses: the golden hash in test_libspu_bus.c was produced by
// psyz 2caa505 (before the buses) and the default mix has to keep matching it.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psyz.h>

static const unsigned char kSceneTri[32] = {
    0x00, 0x04, 0x00, 0x10, 0x21, 0x32, 0x33, 0x44, 0x54, 0x55, 0x66, 0x76, 0x77, 0x77, 0x77, 0x77,
    0x00, 0x03, 0x77, 0x77, 0x66, 0x56, 0x55, 0x44, 0x34, 0x33, 0x22, 0x11, 0x00, 0x00, 0x00, 0xf0};
static const unsigned char kSceneSq[32] = {
    0x00, 0x04, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
    0x00, 0x03, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
static const unsigned char kSceneSine[32] = {
    0x00, 0x04, 0x50, 0x57, 0xa0, 0xa8, 0x50, 0x57, 0xa0, 0xa8, 0x50, 0x57, 0xa0, 0xa8, 0x50, 0x57,
    0x00, 0x03, 0xa0, 0xa8, 0x50, 0x57, 0xa0, 0xa8, 0x50, 0x57, 0xa0, 0xa8, 0x50, 0x57, 0xa0, 0xa8};

static unsigned scene_rng;
static int scene_peak; // largest |sample| of the last render, to show it is not silent
static unsigned scene_rand(void) {
    scene_rng = scene_rng * 1664525u + 1013904223u;
    return scene_rng >> 8;
}

// Calls `between` (may be NULL) after each 441-frame block, so a caller can
// route voices or change bus gains mid-scene.
static uint64_t spu_mix_scene_hash(void (*between)(int block, void* user),
                                   void* user) {
    enum { kBlocks = 300, kBlockFrames = 441 };
    static const unsigned short kPitch[8] = {0x0400, 0x0800, 0x0C00, 0x1000,
                                             0x1400, 0x1800, 0x2000, 0x0A00};
    static short out[kBlockFrames * 2];
    uint64_t h = 1469598103934665603ull;
    unsigned off;
    int v, b, i;

    Psyz_SpuInit();
    Psyz_SpuReset(0);
    for (off = 0; off < 0x200; off += 2) {
        if (off == 0x1A6 || off == 0x1A8 || off == 0x188 || off == 0x18A)
            continue; // transfer address / FIFO / key-on
        Psyz_SpuWrite(off, off == 0x18C || off == 0x18E ? 0xFFFF : 0);
    }
    {
        static unsigned char zeros[4096];
        for (off = 0; off < 0x80000; off += sizeof(zeros))
            Psyz_SpuMemWrite(off, zeros, sizeof(zeros));
    }
    Psyz_SpuMemWrite(0x1080, kSceneTri, 32);
    Psyz_SpuMemWrite(0x1100, kSceneSq, 32);
    Psyz_SpuMemWrite(0x1180, kSceneSine, 32);

    scene_rng = 12345u;
    scene_peak = 0;
    Psyz_SpuWrite(0x180, 0x0400);              // main volume
    Psyz_SpuWrite(0x182, 0x0400);
    Psyz_SpuWrite(0x184, 0x2800);              // reverb output volume
    Psyz_SpuWrite(0x186, 0x2800);
    Psyz_SpuWrite(0x1B0, 0x7FFF);              // CD volume
    Psyz_SpuWrite(0x1B2, 0x7FFF);
    Psyz_SpuWrite(0x1A2, 0x7000 >> 3);         // reverb work area
    for (i = 0; i < 32; i++)                   // reverb preset registers
        Psyz_SpuWrite(0x1C0 + i * 2, (unsigned short)(0x0080 + i * 0x31 + (i & 3) * 0x1000));
    Psyz_SpuWrite(0x1C4, 0x6D80);              // vIIR
    Psyz_SpuWrite(0x1CE, 0xBA80);              // vWALL
    Psyz_SpuWrite(0x1FC, 0x4000);              // vLIN
    Psyz_SpuWrite(0x1FE, 0x4000);              // vRIN
    Psyz_SpuWrite(0x198, 0xAAAA);              // reverb send, voices 1,3,5..
    Psyz_SpuWrite(0x19A, 0x00AA);
    Psyz_SpuWrite(0x1AA, 0x8000 | 0x4000 | 0x0080 | 0x0001); // on, unmute, reverb, CD

    for (v = 0; v < 24; v++) {
        const unsigned base = (unsigned)v << 4;
        const unsigned short addr = (unsigned short)((0x1080 + (v % 3) * 0x80) >> 3);
        Psyz_SpuWrite(base + 0x00, (unsigned short)(0x1800 + (v * 0x0237 & 0x2FFF)));
        Psyz_SpuWrite(base + 0x02, (unsigned short)(0x3000 - (v * 0x0191 & 0x1FFF)));
        Psyz_SpuWrite(base + 0x04, kPitch[v & 7]);
        Psyz_SpuWrite(base + 0x06, addr);
        Psyz_SpuWrite(base + 0x08, (unsigned short)(0x00FF | ((v & 3) << 4) | ((v & 1) ? 0x8000 : 0)));
        Psyz_SpuWrite(base + 0x0A, (unsigned short)(0x5FC0 + (v & 7)));
        Psyz_SpuWrite(base + 0x0E, addr);
    }
    for (b = 0; b < kBlocks; b++) {
        unsigned on = 0, offmask = 0;
        for (v = 0; v < 24; v++) {
            if ((int)(scene_rand() % 40) == 0)
                on |= 1u << v;
            else if ((int)(scene_rand() % 50) == 0)
                offmask |= 1u << v;
        }
        Psyz_SpuWrite(0x18C, (unsigned short)(offmask & 0xFFFF));
        Psyz_SpuWrite(0x18E, (unsigned short)(offmask >> 16));
        Psyz_SpuWrite(0x188, (unsigned short)(on & 0xFFFF));
        Psyz_SpuWrite(0x18A, (unsigned short)(on >> 16));
        if (between)
            between(b, user);
        Psyz_SpuPullSamples(out, kBlockFrames);
        for (i = 0; i < kBlockFrames * 2; i++) {
            if (abs(out[i]) > scene_peak)
                scene_peak = abs(out[i]);
            h ^= (uint8_t)(out[i] & 0xFF);
            h *= 1099511628211ull;
            h ^= (uint8_t)((out[i] >> 8) & 0xFF);
            h *= 1099511628211ull;
        }
    }
    return h;
}

#endif
