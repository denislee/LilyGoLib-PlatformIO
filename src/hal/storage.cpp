/**
 * @file      storage.cpp
 * @brief     SD card and FFat filesystem operations.
 */
#include "storage.h"
#include "system.h"
#include "internal.h"

#ifdef ARDUINO
#include <algorithm>
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#endif

float hw_get_sd_size()
{
    float size = 0.0;
#if defined(ARDUINO)

#if defined(HAS_SD_CARD_SOCKET)
    size = SD.cardSize() / 1024 / 1024 / 1024.0;

#elif defined(USING_FATFS)
    size = FFat.totalBytes() / 1024 / 1024;
#endif

#endif
    return size;
}

void hw_get_storage_info(uint64_t &total, uint64_t &used, uint64_t &free)
{
    total = 0;
    used = 0;
    free = 0;
#if defined(ARDUINO)
#if defined(HAS_SD_CARD_SOCKET)
    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        total = SD.totalBytes();
        used = SD.usedBytes();
        free = total - used;
        instance.unlockSPI();
    }
#elif defined(USING_FATFS)
    total = FFat.totalBytes();
    used = FFat.usedBytes();
    free = FFat.freeBytes();
#endif
#endif
}

void hw_get_local_storage_info(uint64_t &total, uint64_t &used, uint64_t &free)
{
    total = 0;
    used = 0;
    free = 0;
#if defined(ARDUINO)
#if defined(USING_FATFS)
    total = FFat.totalBytes();
    used = FFat.usedBytes();
    free = FFat.freeBytes();
#endif
#endif
}

void hw_mount_sd()
{
#if defined(ARDUINO) && defined(HAS_SD_CARD_SOCKET)
    instance.installSD();
#endif
}

static bool filesystem_dirty = false;

bool hw_get_filesystem_dirty() { return filesystem_dirty; }
void hw_set_filesystem_dirty(bool dirty) { filesystem_dirty = dirty; }

bool hw_save_file(const char *path, const char *content, std::string *error)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f;
    bool lock = false;
    bool is_sd = (HW_SD_ONLINE & hw_get_device_online());
    const char *target = "Internal";

    // If it already exists on Internal, save it there to avoid confusion
    bool exists_internal = FFat.exists(str);

    printf("Attempting to save to %s (SD: %s, Internal Exists: %s)\n",
           str.c_str(), is_sd ? "Yes" : "No", exists_internal ? "Yes" : "No");

    if (exists_internal) {
        f = FFat.open(str, "w");
    } else if (is_sd) {
        instance.lockSPI();
        f = SD.open(str, "w"); // Use "w" for overwrite
        lock = true;
        target = "SD";
    } else {
        f = FFat.open(str, "w"); // Use "w" for overwrite
    }

    if (!f) {
        printf("Failed to open file for writing: %s\n", str.c_str());
        if (lock) instance.unlockSPI();
        if (error) {
            *error = std::string("Cannot open ") + target + " file for writing.";
        }
        return false;
    }

    size_t written = f.print(content);
    f.close();
    if (lock) instance.unlockSPI();

    printf("Saved %u bytes to %s (%s)\n", (unsigned int)written, str.c_str(), lock ? "SD" : "Internal");
    bool ok = (written > 0 || strlen(content) == 0);
    if (!ok && error) {
        *error = std::string("Write to ") + target + " failed (storage full?).";
    }
    return ok;
#else
    (void)error;
    printf("Save to file: %s, content: %s\n", path, content);
    return true;
#endif
}

bool hw_save_internal_file(const char *path, const char *content, std::string *error)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f;

    printf("Attempting to save to internal %s\n", str.c_str());

    f = FFat.open(str, "w");

    if (!f) {
        printf("Failed to open internal file for writing: %s\n", str.c_str());
        if (error) {
            *error = "Cannot open Internal file for writing.";
        }
        return false;
    }

    size_t written = f.print(content);
    f.close();

    printf("Saved %u bytes to internal %s\n", (unsigned int)written, str.c_str());
    bool ok = (written > 0 || strlen(content) == 0);
    if (!ok && error) {
        *error = "Write to Internal failed (storage full?).";
    }
    return ok;
#else
    (void)error;
    printf("Save to internal file: %s, content: %s\n", path, content);
    return true;
#endif
}

bool hw_delete_file(const char *path)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    bool res_sd = false;
    bool res_int = false;
    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        res_sd = SD.remove(str);
        instance.unlockSPI();
    }
    res_int = FFat.remove(str);
    return res_sd || res_int;
