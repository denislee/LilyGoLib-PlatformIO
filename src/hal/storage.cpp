/**
 * @file      storage.cpp
 * @brief     SD card and FFat filesystem operations.
 */
#include "storage.h"
#include "system.h"
#include "internal.h"
#include "notes_crypto.h"

#include <cstring>

#ifdef ARDUINO
#include <algorithm>
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#endif

/* When notes_crypto is enabled+unlocked, the text I/O wrappers below
 * transparently encrypt on save and decrypt on load for any path that
 * `notes_crypto_path_is_protected()` matches. On a locked session, a read
 * that hits encrypted content fails so callers don't show ciphertext. */
static bool content_has_salted_magic(const char *buf, size_t len)
{
    return len >= 8 && memcmp(buf, "Salted__", 8) == 0;
}

/* Returns true if the encoded bytes were written into `out`. Either a
 * passthrough (plaintext copy) or the ciphertext. Returns false only if the
 * caller should abort the save — i.e. crypto is enabled for this path but the
 * session is locked, which would otherwise silently leak plaintext to disk. */
static bool encode_for_write(const char *path, const char *content,
                              std::vector<uint8_t> &out, std::string *error)
{
    size_t n = content ? strlen(content) : 0;
    bool protect = notes_crypto_path_is_protected(path);
    bool enabled = notes_crypto_is_enabled();

    if (protect && enabled) {
        if (!notes_crypto_is_unlocked()) {
            if (error) *error = "Notes are locked. Unlock first.";
            return false;
        }
        if (!notes_crypto_encrypt_buffer((const uint8_t *)content, n, out)) {
            if (error) *error = "Encryption failed.";
            return false;
        }
        return true;
    }
    out.assign((const uint8_t *)content, (const uint8_t *)content + n);
    return true;
}

/* If the just-read string begins with the OpenSSL magic, decrypt it in place.
 * Returns false only if the magic was present but decryption failed; callers
 * treat that as a read error so the user never sees ciphertext as text. */
static bool decode_after_read(std::string &content)
{
    if (!content_has_salted_magic(content.data(), content.size())) return true;
    if (!notes_crypto_is_unlocked()) return false;
    std::string plain;
    if (!notes_crypto_decrypt_buffer((const uint8_t *)content.data(),
                                     content.size(), plain)) return false;
    content = std::move(plain);
    return true;
}

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

// Vendor hooks from lib/LilyGoLib/src/USB_MSC.cpp — no header is exposed
// upstream, so we declare them here rather than reaching into the lib/.
extern bool is_usb_msc_reading();
extern bool is_usb_msc_writing();
extern bool is_usb_msc_mounted();

bool hw_is_usb_msc_reading() {
#ifdef ARDUINO
#if !ARDUINO_USB_MODE
    return is_usb_msc_reading();
#else
    return false;
#endif
#else
    return false;
#endif
}

bool hw_is_usb_msc_writing() {
#ifdef ARDUINO
#if !ARDUINO_USB_MODE
    return is_usb_msc_writing();
#else
    return false;
#endif
#else
    return false;
#endif
}

bool hw_is_usb_msc_mounted() {
#ifdef ARDUINO
#if !ARDUINO_USB_MODE
    return is_usb_msc_mounted();
#else
    return false;
#endif
#else
    return false;
#endif
}

bool hw_get_filesystem_dirty() { return filesystem_dirty; }
void hw_set_filesystem_dirty(bool dirty) { filesystem_dirty = dirty; }

bool hw_save_file(const char *path, const char *content, std::string *error)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    std::vector<uint8_t> payload;
    if (!encode_for_write(path, content, payload, error)) return false;

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

    size_t written = payload.empty() ? 0 : f.write(payload.data(), payload.size());
    f.close();
    if (lock) instance.unlockSPI();

    printf("Saved %u bytes to %s (%s)\n", (unsigned int)written, str.c_str(), lock ? "SD" : "Internal");
    bool ok = (written == payload.size());
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
    std::vector<uint8_t> payload;
    if (!encode_for_write(path, content, payload, error)) return false;

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

    size_t written = payload.empty() ? 0 : f.write(payload.data(), payload.size());
    f.close();

    printf("Saved %u bytes to internal %s\n", (unsigned int)written, str.c_str());
    bool ok = (written == payload.size());
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

