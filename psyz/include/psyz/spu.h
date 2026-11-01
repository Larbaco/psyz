#ifndef PSYZ_SPU_H
#define PSYZ_SPU_H

/**
 * @file spu.h
 * @brief Platform-agnostic PS1 SPU emulation endpoints.
 */

#include <psyz/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PS1 SPU constants (the SPU emulator is platform-agnostic; SDL or any other
 * audio backend pulls 44100 Hz stereo short frames from Psyz_SpuPullSamples).
 */
#define PSYZ_SPU_RAM_SIZE (512 * 1024) /**< must be a power of two */
#define PSYZ_SPU_NUM_VOICES 24         /**< match PS1 voice count */
#define PSYZ_SPU_SAMPLE_RATE 44100     /**< match fixed PS1 sample rate */

/**
 * @brief Initialize SPU emulation state
 *
 * Idempotent; safe to call multiple times.
 */
void Psyz_SpuInit(void);

/**
 * @brief Reset SPU state
 *
 * @param hot When non-zero, RAM contents are preserved across the reset
 *            (mirrors the PSX-Q "hot init" semantics for libspu).
 */
void Psyz_SpuReset(int hot);

/**
 * @brief Write one 16-bit value into the SPU register file
 *
 * Certain registers can trigger a side-effect. Please refer to psxspx docs for
 * SPU reference.
 *
 * @param reg_offset Offset relative to 0x1F801C00; valid range 0x000-0x1FF
 * @param value 16-bit value to write
 */
void Psyz_SpuWrite(unsigned int regOffset, unsigned short value);

/**
 * @brief Read back one 16-bit value from the SPU register file
 *
 * Internally maps to SPU_RXX.
 *
 * @param reg_offset Offset relative to 0x1F801C00; valid range 0x000-0x1FF
 * @return 16-bit register value
 */
unsigned short Psyz_SpuRead(unsigned int regOffset);

/**
 * @brief Read back the current SPU transfer address
 *
 * Maps to the xfer_addr register.
 *
 * @return Current transfer address
 */
unsigned int Psyz_SpuGetTransferAddr(void);

/**
 * @brief Set the SPU RAM transfer address
 *
 * The destination offset into the 512KB RAM. Maps to the PSX SPU register
 * 0x1F801DA6 (xfer_addr) divided by 8.
 *
 * @param addr Transfer address
 */
void Psyz_SpuSetTransferAddr(unsigned int addr);

/**
 * @brief Push one 16-bit word into the SPU transfer FIFO
 *
 * Maps to a write of SPU register 0x1F801DA8 (xfer_fifo). Each call deposits
 * the word at the current transfer address in SPU RAM and bumps the transfer
 * address by 2.
 *
 * @param word 16-bit word to push
 */
void Psyz_SpuFifoWrite(unsigned short word);

/**
 * @brief Faster bulk version of Psyz_SpuFifoWrite
 *
 * Bypasses individual writes. Uses xfer_addr as the destination address and
 * updates it at the end of the call.
 *
 * @param src Source buffer
 * @param size Number of bytes to transfer
 */
void Psyz_SpuFifoWriteBulk(const unsigned char* src, unsigned int size);

/**
 * @brief Read bytes from SPU RAM
 *
 * Wraps at 512 KB. Does not affect xfer_addr. Useful for debugging.
 *
 * @param offset Byte offset into SPU RAM
 * @param dst Destination buffer
 * @param size Number of bytes to read
 */
void Psyz_SpuMemRead(unsigned int offset, void* dst, unsigned int size);

/**
 * @brief Write bytes into SPU RAM
 *
 * Wraps at 512 KB. Does not affect xfer_addr. Useful for debugging.
 *
 * @param offset Byte offset into SPU RAM
 * @param src Source buffer
 * @param size Number of bytes to write
 */
void Psyz_SpuMemWrite(unsigned int offset, const void* src, unsigned int size);

