// Lossless compressor from a recorded event matrix to a pattern-indexed format a
// tiny player can run (ExportToCSource() emits it; ExpandChannelGrid() mirrors it).
//
// Input: rows = distinct (channel, note, velocity) events, cols = time steps,
// matrix[row * numCols + col] == 1 iff that event fires on that step.
//
// Stage 1, channel collapse: a channel emits one event id per step, so rows that
// coincide need different channels. Row r gets id r + kEventIdBase, laid into
// grid[col * channelCount + channel] (0 = rest).
//
// Stage 2, patterns: one 16-bit word per (pattern_id, channel):
//   bits  0..10 : column_data_id  (index into a shared pool)
//   bits 11..12 : res_shift       (hold each entry 2^res_shift steps)
//   bits 13..15 : repeat_shift    (loop of 2^repeat_shift entries)
//   event = column_data[((col & COLUMN_DATA_ID_MASK) << SONG_COLUMN_DATA_ALIGNMENT_SHIFT)
//                       + ((pattern_pos >> res_shift) & ((1 << repeat_shift) - 1))];
// Columns share the pool, so identical and overlapping loops are stored once.
// The alignment shift lets the 11-bit id address more than 2048 entries; entries
// are 8-bit unless ids exceed 255. Each column takes its shortest exact loop
// (verbatim always works), pattern_len 8..128 is tried, the smallest total wins,
// and every result is re-expanded and verified.
//
// Self-test: g++ -std=c++17 -O2 -DCOMPRESS_SEQUENCE_SELF_TEST CompressSequence.cpp -o compress_seq

#include "CompressSequence.h"

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <map>
#include <set>
#include <utility>

// Bit layout, duplicated verbatim into the exported C header.
static const int kColumnDataIdBits   = 11;
static const int kResShiftBits    = 2;
static const int kRepeatShiftBits = 3;

static const int COLUMN_DATA_ID_SHIFT = 0;
static const int RES_SHIFT         = kColumnDataIdBits;                   // 11
static const int REPEAT_SHIFT      = kColumnDataIdBits + kResShiftBits;   // 13

static const uint16_t COLUMN_DATA_ID_MASK = (uint16_t)((1u << kColumnDataIdBits) - 1)  << COLUMN_DATA_ID_SHIFT; // 0x07FF
static const uint16_t RES_MASK         = (uint16_t)((1u << kResShiftBits) - 1)   << RES_SHIFT;          // 0x1800
static const uint16_t REPEAT_MASK      = (uint16_t)((1u << kRepeatShiftBits) - 1) << REPEAT_SHIFT;      // 0xE000

static const int kMaxColumnDataSlots = 1 << kColumnDataIdBits;         // 2048
static const int kMaxResShift     = (1 << kResShiftBits) - 1;    // 3  -> resolution up to 8
static const int kMaxRepeatShift  = (1 << kRepeatShiftBits) - 1; // 7  -> loop up to 128

// Stage 2: patterns.
namespace {
struct ColumnEncoding
{
    int resShift    = 0;
    int repeatShift = 0;
    std::vector<uint16_t> slice; // the loop entries actually stored in the pool
};

// Smallest exact encoding of one channel column of length patternLen.
ColumnEncoding EncodeColumn(const uint16_t* col, int patternLen)
{
    ColumnEncoding best;
    bool haveBest = false;

    for (int resShift = 0; resShift <= kMaxResShift; ++resShift)
    {
        const int res = 1 << resShift;
        if (res > patternLen)
            break;

        // The player holds one entry per run, so each aligned run must be constant.
        bool resOk = true;
        for (int pos = 0; pos < patternLen && resOk; ++pos)
            if (col[pos] != col[pos & ~(res - 1)])
                resOk = false;
        if (!resOk)
            continue;

        const int numActive = patternLen / res; // one sample per held run
        // Smallest loop length (2^repeat_shift) whose cyclic repetition
        // reproduces every active sample; repeat_shift=7 is always valid.
        for (int repeatShift = 0; repeatShift <= kMaxRepeatShift; ++repeatShift)
        {
            const int rep = 1 << repeatShift;
            bool periodic = true;
            for (int j = 0; j < numActive && periodic; ++j)
                if (col[(j & (rep - 1)) * res] != col[j * res])
                    periodic = false;
            if (!periodic)
                continue;

            const int L = std::min(rep, numActive); // stored loop entries
            if (!haveBest || (size_t)L < best.slice.size())
            {
                best.resShift    = resShift;
                best.repeatShift = repeatShift;
                best.slice.resize(L);
                for (int k = 0; k < L; ++k)
                    best.slice[k] = col[k * res];
                haveBest = true;
            }
            break; // smallest valid rep for this res -> shortest slice for this res
        }
    }
    return best; // haveBest guaranteed (resShift==0 gives a full-length fallback)
}

// Packs unique slices into the pool with the smallest alignment shift keeping
// slots < 2048. Longest first, each at the aligned offset growing the pool
// least; unclaimed padding is 0.
bool PackPool(const std::vector<std::vector<uint16_t>>& uniqueSlices,
              std::vector<uint16_t>& outPool,
              std::vector<int>&      outSlotId,
              int&                   outAlignShift)
{
    std::vector<int> order(uniqueSlices.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return uniqueSlices[a].size() > uniqueSlices[b].size(); });

