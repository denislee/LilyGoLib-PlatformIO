/**
 * @file      storage.h
 * @brief     SD card and FFat filesystem operations.
 */
#pragma once

#include "types.h"

void hw_mount_sd();
float hw_get_sd_size();
void hw_get_storage_info(uint64_t &total, uint64_t &used, uint64_t &free);
void hw_get_local_storage_info(uint64_t &total, uint64_t &used, uint64_t &free);

bool hw_save_file(const char *path, const char *content, std::string *error = nullptr);
bool hw_save_internal_file(const char *path, const char *content, std::string *error = nullptr);
bool hw_delete_file(const char *path);
bool hw_delete_internal_file(const char *path);
bool hw_read_file(const char *path, std::string &content);
size_t hw_get_file_size(const char *path);
bool hw_read_file_chunk(const char *path, uint32_t offset, uint32_t size, std::string &content);
bool hw_read_internal_file(const char *path, std::string &content);
void hw_get_txt_files(std::vector<std::string> &list);
void hw_get_internal_txt_files(std::vector<std::string> &list);
void hw_get_sd_txt_files(std::vector<std::string> &list);

// Directory listing including folders. `filter_ext` (e.g. ".txt") limits files
// by extension; pass nullptr or "" to include all files. Folders are always
// included. `mtime` is a unix timestamp (0 when unknown).
struct HwDirEntry {
    std::string path;
    bool is_dir;
    uint32_t mtime;
    uint32_t size;  // bytes; 0 for directories
};
void hw_list_internal_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                              const char *dirname = "/");
void hw_list_sd_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                        const char *dirname = "/");

// Cheap non-allocating count of files (not directories) in the internal FFat
// root. Intended for the status-bar indicator — does not recurse.
uint32_t hw_count_internal_files();
void hw_get_sd_news_files(std::vector<std::string> &list);
void hw_get_news_headers(const char *path, std::vector<std::pair<std::string, size_t>> &headers, bool (*progress_cb)(size_t, size_t) = nullptr);

// Storage preference: when true, user-facing apps (editor, tasks) route
// their reads/writes to the SD card. When false (default), they use internal
// FFat. The SD-preference silently falls back to internal if the card is not
// mounted.
bool hw_get_storage_prefer_sd();
void hw_set_storage_prefer_sd(bool prefer_sd);

// USB MSC preference: when true, the SD card is exposed over USB MSC. 
// When false (default), the internal memory (FFat) is exposed.
bool hw_get_msc_prefer_sd();
void hw_set_msc_prefer_sd(bool prefer_sd);

bool hw_is_usb_msc_reading();
bool hw_is_usb_msc_writing();
bool hw_is_usb_msc_mounted();

// Filesystem dirty flag: set to true whenever a file is saved or deleted.
// Used by high-level apps to invalidate their caches.
bool hw_get_filesystem_dirty();
void hw_set_filesystem_dirty(bool dirty);

// Preference-aware variants used by apps that want to honour the toggle.
bool hw_save_preferred_file(const char *path, const char *content, std::string *error = nullptr);
bool hw_read_preferred_file(const char *path, std::string &content);
void hw_get_preferred_txt_files(std::vector<std::string> &list);

// Partial read: stops after `max_bytes` so callers can build previews cheaply.
// `truncated` (if non-null) is set to true when the file was longer than max_bytes.
bool hw_read_preferred_file_snippet(const char *path, std::string &content,
                                    size_t max_bytes, bool *truncated = nullptr);

// Directory listing with modification time; used to build a stable index key.
// Optional callback: (current_count, total_count_if_known, current_filename)
void hw_get_preferred_txt_files_info(std::vector<std::pair<std::string, uint32_t>> &list,
                                     void (*cb)(int, int, const char *) = nullptr);

// Binary I/O on the preferred storage. Needed for on-disk caches/indexes that
// can contain embedded NUL bytes.
bool hw_save_preferred_bytes(const char *path, const uint8_t *buf, size_t len,
                             std::string *error = nullptr);
bool hw_read_preferred_bytes(const char *path, std::vector<uint8_t> &buf);

// Check if internal storage has too many files and migrate the oldest ones to SD.
// Should be called after saving new files.
void hw_check_and_migrate_storage();

// Manually trigger pruning of internal storage to keep only the 50 newest files.
// Optional callback: (current_count, total_count, current_filename)
void hw_prune_internal_storage(void (*cb)(int, int, const char *) = nullptr);

// Delete from the preferred storage (honours the internal/SD toggle).
bool hw_delete_preferred_file(const char *path);

// One-shot migration: copies every file under FFat's root to SD's root.
// `copied` / `failed` receive the counts (pass nullptr to ignore). On failure
// of the whole operation (e.g. SD unavailable) returns false and sets *error.
// Optional callback: (current_count, total_count_if_known, current_filename)
bool hw_copy_internal_to_sd(int *copied, int *failed, std::string *error = nullptr,
                             void (*cb)(int, int, const char *) = nullptr);