#else
    printf("Delete file: %s\n", path);
    return true;
#endif
}

bool hw_delete_internal_file(const char *path)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    return FFat.remove(str);
#else
    printf("Delete internal file: %s\n", path);
    return true;
#endif
}

bool hw_read_file(const char *path, std::string &content)
{
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f;
    bool lock = false;
    bool is_sd = (HW_SD_ONLINE & hw_get_device_online());

    if (is_sd) {
        instance.lockSPI();
        f = SD.open(str, FILE_READ);
        if (f) {
            lock = true;
        } else {
            instance.unlockSPI();
        }
    }

    if (!f) {
        f = FFat.open(str, FILE_READ);
    }

    if (!f) {
        printf("Failed to open file for reading: %s\n", str.c_str());
        return false;
    }

    size_t size = f.size();
    content.resize(size);
    if (size > 0) {
        f.read((uint8_t *)&content[0], size);
    }
    f.close();
    if (lock) instance.unlockSPI();
    printf("Read %u bytes from %s (%s)\n", (unsigned int)size, str.c_str(), lock ? "SD" : "Internal");
    return true;
#else
    printf("Read from file: %s\n", path);
    content = "Dummy content for simulation";
    return true;
#endif
}

bool hw_read_internal_file(const char *path, std::string &content)
{
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f;

    printf("Attempting to read internal %s\n", str.c_str());

    f = FFat.open(str, FILE_READ);

    if (!f) {
        printf("Failed to open internal file for reading: %s\n", str.c_str());
        return false;
    }
    size_t size = f.size();
    content.resize(size);
    if (size > 0) {
        f.read((uint8_t *)&content[0], size);
    }
    f.close();
    printf("Read %u bytes from internal %s\n", (unsigned int)size, str.c_str());
    return true;
#else
    printf("Read from internal file: %s\n", path);
    content = "Dummy internal content for simulation";
    return true;
#endif
}

#ifdef ARDUINO
struct FileInfo {
    std::string name;
    time_t time;
};

static void list_files(std::vector<FileInfo> &list, fs::FS &fs, const char *dirname, const char *ext,
                       void (*cb)(int, int, const char *) = nullptr)
{
    File root = fs.open(dirname);
    if (!root || !root.isDirectory()) return;

    File file = root.openNextFile();
    int count = 0;
    while (file) {
        if (!file.isDirectory()) {
            String filename = file.name();
            if (filename.endsWith(ext)) {
                list.push_back({filename.c_str(), file.getLastWrite()});
                count++;
                if (cb) cb(count, 0, filename.c_str());
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}
#endif

void hw_get_txt_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    std::vector<FileInfo> file_infos;
    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        list_files(file_infos, SD, "/", ".txt");
        instance.unlockSPI();
    }
    list_files(file_infos, FFat, "/", ".txt");
    // ... (rest remains same)

    std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo& a, const FileInfo& b) {
        if (a.time != b.time) {
            return a.time > b.time;
        }
        // If timestamps are identical or both 0, fall back to sorting by filename
        // descending (since our filenames start with YYYYMMDD_HHMMSS)
        return a.name > b.name;
    });

    for (const auto& fi : file_infos) {
        list.push_back(fi.name);
    }
#else
    list.push_back("test1.txt");
    list.push_back("test2.txt");
#endif
}

void hw_get_internal_txt_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    std::vector<FileInfo> file_infos;
    list_files(file_infos, FFat, "/", ".txt");

    std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo& a, const FileInfo& b) {
        if (a.time != b.time) {
            return a.time > b.time;
        }
        return a.name > b.name;
    });

    for (const auto& fi : file_infos) {
        list.push_back(fi.name);
    }
#else
    list.push_back("internal1.txt");
    list.push_back("internal2.txt");
#endif
}

void hw_get_sd_txt_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    if (HW_SD_ONLINE & hw_get_device_online()) {
        std::vector<FileInfo> file_infos;
        instance.lockSPI();
        list_files(file_infos, SD, "/", ".txt");
        instance.unlockSPI();

        std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo & a, const FileInfo & b) {
            if (a.time != b.time) return a.time > b.time;
            return a.name > b.name;
        });

        for (const auto &fi : file_infos) {
            list.push_back(fi.name);
        }
    }
#else
    list.push_back("sd1.txt");
    list.push_back("sd2.txt");
#endif
}

bool hw_get_storage_prefer_sd()
{
    return user_setting.storage_prefer_sd != 0;
}