    int maxValue = 0;
    for (const auto& s : uniqueSlices)
        for (uint16_t v : s) maxValue = std::max(maxValue, (int)v);

    for (int shift = 0; shift <= 16; ++shift)
    {
        const int align = 1 << shift;
        std::vector<uint16_t> pool;
        std::vector<unsigned char> isPad;
        std::vector<std::vector<int>> alignedAt(maxValue + 1); // value -> aligned offsets holding it
        std::vector<int> slot(uniqueSlices.size());
        bool ok = true;

        for (int i : order)
        {
            const std::vector<uint16_t>& s = uniqueSlices[i];
            const int L    = (int)s.size();
            const int size = (int)pool.size();

            int bestOff  = (size + align - 1) & ~(align - 1);
            int bestGrow = bestOff + L - size;
            for (int off : alignedAt[s[0]])
            {
                if (bestGrow == 0) break;
                const int grow = std::max(0, off + L - size);
                if (grow >= bestGrow) continue;
                bool match = true;
                for (int k = 1; k < L && off + k < size && match; ++k)
                    if (!isPad[off + k] && pool[off + k] != s[k]) match = false;
                if (match) { bestOff = off; bestGrow = grow; }
            }

            const int id = bestOff >> shift;
            if (id >= kMaxColumnDataSlots) { ok = false; break; }
            slot[i] = id;

            if (bestOff + L > size)
            {
                pool.resize(bestOff + L, 0);
                isPad.resize(bestOff + L, 1);
            }
            for (int k = 0; k < L; ++k)
            {
                const int p = bestOff + k;
                if (!isPad[p]) continue;
                pool[p]  = s[k];
                isPad[p] = 0;
                if ((p & (align - 1)) == 0) alignedAt[s[k]].push_back(p);
            }
        }
        if (!ok)
            continue;

        outPool       = std::move(pool);
        outSlotId     = std::move(slot);
        outAlignShift = shift;
        return true;
    }
    return false;
}

// Pattern-compress a channel grid (grid[tick*channelCount+channel]) for one
// fixed pattern-length shift. False if unaddressable within the 11-bit id space.
bool CompressForPatternLen(const std::vector<uint16_t>& grid,
                           int channelCount,
                           int numTicks,
                           int patternLenShift,
                           CompressedSequence& out)
{
    const int patternLen  = 1 << patternLenShift;
    const int numPatterns = (numTicks + patternLen - 1) / patternLen;

    std::vector<ColumnEncoding> enc(numPatterns * channelCount);
    std::map<std::vector<uint16_t>, int> sliceIndex;
    std::vector<std::vector<uint16_t>>   uniqueSlices;
    std::vector<int>                     columnUnique(enc.size());

    std::vector<uint16_t> col(patternLen);

    for (int p = 0; p < numPatterns; ++p)
    {
        for (int ch = 0; ch < channelCount; ++ch)
        {
            for (int pos = 0; pos < patternLen; ++pos)
            {
                const int tick = p * patternLen + pos;
                const int idx  = tick * channelCount + ch;
                col[pos] = (tick < numTicks && idx < (int)grid.size()) ? grid[idx] : 0;
            }

            const int e = p * channelCount + ch;
            enc[e] = EncodeColumn(col.data(), patternLen);

            auto it = sliceIndex.find(enc[e].slice);
            if (it == sliceIndex.end())
            {
                const int u = (int)uniqueSlices.size();
                sliceIndex.emplace(enc[e].slice, u);
                uniqueSlices.push_back(enc[e].slice);
                columnUnique[e] = u;
            }
            else
                columnUnique[e] = it->second;
        }
    }

    std::vector<uint16_t> pool;
    std::vector<int>      slotId;
    int alignShift = 0;
    if (!PackPool(uniqueSlices, pool, slotId, alignShift))
        return false;

    std::vector<uint16_t> columns(enc.size());
    for (size_t e = 0; e < enc.size(); ++e)
    {
        const int id = slotId[columnUnique[e]];
        columns[e] = (uint16_t)(
              (id                   << COLUMN_DATA_ID_SHIFT)
            | (enc[e].resShift      << RES_SHIFT)
            | (enc[e].repeatShift   << REPEAT_SHIFT));
    }

    out.channelCount    = channelCount;
    out.patternLenShift = patternLenShift;
    out.alignmentShift  = alignShift;
    out.numTicks        = numTicks;
    out.numPatterns     = numPatterns;
    out.columns         = std::move(columns);
    out.data            = std::move(pool);
    return true;
}

