/**
 * @file      audio.cpp
 * @brief     Audio player (MP3 task), microphone, FFT, file listing.
 */
#include "audio.h"
#include "internal.h"
#include "system.h"

#ifdef ARDUINO
#include <math.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#include <Esp.h>
#include <mp3dec.h>
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"
#include "../audio/keyboard_audio.h"
#endif

#if defined(HAS_SD_CARD_SOCKET)
#define FILESYSTEM                  SD
#else
#define FILESYSTEM                  FFat
#endif

// --- Player task / event group ----------------------------------------

#ifdef ARDUINO
static TaskHandle_t       playerTaskHandler = NULL;
static QueueHandle_t      playerQueue       = NULL;
static EventGroupHandle_t playerEvent       = NULL;

#define PLAYER_PLAY     _BV(0)
#define PLAYER_END      _BV(1)
#define PLAYER_RUNNING  _BV(2)

// Decode buffer for one MP3 frame (~4.6 KB). Static so the player task's
// stack doesn't have to carry it across xEventGroupWaitBits() — keeps stack
// pressure predictable during long playback.
static int16_t s_mp3_outBuf[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

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

        int err = MP3Decode(decoder, &readPtr, &bytesAvailable, s_mp3_outBuf, 0);
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
        instance.player.write((uint8_t *)s_mp3_outBuf,
                              (size_t)((frameInfo.bitsPerSample / 8) * frameInfo.outputSamps));
#elif defined(USING_AUDIO_CODEC)
        if (codec_online) {
            if (!codec_begin) {
                codec_begin = true;
                instance.codec.open(frameInfo.bitsPerSample, frameInfo.nChans, frameInfo.samprate);
            }
            int ret = instance.codec.write((uint8_t *)s_mp3_outBuf,
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
    xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);

#if defined(USING_PCM_AMPLIFIER)
    if (codec_begin) instance.powerControl(POWER_SPEAK, false);
#elif defined(USING_AUDIO_CODEC)
    if (codec_begin && codec_online) instance.codec.close();
#endif
    return true;
}

// In-memory cursor for keyboard-tone playback (small flash blob).
struct mp3_mem_ctx { const uint8_t *src; size_t remaining; };
static int mp3_fill_mem(uint8_t *dst, size_t maxlen, void *ctx)
{
    auto *m = (mp3_mem_ctx *)ctx;
    if (m->remaining == 0) return 0;
    size_t n = m->remaining < maxlen ? m->remaining : maxlen;
    memcpy(dst, m->src, n);
    m->src += n;
    m->remaining -= n;
    return (int)n;
}

static bool playMP3(uint8_t *src, size_t src_len)
{
    mp3_mem_ctx ctx{src, src_len};
    return play_mp3_with_filler(mp3_fill_mem, &ctx);
}

// Streams MP3 from an open File. SPI bus is locked around each read on
// SD-shared buses so the codec/audio path stays uncontended during decode.
struct mp3_file_ctx { File *f; bool sd_locked; };
static int mp3_fill_file(uint8_t *dst, size_t maxlen, void *ctx)
{
    auto *fc = (mp3_file_ctx *)ctx;
    if (fc->sd_locked) instance.lockSPI();
    int n = (int)fc->f->read(dst, maxlen);
    if (fc->sd_locked) instance.unlockSPI();
    return n;
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
        instance.lockSPI();
        f = SD.open(path);
        if (!f) {
            log_e("SD Open %s failed!", filename);
            instance.unlockSPI();
            return;
        }
        instance.unlockSPI();
    } else {
        f = FFat.open(path);
        if (!f) {
            log_e("FFat Open %s failed!", filename);
            return;
        }
    }

    if (f.size() == 0) {
        log_e("File %s size is 0!", filename);
        if (sd_locked) instance.lockSPI();
        f.close();
        if (sd_locked) instance.unlockSPI();
        return;
    }

    log_i("Streaming %s", filename);
    mp3_file_ctx ctx{&f, sd_locked};
    play_mp3_with_filler(mp3_fill_file, &ctx);

    if (sd_locked) instance.lockSPI();
    f.close();
    if (sd_locked) instance.unlockSPI();
}

