/**
 * @file      audio.cpp
 * @brief     Audio player (MP3 task), microphone recording.
 */
#include "audio.h"
#include "internal.h"
#include "system.h"
#include "../core/spi_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#include <Esp.h>
#include <mp3dec.h>
#endif

// --- Player task / event group ----------------------------------------

#ifdef ARDUINO
static TaskHandle_t       playerTaskHandler = NULL;
static QueueHandle_t      playerQueue       = NULL;
static EventGroupHandle_t playerEvent       = NULL;
// Signalled by the recorder task when it has fully closed the WAV file, so
// hw_rec_stop() can block on it instead of busy-polling recorder_running.
static SemaphoreHandle_t  recorderDoneSem   = NULL;

#define PLAYER_PLAY     _BV(0)
#define PLAYER_END      _BV(1)
#define PLAYER_RUNNING  _BV(2)
// Set by the player when it clears PLAYER_RUNNING on exit, so callers can
// block for the stop to complete rather than polling hw_player_running().
#define PLAYER_STOPPED  _BV(3)

// libhelix needs at least one full frame in the buffer to decode. Layer III
// max frame size is ~1900 bytes; we keep extra headroom for sync search.
#define MP3_REFILL_BUF       (16 * 1024)
#define MP3_MIN_FRAME_BYTES  2048

typedef int (*mp3_fill_cb_t)(uint8_t *dst, size_t maxlen, void *ctx);

// Codec init/teardown and per-frame output. Cached `codec_online` is captured
// once at session start — its bitmask only changes on hotplug and we don't
// want to cross the device-probe path on every frame.
static bool play_mp3_with_filler(mp3_fill_cb_t fill_cb, void *ctx)
{
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) {
        log_e("Could not allocate decoder");
        return false;
    }

    uint8_t *bufStart = (uint8_t *)heap_caps_malloc(MP3_REFILL_BUF, MALLOC_CAP_SPIRAM);
    if (!bufStart) {
        MP3FreeDecoder(decoder);
        return false;
    }

    // One decoded MP3 frame (~4.6 KB). PSRAM like the refill buffer, and
    // allocated per-session rather than a static so it isn't permanently
    // resident in internal RAM between playbacks; a stack local would instead
    // bloat the player task's stack across xEventGroupWaitBits().
    int16_t *outBuf = (int16_t *)heap_caps_malloc(
        sizeof(int16_t) * MAX_NCHAN * MAX_NGRAN * MAX_NSAMP, MALLOC_CAP_SPIRAM);
    if (!outBuf) {
        free(bufStart);
        MP3FreeDecoder(decoder);
        return false;
    }

    uint8_t *readPtr = bufStart;
    int bytesAvailable = 0;
    bool eof = false;
    bool codec_begin = false;
#if defined(USING_AUDIO_CODEC)
    const bool codec_online = (HW_CODEC_ONLINE & hw_get_device_online()) != 0;