#ifdef ARDUINO
/* Recursively walk and remove. Children are snapshotted first because
 * openNextFile() iterators don't survive mutations. */
static bool delete_path_recursive(fs::FS &fs, const String &path)
{
    File f = fs.open(path);
    if (!f) return false;
    if (!f.isDirectory()) {
        f.close();
        return fs.remove(path);
    }

    std::vector<String> children;
    File entry = f.openNextFile();
    while (entry) {
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        String full = path;
        if (!full.endsWith("/")) full += "/";
        full += name;
        children.push_back(full);
        entry.close();
        entry = f.openNextFile();
    }
    f.close();

    for (const auto &c : children) {
        if (!delete_path_recursive(fs, c)) return false;
    }
    return fs.rmdir(path);
}
#endif

bool hw_delete_path(const char *path, bool use_sd)
{
    filesystem_dirty = true;
#ifdef ARDUINO
    if (!path || !path[0]) return false;
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    if (use_sd) {
        if (!(HW_SD_ONLINE & hw_get_device_online())) return false;
        instance.lockSPI();
        bool ok = delete_path_recursive(SD, str);
        instance.unlockSPI();
        return ok;
    }
    return delete_path_recursive(FFat, str);
#else
    (void)use_sd;
    printf("Delete path: %s\n", path);
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
    if (!decode_after_read(content)) {
        content.clear();
        return false;
    }
    return true;
#else
    printf("Read from file: %s\n", path);
    content = "Dummy content for simulation";
    return true;
#endif
}

size_t hw_get_file_size(const char *path)
{
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f;
    bool lock = false;
    bool is_sd = (HW_SD_ONLINE & hw_get_device_online());
    size_t size = 0;

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

    if (f) {
        size = f.size();
        f.close();
    }
    if (lock) instance.unlockSPI();
    return size;
#else
    return 1024;
#endif
}

bool hw_read_file_chunk(const char *path, uint32_t offset, uint32_t size, std::string &content)
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
        printf("Failed to open file for reading chunk: %s\n", str.c_str());
        return false;
    }

    if (offset > f.size()) {
        f.close();
        if (lock) instance.unlockSPI();
        return false;
    }

    f.seek(offset);
    size_t available_size = f.size() - offset;
    size_t read_size = (size < available_size) ? size : available_size;

    content.resize(read_size);
    if (read_size > 0) {
        f.read((uint8_t *)&content[0], read_size);
    }
    f.close();
    if (lock) instance.unlockSPI();
    
    // Attempt to slice content neatly at a space or newline so we don't cut words in half
    // Only if we haven't reached the end of the file.
    if (read_size == size && read_size > 0) {
        int cut_pos = read_size - 1;
        while (cut_pos > 0 && content[cut_pos] != ' ' && content[cut_pos] != '\n' && content[cut_pos] != '\r') {
            cut_pos--;
        }
        if (cut_pos > (int)(size / 2)) {
            // valid cut point
            content.resize(cut_pos);
        } else {
            // If we couldn't find a space, at least ensure we don't cut a UTF-8 character
            cut_pos = read_size - 1;
            while (cut_pos > 0 && (content[cut_pos] & 0xC0) == 0x80) {
                cut_pos--;
            }
            // cut_pos now points to the first byte of a multi-byte char, or a single-byte char
            // To be safe, just cut before this multi-byte char if it might be incomplete
            if (cut_pos > 0 && (content[cut_pos] & 0x80) != 0) {
                content.resize(cut_pos);
            }
        }
    }

    return true;
#else
    content = "Dummy chunk content";
    return true;
#endif
}

