/*
 * Host SEQ/SEP MIDI reader for PSY-Z.
 *
 * Honesty: PSY-Q midiread.c / midinote.c remain INCLUDE_ASM upstream.
 * This file ports the matching logic from Xeeynamo/sotn-decomp
 * `src/main/psxsdk/libsnd/seqread.c` (MIT) onto PSY-Z's SeqStruct field
 * names (PSY-Q 4.0 layout in libsnd_private.h). Not byte-identical to
 * retail libsnd; enough for SEP note-on under SsSeqCalledTbyT.
 *
 * Cursor: psyz uses seq_ptr as the live reader (_SsReadDeltaValue);
 * sotn named that cursor read_pos.
 */
#include <psyz.h>
#include <libsnd.h>
#include <psyz/log.h>
#include "../../decomp/src/libsnd/libsnd_private.h"

#define NRPN_LOOP_START 20
#define NRPN_LOOP_END 30

static unsigned g_sep_noteon_log;

static void _SsGetSeqData(short seq_access_num, short seq_num);

static u8 select_tones(u8* tone_idx, u8* vag_idx) {
    u8 i;
    u8 count = 0;
    VagAtr* atr;

    for (i = 0; i < (u8)_svm_cur.prog_tones; i++) {
        atr = &_svm_tn[_svm_cur.fake_program * 16 + i];
        if (atr->min > (u8)_svm_cur.note || (u8)_svm_cur.note > atr->max) {
            continue;
        }
        vag_idx[count] = (u8)atr->vag;
        tone_idx[count++] = i;
    }
    return count;
}

/* Host stand-in for INCLUDE_ASM _SsVmKeyOn / SpuVmKeyOn. */
static int host_VmKeyOn(
    short seq_sep_no, short vab_id, short prog, u16 note, u16 vel, u16 pan) {
    struct SeqStruct* score =
        &_ss_score[seq_sep_no & 0xFF][(seq_sep_no >> 8) & 0xFF];
    u8 tone_idx[16];
    u8 vag_idx[16];
    u8 i;
    u8 vag_count;
    int tn;
    short voice;
    int keyed = 0;

    if (_SsVmVSetUp(vab_id, prog)) {
        return -1;
    }
    _svm_cur.seq_sep_no = seq_sep_no;
    _svm_cur.note = (char)note;
    _svm_cur.fine = 0;
    if (seq_sep_no == 0x21) {
        _svm_cur.volume = (char)vel;
    } else {
        _svm_cur.volume =
            (char)((vel * score->vol[score->channel_idx]) / 0x7F);
    }
    _svm_cur.pan = (char)pan;
    _svm_cur.mvol = _svm_pg[prog].mvol;
    _svm_cur.mpan = _svm_pg[prog].mpan;
    _svm_cur.prog_tones = _svm_pg[prog].tones;
    if (_svm_cur.fake_program >= (char)_svm_vh->ps) {
        return -1;
    }
    if (vel == 0) {
        for (i = 0; i < (u8)_SsVmMaxVoice; i++) {
            if (_svm_voice[i].note == (s16)note &&
                _svm_voice[i].prog == prog &&
                _svm_voice[i].seq_sep_no == seq_sep_no &&
                _svm_voice[i].vabId == vab_id) {
                _svm_cur.voice = i;
                _SsVmKeyOffNow(0);
                keyed++;
            }
        }
        return keyed;
    }

    vag_count = select_tones(tone_idx, vag_idx);
    for (i = 0; i < vag_count; i++) {
        _svm_cur.tone = tone_idx[i];
        _svm_cur.tone_vag_idx = vag_idx[i];
        tn = _svm_cur.tone + (_svm_cur.fake_program * 16);
        _svm_cur.tone_prior = _svm_tn[tn].prior;
        _svm_cur.tone_vol = _svm_tn[tn].vol;
        _svm_cur.tone_pan = _svm_tn[tn].pan;
        _svm_cur.tone_center = _svm_tn[tn].center;
        _svm_cur.tone_shift = _svm_tn[tn].shift;
        _svm_cur.tone_mode = _svm_tn[tn].mode;
        _svm_cur.tone_min = _svm_tn[tn].min;
        _svm_cur.tone_max = _svm_tn[tn].max;

        voice = _SsVmAlloc(_svm_cur.tone_vag_idx);
        if (voice == _SsVmMaxVoice) {
            continue;
        }
        _svm_cur.voice = voice;
        _svm_voice[voice].unk1b = 1;
        _svm_voice[voice].unk2 = 0;
        _svm_voice[voice].seq_sep_no = seq_sep_no;
        _svm_voice[voice].vabId = vab_id;
        _svm_voice[voice].fake_program = _svm_cur.fake_program;
        _svm_voice[voice].prog = prog;
        _svm_voice[voice].vag_idx = _svm_cur.tone_vag_idx;
        _svm_voice[voice].tone = _svm_cur.tone;
        _svm_voice[voice].note = (s16)note;
        _svm_voice[voice].voll1 = (s16)vel;
        _svm_voice[voice].pan = (char)pan;
        _SsVmDoAllocate();
        if (_svm_cur.tone_vag_idx == 0xFF) {
            vmNoiseOn((char)voice);
        } else {
            _SsVmKeyOnNow(1, note2pitch());
        }
        keyed++;
    }
    return keyed ? keyed : -1;
}

