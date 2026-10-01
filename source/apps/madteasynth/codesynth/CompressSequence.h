// The event-matrix compressor and its C emitter. The self-test main() builds
// with COMPRESS_SEQUENCE_SELF_TEST.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// See CompressSequence.cpp for the format.
struct CompressedSequence
{
    int numRows = 0;   // distinct events
    int numCols = 0;   // time steps

    int channelCount    = 0;   // playback channels after collapse
    int patternLenShift = 0;   // SONG_PATTERN_LEN_SHIFT
    int alignmentShift  = 0;   // SONG_COLUMN_DATA_ALIGNMENT_SHIFT
    int numTicks        = 0;   // == numCols (channel-grid height)
    int numPatterns     = 0;
    int poolElementBits = 8;   // 8 or 16

    std::vector<uint16_t> columns; // numPatterns * channelCount channel-columns
    std::vector<uint16_t> data;  // column_data pool, deduped across columns

    int patternLen() const { return 1 << patternLenShift; }

    size_t sizeBytes() const { return columns.size() * 2 + data.size() * (poolElementBits / 8); }
};

// Row r -> event id r + kEventIdBase; 0 means no event.
static const int kEventIdBase = 1;

// matrix[row*numCols + col] is 1 while that event sounds; the player holds an id
// until it changes. Empty result on invalid input.
// `rowOrder`: where two coinciding rows both have a non-zero order, the lower
// lands on a lower channel and so plays first. Empty or all-zero changes nothing.
CompressedSequence CompressEventMatrix(const std::vector<unsigned char>& matrix,
                                       int numRows, int numCols,
                                       const std::vector<int>& rowOrder = {});

// Emitted as a header/source pair: <prefix>_SongState is sized by
// <PREFIX>_CHANNEL_COUNT, so every TU must take it from the one generated header.

// Format constants, channel count and <prefix>_SongState; no data or code.
std::string ExportToCHeader(const CompressedSequence& cs, const char* symbolPrefix);

// Packed tables plus <prefix>_sequence_tick(); include the header first.
std::string ExportToCSource(const CompressedSequence& cs, const char* symbolPrefix);