// Pattern-compress a channel grid, trying every pattern length.
bool CompressChannelGrid(const std::vector<uint16_t>& grid,
                         int channelCount,
                         int numTicks,
                         CompressedSequence& out)
{
    bool haveBest = false;
    CompressedSequence best;
    for (int shift = 3; shift <= 7; ++shift)
    {
        CompressedSequence cand;
        if (!CompressForPatternLen(grid, channelCount, numTicks, shift, cand))
            continue;
        if (!haveBest || cand.sizeBytes() < best.sizeBytes())
        {
            best = std::move(cand);
            haveBest = true;
        }
    }
    if (haveBest)
        out = std::move(best);
    return haveBest;
}

// Stage 1: rows -> channels. Fewer channels mean denser, less loopable
// timelines, so candidates are scored by real compressed size: first-fit
// orderings, one row per channel, and a cost-guided merge between them.

// Column bitset per row, for O(numCols/64) "do these rows ever coincide?" tests.
struct RowColumnBits
{
    int words = 0;
    std::vector<uint64_t> bits; // numRows * words

    RowColumnBits(const std::vector<unsigned char>& matrix, int numRows, int numCols)
    {
        words = (numCols + 63) / 64;
        bits.assign((size_t)numRows * words, 0);
        for (int r = 0; r < numRows; ++r)
            for (int c = 0; c < numCols; ++c)
                if (matrix[(size_t)r * numCols + c])
                    bits[(size_t)r * words + (c >> 6)] |= (uint64_t)1 << (c & 63);
    }
    const uint64_t* row(int r) const { return &bits[(size_t)r * words]; }
};

// First-fit: walk rows in `order`, placing each on the lowest-indexed channel
// it never coincides with, else opening a new one. Orders give different counts.
int FirstFitAssign(const RowColumnBits& rb, int numRows,
                   const std::vector<int>& order, std::vector<int>& channelOfRow)
{
    channelOfRow.assign(numRows, 0);
    std::vector<std::vector<uint64_t>> chanCols; // active columns per channel
    for (int r : order)
    {
        int placed = -1;
        for (int ch = 0; ch < (int)chanCols.size() && placed < 0; ++ch)
        {
            bool disjoint = true;
            for (int w = 0; w < rb.words && disjoint; ++w)
                if (rb.row(r)[w] & chanCols[ch][w]) disjoint = false;
            if (disjoint) placed = ch;
        }
        if (placed < 0)
        {
            placed = (int)chanCols.size();
            chanCols.emplace_back(rb.row(r), rb.row(r) + rb.words);
        }
        else
            for (int w = 0; w < rb.words; ++w) chanCols[placed][w] |= rb.row(r)[w];
        channelOfRow[r] = placed;
    }
    return (int)chanCols.size();
}

// `before` must land on a lower channel than `after`.
struct OrderEdge { int before; int after; };

// False if two rows collide on a channel, the order constraints are cyclic, or
// the grid is unaddressable. Channels are ordered topologically by the
// constraints, ties by first appearance.
bool CompressAssignment(const std::vector<unsigned char>& matrix,
                        int numRows, int numCols,
                        const std::vector<int>& channelOfRow,
                        const std::vector<OrderEdge>& orderEdges,
                        CompressedSequence& out, long& compressCalls)
{
    // Distinct channel labels, in first-appearance order (label -> node index).
    std::map<int, int> appear;
    for (int r = 0; r < numRows; ++r)
        appear.emplace(channelOfRow[r], (int)appear.size());
    const int channelCount = (int)appear.size();
    if (channelCount == 0)
        return false;

    std::vector<int> denseOfLabel(channelCount);
    if (orderEdges.empty())
    {
        for (int i = 0; i < channelCount; ++i) denseOfLabel[i] = i;
    }
    else
    {
        std::vector<std::set<int>> succ(channelCount);
        std::vector<int> indeg(channelCount, 0);
        for (const OrderEdge& e : orderEdges)
        {
            const int u = appear[channelOfRow[e.before]];
            const int v = appear[channelOfRow[e.after]];
            if (u == v)
                return false; // two coinciding ordered rows share a channel -> invalid
            if (succ[u].insert(v).second) ++indeg[v];
        }
        // Kahn's algorithm, ties in appearance order.
        std::set<int> ready;
        for (int i = 0; i < channelCount; ++i) if (indeg[i] == 0) ready.insert(i);
        int assigned = 0;
        while (!ready.empty())
        {
            const int u = *ready.begin();
            ready.erase(ready.begin());
            denseOfLabel[u] = assigned++;
            for (int v : succ[u]) if (--indeg[v] == 0) ready.insert(v);
        }
        if (assigned != channelCount)
            return false; // cycle -> ordering unsatisfiable for this assignment
    }

    std::vector<uint16_t> grid((size_t)numCols * channelCount, 0);
    for (int r = 0; r < numRows; ++r)
    {
        const int ch = denseOfLabel[appear[channelOfRow[r]]];
        const uint16_t eventId = (uint16_t)(r + kEventIdBase);
        for (int c = 0; c < numCols; ++c)
            if (matrix[(size_t)r * numCols + c])
            {
                uint16_t& cell = grid[(size_t)c * channelCount + ch];
                if (cell != 0) return false; // two rows collide -> invalid assignment
                cell = eventId;
            }
    }

    ++compressCalls;
    CompressedSequence cs;
    if (!CompressChannelGrid(grid, channelCount, numCols, cs))
        return false;
    cs.numRows         = numRows;
    cs.numCols         = numCols;
    cs.poolElementBits = (numRows + kEventIdBase - 1 <= 255) ? 8 : 16;
    out = std::move(cs);
    return true;
}