bool hw_read_internal_bytes_raw(const char *path, std::vector<uint8_t> &buf)
{
    buf.clear();
#ifdef ARDUINO
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    File f = FFat.open(str, FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    buf.resize(size);
    if (size > 0) f.read(buf.data(), size);
    f.close();
    return true;
#else
    (void)path;
    return false;
#endif
}

bool hw_read_sd_bytes_raw(const char *path, std::vector<uint8_t> &buf)
{
    buf.clear();
#ifdef ARDUINO
    if (!(HW_SD_ONLINE & hw_get_device_online())) return false;
    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    instance.lockSPI();
    File f = SD.open(str, FILE_READ);
    if (!f) {
        instance.unlockSPI();
        return false;
    }
    size_t size = f.size();
    buf.resize(size);
    if (size > 0) f.read(buf.data(), size);
    f.close();
    instance.unlockSPI();
    return true;
#else
    (void)path;
    return false;
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
    if (!decode_after_read(content)) {
        content.clear();
        return false;
    }
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
            // Normalize to leaf name - SD returns full path, FFat returns leaf.
            int slash = filename.lastIndexOf('/');
            if (slash >= 0) filename = filename.substring(slash + 1);

            if (filename.endsWith(ext)) {
                // Skip file.getLastWrite() as it can trigger a slow f_stat lookup on some ESP32 FAT
                // implementations. We rely on the chronological filename fallback for sorting.
                list.push_back({filename.c_str(), 0});
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

#ifdef ARDUINO
static void list_entries(std::vector<HwDirEntry> &list, fs::FS &fs,
                         const char *dirname, const char *filter_ext)
{
    File root = fs.open(dirname);
    if (!root || !root.isDirectory()) return;

    bool has_filter = filter_ext && filter_ext[0] != '\0';
    std::vector<HwDirEntry> dirs;
    std::vector<HwDirEntry> files;

    File entry = root.openNextFile();
    while (entry) {
        String name = entry.name();
        // Normalize to leaf name — SD returns full path, FFat returns leaf.
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (entry.isDirectory()) {
            // Skip entry.getLastWrite() as it triggers a slow f_stat lookup on FAT.
            dirs.push_back({std::string(name.c_str()), true, 0u, 0u});
        } else {
            if (!has_filter || name.endsWith(filter_ext)) {
                files.push_back({std::string(name.c_str()), false, 0u, (uint32_t)entry.size()});
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    auto by_path = [](const HwDirEntry &a, const HwDirEntry &b) { return a.path < b.path; };
    std::sort(dirs.begin(), dirs.end(), by_path);
    std::sort(files.begin(), files.end(), by_path);

    list.insert(list.end(), dirs.begin(), dirs.end());
    list.insert(list.end(), files.begin(), files.end());
}
#endif

void hw_list_internal_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                              const char *dirname)
{
    list.clear();
    if (!dirname || !dirname[0]) dirname = "/";
#ifdef ARDUINO
    list_entries(list, FFat, dirname, filter_ext);
#else
    bool all = !(filter_ext && filter_ext[0]);
    bool at_root = (strcmp(dirname, "/") == 0);
    if (at_root) {
        list.push_back({"notes",         true,  1710000000, 0u});
        list.push_back({"drafts",        true,  1711000000, 0u});
        list.push_back({"internal1.txt", false, 1712000000, 256u});
        list.push_back({"internal2.txt", false, 1713000000, 4096u});
        if (all) {
            list.push_back({"readme.md", false, 1714000000, 1536u});
            list.push_back({"data.bin",  false, 1715000000, 1572864u});
        }
    } else if (strcmp(dirname, "/notes") == 0) {
        list.push_back({"hello.txt",  false, 1712100000, 42u});
        list.push_back({"ideas.txt",  false, 1712200000, 812u});
    } else if (strcmp(dirname, "/drafts") == 0) {
        list.push_back({"wip.txt",    false, 1711100000, 128u});
    }
#endif
}

void hw_list_sd_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                        const char *dirname)
{
    list.clear();
    if (!dirname || !dirname[0]) dirname = "/";
#ifdef ARDUINO
    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        list_entries(list, SD, dirname, filter_ext);
        instance.unlockSPI();
    }
#else
    bool all = !(filter_ext && filter_ext[0]);
    bool at_root = (strcmp(dirname, "/") == 0);
    if (at_root) {
        list.push_back({"md",      true,  1710500000, 0u});
        list.push_back({"photos",  true,  1711500000, 0u});
        list.push_back({"sd1.txt", false, 1712500000, 320u});
        list.push_back({"sd2.txt", false, 1713500000, 2048u});
        if (all) {
            list.push_back({"track.mp3", false, 1714500000, 4194304u});
            list.push_back({"image.jpg", false, 1715500000, 524288u});
        }
    } else if (strcmp(dirname, "/md") == 0) {
        list.push_back({"note1.md", false, 1710600000, 640u});
        list.push_back({"note2.md", false, 1710700000, 1280u});
    } else if (strcmp(dirname, "/photos") == 0) {
        list.push_back({"pic1.jpg", false, 1711600000, 262144u});
    }
#endif
}

uint32_t hw_count_internal_files()
{
#ifdef ARDUINO
    uint32_t count = 0;
    File root = FFat.open("/");
    if (!root || !root.isDirectory()) return 0;
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) count++;
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    return count;
#else
    return 4;
#endif
}

void hw_get_sd_news_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    std::vector<FileInfo> file_infos;

    // SD is the primary store for downloaded news. FFat is merged in as a
    // fallback so files that landed internally (because the card was missing
    // or the SD write failed) still show up in the list.
    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        list_files(file_infos, SD, "/news", ".txt");
        instance.unlockSPI();
    }

    std::vector<FileInfo> ffat_infos;
    list_files(ffat_infos, FFat, "/news", ".txt");
    // Dedup by leaf name — prefer the SD copy if the same filename exists in
    // both stores, since that's where the next download would land.
    for (const auto &fi : ffat_infos) {
        bool dup = false;
        for (const auto &ex : file_infos) {
            if (ex.name == fi.name) { dup = true; break; }
        }
        if (!dup) file_infos.push_back(fi);
    }

    std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo & a, const FileInfo & b) {
        if (a.time != b.time) return a.time > b.time;
        return a.name > b.name;
    });

    for (const auto &fi : file_infos) {
        list.push_back(fi.name);
    }