#if defined(USING_AUDIO_CODEC)
// Stream-plays a 16 kHz / 16-bit / mono WAV file from SD without loading it
// all into PSRAM (a 5-minute note is ~10 MB). Header is a fixed-size 44-byte
// RIFF/WAVE/fmt/data layout — we trust it because we write it ourselves.
static void playWAV_sd(const char *filename)
{
    char path[128];
    snprintf(path, sizeof(path), "/%s", filename);
    instance.lockSPI();
    File f = SD.open(path);
    if (!f) {
        instance.unlockSPI();
        return;
    }
    if (f.size() < 44) {
        f.close();
        instance.unlockSPI();
        return;
    }
    if (!f.seek(44)) {
        f.close();
        instance.unlockSPI();
        return;
    }
    instance.unlockSPI();

    if (!(HW_CODEC_ONLINE & hw_get_device_online())) {
        instance.lockSPI();
        f.close();
        instance.unlockSPI();
        return;
    }
    int ret = instance.codec.open(16, 1, HW_REC_SAMPLE_RATE);
    if (ret < 0) {
        instance.lockSPI();
        f.close();
        instance.unlockSPI();
        return;
    }

    xEventGroupSetBits(playerEvent, PLAYER_RUNNING);

    const size_t CHUNK = 4096;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        instance.codec.close();
        instance.lockSPI();
        f.close();
        instance.unlockSPI();
        xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
        return;
    }

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(playerEvent, PLAYER_PLAY | PLAYER_END,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        if (bits & PLAYER_END) break;

        instance.lockSPI();
        int n = f.read(buf, CHUNK);
        instance.unlockSPI();
        if (n <= 0) break;

        instance.codec.write(buf, (size_t)n);
    }

    free(buf);
    instance.codec.close();
    instance.lockSPI();
    f.close();
    instance.unlockSPI();
    xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
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
        case APP_EVENT_PLAY_KEY:
            playMP3((uint8_t * )keyboard_audio, keyboard_audio_mp3_len);
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

// --- FFT analysis -----------------------------------------------------

#ifdef ARDUINO
static int16_t *i2s_buffer = NULL;
static float *fft_input = NULL;
static float *window = NULL;
static int16_t *left_channel = NULL;
static int16_t *right_channel = NULL;
static float *magnitudes = NULL;
static int read_count = 0;

static void process_channel_fft(int16_t *channel_data, float *bands, float freq_per_bin)
{
    if (!fft_input || !window || !magnitudes) return;

    for (int i = 0; i < FFT_SIZE; i++) {
        fft_input[2 * i] = (float)channel_data[i] * 3.0f / 32768.0f * window[i];
        fft_input[2 * i + 1] = 0;
    }

    dsps_fft2r_fc32_aes3(fft_input, FFT_SIZE);
    dsps_bit_rev_fc32(fft_input, FFT_SIZE);
    dsps_cplx2reC_fc32(fft_input, FFT_SIZE);

    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float real = fft_input[2 * i];
        float imag = fft_input[2 * i + 1];
        magnitudes[i] = sqrt(real * real + imag * imag);

        if (magnitudes[i] < 0.00001) magnitudes[i] = 0.00001;
        magnitudes[i] = 20 * log10(magnitudes[i]);
        magnitudes[i] = (magnitudes[i] + 40) / 40;
        magnitudes[i] = constrain(magnitudes[i], 0, 1);
    }

    int bin_count = (FFT_SIZE / 2) / FREQ_BANDS;
    memset(bands, 0, FREQ_BANDS * sizeof(float));

    for (int band = 0; band < FREQ_BANDS; band++) {
        int start_bin = band * bin_count;
        int end_bin = start_bin + bin_count;
        if (end_bin > FFT_SIZE / 2) end_bin = FFT_SIZE / 2;

        float sum = 0;
        int count = 0;
        for (int bin = start_bin; bin < end_bin; bin++) {
            sum += magnitudes[bin];
            count++;
        }

        if (count > 0) {
            bands[band] = sum / count;
        }
    }
}
#endif /*ARDUINO*/

void hw_audio_get_fft_data(FFTData *fft_data)
{
#ifdef ARDUINO
    if (!i2s_buffer || !left_channel || !right_channel) return;

    float freq_per_bin = (float)SAMPLE_RATE / FFT_SIZE;

#if defined(USING_PDM_MICROPHONE)
    instance.mic.readBytes((char *)i2s_buffer, FFT_SIZE * 2 * sizeof(int16_t));
#elif defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        instance.codec.read((uint8_t *)i2s_buffer, FFT_SIZE * 2 * sizeof(int16_t));
    } else {
        return;
    }