#endif

    auto refill = [&]() {
        if (eof) return;
        if (bytesAvailable > 0 && readPtr != bufStart) {
            memmove(bufStart, readPtr, bytesAvailable);
        }
        readPtr = bufStart;
        size_t want = MP3_REFILL_BUF - bytesAvailable;
        int got = fill_cb(bufStart + bytesAvailable, want, ctx);
        if (got <= 0) { eof = true; return; }
        bytesAvailable += got;
        if ((size_t)got < want) eof = true;
    };

    refill();
    if (bytesAvailable <= 0) {
        MP3FreeDecoder(decoder);
        free(bufStart);
        free(outBuf);
        return false;
    }

    xEventGroupSetBits(playerEvent, PLAYER_RUNNING);

    MP3FrameInfo frameInfo;
    while (true) {
        if (!eof && bytesAvailable < MP3_MIN_FRAME_BYTES) refill();

        int offset = MP3FindSyncWord(readPtr, bytesAvailable);
        if (offset < 0) break;
        readPtr += offset;
        bytesAvailable -= offset;

        if (!eof && bytesAvailable < MP3_MIN_FRAME_BYTES) refill();

        int err = MP3Decode(decoder, &readPtr, &bytesAvailable, outBuf, 0);
        if (err) {
            log_e("Decode ERROR: %d", err);
            break;
        }
        MP3GetLastFrameInfo(decoder, &frameInfo);

#if defined(USING_PCM_AMPLIFIER)
        if (!codec_begin) {
            codec_begin = true;
            instance.powerControl(POWER_SPEAK, true);
            log_d("Start PCM Play...");
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
            instance.player.configureTX(frameInfo.samprate, frameInfo.bitsPerSample, (i2s_channel_t)frameInfo.nChans);
#else
            instance.player.configureTX(frameInfo.samprate, (i2s_data_bit_width_t)frameInfo.bitsPerSample, (i2s_slot_mode_t)frameInfo.nChans);
#endif
        }
        instance.player.write((uint8_t *)outBuf,
                              (size_t)((frameInfo.bitsPerSample / 8) * frameInfo.outputSamps));
#elif defined(USING_AUDIO_CODEC)
        if (codec_online) {
            if (!codec_begin) {
                codec_begin = true;
                instance.codec.open(frameInfo.bitsPerSample, frameInfo.nChans, frameInfo.samprate);
            }
            int ret = instance.codec.write((uint8_t *)outBuf,
                                           (size_t)((frameInfo.bitsPerSample / 8) * frameInfo.outputSamps));
            if (ret != 0) {
                log_e("esp_codec_dev_write:0x%X", ret);
            }
        }
#endif

        EventBits_t eventBits = xEventGroupWaitBits(playerEvent, PLAYER_PLAY | PLAYER_END,
                                                    pdFALSE, pdFALSE, portMAX_DELAY);
        if (eventBits & PLAYER_END) break;
    }

    MP3FreeDecoder(decoder);
    free(bufStart);
    free(outBuf);
    xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
    xEventGroupSetBits(playerEvent, PLAYER_STOPPED);

#if defined(USING_PCM_AMPLIFIER)
    if (codec_begin) instance.powerControl(POWER_SPEAK, false);
#elif defined(USING_AUDIO_CODEC)
    if (codec_begin && codec_online) instance.codec.close();
#endif
    return true;
}

// Streams MP3 from an open File. SPI bus is locked around each read on
// SD-shared buses so the codec/audio path stays uncontended during decode.
struct mp3_file_ctx { File *f; bool sd_locked; };
static int mp3_fill_file(uint8_t *dst, size_t maxlen, void *ctx)
{
    auto *fc = (mp3_file_ctx *)ctx;
    core::MaybeSpiLock lock(fc->sd_locked);
    return (int)fc->f->read(dst, maxlen);
}

#if defined(USING_AUDIO_CODEC)
static void playWAV_sd(const char *filename);
#endif

static void hw_sd_play(audio_source_type_t source, const char *filename)
{
    size_t len = strlen(filename);
    bool isMP3 = (len > 4 && strcasecmp(filename + len - 4, ".mp3") == 0);
    bool isWAV = (len > 4 && strcasecmp(filename + len - 4, ".wav") == 0);

#if defined(USING_AUDIO_CODEC)
    if (isWAV && source == AUDIO_SOURCE_SDCARD) {
        playWAV_sd(filename);
        return;
    }
#endif
    if (!isMP3) return;

    char path[128];
    snprintf(path, sizeof(path), "/%s", filename);

    bool sd_locked = (source == AUDIO_SOURCE_SDCARD);
    File f;

    if (sd_locked) {
        core::ScopedSpiLock lock;
        f = SD.open(path);
        if (!f) {
            log_e("SD Open %s failed!", filename);
            return;
        }
    } else {
        f = FFat.open(path);
        if (!f) {
            log_e("FFat Open %s failed!", filename);
            return;
        }
    }

    if (f.size() == 0) {
        log_e("File %s size is 0!", filename);
        core::MaybeSpiLock lock(sd_locked);
        f.close();
        return;
    }

    log_i("Streaming %s", filename);
    mp3_file_ctx ctx{&f, sd_locked};
    play_mp3_with_filler(mp3_fill_file, &ctx);

    {
        core::MaybeSpiLock lock(sd_locked);
        f.close();
    }
}

