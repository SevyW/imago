// ATRAC9 (.at9) RIFF/WAVE container parsing. Pure C, testable on a PC.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t config[4];     // ATRAC9 config data for the decoder
	int channels;
	int sample_rate;
	int block_align;       // bytes per superframe
	uint32_t data_offset;  // start of the audio data in the file
	uint32_t data_size;
} At9Info;

// Returns 0 when buf holds an ATRAC9 WAVE file whose data lies within it.
int at9_parse(const uint8_t *buf, size_t size, At9Info *info);
