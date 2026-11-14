/* N64 ROM files (N-1, XBA-169): byte order (z64 / v64 / n64) normalised to the console's big-endian order, the
 * header (PI timing, checksums, title, game code, region, version), the CIC from the IPL3, and the ED64 / libdragon
 * save-type byte. */
#include "n64_core.h"

#include <string.h>

uint32_t n64_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

n64_rom_order n64_rom_detect(const uint8_t *d, size_t size)
{
    if (!d || size < 4)
        return N64_ROM_UNKNOWN;
    uint32_t w = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    switch (w) {
    case 0x80371240u: return N64_ROM_Z64;
    case 0x37804012u: return N64_ROM_V64;
    case 0x40123780u: return N64_ROM_N64;
    default: break;
    }
    /* Other PI timings (rare): the first byte is 0x80 in the console's order. */
    if (d[0] == 0x80)
        return N64_ROM_Z64;
    if (d[1] == 0x80)
        return N64_ROM_V64;
    if (d[3] == 0x80)
        return N64_ROM_N64;
    return N64_ROM_UNKNOWN;
}

int n64_rom_normalize(uint8_t *d, size_t size)
{
    switch (n64_rom_detect(d, size)) {
    case N64_ROM_Z64:
        return 0;
    case N64_ROM_V64:
        for (size_t i = 0; i + 1 < size; i += 2) {
            uint8_t t = d[i];
            d[i] = d[i + 1];
            d[i + 1] = t;
        }
        return 0;
    case N64_ROM_N64:
        for (size_t i = 0; i + 3 < size; i += 4) {
            uint8_t a = d[i], b = d[i + 1];
            d[i] = d[i + 3];
            d[i + 1] = d[i + 2];
            d[i + 2] = b;
            d[i + 3] = a;
        }
        return 0;
    default:
        return -1;
    }
}

const char *n64_cic_name(n64_cic cic)
{
    switch (cic) {
    case N64_CIC_6101: return "6101";
    case N64_CIC_6102: return "6102/7101";
    case N64_CIC_6103: return "6103/7103";
    case N64_CIC_6105: return "6105/7105";
    case N64_CIC_6106: return "6106/7106";
    case N64_CIC_7102: return "7102";
    case N64_CIC_LIBDRAGON: return "libdragon";
    default: return "unknown (6102)";
    }
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static n64_cic detect_cic(const uint8_t *be, uint32_t crc_fc0)
{
    /* CRC32 of IPL3 over 0x40..0xFFF (n64brew), and over 0x40..0x9FF (ares' table). */
    switch (crc_fc0) {
    case 0x6170A4A1u: return N64_CIC_6101;
    case 0x90BB6CB5u: return N64_CIC_6102;
    case 0x0B050EE0u: return N64_CIC_6103;
    case 0x98BC2C86u: return N64_CIC_6105;
    case 0xACC8580Au: return N64_CIC_6106;
    case 0x009E9EA3u: return N64_CIC_7102;
    default: break;
    }
    switch (n64_crc32(be + 0x40, 0x9C0)) {
    case 0x1DEB51A9u: return N64_CIC_6101;
    case 0xC08E5BD6u: return N64_CIC_6102;
    case 0x03B8376Au: return N64_CIC_6103;
    case 0xCF7F41DCu: return N64_CIC_6105;
    case 0xD1059C6Au: return N64_CIC_6106;
    default: break;
    }
    static const char banner[] = " Libdragon IPL3 ";
    for (size_t i = 0x40; i + sizeof banner - 1 <= 0x1000; i += 4)
        if (memcmp(be + i, banner, sizeof banner - 1) == 0)
            return N64_CIC_LIBDRAGON;
    return N64_CIC_UNKNOWN;
}

int n64_rom_parse(const uint8_t *be, size_t size, n64_rom_info *info)
{
    if (!be || !info || size < 0x1000)
        return -1;
    memset(info, 0, sizeof *info);
    info->order = N64_ROM_Z64;
    info->pi_config = rd32(be);
    info->clock_rate = rd32(be + 4);
    info->boot_address = rd32(be + 8);
    info->release = rd32(be + 12);
    info->crc1 = rd32(be + 16);
    info->crc2 = rd32(be + 20);
    memcpy(info->title, be + 0x20, 20);
    info->title[20] = 0;
    for (int i = 19; i >= 0 && (info->title[i] == ' ' || info->title[i] == 0); i--)
        info->title[i] = 0;
    for (int i = 0; i < 20 && info->title[i]; i++)
        if ((unsigned char)info->title[i] < 32 || (unsigned char)info->title[i] > 126)
            info->title[i] = '?';
    for (int i = 0; i < 4; i++) {
        char ch = (char)be[0x3B + i];
        info->game_code[i] = (ch >= 32 && ch <= 126) ? ch : '?';
    }
    info->game_code[4] = 0;
    info->version = be[0x3F];
    info->region = (char)be[0x3E];
    switch (info->region) {
    case 'D': case 'F': case 'H': case 'I': case 'L': case 'P': case 'S': case 'U': case 'W': case 'X': case 'Y':
        info->pal = 1;
        break;
    default:
        info->pal = 0;
        break;
    }
    info->ipl3_crc = n64_crc32(be + 0x40, 0xFC0);
    info->cic = detect_cic(be, info->ipl3_crc);
    if (be[0x3C] == 'E' && be[0x3D] == 'D') {
        switch (be[0x3F] >> 4) {
        case 1: info->save_hint = N64_SAVE_EEPROM4K; break;
        case 2: info->save_hint = N64_SAVE_EEPROM16K; break;
        case 3: case 4: case 6: info->save_hint = N64_SAVE_SRAM; break; /* 768K / 1M SRAM: the first 32 KB */
        case 5: info->save_hint = N64_SAVE_FLASH; break;
        default: break;
        }
        info->save_hint_rtc = be[0x3F] & 1;
    }
    return 0;
}