#if defined(USING_AUDIO_CODEC)
// Stream-plays a 16 kHz / 16-bit / mono WAV file from SD without loading it
// all into PSRAM (a 5-minute note is ~10 MB). Header is a fixed-size 44-byte
// RIFF/WAVE/fmt/data layout — we trust it because we write it ourselves.
static void playWAV_sd(const char *filename)
{
    char path[128];
    snprintf(path, sizeof(path), "/%s", filename);
    File f;
    {
        core::ScopedSpiLock lock;
        f = SD.open(path);
        if (!f) return;
        if (f.size() < 44) { f.close(); return; }
        if (!f.seek(44))   { f.close(); return; }
    }

    auto close_with_lock = [&] {
        core::ScopedSpiLock lock;
        f.close();
    };

    if (!(HW_CODEC_ONLINE & hw_get_device_online())) {
        close_with_lock();
        return;
    }
    int ret = instance.codec.open(16, 1, HW_REC_SAMPLE_RATE);
    if (ret < 0) {
        close_with_lock();
        return;
    }

    xEventGroupSetBits(playerEvent, PLAYER_RUNNING);

    const size_t CHUNK = 4096;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        instance.codec.close();
        close_with_lock();
        xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
        xEventGroupSetBits(playerEvent, PLAYER_STOPPED);
        return;
    }

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(playerEvent, PLAYER_PLAY | PLAYER_END,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        if (bits & PLAYER_END) break;

        int n;
        {
            core::ScopedSpiLock lock;
            n = f.read(buf, CHUNK);
        }
        if (n <= 0) break;

        instance.codec.write(buf, (size_t)n);
    }

    free(buf);
    instance.codec.close();
    close_with_lock();
    xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
    xEventGroupSetBits(playerEvent, PLAYER_STOPPED);
}
#endif /*USING_AUDIO_CODEC*/

