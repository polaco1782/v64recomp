#ifndef BANJO_AUDIO_H
#define BANJO_AUDIO_H
#include "recomp_audio.h"

/* Original cartridge IPL3 boot ucode and core1 n_aspMain. Task fields are
 * populated by thread5_startAudioTask; fingerprints exclude DMEM BSS. */
static const RecompAudioTaskProfile banjo_audio_profile = {
    .descriptor = {0x80283280, 0x100, 0x80272590, 0x1000, 0x80278e80, 0x800},
    .fingerprint_bytes = {0x100, 0xc60, 0x2b0},
    .sha256 = {"41a5f4f68d34a88f8f849be2ccc0cb7ea616ced19de16c2c1460b66995d99e0f",
               "a31055215f1dfd7e071ae9c0cae3bbaa5bf6e4f94dc58eedb3739304218735c7",
               "2cedcb30f29ab3c12b278a258af10045e37d3ed7c2e20e2ae36db9eabc0f8582"}};
#endif