// Start at one channel per row and greedily apply any merge of two
// coincidence-free channels that compresses smaller. `budget` caps the trials.
void MergeRefine(const std::vector<unsigned char>& matrix, int numRows, int numCols,
                 const RowColumnBits& rb, const std::vector<OrderEdge>& orderEdges,
                 CompressedSequence& best, long& compressCalls, long budget)
{
    std::vector<int> channelOfRow(numRows);
    std::iota(channelOfRow.begin(), channelOfRow.end(), 0); // one channel per row

    CompressedSequence cur;
    if (!CompressAssignment(matrix, numRows, numCols, channelOfRow, orderEdges, cur, compressCalls))
        return;
    if (best.numRows == 0 || cur.sizeBytes() < best.sizeBytes())
        best = cur;

    for (;;)
    {
        // Distinct channel labels currently in use, and their active columns.
        std::map<int, std::vector<uint64_t>> chanCols;
        for (int r = 0; r < numRows; ++r)
        {
            auto& cc = chanCols[channelOfRow[r]];
            if (cc.empty()) cc.assign(rb.words, 0);
            for (int w = 0; w < rb.words; ++w) cc[w] |= rb.row(r)[w];
        }
        std::vector<int> labels;
        for (auto& kv : chanCols) labels.push_back(kv.first);

        bool improved = false;
        for (size_t i = 0; i < labels.size() && !improved; ++i)
        {
            for (size_t j = i + 1; j < labels.size() && !improved; ++j)
            {
                if (compressCalls >= budget) return;
                // Mergeable only if the two channels never fire on the same column.
                const auto& ca = chanCols[labels[i]];
                const auto& cb = chanCols[labels[j]];
                bool disjoint = true;
                for (int w = 0; w < rb.words && disjoint; ++w)
                    if (ca[w] & cb[w]) disjoint = false;
                if (!disjoint) continue;

                std::vector<int> trial = channelOfRow;
                for (int& lbl : trial) if (lbl == labels[j]) lbl = labels[i];

                CompressedSequence cand;
                if (CompressAssignment(matrix, numRows, numCols, trial, orderEdges, cand, compressCalls) &&
                    cand.sizeBytes() < cur.sizeBytes())
                {
                    channelOfRow = std::move(trial);
                    cur = std::move(cand);
                    if (cur.sizeBytes() < best.sizeBytes()) best = cur;
                    improved = true;
                }
            }
        }
        if (!improved) break;
    }
}
} // namespace

