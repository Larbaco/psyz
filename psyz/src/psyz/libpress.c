#include <psyz.h>
#include <libpress.h>
#include <psyz/log.h>
#include "../../../decomp/src/libpress/libpress_private.h"
#include "mdec.h"

void MDEC_reset(int mode) {
    (void)mode;
    Psyz_MdecReset();
}

void MDEC_in(u_long* buf, int size) {
    if (!buf || size < 0)
        return;
    /* size is the payload word count (not including the command word).
       DecDCTPutEnv: MDEC_in(mdec_iq, 32) with mdec_iq[0]=0x40000001 and
       32 quant words at mdec_iq[1..32]; DecDCTin: MDEC_in(buf, *(u_short*)buf). */
    Psyz_MdecCommand((uint32_t)buf[0], size > 0 ? (const void*)(buf + 1) : NULL,
                     (size_t)size);
}

void MDEC_out(u_long* buf, int size) {
    if (!buf || size <= 0)
        return;
    Psyz_MdecRead(buf, (size_t)size);
}

int MDEC_in_sync(void) { return 0; }

int MDEC_out_sync(void) { return 0; }

u_long MDEC_status(void) { return 0; }

/* Weak stub: dw3-pc overrides with port/movies/dct_vlc.c (bsdec). */
__attribute__((weak)) int DecDCTvlc(u_long* bs, u_long* buf) {
    (void)bs;
    (void)buf;
    NOT_IMPLEMENTED;
    return -1;
}