#endif

    read_count++;

    for (int i = 0; i < FFT_SIZE; i++) {
        left_channel[i] = i2s_buffer[2 * i];
        right_channel[i] = i2s_buffer[2 * i + 1];
    }

    process_channel_fft(left_channel, fft_data->left_bands, freq_per_bin);
    process_channel_fft(right_channel, fft_data->right_bands, freq_per_bin);
#endif /*ARDUINO*/
}

bool hw_set_mic_start()
{
#ifdef ARDUINO
    int ret ;

#ifdef USING_AUDIO_CODEC
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        ret = instance.codec.open(16, instance.getCodecInputChannels(), 16000);
        if (ret < 0) {
            log_e("Audio codec open failed:0x%X", ret);
            return false;
        }
    } else {
        return false;
    }
#endif /*USING_AUDIO_CODEC*/

    // Allocate FFT buffers in PSRAM
    if (!i2s_buffer) i2s_buffer = (int16_t *)ps_malloc(FFT_SIZE * 2 * sizeof(int16_t));
    if (!fft_input) fft_input = (float *)heap_caps_aligned_alloc(16, FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!window) window = (float *)heap_caps_aligned_alloc(16, FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!left_channel) left_channel = (int16_t *)ps_malloc(FFT_SIZE * sizeof(int16_t));
    if (!right_channel) right_channel = (int16_t *)ps_malloc(FFT_SIZE * sizeof(int16_t));
    if (!magnitudes) magnitudes = (float *)ps_malloc((FFT_SIZE / 2) * sizeof(float));

    if (!i2s_buffer || !fft_input || !window || !left_channel || !right_channel || !magnitudes) {
        log_e("FFT buffer allocation failed!");
        return false;
    }

    ret = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (ret != ESP_OK) {
        log_e("fft init failed = %i\n", ret);
        return false;
    }

    dsps_wind_hann_f32(window, FFT_SIZE);

#endif /*ARDUINO*/

    return true;
}

void hw_set_mic_stop()
{
#ifdef ARDUINO
#ifdef USING_AUDIO_CODEC
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        instance.codec.close();
    }
#endif
    dsps_fft2r_deinit_fc32();

    // Free buffers
    if (i2s_buffer) { free(i2s_buffer); i2s_buffer = NULL; }
    if (fft_input) { free(fft_input); fft_input = NULL; }
    if (window) { free(window); window = NULL; }
    if (left_channel) { free(left_channel); left_channel = NULL; }
    if (right_channel) { free(right_channel); right_channel = NULL; }
    if (magnitudes) { free(magnitudes); magnitudes = NULL; }
#endif /*ARDUINO*/
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
        instance.lockSPI();
        if (recFile) recFile.close();
        instance.unlockSPI();
        recorder_running = false;
        recorderTaskHandler = NULL;
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

        instance.lockSPI();
        size_t written = recFile.write(write_src, write_len);
        instance.unlockSPI();
        recorder_bytes += written;
        if (written != write_len) {
            log_e("recorder: short write %u/%u (SD full?)",
                  (unsigned)written, (unsigned)write_len);
            break;
        }
    }

    // Finalize WAV header with actual data size.
    instance.lockSPI();
    if (recFile) {
        recFile.seek(0);
        wav_write_header(recFile, recorder_bytes);
        recFile.close();
    }
    instance.unlockSPI();

#if defined(USING_AUDIO_CODEC)
    if (codec_online) instance.codec.close();
#endif

    free(in_buf);
    free(mono_buf);
    recorder_running = false;
    recorderTaskHandler = NULL;
    vTaskDelete(NULL);
}
#endif /*ARDUINO*/

bool hw_rec_start(const char *sd_path)
{
#ifdef ARDUINO
    if (recorder_running || hw_player_running()) return false;
    if (!hw_mic_available()) return false;
    if (!sd_path || !sd_path[0]) return false;

    instance.lockSPI();
    recFile = SD.open(sd_path, FILE_WRITE);
    if (!recFile) {
        log_e("recorder: SD.open(%s) failed", sd_path);
        instance.unlockSPI();
        return false;
    }
    wav_write_header(recFile, 0);
    instance.unlockSPI();

#if defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        int ret = instance.codec.open(16, instance.getCodecInputChannels(),
                                      HW_REC_SAMPLE_RATE);
        if (ret < 0) {
            log_e("recorder: codec.open failed 0x%X", ret);
            instance.lockSPI();
            recFile.close();
            SD.remove(sd_path);
            instance.unlockSPI();
            return false;
        }
    }