// Many row->channel candidates, each compressed; the smallest wins.
CompressedSequence CompressEventMatrix(const std::vector<unsigned char>& matrix,
                                       int numRows, int numCols,
                                       const std::vector<int>& rowOrder)
{
    CompressedSequence best;
    if (numRows <= 0 || numCols <= 0)
        return best;

    RowColumnBits rb(matrix, numRows, numCols);
    long compressCalls = 0;

    // An edge for every pair of coinciding rows whose non-zero orders differ;
    // usually empty.
    std::vector<OrderEdge> orderEdges;
    bool haveOrder = false;
    if ((int)rowOrder.size() == numRows)
        for (int o : rowOrder) if (o != 0) { haveOrder = true; break; }
    if (haveOrder)
    {
        for (int a = 0; a < numRows; ++a)
        {
            if (rowOrder[a] == 0) continue;
            for (int b = a + 1; b < numRows; ++b)
            {
                if (rowOrder[b] == 0 || rowOrder[a] == rowOrder[b]) continue;
                bool coincide = false;
                for (int w = 0; w < rb.words && !coincide; ++w)
                    if (rb.row(a)[w] & rb.row(b)[w]) coincide = true;
                if (!coincide) continue;
                if (rowOrder[a] < rowOrder[b]) orderEdges.push_back({ a, b });
                else                           orderEdges.push_back({ b, a });
            }
        }
    }

    // Per-row activity (trigger count) and conflict degree, for order heuristics.
    std::vector<int> activity(numRows, 0), degree(numRows, 0);
    for (int r = 0; r < numRows; ++r)
    {
        for (int w = 0; w < rb.words; ++w)
        {
            uint64_t x = rb.row(r)[w];
            while (x) { x &= x - 1; ++activity[r]; }
        }
        for (int s = 0; s < numRows; ++s)
        {
            if (s == r) continue;
            for (int w = 0; w < rb.words; ++w)
                if (rb.row(r)[w] & rb.row(s)[w]) { ++degree[r]; break; }
        }
    }

    // A spread of first-fit orderings: first-appearance, by activity, by degree,
    // and a few deterministic shuffles. Each explores a different collapse.
    std::vector<std::vector<int>> orders;
    std::vector<int> base(numRows);
    std::iota(base.begin(), base.end(), 0);
    orders.push_back(base);                                                   // first appearance
    { auto o = base; std::sort(o.begin(), o.end(), [&](int a,int b){return activity[a]>activity[b];}); orders.push_back(o); }
    { auto o = base; std::sort(o.begin(), o.end(), [&](int a,int b){return activity[a]<activity[b];}); orders.push_back(o); }
    { auto o = base; std::sort(o.begin(), o.end(), [&](int a,int b){return degree[a]>degree[b];});     orders.push_back(o); } // Welsh-Powell
    { auto o = base; std::sort(o.begin(), o.end(), [&](int a,int b){return degree[a]<degree[b];});     orders.push_back(o); }
    {
        uint32_t s = 0x9E3779B9u;
        for (int k = 0; k < 6; ++k)
        {
            auto o = base;
            for (int i = numRows - 1; i > 0; --i)
            { s ^= s<<13; s ^= s>>17; s ^= s<<5; std::swap(o[i], o[(int)(s % (uint32_t)(i + 1))]); }
            orders.push_back(o);
        }
    }

    std::vector<int> channelOfRow;
    for (auto& order : orders)
    {
        FirstFitAssign(rb, numRows, order, channelOfRow);
        CompressedSequence cand;
        if (CompressAssignment(matrix, numRows, numCols, channelOfRow, orderEdges, cand, compressCalls))
            if (best.numRows == 0 || cand.sizeBytes() < best.sizeBytes())
                best = std::move(cand);
    }

    // Constraints can reject first-fit candidates as cyclic; one channel per row
    // always satisfies them.
    if (haveOrder)
    {
        std::vector<int> perRow(numRows);
        std::iota(perRow.begin(), perRow.end(), 0);
        CompressedSequence cand;
        if (CompressAssignment(matrix, numRows, numCols, perRow, orderEdges, cand, compressCalls))
            if (best.numRows == 0 || cand.sizeBytes() < best.sizeBytes())
                best = std::move(cand);
    }

    // O(channels^2) trials, so only for modest row counts.
    if (numRows <= 64)
        MergeRefine(matrix, numRows, numCols, rb, orderEdges, best, compressCalls, /*budget=*/8000);

    return best;
}

// Reference expand/verify, using the exported player's arithmetic.
std::vector<uint16_t> ExpandChannelGrid(const CompressedSequence& cs)
{
    std::vector<uint16_t> grid((size_t)cs.numTicks * cs.channelCount, 0);
    const int patternLen = cs.patternLen();

    for (int p = 0; p < cs.numPatterns; ++p)
        for (int ch = 0; ch < cs.channelCount; ++ch)
        {
            const uint16_t column = cs.columns[p * cs.channelCount + ch];
            const int resShift    = (column & RES_MASK)    >> RES_SHIFT;
            const int repeatShift = (column & REPEAT_MASK) >> REPEAT_SHIFT;
            const int base        = (int)((column & COLUMN_DATA_ID_MASK) << cs.alignmentShift);

            for (int pos = 0; pos < patternLen; ++pos)
            {
                const int tick = p * patternLen + pos;
                if (tick >= cs.numTicks) break;
                grid[(size_t)tick * cs.channelCount + ch] =
                    cs.data[base + ((pos >> resShift) & ((1 << repeatShift) - 1))];
            }
        }
    return grid;
}

// Reconstructs the original rows x cols boolean event matrix.
std::vector<unsigned char> ExpandToMatrix(const CompressedSequence& cs)
{
    std::vector<unsigned char> matrix((size_t)cs.numRows * cs.numCols, 0);
    std::vector<uint16_t> grid = ExpandChannelGrid(cs);
    for (int col = 0; col < cs.numCols; ++col)
        for (int ch = 0; ch < cs.channelCount; ++ch)
        {
            const uint16_t ev = grid[(size_t)col * cs.channelCount + ch];
            if (ev != 0)
            {
                const int row = ev - kEventIdBase;
                if (row >= 0 && row < cs.numRows)
                    matrix[(size_t)row * cs.numCols + col] = 1;
            }
        }
    return matrix;
}

