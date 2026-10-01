// The recorded event matrix and its "CSQ1" wire format (codesynth.seq, plugin state).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Row r owns event id r + kEventIdBase.
struct SongEventRow { int channel; int note; int velocity; };

// One row per distinct event, one column per 16th; a held note is a run of 1s.
struct SongSeq
{
  std::vector<SongEventRow>  rows;
  int                        cols = 0;
  std::vector<unsigned char> matrix;         // rows.size() * cols, row-major
  double                     samplesPerStep = 0.0; // 16th-note length in samples

  bool empty() const { return rows.empty() || cols <= 0; }
};

// "CSQ1" blob, native-endian:
//   char     magic[4]     "CSQ1"
//   int32    numRows
//   int32    numCols
//   double   samplesPerStep
//   int32[numRows*3]      channel, note, velocity per row
//   uint8[numRows*numCols] matrix, row-major

// False (leaving `out` empty) when there's nothing recorded.
bool SongSeqEncode(const SongSeq& seq, std::string& out);

// False on a truncated/foreign blob, leaving `out` untouched.
bool SongSeqDecode(const char* data, size_t len, SongSeq& out);

bool SongSeqWriteFile(const SongSeq& seq, const std::string& path);
bool SongSeqReadFile(const std::string& path, SongSeq& out);