#endif

    recPath            = sd_path;
    recorder_stop_req  = false;
    recorder_bytes     = 0;
    recorder_start_ms  = millis();
    recorder_running   = true;

    if (xTaskCreate(recorderTask, "app/rec", 8 * 1024, NULL, 12,
                    &recorderTaskHandler) != pdPASS) {
        log_e("recorder: task create failed");
        recorder_running = false;
#if defined(USING_AUDIO_CODEC)
        if (HW_CODEC_ONLINE & hw_get_device_online()) {
            instance.codec.close();
        }
#endif
        instance.lockSPI();
        recFile.close();
        SD.remove(sd_path);
        instance.unlockSPI();
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
    while (recorder_running) {
        delay(5);
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
        printf("Audio codec not online!\n");
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

// --- File listing / playback control ----------------------------------

#ifdef ARDUINO
static void listDir(std::vector<AudioParams_t> &list, fs::FS &fs, const char *dirname, uint8_t levels, audio_source_type_t source_type)
{
    Serial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if (!root) {
        Serial.println("- failed to open directory");
        return;
    }
    if (!root.isDirectory()) {
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if (levels) {
                std::string next_dir = dirname;
                if (next_dir != "/") next_dir += "/";
                next_dir += file.name();
                listDir(list, fs, next_dir.c_str(), levels - 1, source_type);
            }
        } else {
            String filename = file.name();
            if (filename.endsWith(".mp3") || filename.endsWith(".wav")) {
                list.push_back({source_type, filename.c_str()});
            }

            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}

static void hw_fat_list(std::vector<AudioParams_t> &list, const char *dirname, uint8_t levels)
{
    Serial.printf("FFAT Listing directory: %s\n", dirname);
    listDir(list, FFat, dirname, levels, AUDIO_SOURCE_FATFS);
}

static bool hw_sd_list(std::vector<AudioParams_t> &list, const char *dirname, uint8_t levels)
{
#if defined(HAS_SD_CARD_SOCKET)
    instance.lockSPI();
    if (instance.installSD()) {
        Serial.println("SD Card mount success.");
    } else {
        Serial.println("SD Card mount failed.");
        instance.unlockSPI();
        return false;
    }
    listDir(list, SD, dirname, levels, AUDIO_SOURCE_SDCARD);
    instance.unlockSPI();
#endif
    return true;
}
#endif

void hw_get_filesystem_music(std::vector<AudioParams_t> &list)
{
    list.clear();

#if defined(ARDUINO)

#if defined(HAS_SD_CARD_SOCKET)
    Serial.println("\n================== SD Music List ==================");
    hw_sd_list(list, "/", 0);
#endif

    Serial.println("\n================== FFat Music List ==================");
    hw_fat_list(list, "/", 0);

#else
    list.push_back({AUDIO_SOURCE_FATFS, "/abc.mp3"});
    list.push_back({AUDIO_SOURCE_FATFS, "/ccc.mp3"});
    list.push_back({AUDIO_SOURCE_FATFS, "/ddd.mp3"});
#endif
}

void hw_set_sd_music_play(audio_source_type_t source_type, const char *filename)
{
    audio_params_t params = {
        .event = APP_EVENT_PLAY,
        .filename = filename,
        .source_type = source_type
    };
    printf("hw_set_sd_music_play : %s source_type:%d\n", filename, source_type);
#ifdef ARDUINO
    xEventGroupClearBits(playerEvent, PLAYER_PLAY | PLAYER_END);
    if (hw_player_running()) {
        xEventGroupSetBits(playerEvent, PLAYER_END);
        Serial.println("Wait hw_player_running stop...");
        while (hw_player_running()) {
            delay(2);
        }
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
    xEventGroupClearBits(playerEvent, PLAYER_PLAY | PLAYER_END);
    if (hw_player_running()) {
        xEventGroupSetBits(playerEvent, PLAYER_END);
        while (hw_player_running()) {
            delay(2);
        }
    }
#endif
}

void hw_set_sd_music_pause()
{
    printf("playerTaskHandler pause!\n");
#ifdef ARDUINO
    xEventGroupClearBits(playerEvent, PLAYER_PLAY);
#endif
}

void hw_set_sd_music_resume()
{
    printf("playerTaskHandler resume!\n");
#ifdef ARDUINO
    xEventGroupSetBits(playerEvent, PLAYER_PLAY);
#endif
}

bool hw_player_running()
{
#ifdef ARDUINO
    return xEventGroupGetBits(playerEvent) & PLAYER_RUNNING;
#endif
    return true;
}
