#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psyz.h>
#include <libspu.h>
#include "../../decomp/src/libsnd/libsnd_private.h"

#include "ztest.h"
#include "spu_mix_scene.h"

// Host mixer buses (psyz/spu.h). Kept apart from test_libspu.c because
// libsnd_private.h and that file declare the same libspu internals.

ZTEST_SETUP(spu_bus) {
    zskip_targets("ps1");
    Psyz_AudioPause();
    Psyz_AudioLock();
    Psyz_SpuInit();
    Psyz_SpuReset(0);
}

ZTEST_TEARDOWN(spu_bus) {
    Psyz_SpuSetBusGain(0, PSYZ_SPU_GAIN_UNITY);
    Psyz_SpuSetBusGain(1, PSYZ_SPU_GAIN_UNITY);
    Psyz_SpuSetCdGain(PSYZ_SPU_GAIN_UNITY);
    Psyz_SpuEnableBusStats(0);
    Psyz_AudioUnlock();
    Psyz_AudioUnpause();
}

enum { kSampleAddr = 0x1080 };

static const unsigned char kAdpcmTriangle[64] = {
    0x00, 0x06, 0x10, 0x32, 0x54, 0x76, 0x56, 0x34, 0x12, 0xe0, 0xcd,
    0xab, 0x89, 0xa9, 0xcb, 0xed, 0x00, 0x00, 0x10, 0x32, 0x54, 0x76,
    0x56, 0x34, 0x12, 0xe0, 0xcd, 0xab, 0x89, 0xa9, 0xcb, 0xed, 0x00,
    0x00, 0x10, 0x32, 0x54, 0x76, 0x56, 0x34, 0x12, 0xe0, 0xcd, 0xab,
    0x89, 0xa9, 0xcb, 0xed, 0x00, 0x03, 0x10, 0x32, 0x54, 0x76, 0x56,
    0x34, 0x12, 0xe0, 0xcd, 0xab, 0x89, 0xa9, 0xcb, 0xed,
};

static void spu_reset_quiet(void) {
    Psyz_SpuReset(0);
    Psyz_SpuWrite(0x1AA, 0);      // SPU_CTRL: disable
    Psyz_SpuWrite(0x180, 0);      // SPU_VOL_MAIN_LEFT
    Psyz_SpuWrite(0x182, 0);      // SPU_VOL_MAIN_RIGHT
    Psyz_SpuWrite(0x184, 0);      // SPU_REVERB_LEFT
    Psyz_SpuWrite(0x186, 0);      // SPU_REVERB_RIGHT
    Psyz_SpuWrite(0x18C, 0xFFFF); // KEY_OFF_LOW
    Psyz_SpuWrite(0x18E, 0xFFFF); // KEY_OFF_HIGH
    Psyz_SpuWrite(0x190, 0);      // PITCH_MOD_LOW
    Psyz_SpuWrite(0x192, 0);      // PITCH_MOD_HIGH
    Psyz_SpuWrite(0x194, 0);      // NOISE_EN_LOW
    Psyz_SpuWrite(0x196, 0);      // NOISE_EN_HIGH
    Psyz_SpuWrite(0x198, 0);      // REVERB_EN_LOW
    Psyz_SpuWrite(0x19A, 0);      // REVERB_EN_HIGH
    Psyz_SpuWrite(0x1A2, 0xFFFF); // REVERB_ADDR
    Psyz_SpuWrite(0x1AA, 0x8000); // SPU_CTRL: enable
}

// --- host mixer buses (psyz/spu.h) -----------------------------------------

// Key on `voice` on the looping triangle sample at full voice volume.
static void bus_keyon(unsigned int voice, unsigned short vol) {
    const unsigned int base = voice << 4;
    Psyz_SpuWrite(base + 0x00, vol);
    Psyz_SpuWrite(base + 0x02, vol);
    Psyz_SpuWrite(base + 0x04, 0x1000);
    Psyz_SpuWrite(base + 0x06, (unsigned short)(kSampleAddr >> 3));
    Psyz_SpuWrite(base + 0x08, 0x000f);
    Psyz_SpuWrite(base + 0x0A, 0x1fc0);
    Psyz_SpuWrite(base + 0x0E, (unsigned short)(kSampleAddr >> 3));
    Psyz_SpuWrite(0x188, 1u << voice);
}

