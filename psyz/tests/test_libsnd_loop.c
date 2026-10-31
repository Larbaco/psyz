#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psyz.h>
#include <libspu.h>
#include <libsnd.h>
#include "../../decomp/src/libsnd/libsnd_private.h"

#include "ztest.h"

// SEQ loops (host reader, psyz/src/psyz/midiread_host.c). PS1 sequences loop
// with NRPN events: CC#99 = 20 starts the loop, CC#6 (Data Entry) right after
// carries the repeat count (127 = forever), CC#99 = 30 ends it. A reader that
// drops the Data Entry value never jumps back and the music plays once. The
// tests feed hand-made SEQ data to the reader and count how often a marker
// event runs.

enum { kMaxTicks = 1000 };

static char s_table[SS_SEQ_TABSIZ * 2 * 2];

ZTEST_SETUP(snd_loop) {
    zskip_targets("ps1");
    Psyz_AudioPause();
    Psyz_AudioLock();
    Psyz_SpuInit();
    Psyz_SpuReset(0);
    SsInit();
    SsSetTableSize(s_table, 2, 2);
    SsSetTickMode(SS_NOTICK);
}

ZTEST_TEARDOWN(snd_loop) {

    Psyz_AudioUnlock();
    Psyz_AudioUnpause();
}

// 'pQES' + version 1, resolution 48, tempo 500000 us (120 bpm), 4/4.
static size_t seq_header(unsigned char* out) {
    static const unsigned char hdr[] = {'p', 'Q', 'E', 'S', 0, 0, 0, 1, 0x00,
                                        0x30, 0x07, 0xA1, 0x20, 4, 4};
    memcpy(out, hdr, sizeof(hdr));
    return sizeof(hdr);
}

static size_t put(unsigned char* out, size_t n, const unsigned char* ev,
                  size_t len) {
    memcpy(out + n, ev, len);
    return n + len;
}

// Marker events: CC#10 (panpot of channel 0) A = 0x11 (loop body), B = 0x22,
// C = 0x33 (after the loop). Every event is followed by its delta time.
enum { kMarkA = 0x11, kMarkB = 0x22, kMarkC = 0x33 };

// loop_count < 0: no loop events, A B C straight through.
static size_t build_seq(unsigned char* out, int loop_count) {
    size_t n = seq_header(out);
    static const unsigned char first_delta[] = {0x00};
    static const unsigned char loop_start[] = {0xB0, 0x63, 20, 0x00};
    static const unsigned char mark_a[] = {0xB0, 0x0A, kMarkA, 0x30};
    static const unsigned char mark_b[] = {0xB0, 0x0A, kMarkB, 0x30};
    static const unsigned char loop_end[] = {0xB0, 0x63, 30, 0x00};
    static const unsigned char mark_c[] = {0xB0, 0x0A, kMarkC, 0x30};
    static const unsigned char end_of_track[] = {0xFF, 0x2F, 0x00};
    unsigned char count[] = {0xB0, 0x06, 0, 0x00};

    n = put(out, n, first_delta, sizeof(first_delta));
    if (loop_count >= 0) {
        count[2] = (unsigned char)loop_count;
        n = put(out, n, loop_start, sizeof(loop_start));
        n = put(out, n, count, sizeof(count));
    }
    n = put(out, n, mark_a, sizeof(mark_a));
    n = put(out, n, mark_b, sizeof(mark_b));
    if (loop_count >= 0) {
        n = put(out, n, loop_end, sizeof(loop_end));
    }
    n = put(out, n, mark_c, sizeof(mark_c));
    n = put(out, n, end_of_track, sizeof(end_of_track));
    return n;
}

struct run_result {
    int a_runs;       // times marker A ran
    int c_runs;       // times marker C ran
    int ended;        // end-of-track stopped the sequence
    int ticks_to_end; // vsync ticks until it did (kMaxTicks if it did not)
};

static struct run_result run_seq(int loop_count, int l_count, int ticks) {
    static unsigned char data[128];
    struct run_result r = {0, 0, 0, ticks};
    struct SeqStruct* score;
    int prev = -1;
    int seq;
    int t;