void hw_set_storage_prefer_sd(bool prefer_sd)
{
    user_setting.storage_prefer_sd = prefer_sd ? 1 : 0;
#ifdef ARDUINO
    save_user_setting_nvs();
    if (prefer_sd) {
        // Mount SD on demand so the new preference takes effect immediately.
        hw_mount_sd();
    }
#endif
}

bool hw_get_msc_prefer_sd()
{
    return user_setting.msc_prefer_sd != 0;
}

void hw_set_msc_prefer_sd(bool prefer_sd)
{
    user_setting.msc_prefer_sd = prefer_sd ? 1 : 0;
#ifdef ARDUINO
    instance.setMSCPreferSD(prefer_sd);
    save_user_setting_nvs();
    ui_msg_pop_up("USB MSC", "Settings updated.\nPlease reboot to apply\nMSC target change.");
#endif
}

// Effective storage pick: honour the user preference, but fall back to internal
// when the SD card is not present/mounted — writes silently landing on a
// missing card would be surprising.
static bool storage_should_use_sd()
{
#ifdef ARDUINO
    if (!user_setting.storage_prefer_sd) return false;
    return (HW_SD_ONLINE & hw_get_device_online()) != 0;
#else
    return false;
#endif
}

void hw_prune_internal_storage(void (*cb)(int, int, const char *))
{
#ifdef ARDUINO
    if (cb) cb(0, 0, "Scanning internal storage...");
    std::vector<FileInfo> infos;
    list_files(infos, FFat, "/", ".txt");

    // Filter out tasks.txt
    infos.erase(std::remove_if(infos.begin(), infos.end(),
                               [](const FileInfo &fi) {
                                   return fi.name == "/tasks.txt" || fi.name == "tasks.txt";
                               }),
                infos.end());

    if (infos.size() <= 50) {
        if (cb) cb(0, 0, "No pruning needed (<= 50 files).");
        return;
    }

    // Sort by time ascending (oldest first)
    std::sort(infos.begin(), infos.end(), [](const FileInfo &a, const FileInfo &b) {
        if (a.time != b.time) return a.time < b.time;
        return a.name < b.name;
    });

    size_t total_to_delete = infos.size() - 50;

    // Check if SD is available for backup
    bool sd_online = (HW_SD_ONLINE & hw_get_device_online());
    if (!sd_online) {
        hw_mount_sd();
        sd_online = (HW_SD_ONLINE & hw_get_device_online());
    }

    // Phase 1: Backup (if SD available)
    if (sd_online) {
        for (size_t i = 0; i < total_to_delete; ++i) {
            String path = infos[i].name.c_str();
            if (!path.startsWith("/")) path = "/" + path;

            if (cb) cb((int)i, (int)total_to_delete, ("Backing up: " + path).c_str());

            File src = FFat.open(path);
            if (src) {
                size_t sz = src.size();
                std::vector<uint8_t> buf(sz);
                src.read(buf.data(), sz);
                src.close();

                instance.lockSPI();
                File dst = SD.open(path, "w");
                if (dst) {
                    dst.write(buf.data(), sz);
                    dst.close();
                }
                instance.unlockSPI();
            }
        }
    }

    // Phase 2: Pruning
    for (size_t i = 0; i < total_to_delete; ++i) {
        String path = infos[i].name.c_str();
        if (!path.startsWith("/")) path = "/" + path;

        if (cb) cb((int)i, (int)total_to_delete, ("Deleting: " + path).c_str());
        printf("Manual pruning: %s\n", path.c_str());
        FFat.remove(path);
    }

    if (cb) cb((int)total_to_delete, (int)total_to_delete, "Pruning complete.");
    filesystem_dirty = true;
#endif
}

static void prune_internal_storage()
{
#ifdef ARDUINO
    if (!user_setting.prune_internal) return;
    hw_prune_internal_storage(nullptr);
#endif
}

bool hw_save_preferred_file(const char *path, const char *content, std::string *error)
{
    filesystem_dirty = true;
    bool ok = hw_save_internal_file(path, content, error);

    // Redundant save to SD if available
    if (HW_SD_ONLINE & hw_get_device_online()) {
#ifdef ARDUINO
        String str = (path[0] == '/') ? String(path) : ("/" + String(path));
        instance.lockSPI();
        File f = SD.open(str, "w");
        if (f) {
            f.print(content);
            f.close();
        }
        instance.unlockSPI();
#endif
    }

    // Ensure internal storage limit
    prune_internal_storage();

    return ok;
}