void _SsSeqPlay(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    s32 acc;

    if (score->delta_value - score->unk54 > 0) {
        if (score->unk52 > 0) {
            score->unk52--;
        } else if (score->unk52 == 0) {
            score->unk52 = score->unk54;
            score->delta_value--;
        } else {
            score->delta_value -= score->unk54;
        }
    } else if (score->delta_value <= score->unk54) {
        acc = score->delta_value;
        do {
            do {
                _SsGetSeqData(seq_access_num, seq_num);
            } while (score->delta_value == 0);
            acc += score->delta_value;
        } while (acc < score->unk54);
        score->delta_value = acc - score->unk54;
    }
}

static void _SsGetSeqData(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 status;
    u8 note;
    u8 vel;

    status = *score->seq_ptr++;
    if (status & 0x80) {
        score->channel_idx = status & 0xF;
        switch (status & 0xF0) {
        case 0x90:
            score->running_status = 0x90;
            note = *score->seq_ptr++;
            vel = *score->seq_ptr++;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            _SsNoteOn(seq_access_num, seq_num, note, vel);
            return;
        case 0x80:
            score->running_status = 0x90;
            note = *score->seq_ptr++;
            (void)*score->seq_ptr++;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            _SsNoteOn(seq_access_num, seq_num, note, 0);
            return;
        case 0xB0:
            score->running_status = 0xB0;
            _SsSetControlChange(seq_access_num, seq_num, *score->seq_ptr++);
            return;
        case 0xC0:
            score->running_status = 0xC0;
            _SsSetProgramChange(seq_access_num, seq_num, *score->seq_ptr++);
            return;
        case 0xE0:
            score->running_status = 0xE0;
            score->seq_ptr++; /* skip LSB; pitchbend reads MSB */
            _SsSetPitchBend(seq_access_num, seq_num);
            return;
        case 0xF0:
            score->running_status = 0xFF;
            _SsGetMetaEvent(seq_access_num, seq_num, *score->seq_ptr++);
            return;
        default:
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            return;
        }
    }

    switch (score->running_status) {
    case 0x90:
        vel = *score->seq_ptr++;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        _SsNoteOn(seq_access_num, seq_num, status, vel);
        return;
    case 0xB0:
        _SsSetControlChange(seq_access_num, seq_num, status);
        return;
    case 0xC0:
        _SsSetProgramChange(seq_access_num, seq_num, status);
        return;
    case 0xE0:
        _SsSetPitchBend(seq_access_num, seq_num);
        return;
    case 0xFF:
        _SsGetMetaEvent(seq_access_num, seq_num, status);
        return;
    default:
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        return;
    }
}