static void playerTask(void *args)
{
    audio_params_t params;
    while (1) {
        if (xQueueReceive(playerQueue, &params, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (params.event) {
        case APP_EVENT_PLAY:
            log_d("Event: filename:%s source:%d", params.filename, params.source_type);
            hw_sd_play(params.source_type, params.filename);
            break;
        case APP_EVENT_RECOVER:
            break;
        default:
            break;
        }
    }
    playerTaskHandler = NULL;
    vTaskDelete(NULL);
}
#endif // ARDUINO

void hw_audio_init()
{
#ifdef ARDUINO
    playerQueue = xQueueCreate(2, sizeof(audio_params_t));
    playerEvent = xEventGroupCreate();
    recorderDoneSem = xSemaphoreCreateBinary();
    xTaskCreate(playerTask, "app/play", 8 * 1024, NULL, 12, &playerTaskHandler);
#endif
}

void hw_audio_deinit_task()
{
#ifdef ARDUINO
    if (playerTaskHandler) {
        vTaskDelete(playerTaskHandler);
        playerTaskHandler = NULL;
    }
#endif
}

// --- Recording: WAV / 16 kHz / 16-bit / mono → SD card ----------------

bool hw_mic_available()
{
#if defined(ARDUINO) && defined(USING_PDM_MICROPHONE)
    return true;
#elif defined(ARDUINO) && defined(USING_AUDIO_CODEC)
    return (HW_CODEC_ONLINE & hw_get_device_online()) != 0;
#else
    return false;
#endif
}

#ifdef ARDUINO
static TaskHandle_t    recorderTaskHandler = NULL;
static volatile bool   recorder_stop_req  = false;
static volatile bool   recorder_running   = false;
static volatile uint32_t recorder_start_ms = 0;
static volatile uint32_t recorder_bytes   = 0;
static File            recFile;
static std::string     recPath;

static void wav_write_header(File &f, uint32_t data_bytes)
{
    uint32_t sample_rate = HW_REC_SAMPLE_RATE;
    uint16_t channels    = 1;
    uint16_t bits        = 16;
    uint32_t byte_rate   = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);
    uint32_t riff_size   = 36 + data_bytes;
    uint32_t fmt_size    = 16;
    uint16_t fmt_pcm     = 1;

    uint8_t h[44];
    memcpy(h +  0, "RIFF", 4);
    memcpy(h +  4, &riff_size, 4);
    memcpy(h +  8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &fmt_pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &sample_rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    f.write(h, 44);
}

static void recorderTask(void *args)
{
    const int frame_samples = 512;
    // Match the codec's actual channel count so the sample rate in the file
    // header matches real time. Reading stereo-sized blocks from a codec
    // opened as mono decimates the audio and causes 2x-fast playback.
    const uint8_t in_channels = instance.getCodecInputChannels();
    const size_t in_bytes   = (size_t)frame_samples * in_channels * sizeof(int16_t);
    const size_t mono_bytes = (size_t)frame_samples * sizeof(int16_t);

    int16_t *in_buf   = (int16_t *)heap_caps_malloc(in_bytes,   MALLOC_CAP_SPIRAM);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(mono_bytes, MALLOC_CAP_SPIRAM);

    if (!in_buf || !mono_buf) {
        log_e("recorder: buffer alloc failed");
        if (in_buf) free(in_buf);
        if (mono_buf) free(mono_buf);
        {
            core::ScopedSpiLock lock;
            if (recFile) recFile.close();
        }
        recorder_running = false;
        recorderTaskHandler = NULL;
        if (recorderDoneSem) xSemaphoreGive(recorderDoneSem);
        vTaskDelete(NULL);
        return;
    }

#if defined(USING_AUDIO_CODEC)
    // Codec online state is hotplug-driven; latch once for the recording
    // session instead of probing the device bitmask on every frame.
    const bool codec_online = (HW_CODEC_ONLINE & hw_get_device_online()) != 0;
#endif

    while (!recorder_stop_req) {
        uint32_t elapsed = millis() - recorder_start_ms;
        if (elapsed >= HW_REC_MAX_MS) break;

#if defined(USING_PDM_MICROPHONE)
        instance.mic.readBytes((char *)in_buf, in_bytes);
#elif defined(USING_AUDIO_CODEC)
        if (!codec_online) break;
        int rret = instance.codec.read((uint8_t *)in_buf, in_bytes);
        if (rret != 0) {
            log_e("codec.read failed: 0x%X", rret);
            break;
        }
#else
        break;
#endif

        const uint8_t *write_src;
        size_t write_len;
        if (in_channels >= 2) {
            for (int i = 0; i < frame_samples; ++i) {
                int32_t s = (int32_t)in_buf[2 * i] + (int32_t)in_buf[2 * i + 1];
                mono_buf[i] = (int16_t)(s / 2);
            }
            write_src = (const uint8_t *)mono_buf;
            write_len = mono_bytes;
        } else {
            write_src = (const uint8_t *)in_buf;
            write_len = in_bytes;
        }

        size_t written;
        {
            core::ScopedSpiLock lock;
            written = recFile.write(write_src, write_len);
        }
        recorder_bytes += written;
        if (written != write_len) {
            log_e("recorder: short write %u/%u (SD full?)",
                  (unsigned)written, (unsigned)write_len);
            break;
        }
    }

    // Finalize WAV header with actual data size.
    {
        core::ScopedSpiLock lock;
        if (recFile) {
            recFile.seek(0);
            wav_write_header(recFile, recorder_bytes);
            recFile.close();
        }
    }

#if defined(USING_AUDIO_CODEC)
    if (codec_online) instance.codec.close();
#endif

    free(in_buf);
    free(mono_buf);
    recorder_running = false;
    recorderTaskHandler = NULL;
    if (recorderDoneSem) xSemaphoreGive(recorderDoneSem);
    vTaskDelete(NULL);
}
#endif /*ARDUINO*/

bool hw_rec_start(const char *sd_path)
{
#ifdef ARDUINO
    if (recorder_running || hw_player_running()) return false;
    if (!hw_mic_available()) return false;
    if (!sd_path || !sd_path[0]) return false;

    {
        core::ScopedSpiLock lock;
        recFile = SD.open(sd_path, FILE_WRITE);
        if (!recFile) {
            log_e("recorder: SD.open(%s) failed", sd_path);
            return false;
        }
        wav_write_header(recFile, 0);
    }

#if defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        int ret = instance.codec.open(16, instance.getCodecInputChannels(),
                                      HW_REC_SAMPLE_RATE);
        if (ret < 0) {
            log_e("recorder: codec.open failed 0x%X", ret);
            core::ScopedSpiLock lock;
            recFile.close();
            SD.remove(sd_path);
            return false;
        }
    }
#endif

    recPath            = sd_path;
    recorder_stop_req  = false;
    recorder_bytes     = 0;
    recorder_start_ms  = millis();
    recorder_running   = true;

    // Clear any completion token left by a self-terminated prior recording so
    // hw_rec_stop() waits for *this* session, not a stale one.
    if (recorderDoneSem) xSemaphoreTake(recorderDoneSem, 0);

    // Pinned to core 0 (OPTIMIZATION_PHASE3.md P3.10): unpinned at prio 12 could
    // land on core 1 and preempt the lvgl task (prio 8), stalling frames for
    // codec-frame durations while recording.
    if (xTaskCreatePinnedToCore(recorderTask, "app/rec", 8 * 1024, NULL, 12,
                    &recorderTaskHandler, 0) != pdPASS) {
        log_e("recorder: task create failed");
        recorder_running = false;
#if defined(USING_AUDIO_CODEC)
        if (HW_CODEC_ONLINE & hw_get_device_online()) {
            instance.codec.close();
        }
#endif
        {
            core::ScopedSpiLock lock;
            recFile.close();
            SD.remove(sd_path);
        }
        return false;
    }
    return true;
#else
    (void)sd_path;
    return false;
#endif
}

