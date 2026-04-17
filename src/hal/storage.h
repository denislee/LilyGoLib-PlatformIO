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
bool hw_read_internal_file(const char *path, std::string &content);
void hw_get_txt_files(std::vector<std::string> &list);
void hw_get_internal_txt_files(std::vector<std::string> &list);

// Storage preference: when true, user-facing apps (editor, tasks, blog) route
// their reads/writes to the SD card. When false (default), they use internal
// FFat. The SD-preference silently falls back to internal if the card is not
// mounted.
bool hw_get_storage_prefer_sd();
void hw_set_storage_prefer_sd(bool prefer_sd);

// USB MSC preference: when true, the SD card is exposed over USB MSC. 
// When false (default), the internal memory (FFat) is exposed.
bool hw_get_msc_prefer_sd();
void hw_set_msc_prefer_sd(bool prefer_sd);

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

// Delete from the preferred storage (honours the internal/SD toggle).
bool hw_delete_preferred_file(const char *path);

// One-shot migration: copies every file under FFat's root to SD's root.
// `copied` / `failed` receive the counts (pass nullptr to ignore). On failure
// of the whole operation (e.g. SD unavailable) returns false and sets *error.
bool hw_copy_internal_to_sd(int *copied, int *failed, std::string *error = nullptr);