// True if decompression reproduces the original matrix exactly.
bool VerifyLossless(const std::vector<unsigned char>& original, const CompressedSequence& cs)
{
    std::vector<unsigned char> got = ExpandToMatrix(cs);
    const size_t n = (size_t)cs.numRows * cs.numCols;
    if (got.size() != n || original.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if ((original[i] ? 1 : 0) != got[i])
            return false;
    return true;
}

// C export.
static void SplitPrefix(const char* symbolPrefix, std::string& P, std::string& U)
{
    P = symbolPrefix ? symbolPrefix : "song";
    U = P;
    for (char& c : U) c = (char)toupper((unsigned char)c);
}

std::string ExportToCHeader(const CompressedSequence& cs, const char* symbolPrefix)
{
    std::string P, U;
    SplitPrefix(symbolPrefix, P, U);

    const char* eventType = (cs.poolElementBits == 16) ? "unsigned short" : "unsigned char";

    std::string s;
    auto line = [&s](const char* fmt, ...) {
        char lb[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(lb, sizeof(lb), fmt, ap);
        va_end(ap);
        s += lb; s += "\n";
    };

    line("// Auto-generated by CompressSequence.cpp -- do not edit.");
    line("#ifndef %s_SEQUENCE_H", U.c_str());
    line("#define %s_SEQUENCE_H", U.c_str());
    line("");
    line("// Original event matrix was %d rows (events) x %d cols (steps).", cs.numRows, cs.numCols);
    line("// Event id N on a channel maps back to matrix row (N - %d).", kEventIdBase);
    line("#define %s_NUM_ROWS                   %d", U.c_str(), cs.numRows);
    line("#define %s_NUM_COLS                   %d", U.c_str(), cs.numCols);
    line("#define %s_CHANNEL_COUNT              %d", U.c_str(), cs.channelCount);
    line("#define %s_PATTERN_LEN_SHIFT          %d", U.c_str(), cs.patternLenShift);
    line("#define %s_COLUMN_DATA_ALIGNMENT_SHIFT   %d", U.c_str(), cs.alignmentShift);
    line("#define %s_PATTERN_COUNT              %d", U.c_str(), cs.numPatterns);
    line("#define %s_SEQUENCE_COUNT             %d  // numPatterns * channels",
         U.c_str(), (int)cs.columns.size());
    line("#define %s_COLUMN_DATA_COUNT        %d", U.c_str(), (int)cs.data.size());
    line("");
    line("// 16-bit channel-column bit layout.");
    line("#define %s_COLUMN_DATA_ID_MASK 0x%04X", U.c_str(), (unsigned)COLUMN_DATA_ID_MASK);
    line("#define %s_RES_MASK         0x%04X", U.c_str(), (unsigned)RES_MASK);
    line("#define %s_RES_SHIFT        %d",     U.c_str(), RES_SHIFT);
    line("#define %s_REPEAT_MASK      0x%04X", U.c_str(), (unsigned)REPEAT_MASK);
    line("#define %s_REPEAT_SHIFT     %d",     U.c_str(), REPEAT_SHIFT);
    line("");
    line("// Caller-owned playback state, passed by pointer so independent callers");
    line("// (e.g. an audio-thread renderer and a UI-thread visual renderer) can each");
    line("// hold their own -- see %s_sequence_tick. All-zero is the initial state.", P.c_str());
    line("//");
    line("// The struct is deliberately NOT opaque: embedded callers want to place it");
    line("// themselves (static storage, an arena, inside a bigger struct). The price");
    line("// is that its layout is ABI between whoever allocates it and the generated");
    line("// player, and %s_CHANNEL_COUNT above -- which the exporter re-derives from", U.c_str());
    line("// the recorded matrix on every export, so it CHANGES when the song does --");
    line("// sets that layout. Never hand-copy this struct or the count into another");
    line("// header: include this one. (A hand-copy that went stale at 2 channels while");
    line("// the song moved to 3 is exactly how this header came to exist.)");
    line("typedef struct {");
    line("    int latched_tick; // song tick + 1 last latched, 0 = none yet");
    line("    %s channel_event[%s_CHANNEL_COUNT];", eventType, U.c_str());
    line("    int channel_timestamp[%s_CHANNEL_COUNT];", U.c_str());
    line("} %s_SongState;", P.c_str());
    line("");
    line("#endif // %s_SEQUENCE_H", U.c_str());

    return s;
}

std::string ExportToCSource(const CompressedSequence& cs, const char* symbolPrefix)
{
    std::string P, U;
    SplitPrefix(symbolPrefix, P, U);

    const char* poolType  = (cs.poolElementBits == 16) ? "unsigned short" : "unsigned char";
    const char* eventType = (cs.poolElementBits == 16) ? "unsigned short" : "unsigned char";

    std::string s;
    char buf[256];
    auto line = [&s](const char* fmt, ...) {
        char lb[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(lb, sizeof(lb), fmt, ap);
        va_end(ap);
        s += lb; s += "\n";
    };

    line("// Auto-generated by CompressSequence.cpp -- do not edit.");
    line("// Constants and %s_SongState come from the generated header (see", P.c_str());
    line("// ExportToCHeader); this file is the tables and the player only.");
    line("");
    line("// Audio samples per song tick -- set to taste (e.g. samplesPerStep).");
    line("#ifndef %s_SAMPLES_PER_SONGTICK", U.c_str());
    line("#define %s_SAMPLES_PER_SONGTICK      4410", U.c_str());
    line("#endif");
    line("");

    line("static const %s %s_column_data[%s_COLUMN_DATA_COUNT] = {",
         poolType, P.c_str(), U.c_str());
    {
        std::string row = "    ";
        for (size_t i = 0; i < cs.data.size(); ++i)
        {
            snprintf(buf, sizeof(buf), "%u,", (unsigned)cs.data[i]);
            row += buf;
            if ((i % 16) == 15) { s += row; s += "\n"; row = "    "; }
        }
        if (row.size() > 4) { s += row; s += "\n"; }
    }
    line("};");
    line("");

    line("static const unsigned short %s_sequence[%s_SEQUENCE_COUNT] = {", P.c_str(), U.c_str());
    {
        std::string row = "    ";
        for (size_t i = 0; i < cs.columns.size(); ++i)
        {
            snprintf(buf, sizeof(buf), "0x%04X,", (unsigned)cs.columns[i]);
            row += buf;
            if ((i % 12) == 11) { s += row; s += "\n"; row = "    "; }
        }
        if (row.size() > 4) { s += row; s += "\n"; }
    }
    line("};");
    line("");

    line("// Advance the sequencer to the tick sample_id falls in (safe every audio");
    line("// batch; a zeroed state latches tick 0 on the first call). Each channel's");
    line("// event id sustains its note, re-triggering only when the id changes; 0");
    line("// silences.");
    line("static void %s_sequence_tick(int sample_id, %s_SongState* state)", P.c_str(), P.c_str());
    line("{");
    line("    int song_tick   = sample_id / %s_SAMPLES_PER_SONGTICK;", U.c_str());
    line("    if (song_tick + 1 == state->latched_tick)");
    line("        return;");
    line("    state->latched_tick = song_tick + 1;");
    line("    int pattern_len = (1 << %s_PATTERN_LEN_SHIFT);", U.c_str());
    line("    int pattern_pos = song_tick & (pattern_len - 1);");
    line("    int pattern_id  = song_tick / pattern_len;");
    line("    if (pattern_id >= %s_PATTERN_COUNT)", U.c_str());
    line("        return;");
    line("    for (int channel_id = 0; channel_id < %s_CHANNEL_COUNT; channel_id++) {", U.c_str());
    line("        unsigned short column = %s_sequence[pattern_id * %s_CHANNEL_COUNT + channel_id];",
         P.c_str(), U.c_str());
    line("        int res_shift = (column & %s_RES_MASK) >> %s_RES_SHIFT;", U.c_str(), U.c_str());
    line("        int repeat_shift = (column & %s_REPEAT_MASK) >> %s_REPEAT_SHIFT;", U.c_str(), U.c_str());
    line("        int base = (column & %s_COLUMN_DATA_ID_MASK) << %s_COLUMN_DATA_ALIGNMENT_SHIFT;",
         U.c_str(), U.c_str());
    line("        int event_id = %s_column_data[base + ((pattern_pos >> res_shift) & ((1 << repeat_shift) - 1))];",
         P.c_str());
    line("        if (event_id != (int)state->channel_event[channel_id]) {");
    line("            state->channel_event[channel_id]     = (%s)event_id;", eventType);
    line("            state->channel_timestamp[channel_id] = sample_id;");
    line("        }");
    line("    }");
    line("}");

    return s;
}

#ifdef COMPRESS_SEQUENCE_SELF_TEST

namespace {
struct Rng { uint32_t s; uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; } };

int g_pass = 0, g_fail = 0;

void Report(const CompressedSequence& cs, const std::vector<unsigned char>& m)
{
    // Lower bound on channels = max simultaneous events in any column.
    int maxSimul = 0;
    for (int c = 0; c < cs.numCols; ++c)
    {
        int n = 0;
        for (int r = 0; r < cs.numRows; ++r)
            if (m[(size_t)r * cs.numCols + c]) ++n;
        if (n > maxSimul) maxSimul = n;
    }
    printf("         rows=%d cols=%d -> channels=%d (lower bound=%d) "
           "patLenShift=%d align=%d pats=%d pool=%zu(%db) bytes=%zu\n",
           cs.numRows, cs.numCols, cs.channelCount, maxSimul,
           cs.patternLenShift, cs.alignmentShift, cs.numPatterns,
           cs.data.size(), cs.poolElementBits, cs.sizeBytes());
}

bool RoundTrip(const std::vector<unsigned char>& matrix, int rows, int cols,
               const std::string& name, bool verbose)
{
    CompressedSequence cs = CompressEventMatrix(matrix, rows, cols);
    bool ok = VerifyLossless(matrix, cs);
    // Channel count must be >= the max simultaneous events (a hard lower bound).
    int maxSimul = 0;
    for (int c = 0; c < cols; ++c) { int n=0; for (int r=0;r<rows;++r) if (matrix[(size_t)r*cols+c]) ++n; if(n>maxSimul) maxSimul=n; }
    bool boundOk = (rows == 0) || (cs.channelCount >= maxSimul && cs.channelCount <= rows);
    if (ok && boundOk) { ++g_pass; printf("  ok   %s\n", name.c_str()); }
    else               { ++g_fail; printf("  FAIL %s (lossless=%d bound=%d)\n", name.c_str(), ok, boundOk); }
    if (verbose) Report(cs, matrix);
    return ok && boundOk;
}
} // namespace

int main()
{
    printf("CompressSequence self-test (event-matrix -> channels -> patterns)\n\n");

    printf("edge cases:\n");
    {
        // Two events that never coincide -> must collapse to 1 channel.
        const int rows = 2, cols = 32;
        std::vector<unsigned char> m((size_t)rows * cols, 0);
        for (int c = 0; c < cols; c += 4) m[0 * cols + c] = 1;        // event 0 on even 4s
        for (int c = 2; c < cols; c += 4) m[1 * cols + c] = 1;        // event 1 offset, never overlaps
        RoundTrip(m, rows, cols, "non-overlapping events collapse to 1 channel", true);
    }
    {
        // Three events all firing on col 0 -> needs 3 channels there.
        const int rows = 3, cols = 16;
        std::vector<unsigned char> m((size_t)rows * cols, 0);
        for (int r = 0; r < rows; ++r) m[r * cols + 0] = 1;
        for (int r = 0; r < rows; ++r) m[r * cols + (r + 1)] = 1;
        RoundTrip(m, rows, cols, "simultaneous events force separate channels", true);
    }
    {
        std::vector<unsigned char> m(4 * 64, 0);
        RoundTrip(m, 4, 64, "empty matrix (no triggers)", true);
    }
    {
        // A realistic little drum recording: kick/snare/hat/bass as 4 rows.
        const int rows = 4, cols = 128;
        std::vector<unsigned char> m((size_t)rows * cols, 0);
        for (int t = 0; t < cols; ++t) {
            if (t % 8 == 0)  m[0 * cols + t] = 1; // kick
            if (t % 8 == 4)  m[1 * cols + t] = 1; // snare
            if (t % 2 == 0)  m[2 * cols + t] = 1; // hat
            if (t % 16 == 2) m[3 * cols + t] = 1; // bass
        }
        RoundTrip(m, rows, cols, "drum recording (kick/snare/hat/bass)", true);
    }
    {
        // Many rows, forcing 16-bit event ids (> 255 rows).
        const int rows = 300, cols = 8;
        std::vector<unsigned char> m((size_t)rows * cols, 0);
        for (int r = 0; r < rows; ++r) m[r * cols + (r % cols)] = 1; // spread across cols
        RoundTrip(m, rows, cols, "300 rows -> 16-bit event pool", true);
    }

    printf("\nrandom fuzz:\n");
    Rng rng{ 0xC0FFEEu };
    for (int iter = 0; iter < 300; ++iter)
    {
        const int rows = 1 + (rng.next() % 40);
        const int cols = 1 + (rng.next() % 400);
        const int density = 1 + (rng.next() % 6); // 1..6 -> ~1/2..1/7 fill
        std::vector<unsigned char> m((size_t)rows * cols, 0);
        for (auto& v : m) v = ((rng.next() % (density + 1)) == 0) ? 1 : 0;

        char nm[80];
        snprintf(nm, sizeof(nm), "fuzz #%d rows=%d cols=%d density=%d", iter, rows, cols, density);
        if (!RoundTrip(m, rows, cols, nm, false))
        {
            printf("  (stopping at first failure)\n");
            break;
        }
    }

    printf("\nstructured song + C export:\n");
    {
        const int rows = 6, cols = 256;
        std::vector<unsigned char> m((size_t)rows * cols, 0);
        for (int t = 0; t < cols; ++t) {
            if (t % 8 == 0)  m[0 * cols + t] = 1;
            if (t % 8 == 4)  m[1 * cols + t] = 1;
            if (t % 2 == 0)  m[2 * cols + t] = 1;
            if (t % 16 == 2) m[3 * cols + t] = 1;
            if (t % 32 == 5) m[4 * cols + t] = 1;
            if (t % 3 == 0)  m[5 * cols + t] = 1;
        }
        CompressedSequence cs = CompressEventMatrix(m, rows, cols);
        bool ok = VerifyLossless(m, cs);
        if (ok) ++g_pass; else ++g_fail;
        printf("  %s structured song lossless\n", ok ? "ok  " : "FAIL");
        Report(cs, m);
        const size_t raw = m.size();
        printf("         raw matrix=%zu bytes  compressed=%zu bytes  ratio=%.2fx\n",
               raw, cs.sizeBytes(), raw ? (double)raw / cs.sizeBytes() : 0.0);

        std::string c = ExportToCSource(cs, "demo");
        printf("\n----- ExportToCSource(\"demo\") preview (first 44 lines) -----\n");
        int shown = 0;
        for (size_t i = 0; i < c.size() && shown < 44; ++i) { putchar(c[i]); if (c[i] == '\n') ++shown; }
        printf("----- (%zu bytes total) -----\n", c.size());
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif // COMPRESS_SEQUENCE_SELF_TEST
