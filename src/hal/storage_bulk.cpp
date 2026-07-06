/**
 * @file      storage_bulk.cpp
 * @brief     Bulk-copy operations split out from storage.cpp.
 *
 * Both functions are coordination layers that read from the on-device
 * stores via the public storage.h API and forward to the hub (notes
 * upload) or the SD card. They have no file-static dependencies on
 * storage.cpp — no helpers, no globals — so the extraction is a clean
 * cut along the API boundary.
 */
#include "storage.h"
#include "system.h"
#include "hub.h"
#include "../core/spi_lock.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#endif

bool hw_copy_all_notes_to_hub(int *copied, int *failed, std::string *error,
                              void (*cb)(int, int, const char *))
{
    if (copied) *copied = 0;
    if (failed) *failed = 0;
#ifdef ARDUINO
    if (!hal::hub_is_enabled()) {
        if (error) *error = "Hub is not enabled.";
        return false;
    }

    // Walk internal first, then SD; same-name conflicts resolve in favour of
    // internal (which is the user's most-recently-edited copy by convention
    // — see notes-sync rationale). Read raw bytes so encrypted Salted__
    // blobs ride through verbatim.
    std::vector<std::string> internal_names;
    hw_get_internal_txt_files(internal_names);

    std::vector<std::string> sd_names;
    if (HW_SD_ONLINE & hw_get_device_online()) {
        hw_get_sd_txt_files(sd_names);
    }

    auto strip_slash = [](std::string &p) {
        if (!p.empty() && p[0] == '/') p.erase(0, 1);
    };

    struct UpItem { std::string name; bool internal; };
    std::vector<UpItem> items;
    items.reserve(internal_names.size() + sd_names.size());

    // Set-backed dedupe: with 200 internal + 200 SD notes the old nested
    // linear scan was ~40k string compares; a set makes each SD lookup O(log n).
    std::set<std::string> seen;
    for (auto &n : internal_names) {
        strip_slash(n);
        if (n.empty()) continue;
        items.push_back({n, true});
        seen.insert(n);
    }
    for (auto &n : sd_names) {
        strip_slash(n);
        if (n.empty()) continue;
        if (seen.count(n)) continue;
        items.push_back({n, false});
    }

    int total = (int)items.size();
    int ok = 0, fail = 0;
    std::string last_err;
    for (int i = 0; i < total; ++i) {
        const auto &it = items[i];
        if (cb) cb(i, total, it.name.c_str());

        std::vector<uint8_t> bytes;
        std::string abs = "/" + it.name;
        bool read_ok = it.internal
            ? hw_read_internal_bytes_raw(abs.c_str(), bytes)
            : hw_read_sd_bytes_raw(abs.c_str(), bytes);
        if (!read_ok) {
            fail++;
            continue;
        }
        HalError uerr = hal::hub_upload_note(it.name.c_str(), bytes.data(), bytes.size());
        if (uerr == HalError::Ok) {
            ok++;
        } else {
            fail++;
            last_err = hal_error_string(uerr);
        }
    }
    if (cb) cb(total, total, "Done");

    if (copied) *copied = ok;
    if (failed) *failed = fail;
    if (ok == 0 && total > 0) {
        if (error) *error = last_err.empty() ? "No notes uploaded." : last_err;
        return false;
    }
    return true;
#else
    (void)error; (void)cb;
    return false;
#endif
}

bool hw_copy_internal_to_sd(int *copied, int *failed, std::string *error, void (*cb)(int, int, const char *))
{
    if (copied) *copied = 0;
    if (failed) *failed = 0;
#ifdef ARDUINO
    if (!(HW_SD_ONLINE & hw_get_device_online())) {
        // Attempt to mount before giving up.
        hw_mount_sd();
        if (!(HW_SD_ONLINE & hw_get_device_online())) {
            if (error) *error = "SD card not available.";
            return false;
        }
    }

    File root = FFat.open("/");
    if (!root || !root.isDirectory()) {
        if (error) *error = "Cannot open internal storage.";
        return false;
    }

    // First pass: count files
    int total_files = 0;
    File count_entry = root.openNextFile();
    while (count_entry) {
        if (!count_entry.isDirectory()) {
            total_files++;
        }
        count_entry.close();
        count_entry = root.openNextFile();
    }
    root.close();

    // Re-open for copying
    root = FFat.open("/");
    int ok_count = 0;
    int fail_count = 0;
    int current_idx = 0;
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            String dst = (name[0] == '/') ? String(name) : ("/" + String(name));

            if (cb) cb(current_idx, total_files, name);

            size_t size = entry.size();
            std::string buf;
            buf.resize(size);
            if (size > 0) {
                entry.read((uint8_t *)&buf[0], size);
            }
            entry.close();

            bool wrote = false;
            {
                core::ScopedSpiLock lock;
                File out = SD.open(dst, "w");
                if (out) {
                    size_t n = size > 0 ? out.write((const uint8_t *)buf.data(), size) : 0;
                    wrote = (size == 0) || (n == size);
                    out.close();
                }
            }

            if (wrote) ok_count++;
            else fail_count++;

            current_idx++;
        } else {
            entry.close();
        }
        entry = root.openNextFile();
    }
    root.close();

    if (cb) cb(total_files, total_files, "Done");

    if (copied) *copied = ok_count;
    if (failed) *failed = fail_count;
    if (fail_count > 0 && error) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d file(s) failed to copy.", fail_count);
        *error = msg;
    }
    return fail_count == 0;
#else
    (void)error;
    (void)cb;
    return false;
#endif
}
