
#include "SongSeq.h"

#include <cstdint>
#include <cstring>
#include "Utf8File.h"

namespace {
const char kSeqMagic[4] = { 'C', 'S', 'Q', '1' };
}

bool SongSeqEncode(const SongSeq& seq, std::string& out)
{
  out.clear();
  if (seq.empty()) return false;

  int32_t numRows = (int32_t)seq.rows.size();
  int32_t numCols = (int32_t)seq.cols;
  double  step    = seq.samplesPerStep;

  auto append = [&out](const void* p, size_t n) { out.append((const char*)p, n); };
  append(kSeqMagic, sizeof(kSeqMagic));
  append(&numRows, sizeof(numRows));
  append(&numCols, sizeof(numCols));
  append(&step, sizeof(step));
  for (const SongEventRow& row : seq.rows) {
    int32_t triplet[3] = { row.channel, row.note, row.velocity };
    append(triplet, sizeof(triplet));
  }
  if (!seq.matrix.empty())
    append(seq.matrix.data(), seq.matrix.size());
  return true;
}

bool SongSeqDecode(const char* data, size_t len, SongSeq& out)
{
  if (!data) return false;

  size_t pos = 0;
  auto take = [&](void* dst, size_t n) {
    if (pos + n > len) return false;
    memcpy(dst, data + pos, n);
    pos += n;
    return true;
  };

  char magic[4];
  if (!take(magic, sizeof(magic)) || memcmp(magic, kSeqMagic, sizeof(kSeqMagic)) != 0)
    return false;

  int32_t numRows = 0, numCols = 0;
  double  step = 0.0;
  if (!take(&numRows, sizeof(numRows))) return false;
  if (!take(&numCols, sizeof(numCols))) return false;
  if (!take(&step, sizeof(step))) return false;
  if (numRows < 0 || numCols < 0) return false;
  // Guard the row*col multiply below against overflow on a bogus header.
  if ((int64_t)numRows * (int64_t)numCols > (int64_t)64 * 1024 * 1024) return false;

  std::vector<SongEventRow> rows((size_t)numRows);
  for (int32_t r = 0; r < numRows; r++) {
    int32_t triplet[3] = { 0, 0, 0 };
    if (!take(triplet, sizeof(triplet))) return false;
    rows[(size_t)r] = { triplet[0], triplet[1], triplet[2] };
  }

  std::vector<unsigned char> matrix((size_t)numRows * (size_t)numCols);
  if (!matrix.empty() && !take(matrix.data(), matrix.size())) return false;

  out.rows = std::move(rows);
  out.cols = numCols;
  out.matrix = std::move(matrix);
  out.samplesPerStep = step;
  return true;
}

bool SongSeqWriteFile(const SongSeq& seq, const std::string& path)
{
  std::string blob;
  return SongSeqEncode(seq, blob) && WriteWholeFile(path, blob);
}

bool SongSeqReadFile(const std::string& path, SongSeq& out)
{
  std::string blob;
  return ReadWholeFile(path, blob) && SongSeqDecode(blob.data(), blob.size(), out);
}