static void bus_scene(void) {
    unsigned char upload[128];
    spu_reset_quiet();
    memcpy(upload, kAdpcmTriangle, 64);
    memset(upload + 64, 0xAA, 64);
    Psyz_SpuMemWrite(kSampleAddr, upload, sizeof(upload));
    Psyz_SpuWrite(0x1AA, 0x8000 | 0x4000);
    Psyz_SpuWrite(0x180, 0x3FFF);
    Psyz_SpuWrite(0x182, 0x3FFF);
    Psyz_SpuWrite(0x18C, 0);
    Psyz_SpuWrite(0x18E, 0);
    Psyz_SpuSetBusGain(0, PSYZ_SPU_GAIN_UNITY);
    Psyz_SpuSetBusGain(1, PSYZ_SPU_GAIN_UNITY);
    Psyz_SpuSetCdGain(PSYZ_SPU_GAIN_UNITY);
}

static void bus_render(short* out, int frames) {
    Psyz_SpuPullSamples(out, frames);
}

// Unity gains are the stock mix: setting them changes nothing, even with a
// voice on a non-default bus.
ZTEST(spu_bus, BusUnityGainIsBitIdentical) {
    enum { kFrames = 1500 };
    static short a[kFrames * 2], b[kFrames * 2];

    bus_scene();
    bus_keyon(1, 0x3FFF);
    bus_keyon(2, 0x2000);
    bus_render(a, kFrames);

    bus_scene();
    bus_keyon(1, 0x3FFF);
    bus_keyon(2, 0x2000);
    Psyz_SpuSetVoiceBus(2, 1); // a bus with unity gain is the same mix
    bus_render(b, kFrames);
    zexpect_s32_eq(0, memcmp(a, b, sizeof(a)));
}

// A bus gain scales its own voices only. The stats prove it per bus, and the
// mix with bus 1 muted equals the mix of bus 0 alone.
ZTEST(spu_bus, BusGainScalesOnlyItsVoices) {
    enum { kFrames = 1500 };
    static short both[kFrames * 2], only0[kFrames * 2], half[kFrames * 2];
    PsyzSpuBusStats st[PSYZ_SPU_NUM_BUSES + 1];
    unsigned long long quarter;
    int i, diff = 0;

    bus_scene();
    bus_keyon(1, 0x3FFF);
    bus_render(only0, kFrames); // voice 1 alone

    bus_scene();
    bus_keyon(1, 0x3FFF);
    bus_keyon(2, 0x3FFF);
    Psyz_SpuSetVoiceBus(2, 1);
    Psyz_SpuSetBusGain(1, 0);
    bus_render(both, kFrames);
    zexpect_s32_eq(0, memcmp(only0, both, sizeof(only0)));

    bus_scene();
    bus_keyon(1, 0x3FFF);
    bus_keyon(2, 0x3FFF);
    Psyz_SpuSetVoiceBus(2, 1);
    Psyz_SpuSetBusGain(1, PSYZ_SPU_GAIN_UNITY / 2);
    Psyz_SpuEnableBusStats(1);
    bus_render(half, kFrames);
    Psyz_SpuGetBusStats(st);
    Psyz_SpuEnableBusStats(0);
    zexpect_s32_eq(1, st[0].frames == kFrames);
    // bus 0 untouched, bus 1 at half amplitude = a quarter of the energy
    zexpect_s32_eq(1, st[0].in_energy > 0);
    zexpect_s32_eq(1, st[0].in_energy == st[0].out_energy);
    zexpect_s32_eq(1, st[1].in_energy > 0);
    quarter = st[1].in_energy / 4;
    zexpect_s32_eq(1, st[1].out_energy + quarter / 16 >= quarter &&
                          st[1].out_energy <= quarter + quarter / 16);
    // the half-gain mix is neither "bus 1 muted" nor "bus 1 full"
    for (i = 0; i < kFrames * 2; i++) {
        if (half[i] != only0[i])
            diff++;
    }
    zexpect_s32_gt(0, diff);
    zexpect_s32_eq(0, st[PSYZ_SPU_BUS_CD].active);
}