    build_seq(data, loop_count);
    seq = SsSeqOpen((unsigned long*)data, 0);
    zassert_s32_ge(0, seq);
    score = &_ss_score[seq][0];
    SsSeqPlay(seq, SSPLAY_PLAY, l_count);
    for (t = 0; t < ticks; t++) {
        SsSeqCalledTbyT();
        if (score->panpot[0] != prev) {
            r.a_runs += score->panpot[0] == kMarkA;
            r.c_runs += score->panpot[0] == kMarkC;
            prev = score->panpot[0];
        }
        if (score->flags == 0) { // SsSeqCalledTbyT stops and clears it
            r.ended = 1;
            r.ticks_to_end = t;
            break;
        }
    }
    SsSeqClose(seq);
    return r;
}

// Without loop events one pass runs A B C and stops (l_count 1, what the
// game passes to SsSepPlay).
ZTEST(snd_loop, plain_sequence_plays_once) {
    struct run_result r = run_seq(-1, 1, kMaxTicks);

    zassert_s32_eq(1, r.a_runs);
    zassert_s32_eq(1, r.c_runs);
    zassert_s32_eq(1, r.ended);
}

// Count 3: the body (A B) runs three times, then C, then the end.
ZTEST(snd_loop, nrpn_loop_runs_count_times) {
    struct run_result r = run_seq(3, 1, kMaxTicks);

    zassert_s32_eq(3, r.a_runs);
    zassert_s32_eq(1, r.c_runs);
    zassert_s32_eq(1, r.ended);
}

// Count 1 and count 0 (no count latched) both play the body once.
ZTEST(snd_loop, nrpn_loop_count_one_plays_body_once) {
    struct run_result r = run_seq(1, 1, kMaxTicks);

    zassert_s32_eq(1, r.a_runs);
    zassert_s32_eq(1, r.c_runs);
    zassert_s32_eq(1, r.ended);
}

// Count 127 is "forever": the sequence never reaches C or its end, and the
// body keeps running well past what one pass takes. This is the music case.
ZTEST(snd_loop, nrpn_loop_127_never_ends) {
    struct run_result r = run_seq(127, 1, kMaxTicks);

    zassert_s32_eq(0, r.ended);
    zassert_s32_eq(0, r.c_runs);
    zassert_s32_gt(5, r.a_runs);
}

// The loop starts over after a stop and play, as when the inn restarts the
// music: the per-play loop state is cleared by SsSeqStop.
ZTEST(snd_loop, nrpn_loop_127_survives_stop_and_replay) {
    static unsigned char data[128];
    struct SeqStruct* score;
    int seq;
    int t;
    int a_runs = 0;
    int prev = -1;
    int round;

    build_seq(data, 127);
    seq = SsSeqOpen((unsigned long*)data, 0);
    zassert_s32_ge(0, seq);
    score = &_ss_score[seq][0];
    for (round = 0; round < 2; round++) {
        a_runs = 0;
        prev = -1;
        SsSeqPlay(seq, SSPLAY_PLAY, 1);
        for (t = 0; t < kMaxTicks; t++) {
            SsSeqCalledTbyT();
            if (score->panpot[0] != prev) {
                a_runs += score->panpot[0] == kMarkA;
                prev = score->panpot[0];
            }
        }
        zassert_s32_eq(1, score->flags & SEQ_FLAG_1);
        zassert_s32_gt(5, a_runs);
        SsSeqStop(seq);
        SsSeqCalledTbyT();
    }
    SsSeqClose(seq);
}

// End of track with l_count 0 (SSPLAY_INFINITY) restarts the track from its
// first event instead of stopping, like retail (0x8002F7F4).
ZTEST(snd_loop, end_of_track_restarts_with_infinite_l_count) {
    struct run_result r = run_seq(-1, SSPLAY_INFINITY, kMaxTicks);

    zassert_s32_eq(0, r.ended);
    zassert_s32_gt(5, r.a_runs);
}

// l_count 3 without loop events plays the track three times.
ZTEST(snd_loop, end_of_track_restarts_l_count_times) {
    struct run_result r = run_seq(-1, 3, kMaxTicks);

    zassert_s32_eq(3, r.a_runs);
    zassert_s32_eq(3, r.c_runs);
    zassert_s32_eq(1, r.ended);
}