#else
    list.push_back("/news/example1.txt");
    list.push_back("/news/example2.txt");
#endif
}

void hw_get_news_headers(const char *path, std::vector<std::pair<std::string, size_t>> &headers, bool (*progress_cb)(size_t, size_t))
{
    headers.clear();
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
        return;
    }

    size_t total_size = f.size();

    const size_t buf_size = 2048;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (!buf) {
        f.close();
        if (lock) instance.unlockSPI();
        return;
    }

    size_t global_offset = 0;
    std::string current_line;
    current_line.reserve(256);
    size_t line_start_offset = 0;
    bool aborted = false;
    // Capture generously so long HN-style titles survive intact; the UI
    // list truncates visually with long-mode scroll on focus.
    const size_t MAX_LINE_CAPTURE = 256;
    bool line_truncated = false;

    // A "N. " title is accepted only at a post boundary: file start, or
    // immediately following a separator line of dashes (------). This avoids
    // matching "1. foo" or similar patterns that appear inside article bodies.
    int expected_post_num = 1;
    bool at_post_boundary = true;

    while (f.available()) {
        size_t bytes_read = f.read(buf, buf_size);
        if (bytes_read == 0) break;

        for (size_t i = 0; i < bytes_read; i++) {
            char c = buf[i];

            if (c == '\n' || c == '\r') {
                if (!current_line.empty()) {
                    // Detect dash-separator line (3+ dashes, only dashes).
                    bool is_separator = (current_line.size() >= 3);
                    if (is_separator) {
                        for (char lc : current_line) {
                            if (lc != '-') { is_separator = false; break; }
                        }
                    }

                    // Post 1 is always allowed (header/preamble lines before
                    // it don't count as an article body). Posts 2+ require a
                    // preceding dash separator to avoid matching stray
                    // "N. text" inside an article.
                    if (expected_post_num == 1 || at_post_boundary) {
                        size_t k = 0;
                        while (k < current_line.size() && current_line[k] >= '0' && current_line[k] <= '9') ++k;
                        if (k > 0 && k + 1 < current_line.size() &&
                            current_line[k] == '.' && current_line[k + 1] == ' ') {
                            int num = atoi(current_line.substr(0, k).c_str());
                            if (num == expected_post_num) {
                                headers.push_back({current_line.substr(k + 2), line_start_offset});
                                expected_post_num++;
                            }
                        }
                    }

                    // Update boundary state for the NEXT non-empty line.
                    at_post_boundary = is_separator;

                    current_line.clear();
                    line_truncated = false;
                }
            } else {
                if (current_line.empty()) {
                    line_start_offset = global_offset + i;
                }
                if (!line_truncated) {
                    current_line += c;
                    if (current_line.size() >= MAX_LINE_CAPTURE) line_truncated = true;
                }
            }
        }
        global_offset += bytes_read;

        if (progress_cb) {
            if (lock) instance.unlockSPI();
            bool keep_going = progress_cb(global_offset, total_size);
            if (lock) instance.lockSPI();
            if (!keep_going) {
                aborted = true;
                break;
            }
        }
    }

    if (aborted) {
        headers.clear();
    }

    free(buf);
    f.close();
    if (lock) instance.unlockSPI();
