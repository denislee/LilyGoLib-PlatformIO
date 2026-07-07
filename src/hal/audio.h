/**
 * @file      audio.h
 * @brief     Audio player, microphone recording, audio effects.
 */
#pragma once

#include "types.h"

void hw_set_sd_music_play(audio_source_type_t source_type, const char *filename);
void hw_set_play_stop();
bool hw_player_running();

void hw_set_volume(uint8_t volume);
uint8_t hw_get_volume();

bool hw_get_speaker_enable();
void hw_set_speaker_enable(bool en);

// --- Recording (WAV, 16 kHz / 16-bit / mono) to SD card ---
#define HW_REC_SAMPLE_RATE 16000u
#define HW_REC_BYTES_PER_SEC (HW_REC_SAMPLE_RATE * 2u)   // mono, 16-bit
#define HW_REC_MAX_MS (5u * 60u * 1000u)

bool hw_mic_available();
bool hw_rec_start(const char *sd_path);
void hw_rec_stop();
bool hw_rec_running();
uint32_t hw_rec_elapsed_ms();
uint32_t hw_rec_bytes_written();
