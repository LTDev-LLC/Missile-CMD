#pragma once

// In-memory storage API subset with firmware-style paths, errors, and directory metadata

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
// Route app-relative paths into the virtual filesystem instead of accessing the host disk
#define STORAGE_APP_DATA_PATH_PREFIX "/data"
#define APP_DATA_PATH(x)             STORAGE_APP_DATA_PATH_PREFIX "/" x
#define RECORD_STORAGE               "storage"
typedef struct Storage {
    int unused;
} Storage;
typedef struct File File;
typedef enum {
    FSE_OK,
    FSE_NOT_EXIST,
    FSE_EXIST,
    FSE_DENIED,
    FSE_INVALID_NAME,
    FSE_INTERNAL
} FS_Error;
typedef enum {
    FSAM_READ,
    FSAM_WRITE
} FS_AccessMode;
typedef enum {
    FSOM_OPEN_EXISTING,
    FSOM_CREATE_ALWAYS
} FS_OpenMode;
// Report virtual regular files only, excluding directory entries
bool storage_file_exists(Storage* storage, const char* path);
// Allocate an independent cursor and error state for a virtual file handle
File* storage_file_alloc(Storage* storage);
// Release the handle while leaving its backing data in the virtual filesystem
void storage_file_free(File* file);
// Open a virtual file, truncate on creation, and honor injected open or mutation failures
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode);
// Accept a file close without changing its retained in-memory contents
bool storage_file_close(File* file);
// Return the backing file length in bytes
uint64_t storage_file_size(File* file);
// Read up to the remaining bytes, advancing the cursor unless a read fault is injected
size_t storage_file_read(File* file, void* data, size_t size);
// Write within the fixed backing buffer, simulating a short write on injected failure
size_t storage_file_write(File* file, const void* data, size_t size);
// Simulate a successful sync unless a one-shot or mutation-countdown fault applies
bool storage_file_sync(File* file);
// Copy a virtual file to its destination and retire the source after fault checks
FS_Error storage_common_rename(Storage* storage, const char* source, const char* dest);
// Remove a virtual entry while rejecting nonempty directories and injected failures
FS_Error storage_common_remove(Storage* storage, const char* path);

typedef struct {
    uint8_t flags;
    uint64_t size;
} FileInfo;
#define FSF_DIRECTORY 1U
// Test the directory flag populated by the host storage stubs
bool file_info_is_dir(const FileInfo* info);
// Return virtual entry metadata without opening a file handle
FS_Error storage_common_stat(Storage* storage, const char* path, FileInfo* info);
// Create a virtual directory, reporting existing paths and injected failures
FS_Error storage_common_mkdir(Storage* storage, const char* path);
// Open a virtual directory and preserve the device restriction on trailing slashes
bool storage_dir_open(File* file, const char* path);
// Detach the directory backing entry from its reusable scan handle
bool storage_dir_close(File* file);
// Visit direct children only, reporting end-of-directory separately from injected errors
bool storage_dir_read(File* file, FileInfo* info, char* name, uint16_t capacity);
// Return the last error recorded on the virtual file or directory handle
FS_Error storage_file_get_error(File* file);

bool storage_file_seek(File* file, uint32_t offset, bool from_start);