#else
    headers.push_back({"Example title 1", 0});
    headers.push_back({"Example title 2", 100});
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

#ifdef ARDUINO
    /* Encode once so the internal and SD copies share identical bytes (and
     * therefore an identical salt when encrypted). */
    std::vector<uint8_t> payload;
    if (!encode_for_write(path, content, payload, error)) return false;

    String str = (path[0] == '/') ? String(path) : ("/" + String(path));
    bool ok_int = false;
    {
        File f = FFat.open(str, "w");
        if (f) {
            size_t w = payload.empty() ? 0 : f.write(payload.data(), payload.size());
            f.close();
            ok_int = (w == payload.size());
        }
        if (!ok_int && error) {
            *error = "Cannot write to Internal.";
        }
    }

    if (HW_SD_ONLINE & hw_get_device_online()) {
        instance.lockSPI();
        File f = SD.open(str, "w");
        if (f) {
            if (!payload.empty()) f.write(payload.data(), payload.size());
            f.close();
        }
        instance.unlockSPI();
    }

    prune_internal_storage();
    return ok_int;
#else
    (void)error;
    printf("Save to preferred file: %s, content: %s\n", path, content);
    return true;
#endif
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

    /* Probe the first 8 bytes so encrypted files take the full-read path.
     * CBC ciphertext can't be partially decrypted, so for protected files we
     * must read the whole thing, decrypt, then truncate the plaintext. */
    File f = FFat.open(str, FILE_READ);
    if (!f) return false;
    size_t total = f.size();

    uint8_t probe[8] = {0};
    size_t probe_len = total < 8 ? total : 8;
    if (probe_len) f.read(probe, probe_len);
    bool is_enc = content_has_salted_magic((const char *)probe, probe_len);

    if (is_enc) {
        /* Read the whole file and decrypt. */
        std::string full;
        full.resize(total);
        f.seek(0);
        if (total) f.read((uint8_t *)&full[0], total);
        f.close();
        if (!decode_after_read(full)) return false;
        if (full.size() > max_bytes) {
            content.assign(full.data(), max_bytes);
            if (truncated) *truncated = true;
        } else {
            content = std::move(full);
        }
        return true;
    }

    /* Plaintext: cheap partial read like before. */
    f.seek(0);
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