void hw_rec_stop()
{
#ifdef ARDUINO
    if (!recorder_running) return;
    recorder_stop_req = true;
    // Block (not busy-poll) until the recorder task has flushed its final SD
    // write, rewritten the WAV header and closed the file. Bounded so a stalled
    // SD write can't freeze the UI thread indefinitely; on timeout we fall back
    // to the old poll to preserve the synchronous "file is closed on return"
    // contract callers (finalize_recording) rely on.
    if (recorderDoneSem) {
        if (xSemaphoreTake(recorderDoneSem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            while (recorder_running) delay(5);
        }
    } else {
        while (recorder_running) delay(5);
    }
#endif
}

bool hw_rec_running()
{
#ifdef ARDUINO
    return recorder_running;
#else
    return false;
#endif
}

uint32_t hw_rec_elapsed_ms()
{
#ifdef ARDUINO
    if (!recorder_running) return 0;
    return (uint32_t)(millis() - recorder_start_ms);
#else
    return 0;
#endif
}

uint32_t hw_rec_bytes_written()
{
#ifdef ARDUINO
    return recorder_bytes;
#else
    return 0;
#endif
}

// --- Speaker / volume -------------------------------------------------

bool hw_get_speaker_enable() { return user_setting.speaker_enable; }
void hw_set_speaker_enable(bool en) {
    user_setting.speaker_enable = en;
#ifdef ARDUINO
    instance.powerControl(POWER_SPEAK, en);
    delay(10);
#endif
}

void hw_set_volume(uint8_t volume)
{
#if defined(ARDUINO) && defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        instance.codec.setVolume(volume);
    } else {
        log_d("Audio codec not online!");
    }
#endif //USING_AUDIO_CODEC
}

uint8_t hw_get_volume()
{
#if defined(ARDUINO) && defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        return instance.codec.getVolume();
    } else {
        return 0;
    }
#else
    return 100;
#endif //USING_AUDIO_CODEC
}

// --- Playback control -------------------------------------------------

void hw_set_sd_music_play(audio_source_type_t source_type, const char *filename)
{
    audio_params_t params = {
        .event = APP_EVENT_PLAY,
        .filename = filename,
        .source_type = source_type
    };
    log_d("hw_set_sd_music_play : %s source_type:%d", filename, source_type);
#ifdef ARDUINO
    xEventGroupClearBits(playerEvent, PLAYER_PLAY | PLAYER_END | PLAYER_STOPPED);
    if (hw_player_running()) {
        xEventGroupSetBits(playerEvent, PLAYER_END);
        Serial.println("Wait hw_player_running stop...");
        // Block on PLAYER_STOPPED rather than busy-polling; bounded so a wedged
        // player task can't freeze the UI thread.
        xEventGroupWaitBits(playerEvent, PLAYER_STOPPED, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(2000));
        Serial.println("hw_player_running stopped.");
    }
    xEventGroupSetBits(playerEvent, PLAYER_PLAY);
    xQueueSend(playerQueue, &params, portMAX_DELAY);
    Serial.println("hw_set_sd_music_play send done\n");
#endif
}

void hw_set_play_stop()
{
#ifdef ARDUINO
    xEventGroupClearBits(playerEvent, PLAYER_PLAY | PLAYER_END | PLAYER_STOPPED);
    if (hw_player_running()) {
        xEventGroupSetBits(playerEvent, PLAYER_END);
        xEventGroupWaitBits(playerEvent, PLAYER_STOPPED, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(2000));
    }
#endif
}

bool hw_player_running()
{
#ifdef ARDUINO
    return xEventGroupGetBits(playerEvent) & PLAYER_RUNNING;
#endif
    return true;
}
