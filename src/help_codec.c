#include "help.h"
#include "binary.h"
#include <string.h>

bool mc_help_header_valid(const uint8_t* header, size_t size) {
    return size >= MC_HELP_HEADER_BYTES + McHelpPageCount * MC_HELP_ENTRY_BYTES &&
           size <= UINT16_MAX && !memcmp(header, "MCH1", 4) &&
           mc_read_u32(header + 4) == MC_HELP_CATALOG_ID &&
           mc_read_u16(header + 8) == McHelpPageCount &&
           mc_read_u16(header + 10) == MC_HELP_ENTRY_BYTES;
}
bool mc_help_page_valid(const uint8_t* entry, McHelpPage* page) {
    const uint8_t length = entry[2], count = entry[3];
    if(!length || length > MC_HELP_PAGE_BYTES || !count || count > 5 || page->text[length - 1] ||
       mc_crc32((const uint8_t*)page->text, length) != mc_read_u32(entry + 4))
        return false;
    uint8_t lines = 0;
    for(uint8_t i = 0; i < length; i++) {
        unsigned char c = page->text[i];
        if(!c) {
            if(!i || !page->text[i - 1]) return false;
            lines++;
        } else if(c < 32 || c > 126)
            return false;
    }
    if(lines != count) return false;
    page->lines = lines;
    return true;
}
