#include "../main/AmiiboAuth.h"
#include "../main/AmiiboData.h"

constexpr bool masked_dump() {
    uint8_t m[540] = {};
    m[0]=4; m[1]=0x11; m[2]=0x22; m[4]=0x33; m[5]=0x44; m[6]=0x55; m[7]=0x66;
    m[538]=0xAB;
    return amiibo::restore_masked_credentials(m) &&
        m[532]==0x88 && m[533]==0x33 && m[534]==0xCC && m[535]==0x77 &&
        m[536]==0x80 && m[537]==0x80 && m[538]==0xAB;
}
constexpr bool explicit_credentials() {
    for (unsigned position=532; position<538; ++position) {
        uint8_t m[540]={}; m[position]=0x42;
        if (amiibo::restore_masked_credentials(m)) return false;
        for (unsigned i=0;i<540;++i) if (m[i] != (i==position ? 0x42 : 0)) return false;
    }
    return true;
}
constexpr bool real_dump() {
    uint8_t m[540]={};
    for(unsigned i=0;i<540;++i) m[i]=AMIIBO_DATA[i];
    if (!amiibo::restore_masked_credentials(m)) return false;
    if (amiibo::restore_masked_credentials(m)) return false;
    for(unsigned i=0;i<540;++i) {
        if ((i<532 || i>=538) && m[i]!=AMIIBO_DATA[i]) return false;
    }
    return m[536]==0x80 && m[537]==0x80;
}
static_assert(masked_dump(), "UID mapping and credentials");
static_assert(explicit_credentials(), "Never overwrite explicit or partially present credentials");
static_assert(real_dump(), "Actual dump: change only masked credentials, idempotent");
