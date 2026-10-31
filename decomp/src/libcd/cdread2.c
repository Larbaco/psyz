#include <common.h>
#include <libcd.h>
#include "libcd_stream.h"

#ifdef __psyz
s32 StMode;
/* CdlModeStream is 0x100 — does not fit in the u8 Setmode param. Host libcd
 * needs an explicit stream-active latch for ReadS / Psyz_CdPumpStream. */
void Psyz_CdSetStreamActive(int active);
#endif

static inline void StCdInterrupt2(u_char intr, u_char* result) {
    StCdInterrupt();
}

int CdRead2(long mode) {
    u8 param = mode;
    CdControl(CdlSetmode, &param, NULL);
    if (mode & CdlModeStream) {
        if (mode & CdlModeSize1) {
            StMode = 0;
        } else {
            StMode = 1;
        }
        CdDataCallback(data_ready_callback);
        CdReadyCallback(StCdInterrupt2);
#ifdef __psyz
        Psyz_CdSetStreamActive(1);
#endif
    }
    return CdControl(CdlReadS, NULL, NULL);
}