// libsnd routes a voice by its sequence track at key-on; SsUtKeyOn voices
// (seq_sep_no 0x21) and unrouted tracks stay on bus 0.
ZTEST(spu_bus, BusFollowsLibsndTrack) {
    static short sink[1500 * 2];
    PsyzSpuBusStats st[PSYZ_SPU_NUM_BUSES + 1];

    bus_scene();
    Psyz_SndSetSeqBus(3, 5, 1);
    _svm_voice[1].seq_sep_no = (5 << 8) | 3; // routed track -> bus 1
    _svm_voice[2].seq_sep_no = 0x21;         // SsUtKeyOn -> bus 0
    _svm_voice[3].seq_sep_no = (6 << 8) | 3; // other track -> bus 0
    Psyz_SpuEnableBusStats(1);
    bus_keyon(1, 0x3FFF);
    bus_keyon(2, 0x3FFF);
    bus_keyon(3, 0x3FFF);
    bus_render(sink, 1500);
    Psyz_SpuGetBusStats(st);
    Psyz_SpuEnableBusStats(0);
    // two voices on bus 0, one on bus 1: voice-frames follow the routing
    zexpect_s32_eq(1, st[1].active > 0);
    zexpect_s32_eq(1, st[0].active == st[1].active * 2);
    // a bad route is ignored
    Psyz_SndSetSeqBus(3, 5, PSYZ_SPU_NUM_BUSES);
    Psyz_SndSetSeqBus(-1, 0, 1);
    Psyz_SndSetSeqBus(0, 16, 1);
    Psyz_SndSetSeqBus(3, 5, 0); // leave bus 0 for later tests
}

// Hash of spu_mix_scene_hash() rendered by psyz 2caa505, the last commit
// before the buses (build the scene header against that tree to reproduce).
#define kSceneGolden 0x95190ab95516a0c4ull

static void scene_routes(int first_voice, int bus, int seq, int sep) {
    int v;
    Psyz_SndSetSeqBus((short)seq, (short)sep, bus);
    for (v = first_voice; v < first_voice + 8; v++)
        _svm_voice[v].seq_sep_no = (short)((sep << 8) | seq);
}

// The default mix is the mix the SPU had before the buses existed: with unity
// gains, even voices spread over every bus render the same samples, and the
// scene is a busy one (24 voices, reverb, envelopes, key-on / key-off).
ZTEST(spu_bus, BusDefaultMixMatchesTheMixerWithoutBuses) {
    uint64_t h;
    int v;

    for (v = 0; v < 24; v++)
        _svm_voice[v].seq_sep_no = 0x21;
    h = spu_mix_scene_hash(NULL, NULL);
    zexpect_s32_eq(1, scene_peak > 1000 && scene_peak < 32767);
    zexpect_u32_eq((uint32_t)(kSceneGolden >> 32), (uint32_t)(h >> 32));
    zexpect_u32_eq((uint32_t)kSceneGolden, (uint32_t)h);

    scene_routes(8, 1, 1, 2);
    scene_routes(16, 2, 1, 3);
    Psyz_SpuSetBusGain(3, 0x1234); // an unused bus does not matter
    h = spu_mix_scene_hash(NULL, NULL);
    zexpect_u32_eq((uint32_t)(kSceneGolden >> 32), (uint32_t)(h >> 32));
    zexpect_u32_eq((uint32_t)kSceneGolden, (uint32_t)h);

    // ...and a real gain on one bus does change the render
    Psyz_SpuSetBusGain(1, PSYZ_SPU_GAIN_UNITY - 1);
    h = spu_mix_scene_hash(NULL, NULL);
    zexpect_s32_eq(1, h != kSceneGolden);

    Psyz_SpuSetBusGain(1, PSYZ_SPU_GAIN_UNITY);
    Psyz_SpuSetBusGain(3, PSYZ_SPU_GAIN_UNITY);
    Psyz_SndSetSeqBus(1, 2, 0);
    Psyz_SndSetSeqBus(1, 3, 0);
    for (v = 0; v < 24; v++)
        _svm_voice[v].seq_sep_no = 0x21;
}
