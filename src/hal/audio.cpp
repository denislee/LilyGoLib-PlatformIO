/**
 * @file      audio.cpp
 * @brief     Audio player (MP3 task), microphone, FFT, file listing.
 */
#include "audio.h"
#include "internal.h"

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

static bool playMP3(uint8_t *src, size_t src_len)
{
    int16_t outBuf[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
    uint8_t *readPtr = NULL;
    int bytesAvailable = 0, err = 0, offset = 0;
    MP3FrameInfo frameInfo;
    HMP3Decoder decoder = NULL;
    bool codec_begin = false;

    bytesAvailable = src_len;
    readPtr = src;

    decoder = MP3InitDecoder();
    if (decoder == NULL) {
        log_e("Could not allocate decoder");
        return false;
    }
    xEventGroupSetBits(playerEvent, PLAYER_RUNNING);
    do {
        offset = MP3FindSyncWord(readPtr, bytesAvailable);
        if (offset < 0) {
            break;
        }
        readPtr += offset;
        bytesAvailable -= offset;
        err = MP3Decode(decoder, &readPtr, &bytesAvailable, outBuf, 0);
        if (err) {
            log_e("Decode ERROR: %d", err);
            MP3FreeDecoder(decoder);
            xEventGroupClearBits(playerEvent, PLAYER_RUNNING);
            return false;
        } else {
            MP3GetLastFrameInfo(decoder, &frameInfo);
#if  defined(USING_PCM_AMPLIFIER)

            if (!codec_begin) {
                codec_begin = true;
                instance.powerControl(POWER_SPEAK, true);
                log_d("Start PCM Play...");
#if  ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
                printf("sample rate:%d bitPs:%d ch:%d\n", frameInfo.samprate, frameInfo.bitsPerSample, (i2s_channel_t)frameInfo.nChans);
                instance.player.configureTX(frameInfo.samprate, frameInfo.bitsPerSample, (i2s_channel_t)frameInfo.nChans);
#else
                instance.player.configureTX(frameInfo.samprate, (i2s_data_bit_width_t)frameInfo.bitsPerSample, (i2s_slot_mode_t)frameInfo.nChans);
#endif
            }

            instance.player.write((uint8_t *)outBuf, (size_t)((frameInfo.bitsPerSample / 8) * frameInfo.outputSamps));

#elif defined(USING_AUDIO_CODEC)
            if (!codec_begin) {
                codec_begin = true;
                if (HW_CODEC_ONLINE & hw_get_device_online()) {
                    // Serial.printf("Set sample rate:%d bitsPerSample:%d\n", frameInfo.samprate, frameInfo.bitsPerSample);
                    int ret = instance.codec.open(frameInfo.bitsPerSample, frameInfo.nChans, frameInfo.samprate);
                    // Serial.printf("esp_codec_dev_open:0x%X\n", ret);
                }
            }
            if (HW_CODEC_ONLINE & hw_get_device_online()) {
                int ret = instance.codec.write((uint8_t *)outBuf, (size_t)((frameInfo.bitsPerSample / 8) * frameInfo.outputSamps));
                if (ret != 0) {
                    Serial.printf("esp_codec_dev_write:0x%X\n", ret);
                }
            }
#endif
        }

WAIT:
        EventBits_t eventBits =  xEventGroupWaitBits(playerEvent, PLAYER_PLAY | PLAYER_END
                                 , pdFALSE, pdFALSE, portMAX_DELAY);

        if (eventBits & PLAYER_END) {
            // printf("TASK END\n");
            break;
        }

    } while (true);

    MP3FreeDecoder(decoder);
    xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);

#if  defined(USING_PCM_AMPLIFIER)
    instance.powerControl(POWER_SPEAK, false);
#elif defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        instance.codec.close();
    }
#endif
    return true;
}

static void hw_sd_play(audio_source_type_t source, const char *filename)
{
    size_t len = strlen(filename);
    bool isMP3 = (len > 4 && strcasecmp(filename + len - 4, ".mp3") == 0);
    bool lock = false;

    char path[128];
    snprintf(path, sizeof(path), "/%s", filename);
    File f;

    if (source == AUDIO_SOURCE_SDCARD) {
        // T-Watch-S3-Ultra or T-LoRa-Pager is SPI bus-shared, lock the SPI bus before use
        instance.lockSPI();
        f = SD.open(path);
        if (f) {
            lock = true;
        } else {
            Serial.printf("SD Open %s failed!\n", filename);
            // T-Watch-S3-Ultra or T-LoRa-Pager is SPI bus-shared and releases the bus after use.
            instance.unlockSPI();
            return;
        }
    } else {
        f = FFat.open(path);
        if (!f) {
            Serial.printf("FFat Open %s failed!\n", filename);
            return;
        }
    }

    size_t file_size = f.size();
    if (file_size == 0) {
        Serial.printf("File %s size is 0!\n", filename);
        f.close();
        if (lock) {
            // T-Watch-S3-Ultra or T-LoRa-Pager is SPI bus-shared and releases the bus after use.
            instance.unlockSPI();
        }
        return ;
    }
    uint8_t *buf  = (uint8_t *)ps_malloc(file_size);
    if (!buf) {
        Serial.println("ps malloc failed!");
        f.close();
        if (lock) {
            // T-Watch-S3-Ultra or T-LoRa-Pager is SPI bus-shared and releases the bus after use.
            instance.unlockSPI();
        }
        return ;
    }

    size_t read_size =  f.readBytes((char *)buf, file_size);
    f.close();

    // SPI bus-shared and releases the bus after use.
    if (lock) {
        instance.unlockSPI();  //Release lock
    }

    if (read_size == file_size) {
        Serial.print("Playing ");
        Serial.println(filename);
        if (isMP3) {
            playMP3(buf, read_size);
        } else {
        }
        Serial.println("Play done..");
    }
    free(buf);
}

static void playerTask(void *args)
{
    audio_params_t params;
    while (1) {
        if (xQueueReceive(playerQueue, &params, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (params.event) {
        case APP_EVENT_PLAY:
            Serial.printf("Event: filename:%s source:%d\n", params.filename, params.source_type);
            hw_sd_play(params.source_type, params.filename);
            break;
        case APP_EVENT_PLAY_KEY:
            // Serial.println("APP_EVENT_PLAY_KEY");
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