/**
 * @brief Direct pointer to the 512 KB SPU RAM
 *
 * For tests and offline rendering.
 *
 * @return Pointer to SPU RAM
 */
unsigned char* Psyz_SpuGetRam(void);

/**
 * @brief Generate stereo 16-bit LE PCM frames into out (interleaved L, R)
 *
 * Internally used by the PsyZ Audio subsystem, it can also be used for offline
 * rendering and unit tests.
 *
 * @param out Output buffer
 * @param num_frames Number of stereo frames to generate
 */
void Psyz_SpuPullSamples(short* out, int numFrames);

/**
 * @name Mixer buses (host extension, not a PS1 feature)
 *
 * The PS1 SPU has a single voice mix. This emulator lets the host give each
 * voice a bus and each bus its own gain, so a port can offer separate music
 * and sound-effect volumes. A voice takes its bus when it is keyed on and
 * keeps it until the next key-on. Every bus defaults to unity gain, and a
 * unity gain leaves the mix bit-identical to a build without buses.
 * @{
 */
#define PSYZ_SPU_NUM_BUSES 4       /**< voice buses 0..3; bus 0 is the default */
#define PSYZ_SPU_BUS_CD 4          /**< stats index of the CD/XA input */
#define PSYZ_SPU_GAIN_UNITY 0x8000 /**< Q15 gain 1.0 */

/**
 * @brief Route a libsnd sequence track to a bus
 *
 * Voices started by the SEP/SEQ player for track (seq_access_num, sep_num)
 * take this bus at key-on. Voices that libsnd starts directly (SsUtKeyOn) and
 * tracks never assigned stay on bus 0.
 *
 * @param seq_access_num SEQ access number returned by SsSepOpen
 * @param sep_num Track number within the SEP
 * @param bus 0..PSYZ_SPU_NUM_BUSES-1 (anything else is ignored)
 */
void Psyz_SndSetSeqBus(short seq_access_num, short sep_num, int bus);

/**
 * @brief Set the bus of a voice until its next key-on (tests, direct use)
 */
void Psyz_SpuSetVoiceBus(int voice, int bus);

/**
 * @brief Set a bus gain
 *
 * Applied to each voice's left/right contribution after the voice volume
 * registers, so it also scales that voice's reverb send. Survives
 * Psyz_SpuReset.
 *
 * @param bus 0..PSYZ_SPU_NUM_BUSES-1
 * @param gain Q15, PSYZ_SPU_GAIN_UNITY is 1.0; clamped to 0..UNITY
 */
void Psyz_SpuSetBusGain(int bus, int gain);

/**
 * @brief Set the gain of the CD/XA input (movie audio), same format
 */
void Psyz_SpuSetCdGain(int gain);

/**
 * @brief Per-bus mixer statistics for tests and diagnostics
 *
 * Counted per output frame while enabled. "in" is the contribution before
 * the bus gain, "out" after it. The energies are sums of squares of the left
 * and right samples.
 */
typedef struct PsyzSpuBusStats {
    unsigned long long frames;     /**< output frames counted */
    unsigned long long active;     /**< voice-frames mixed on this bus */
    unsigned long long in_energy;  /**< sum of squares before the bus gain */
    unsigned long long out_energy; /**< sum of squares after the bus gain */
    int in_peak;                   /**< largest |sample| before the gain */
    int out_peak;                  /**< largest |sample| after the gain */
} PsyzSpuBusStats;

/** @brief Start or stop counting, clearing the counters either way */
void Psyz_SpuEnableBusStats(int enable);

/**
 * @brief Copy the counters of buses 0..PSYZ_SPU_NUM_BUSES-1 and the CD input
 *
 * @param out PSYZ_SPU_NUM_BUSES + 1 entries; the last one is PSYZ_SPU_BUS_CD
 */
void Psyz_SpuGetBusStats(PsyzSpuBusStats out[PSYZ_SPU_NUM_BUSES + 1]);
/** @} */

#ifdef __cplusplus
}
#endif

#endif