void _SsNoteOn(
    short seq_access_num, short seq_num, unsigned char note, unsigned char vel) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 ch = score->channel_idx;
    short seq_sep = (short)(seq_access_num | (seq_num << 8));
    int rc;

    if (((score->channel_mute >> ch) & 1) || score->voll == 0) {
        return;
    }
    rc = host_VmKeyOn(seq_sep, score->vab_id, score->programs[ch], note, vel,
                      score->panpot[ch]);
    if (vel && g_sep_noteon_log < 32) {
        INFOF("SEP note-on seq=%d tr=%d ch=%d note=%u vel=%u prog=%u rc=%d",
              seq_access_num, seq_num, ch, note, vel, score->programs[ch], rc);
        g_sep_noteon_log++;
    }
}

void _SsSetProgramChange(
    short seq_access_num, short seq_num, unsigned char prog) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    score->programs[score->channel_idx] = prog;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsSetPitchBend(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    score->seq_ptr++; /* consume MSB; bend not applied yet */
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContMainVol(
    short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    score->vol[score->channel_idx] = val;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContPanpot(
    short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    score->panpot[score->channel_idx] = val;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContExpression(
    short seq_access_num, short seq_num, unsigned char val) {
    /* Full prog-vol path still ASM; treat expression as channel volume. */
    _SsContMainVol(seq_access_num, seq_num, val);
}

void _SsContDamper(
    short seq_access_num, short seq_num, unsigned char val) {
    if (val < 0x40) {
        _SsVmDamperOff();
    } else {
        _svm_damper = 1;
    }
    _ss_score[seq_access_num][seq_num].delta_value =
        _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContExternal(
    short seq_access_num, short seq_num, unsigned char val) {
    SsUtSetReverbDepth(val, val);
    _ss_score[seq_access_num][seq_num].delta_value =
        _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContBankChange(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 val = *score->seq_ptr++;
    score->vab_id = val;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContResetAll(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    SsUtReverbOff();
    _SsVmDamperOff();
    score->programs[score->channel_idx] = score->channel_idx;
    score->unk18 = 0;
    score->unk19 = 0;
    score->vol[score->channel_idx] = 0x7F;
    score->panpot[score->channel_idx] = 0x40;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

/*
 * NRPN / RPN / Data Entry state machine. Field use follows retail libsnd
 * (_SsContNrpn1 0x80030F48, _SsContNrpn2 0x80031048, _SsContRpn1/2
 * 0x80031188/0x800311F8, _SsContDataEntry 0x800307F8 in the US executable),
 * not the sotn-decomp names:
 *   fn_idx (0x1A)  last CC#98 value (NRPN LSB, the VAB attribute index)
 *   unk1B  (0x1B)  last CC#99 value (NRPN MSB: 20 loop start, 30 loop end)
 *   unk1C  (0x1C)  1 after a loop start, until Data Entry latches the count
 *   unk1D  (0x1D)  loop count from Data Entry (0 none, 1..126, 127 forever)
 *   unk15  (0x15)  1 once the loop count has been latched
 *   unk18/19       CC#100 / CC#101 values, unk1E counts them
 *   unk1F  (0x1F)  counts CC#98 / CC#99 values since the last Data Entry
 *   next_sep_pos   (0x08) loop start position (the position after the
 *                  loop-start event's delta time)
 */
#define NRPN_USER_CB 40

void _SsContNrpn1(short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    /* retail also calls the user NRPN callback when unk1B == 40; the host
     * has no SsSetNrpnFunction. */
    if (score->unk1B != NRPN_LOOP_END && score->unk1B != NRPN_LOOP_START &&
        score->unk1B != NRPN_USER_CB) {
        score->fn_idx = val;
        score->unk1C = 0;
        score->unk1F++;
    }
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContNrpn2(short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    switch (val) {
    case NRPN_LOOP_START:
        score->unk1B = val;
        score->unk1C = 1;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        score->next_sep_pos = score->seq_ptr;
        break;
    case NRPN_LOOP_END:
        score->unk1B = val;
        if (score->unk1D == 0) {
            score->unk15 = 0;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        } else if (score->unk1D < 0x7F) {
            score->unk1D--;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            if (score->unk1D != 0) {
                score->seq_ptr = score->next_sep_pos;
                INFOF("SEP loop end -> loop start (count %d left) seq=%d tr=%d",
                      score->unk1D, seq_access_num, seq_num);
            } else {
                score->unk15 = 0;
            }
        } else {
            (void)_SsReadDeltaValue(seq_access_num, seq_num);
            score->delta_value = 0;
            score->seq_ptr = score->next_sep_pos;
            INFOF("SEP loop end -> loop start (count infinite) seq=%d tr=%d",
                  seq_access_num, seq_num);
        }
        break;
    default:
        score->unk1B = val;
        score->unk1F++;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        break;
    }
}

void _SsContRpn1(short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    score->unk18 = val;
    score->unk1E++;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsContRpn2(short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    score->unk19 = val;
    score->unk1E++;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

/*
 * CC#6. Right after a loop start (CC#99 = 20) it carries the loop count; this
 * is the only place unk1D is set, so a host that ignores it never loops (the
 * track runs into its end-of-track meta and stops). Otherwise it completes a
 * CC#100/101 pair (RPN: pitch bend range) or a CC#98/99 pair (NRPN: VAB
 * attribute, SsFCALL.ccentry[fn_idx]); both only rewrite VAB data or the bend
 * range of sounding voices, which the host does not model, so the pair is
 * consumed and dropped.
 */
void _SsContDataEntry(
    short seq_access_num, short seq_num, unsigned char val) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    if (score->unk1C == 1 && score->unk15 == 0) {
        score->unk1D = val;
        score->unk1C = 0;
        score->unk15 = 1;
        INFOF("SEP loop count %d seq=%d tr=%d", val, seq_access_num, seq_num);
    } else if (score->unk1E == 2) {
        score->unk1E = 0;
    } else if (score->unk1F == 2) {
        static unsigned s_nrpn_attr_log;
        if (s_nrpn_attr_log < 8) {
            s_nrpn_attr_log++;
            INFOF("SEP NRPN VAB attr %d=%d (msb %d) ignored seq=%d tr=%d",
                  score->fn_idx, val, score->unk1B, seq_access_num, seq_num);
        }
        score->unk1F = 0;
    }
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}

void _SsSetControlChange(
    short seq_access_num, short seq_num, unsigned char control) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 val = *score->seq_ptr++;

    switch (control) {
    case 0x0:
        score->vab_id = val;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        break;
    case 0x6:
        _SsContDataEntry(seq_access_num, seq_num, val);
        break;
    case 0x7:
        _SsContMainVol(seq_access_num, seq_num, val);
        break;
    case 0xA:
        _SsContPanpot(seq_access_num, seq_num, val);
        break;
    case 0xB:
        _SsContExpression(seq_access_num, seq_num, val);
        break;
    case 0x40:
        _SsContDamper(seq_access_num, seq_num, val);
        break;
    case 0x5B:
        _SsContExternal(seq_access_num, seq_num, val);
        break;
    case 0x62:
        _SsContNrpn1(seq_access_num, seq_num, val);
        break;
    case 0x63:
        _SsContNrpn2(seq_access_num, seq_num, val);
        break;
    case 0x64:
        _SsContRpn1(seq_access_num, seq_num, val);
        break;
    case 0x65:
        _SsContRpn2(seq_access_num, seq_num, val);
        break;
    case 0x79:
        _SsContResetAll(seq_access_num, seq_num);
        break;
    default:
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        break;
    }
}

void _SsGetMetaEvent(
    short seq_access_num, short seq_num, unsigned char meta) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u32 us_qn;
    s32 bpm;

    switch (meta) {
    case 0x51:
        us_qn = ((u32)score->seq_ptr[0] << 16) | ((u32)score->seq_ptr[1] << 8) |
                score->seq_ptr[2];
        score->seq_ptr += 3;
        if (us_qn == 0) {
            us_qn = 500000;
        }
        bpm = 60000000 / (s32)us_qn;
        score->tempo = bpm;
        score->unk94 = (u32)bpm;
        if ((score->resolution * score->tempo * 10) < (s32)(VBLANK_MINUS * 60)) {
            score->unk54 = score->unk52 =
                (s16)((VBLANK_MINUS * 600) /
                      (score->resolution * score->tempo));
        } else {
            score->unk52 = -1;
            score->unk54 = (s16)((score->resolution * score->tempo * 10) /
                                 (VBLANK_MINUS * 60));
            if ((s32)(VBLANK_MINUS * 30) <
                (score->resolution * score->tempo * 10) %
                    (s32)(VBLANK_MINUS * 60)) {
                score->unk54++;
            }
        }
        score->unk56 = score->unk54;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        return;
    case 0x2F:
        /* Retail end-of-track handler (0x8002F7F4): while passes remain
         * (l_count 0 = forever) the track restarts from its first event (the
         * play-from position with SEQ_FLAG_400), not from an NRPN loop start. */
        score->unk21++;
        if (score->l_count == 0 || (s8)score->unk21 < (s8)score->l_count) {
            u8* start = (score->flags & SEQ_FLAG_400) ? score->loop_pos
                                                      : score->read_pos;
            score->unk88 = 0;
            score->unk1C = 0;
            score->delta_value = 0;
            score->seq_ptr = start;
            if (score->l_count != 0) {
                score->next_sep_pos = start;
            }
            return;
        }
        score->flags &= ~(SEQ_FLAG_1 | SEQ_FLAG_2 | SEQ_FLAG_8);
        score->flags |= SEQ_FLAG_200 | SEQ_FLAG_4;
        score->unk22 = 0;
        /* retail rewinds the play position so SsSepReplay starts over */
        score->next_sep_pos = (score->flags & SEQ_FLAG_400) ? score->loop_pos
                                                            : score->read_pos;
        _SsVmSeqKeyOff((s16)(seq_access_num | (seq_num << 8)));
        score->delta_value = score->unk54;
        score->play_mode = SSPLAY_PAUSE;
        INFOF("SEP end-of-track seq=%d tr=%d", seq_access_num, seq_num);
        return;
    default:
        /* Unknown meta: stop advancing this tick safely */
        score->delta_value = score->unk54 > 0 ? score->unk54 : 1;
        WARNF("SEP unknown meta 0x%02X seq=%d tr=%d", meta, seq_access_num,
              seq_num);
        return;
    }
}

void _SsVmGetSeqVol(short seq_sep_no, short* voll, short* volr) {
    struct SeqStruct* score =
        &_ss_score[seq_sep_no & 0xFF][(seq_sep_no >> 8) & 0xFF];
    _svm_cur.seq_sep_no = seq_sep_no;
    if (voll) {
        *voll = score->voll;
    }
    if (volr) {
        *volr = score->volr;
    }
}

void SsSeqSetDecrescendo(short seq_access_num, short vol, long v_time) {
    _SsSndSetVolData(seq_access_num, 0, vol, (int)v_time);
    _ss_score[seq_access_num][0].flags |= SEQ_FLAG_20;
    _ss_score[seq_access_num][0].flags &= ~SEQ_FLAG_10;
}

void SsSepSetDecrescendo(
    short sep_access_num, short seq_num, short vol, long v_time) {
    _SsSndSetVolData(sep_access_num, seq_num, vol, (int)v_time);
    _ss_score[sep_access_num][seq_num].flags |= SEQ_FLAG_20;
    _ss_score[sep_access_num][seq_num].flags &= ~SEQ_FLAG_10;
}