bool hw_read_preferred_file(const char *path, std::string &content)
{
    // Strictly use internal storage for core apps primary logic
    return hw_read_internal_file(path, content);
}

void hw_get_preferred_txt_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    std::vector<FileInfo> file_infos;
    // Strictly use internal storage
    list_files(file_infos, FFat, "/", ".txt");

    // Sort descending by time/name
    std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo &a, const FileInfo &b) {
        if (a.time != b.time) return a.time > b.time;
        return a.name > b.name;
    });

    for (const auto &fi : file_infos) {
        list.push_back(fi.name);
    }
#else
    list.push_back("preferred1.txt");
    list.push_back("preferred2.txt");
#endif
}

bool hw_read_preferred_file_snippet(const char *path, std::string &content, size_t max_bytes, bool *truncated)
{
    content.clear();
    if (truncated) *truncated = false;
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f;

    // Strictly use internal storage
    f = FFat.open(str, FILE_READ);

    if (!f) return false;

    size_t total = f.size();
    size_t to_read = total < max_bytes ? total : max_bytes;
    content.resize(to_read);
    if (to_read > 0) {
        f.read((uint8_t *)&content[0], to_read);
    }
    f.close();

    if (truncated) *truncated = total > max_bytes;
    return true;
#else
    (void)path; (void)max_bytes;
    content = "simulated preview...";
    if (truncated) *truncated = true;
    return true;
#endif
}

void hw_get_preferred_txt_files_info(std::vector<std::pair<std::string, uint32_t>> &list,
                                     void (*cb)(int, int, const char *))
{
    list.clear();
#ifdef ARDUINO
    std::vector<FileInfo> infos;
    // Strictly use internal storage
    list_files(infos, FFat, "/", ".txt", cb);

    list.reserve(infos.size());
    for (const auto &fi : infos) {
        list.emplace_back(fi.name, (uint32_t)fi.time);
    }
#endif
}

bool hw_save_preferred_bytes(const char *path, const uint8_t *buf, size_t len, std::string *error)
{
    // Don't set dirty for the index file itself!
    bool is_index = (strstr(path, "_idx.bin") != nullptr);
    if (!is_index) {
        filesystem_dirty = true;
    }

#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));

    // Save to Internal
    File f = FFat.open(str, "w");
    if (!f) {
        if (error) *error = "Cannot open Internal file for writing.";
        return false;
    }
    size_t written = f.write(buf, len);
    f.close();
    bool ok = (written == len);

    // Redundant save to SD if available
    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        File fsd = SD.open(str, "w");
        if (fsd) {
            fsd.write(buf, len);
            fsd.close();
        }
        instance.unlockSPI();
    }

    // Only prune if it was a data file (not the index)
    if (!is_index) {
        prune_internal_storage();
    }

    return ok;
#else
    (void)path; (void)buf; (void)len; (void)error;
    return true;
#endif
}

bool hw_read_preferred_bytes(const char *path, std::vector<uint8_t> &buf)
{
    buf.clear();
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    bool use_sd = storage_should_use_sd();
    File f;
    bool lock = false;

    if (use_sd) {
        instance.lockSPI();
        f = SD.open(str, FILE_READ);
        if (f) {
            lock = true;
        } else {
            instance.unlockSPI();
        }
    }
    if (!f) {
        f = FFat.open(str, FILE_READ);
    }
    if (!f) return false;

    size_t size = f.size();
    buf.resize(size);
    if (size > 0) {
        f.read(buf.data(), size);
    }
    f.close();
    if (lock) instance.unlockSPI();
    return true;
#else
    (void)path;
    return false;
#endif
}

bool hw_delete_preferred_file(const char *path)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    if (storage_should_use_sd()) {
        instance.lockSPI();
        bool ok = SD.remove(str);
        instance.unlockSPI();
        return ok;
    }
    return FFat.remove(str);
#else
    (void)path;
    return true;
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
            String name = entry.name();
            String dst = name.startsWith("/") ? name : ("/" + name);

            if (cb) cb(current_idx, total_files, name.c_str());

            size_t size = entry.size();
            std::string buf;
            buf.resize(size);
            if (size > 0) {
                entry.read((uint8_t *)&buf[0], size);
            }
            entry.close();

            instance.lockSPI();
            File out = SD.open(dst, "w");
            bool wrote = false;
            if (out) {
                size_t n = size > 0 ? out.write((const uint8_t *)buf.data(), size) : 0;
                wrote = (size == 0) || (n == size);
                out.close();
            }
            instance.unlockSPI();

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
