#include "help_storage.h"
#include "binary.h"
#include <string.h>

void mc_help_read(Storage* storage, McHelpPage* page) {
    page->status = McHelpUnavailable;
    page->lines = 0;
    memset(page->text, 0, sizeof(page->text));
    if(page->id >= McHelpPageCount) return;
    File* file = storage_file_alloc(storage);
    if(!file) return;
    uint8_t header[MC_HELP_HEADER_BYTES], entry[MC_HELP_ENTRY_BYTES];
    if(storage_file_open(file, MC_HELP_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t size = storage_file_size(file);
        if(size <= UINT16_MAX &&
           storage_file_read(file, header, sizeof(header)) == sizeof(header) &&
           mc_help_header_valid(header, (size_t)size) &&
           storage_file_seek(file, MC_HELP_HEADER_BYTES + page->id * MC_HELP_ENTRY_BYTES, true) &&
           storage_file_read(file, entry, sizeof(entry)) == sizeof(entry)) {
            const uint16_t offset = mc_read_u16(entry);
            const uint8_t length = entry[2];
            if(offset >= MC_HELP_HEADER_BYTES + McHelpPageCount * MC_HELP_ENTRY_BYTES && length &&
               length <= sizeof(page->text) && offset + (uint32_t)length <= size &&
               storage_file_seek(file, offset, true) &&
               storage_file_read(file, page->text, length) == length &&
               mc_help_page_valid(entry, page))
                page->status = McHelpReady;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
}
