#include "pgxp.h"
#include "../utils/memory.h"
#include "../utils/rde_symbols.h"
#include "../utils/settings.h"
#include "widescreen.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

// PGXP: precise vertex positions for RDE's software GTE and GP0 path.
//
// Capture (emulation thread). Every GTE command goes through the one call
// into gte_exec_command. A wrapper runs it, then recomputes each RTPS/RTPT
// projection from the exact products (no integer IR or SZ truncation, exact
// divide, the widescreen x scale) and stores it in per-frame tables keyed by
// the exact 32-bit words the game copies into its packets: the SXY word for
// integer polygons and RDE's 1/8-pixel ring word for fine polygons. A word
// several vertices project to in one frame (common for whole pixels) keeps
// up to four positions, each with the sequence numbers of the projections
// that produced it, and more make it ambiguous. Within one projection window
// (kIntegerSpread) a word keeps its first position for every vertex of
// similar depth, so polygons that share a word there (neighbours of a dense
// fan, or two corners of one polygon) all resolve it the same way.
//
// Face records. Each RTPT, and an RTPT with the RTPS right after it (a
// quad's fourth corner), also stores its positions keyed by its corner words
// in ascending order. A polygon whose corner words are exactly such a batch
// takes its positions from it: the projections that produced its own
// corners, whatever else landed on those words. Neighbours that share a
// vertex projected it from the same inputs, so they get the same position.
//
// Otherwise a polygon takes for each corner the position whose projections
// lie closest to those of its other corners (the corners of one polygon are
// projected together); a corner on which equally close picks disagree is
// left without a record. The first GTE command after an OT walk starts a new
// frame. A polygon takes all its records from one frame: the current one, or
// the previous one when more of its corner words are known there (a packet
// drawn again unchanged).
//
// Precise skinning. The skinners transform model vertices with
// MVMVA (rotation and translation) and blend two joints with GPF/GPL, and
// store the truncated results as the integer vertices the face builders
// project. The wrapper keeps the dropped fraction of each result, keyed by
// the integer x, y, z it produced and numbered in skinning order, and a
// projection whose input vertex has such a record projects the precise point
// instead. Transforms wait in a short queue: the two joint transforms a blend
// consumes leave it there (they lie in the same unit cell as the blended
// vertex and are never projected), the others become records when a
// projection, a full queue or a new frame flushes it. A point several skinned
// vertices truncate to keeps up to four fractions; the vertices of one face
// are skinned close together, so each projection takes for each such vertex
// the fraction whose skinning numbers lie closest to those of the batch's
// other vertices (or, for a lone vertex, to the last batch that used skin
// records), within kSkinSpread. A fraction a projection has used is replaced
// first when a later pass needs room under its key.
//
// Precise matrices. The joint and object matrices are composed
// with CompMatrix, which truncates every product to whole units and
// saturates the rotation entries to 16 bits. A wrapper on its port
// recomputes the composition from the precise parent and local matrices
// (records of earlier compositions) and keeps the dropped part, keyed by the
// integer matrix it produced. A skinning transform or a projection whose GTE
// matrix has such a record uses the precise matrix. Once a record has been
// used, the next composition with the same result replaces it: it is a later
// pass, and every later use follows it.
//
// Transport (emulation thread). The OT walk gathers the packet words for the
// render thread. Before a polygon whose vertex words have records, a private
// GP0 command carrying the records is inserted into the same stream, so the
// records stay in order with their polygon.
//
// Apply (render thread). The GP0 handlers of the polygon ranges are wrapped:
// the private command arms its records for the polygon that follows, and the
// two vertex emits replace the position of a vertex whose word has a record,
// passing the depth in position z for the replacement polygon shaders. Fine
// polygons are corrected per corner; integer polygons only when every corner
// word is known and their projections lie at most kIntegerSpread apart,
// which leaves alone 2D primitives whose corners only happen to share a
// whole pixel with projected vertices. One corner word of an integer polygon
// may be ambiguous: that is a property of the word in that frame, so every
// polygon sharing it leaves it uncorrected and no crack opens.
//
// Sub-pixel fine vertices. The fine vertex emit keeps RDE's 1/8-pixel
// positions instead of rounding them to the 4x render grid.
//
// Batch bounds. RDE draws each batch with a viewport of its vertices'
// bounding box truncated to whole render pixels, which cut the last fraction
// of a pixel off sub-pixel geometry at every batch edge (thin seams between
// texture regions). The box's far edges are rounded up instead.
//
// Culling. After NCLIP (GTE command and RDE's fine NCLIP), MAC0 is recomputed
// from the precise positions when all three SXY entries have one.
//
// F6 (polled during the OT walk while the game window has the focus)
// switches all of it off and on at run time.

namespace {

// GTE command numbers (low 6 bits) and flags of the command word
constexpr uint32_t kGteRtpsAlias = 0x00;
constexpr uint32_t kGteRtps = 0x01;
constexpr uint32_t kGteNclip = 0x06;
constexpr uint32_t kGteMvmva = 0x12;
constexpr uint32_t kGteRtpt = 0x30;
constexpr uint32_t kGteGpf = 0x3D;
constexpr uint32_t kGteGpl = 0x3E;
constexpr uint32_t kGteShiftFlag = 1u << 19;
constexpr uint32_t kGteLimitFlag = 1u << 10;
// MVMVA fields: matrix (0 = rotation), vector (3 = IR), translation (0 = TR)
constexpr uint32_t kGteMvmvaMatrixAndTranslation = (3u << 17) | (3u << 13);
constexpr uint32_t kGteMvmvaVectorShift = 15;
constexpr uint32_t kGteMvmvaVectorIr = 3;
constexpr uintptr_t kGteVectorStride = 8;
constexpr uintptr_t kGteWordStride = 4;

// GP0 polygon command bits (PS1 form)
constexpr uint32_t kGp0Gouraud = 0x10000000;
constexpr uint32_t kGp0Quad = 0x08000000;
constexpr uint32_t kGp0Textured = 0x04000000;
constexpr uint32_t kGp0PolylineFlag = 0x08000000;
constexpr uint32_t kGp0RectSizeMask = 0x18000000;
constexpr uint32_t kGp0PolylineEndMask = 0xF000F000;
constexpr uint32_t kGp0PolylineEnd = 0x50005000;
// RDE's fine polygons: commands 0xC0-0xFF with this bit set
constexpr uint32_t kGp0FineFlag = 0x10000000;
constexpr uint32_t kGp0FillCommand = 0x02;

// Private GP0 command: tag | perspective flag | record count, then the
// polygon's command word, then per record its vertex word and x, y, w
constexpr uint32_t kMarkerMask = 0xFFFFF000;
constexpr uint32_t kMarkerTag = 0xEBC5A000;
constexpr uint32_t kMarkerPerspective = 0x8;
constexpr uint32_t kMarkerCountMask = 0x7;
constexpr uint32_t kMarkerHeaderWords = 2;
constexpr uint32_t kRecordWords = 4;
constexpr uint32_t kMaxCorners = 4;

// A packet holds at most 255 words, so at most 63 polygons
constexpr uint32_t kScratchWords = 2048;

constexpr ULONGLONG kHotkeyPollMs = 15;

struct PreciseVertex {
  float x;
  float y;
  float w;
};

// One position of a vertex word, with the sequence numbers of the first and
// the last projection that produced it
struct VertexCandidate {
  PreciseVertex vertex;
  uint32_t first;
  uint32_t last;
};

// Positions a vertex word was projected to in one frame
constexpr uint32_t kCandidates = 4;
struct VertexEntry {
  uint32_t count;
  VertexCandidate candidates[kCandidates];
};

// Corners of an integer polygon must come from projections at most this many
// apart (one RTPT, an RTPT and an RTPS, or a short run of RTPS)
constexpr uint32_t kIntegerSpread = 256;
// Positions of one word within the window and at most this relative depth
// apart are one vertex
constexpr float kSameSurfaceDepth = 1.0f / 16.0f;

// Fraction a skinner's transform or blend dropped from each axis
struct SkinOffset {
  float d[3];
};

// One fraction of a skinned point, with the skinning numbers of the first and
// the last result that produced it
struct SkinCandidate {
  SkinOffset offset;
  uint32_t first;
  uint32_t last;
};

// Fractions of the skinned vertices that truncate to one point in one frame;
// bit i of used is set once a projection has taken candidate i
struct SkinEntry {
  uint32_t count;
  uint32_t used;
  SkinCandidate candidates[kCandidates];
};

// A skinned vertex's fraction is taken only when the skinning numbers of the
// batch's records lie at most this far apart (more vertices than one model
// part skins)
constexpr uint32_t kSkinSpread = 4096;

// A GTE or PS1 matrix: nine 4.12 rotation entries and a translation
struct MatrixKey {
  int16_t rotation[9];
  int16_t pad;
  int32_t translation[3];
};

bool operator==(const MatrixKey &a, const MatrixKey &b) {
  return memcmp(&a, &b, sizeof(a)) == 0;
}

// What a matrix composition dropped from each entry of its result
struct MatrixOffset {
  float rotation[9];
  float translation[3];
};

// Words of the corners of one projection batch (an RTPT, or an RTPT and the
// RTPS right after it), in ascending order
struct FaceKey {
  uint32_t corners;
  uint32_t words[kMaxCorners];
};

bool operator==(const FaceKey &a, const FaceKey &b) {
  return memcmp(&a, &b, sizeof(a)) == 0;
}

// Positions of a batch's corners, in the order of the key's words
struct FaceRecord {
  PreciseVertex vertices[kMaxCorners];
};

constexpr uint32_t kStampAmbiguous = 0x80000000u;
constexpr uint32_t kStampUsed = 0x40000000u;
constexpr uint32_t kStampEpoch = 0x3FFFFFFFu;
constexpr uint32_t kTableProbes = 32;
constexpr float kSamePosition = 1.0f / 64.0f;
constexpr float kSameDepth = 1.0f / 1024.0f;
constexpr float kSameOffset = 1.0f / 1024.0f;
// Skinned points this close are one point: at most about 0.01 pixel apart
// on screen
constexpr float kSameSkinOffset = 1.0f / 64.0f;
// Temporary (stats): compositions whose dropped part exceeds these, which
// deep or scaled chains reach
constexpr double kLargeRotationOffset = 4.0;
constexpr double kLargeTranslationOffset = 64.0;

bool SameValue(const PreciseVertex &a, const PreciseVertex &b) {
  return std::fabs(a.x - b.x) <= kSamePosition &&
         std::fabs(a.y - b.y) <= kSamePosition &&
         std::fabs(a.w - b.w) <= a.w * kSameDepth;
}

bool SameValue(const FaceRecord &a, const FaceRecord &b) {
  for (uint32_t i = 0; i < kMaxCorners; ++i) {
    if (!SameValue(a.vertices[i], b.vertices[i]))
      return false;
  }
  return true;
}

bool SameValue(const SkinOffset &a, const SkinOffset &b) {
  return std::fabs(a.d[0] - b.d[0]) <= kSameSkinOffset &&
         std::fabs(a.d[1] - b.d[1]) <= kSameSkinOffset &&
         std::fabs(a.d[2] - b.d[2]) <= kSameSkinOffset;
}

bool SameValue(const MatrixOffset &a, const MatrixOffset &b) {
  for (int i = 0; i < 9; ++i) {
    if (std::fabs(a.rotation[i] - b.rotation[i]) > kSameOffset)
      return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(a.translation[i] - b.translation[i]) > kSameOffset)
      return false;
  }
  return true;
}

// What an insert did: a new key, a value merged into its record, a value
// that made the key ambiguous, a used record replaced, or nothing (the
// probe sequence is full)
enum class Insertion { kAdded, kMerged, kConflict, kRenewed, kFull };

// a is a later sequence number than b
bool Later(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

// A value table slot starts with the first record and merges the later ones
// of the same key; a conflict makes the key ambiguous
template <typename Value> void InitValue(Value *slot, const Value &value) {
  *slot = value;
}

template <typename Value>
Insertion MergeValue(Value *slot, const Value &value) {
  return SameValue(*slot, value) ? Insertion::kMerged : Insertion::kConflict;
}

void InitValue(VertexEntry *slot, const VertexCandidate &candidate) {
  slot->count = 1;
  slot->candidates[0] = candidate;
}

Insertion MergeValue(VertexEntry *slot, const VertexCandidate &candidate) {
  for (uint32_t i = 0; i < slot->count; ++i) {
    if (SameValue(slot->candidates[i].vertex, candidate.vertex)) {
      slot->candidates[i].last = candidate.last;
      return Insertion::kMerged;
    }
  }
  for (uint32_t i = 0; i < slot->count; ++i) {
    VertexCandidate &existing = slot->candidates[i];
    if (candidate.first - existing.last <= kIntegerSpread &&
        std::fabs(existing.vertex.w - candidate.vertex.w) <=
            existing.vertex.w * kSameSurfaceDepth) {
      existing.last = candidate.last;
      return Insertion::kMerged;
    }
  }
  if (slot->count == kCandidates)
    return Insertion::kConflict;
  slot->candidates[slot->count++] = candidate;
  return Insertion::kMerged;
}

void InitValue(SkinEntry *slot, const SkinCandidate &candidate) {
  slot->count = 1;
  slot->used = 0;
  slot->candidates[0] = candidate;
}

// A used candidate that gets a result again, or that a new fraction replaces
// when the key is full, is renewed: the result belongs to a later pass
Insertion MergeValue(SkinEntry *slot, const SkinCandidate &candidate) {
  for (uint32_t i = 0; i < slot->count; ++i) {
    SkinCandidate &existing = slot->candidates[i];
    if (!SameValue(existing.offset, candidate.offset))
      continue;
    const uint32_t bit = 1u << i;
    if (slot->used & bit) {
      slot->used &= ~bit;
      existing = candidate;
      return Insertion::kRenewed;
    }
    existing.last = candidate.last;
    return Insertion::kMerged;
  }
  if (slot->count < kCandidates) {
    slot->candidates[slot->count++] = candidate;
    return Insertion::kMerged;
  }
  if (!slot->used)
    return Insertion::kConflict;
  uint32_t oldest = kCandidates;
  for (uint32_t i = 0; i < slot->count; ++i) {
    if ((slot->used & (1u << i)) &&
        (oldest == kCandidates ||
         Later(slot->candidates[oldest].last, slot->candidates[i].last)))
      oldest = i;
  }
  slot->used &= ~(1u << oldest);
  slot->candidates[oldest] = candidate;
  return Insertion::kRenewed;
}

uint32_t HashKey(uint32_t key) { return key * 0x9E3779B1u; }

uint32_t HashKey(uint64_t key) {
  return static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 32);
}

template <typename Key> uint32_t HashWords(const Key &key) {
  uint32_t words[sizeof(Key) / 4];
  memcpy(words, &key, sizeof(words));
  uint64_t hash = 0;
  for (uint32_t word : words)
    hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
  return static_cast<uint32_t>(hash >> 32);
}

uint32_t HashKey(const MatrixKey &key) { return HashWords(key); }

uint32_t HashKey(const FaceKey &key) { return HashWords(key); }

enum class Lookup { kAbsent, kAmbiguous, kFound };

// Open-addressed records by value, one half per frame parity; a slot whose
// stamp is not the current frame is free
template <typename Key, typename Value, uint32_t kBits> class ValueTable {
public:
  // A record that a consumer has used (Use) is replaced by the next insert
  // of its key, whatever its value, instead of merged with it: that insert
  // is the next pass over the same data, which later lookups belong to
  template <typename Record>
  Insertion Insert(const Key &key, const Record &record, uint32_t epoch) {
    const uint32_t stamp = epoch & kStampEpoch;
    Slot *slots = slots_[stamp & 1];
    uint32_t index = Index(key);
    for (uint32_t probe = 0; probe < kTableProbes; ++probe) {
      Slot &slot = slots[index];
      if ((slot.stamp & kStampEpoch) != stamp) {
        slot.key = key;
        slot.stamp = stamp;
        InitValue(&slot.value, record);
        return Insertion::kAdded;
      }
      if (slot.key == key) {
        if (slot.stamp & kStampUsed) {
          slot.stamp = stamp;
          InitValue(&slot.value, record);
          return Insertion::kRenewed;
        }
        if (slot.stamp & kStampAmbiguous)
          return Insertion::kMerged;
        const Insertion result = MergeValue(&slot.value, record);
        if (result == Insertion::kConflict)
          slot.stamp |= kStampAmbiguous;
        return result;
      }
      index = (index + 1) & (kSize - 1);
    }
    return Insertion::kFull;
  }

  // The record of key from the frame epoch; it stays valid until the frame
  // after next
  Lookup FindIn(const Key &key, uint32_t epoch, const Value **out) const {
    const Slot *slot = Locate(key, epoch);
    if (!slot)
      return Lookup::kAbsent;
    if (slot->stamp & kStampAmbiguous)
      return Lookup::kAmbiguous;
    *out = &slot->value;
    return Lookup::kFound;
  }

  // The record of key from the frame epoch, else from the one before
  Lookup Find(const Key &key, uint32_t epoch, const Value **out) const {
    for (uint32_t age = 0; age < 2; ++age) {
      const Lookup result = FindIn(key, epoch - age, out);
      if (result != Lookup::kAbsent)
        return result;
    }
    return Lookup::kAbsent;
  }

  // Find for a consumer that marks what it takes inside the record
  Lookup Find(const Key &key, uint32_t epoch, Value **out) {
    const Value *value = nullptr;
    const Lookup result = Find(key, epoch, &value);
    *out = const_cast<Value *>(value);
    return result;
  }

  // Find for the consumer of the records: marks a record of the frame epoch
  // as used
  Lookup Use(const Key &key, uint32_t epoch, const Value **out) {
    Slot *slot = const_cast<Slot *>(Locate(key, epoch));
    if (!slot)
      return FindIn(key, epoch - 1, out);
    slot->stamp |= kStampUsed;
    if (slot->stamp & kStampAmbiguous)
      return Lookup::kAmbiguous;
    *out = &slot->value;
    return Lookup::kFound;
  }

private:
  static constexpr uint32_t kSize = 1u << kBits;
  struct Slot {
    Key key;
    uint32_t stamp;
    Value value;
  };
  static uint32_t Index(const Key &key) {
    return HashKey(key) >> (32 - kBits);
  }
  const Slot *Locate(const Key &key, uint32_t epoch) const {
    const uint32_t stamp = epoch & kStampEpoch;
    const Slot *slots = slots_[stamp & 1];
    uint32_t index = Index(key);
    for (uint32_t probe = 0; probe < kTableProbes; ++probe) {
      const Slot &slot = slots[index];
      if ((slot.stamp & kStampEpoch) != stamp)
        return nullptr;
      if (slot.key == key)
        return &slot;
      index = (index + 1) & (kSize - 1);
    }
    return nullptr;
  }
  Slot slots_[2][kSize];
};

typedef ValueTable<uint32_t, VertexEntry, 16> VertexTable;
typedef ValueTable<FaceKey, FaceRecord, 15> FaceTable;
typedef ValueTable<uint64_t, SkinEntry, 15> SkinTable;
typedef ValueTable<MatrixKey, MatrixOffset, 13> MatrixTable;

// Records of the frame being captured (emulation thread)
VertexTable g_fineTable;
VertexTable g_integerTable;
FaceTable g_fineFaces;
FaceTable g_integerFaces;
SkinTable g_skinTable;
MatrixTable g_matrixTable;

// Temporary: why corrections drop out, per polygon kind (emulation thread).
// Polygons count when at least one corner word has a record.
struct PolygonStats {
  uint32_t polygons;
  // All corners from their projection batch's record
  uint32_t faces;
  // All corners from word records
  uint32_t complete;
  // Some corners from word records
  uint32_t partial;
  // Left uncorrected
  uint32_t rejected;
  // Records taken from the previous frame
  uint32_t previous;
  // Some but not all corner words known in this frame, some in the previous
  uint32_t split;
  // Corners left uncorrected by reason
  uint32_t absent;
  uint32_t ambiguous;
  uint32_t ties;
  // Integer polygons whose corners were projected too far apart
  uint32_t spread;
  // Words that got a fifth position, batches with two different positions
  uint32_t crowdedWords;
  uint32_t faceConflicts;
};

// Temporary: counters printed once a second with pgxp_stats=1 (emulation
// thread)
struct DropoutStats {
  // OT walks, and frames (walks followed by GTE work)
  uint32_t walks;
  uint32_t frames;
  uint32_t projections;
  // Projections not matching RDE's own result (or behind the camera)
  uint32_t rejected;
  uint32_t tableFull;
  PolygonStats fine;
  PolygonStats integer;
  // Skin records: fractions that found a key full of unused ones (it turns
  // ambiguous), used fractions refreshed or replaced by a later pass,
  // projected vertices left integer because their key is ambiguous or its
  // fractions tie or lie too far in skinning order from the batch's others,
  // projected vertices whose key had several fractions and that took one by
  // skinning order, transforms a blend consumed, broken GPF/GPL chains
  uint32_t skinConflicts;
  uint32_t skinRenewed;
  uint32_t skinAmbiguous;
  uint32_t skinResolved;
  uint32_t skinConsumed;
  uint32_t blendsBroken;
  // Matrix records: compositions that did not recompute, accepted ones with
  // a saturated rotation entry, accepted ones with a large dropped part,
  // conflicts, used records replaced, GTE matrices whose key was ambiguous
  uint32_t matrixRejected;
  uint32_t matrixSaturated;
  uint32_t matrixLarge;
  uint32_t matrixConflicts;
  uint32_t matrixRenewed;
  uint32_t matrixAmbiguous;
};
DropoutStats g_stats = {};
bool g_statsEnabled = false;
ULONGLONG g_statsTime = 0;

// Counts an insert that failed for lack of room or made a key ambiguous
void CountInsertion(Insertion insertion, uint32_t *conflicts,
                    uint32_t *renewed) {
  switch (insertion) {
  case Insertion::kAdded:
  case Insertion::kMerged:
    break;
  case Insertion::kConflict:
    ++*conflicts;
    break;
  case Insertion::kRenewed:
    if (renewed)
      ++*renewed;
    break;
  case Insertion::kFull:
    ++g_stats.tableFull;
    break;
  }
}

void PrintPolygonStats(const char *kind, const PolygonStats &stats) {
  std::cout << "[PGXP] " << kind << ": polygons " << stats.polygons
            << " faces " << stats.faces << " words " << stats.complete
            << " partial " << stats.partial << " rejected " << stats.rejected
            << " previous " << stats.previous << " split " << stats.split
            << " | corners absent "
            << stats.absent << " ambiguous " << stats.ambiguous << " ties "
            << stats.ties << " spread " << stats.spread
            << " | crowded words " << stats.crowdedWords
            << " face conflicts " << stats.faceConflicts << std::endl;
}

// Once a second while pgxp_stats is set
void ReportStats() {
  if (!g_statsEnabled)
    return;
  const ULONGLONG now = GetTickCount64();
  if (now - g_statsTime < 1000)
    return;
  g_statsTime = now;
  const DropoutStats &stats = g_stats;
  std::cout << "[PGXP] walks " << stats.walks << " frames " << stats.frames
            << " projections " << stats.projections << " rejected "
            << stats.rejected << " table full " << stats.tableFull
            << std::endl;
  PrintPolygonStats("fine", stats.fine);
  PrintPolygonStats("integer", stats.integer);
  std::cout << "[PGXP] skin: conflicts " << stats.skinConflicts << " renewed "
            << stats.skinRenewed << " ambiguous " << stats.skinAmbiguous
            << " by sequence " << stats.skinResolved << " consumed "
            << stats.skinConsumed << " broken blends " << stats.blendsBroken
            << " | matrix: rejected " << stats.matrixRejected << " saturated "
            << stats.matrixSaturated << " large " << stats.matrixLarge
            << " conflicts " << stats.matrixConflicts << " renewed "
            << stats.matrixRenewed << " ambiguous " << stats.matrixAmbiguous
            << std::endl;
  g_stats = {};
}
// Frame number of the records being captured; slots of other frames are free
uint32_t g_epoch = 2;
bool g_walkSinceCapture = false;
// Number of the next projected vertex
uint32_t g_sequence = 0;
// Number of the next skinning result (transform or blend)
uint32_t g_skinSequence = 0;

// Skinning results not yet recorded (emulation thread), the oldest at begin:
// a blend consumes the latest one of each point it reads. The skinners blend
// at most 16 vertices from 32 transforms.
struct PendingResult {
  uint64_t key;
  SkinCandidate candidate;
  bool consumed;
};
constexpr uint32_t kPendingResults = 64;
struct PendingResults {
  uint32_t begin;
  uint32_t count;
  PendingResult items[kPendingResults];
};
PendingResults g_pending = {};

// Skinning numbers of the fractions the last projection batch took, for a
// batch with a single skinned vertex (emulation thread)
struct SkinAnchor {
  bool valid;
  uint32_t first;
  uint32_t last;
};
SkinAnchor g_skinAnchor = {};

// The record of the matrix last looked up for the GTE (emulation thread);
// generation changes with every matrix record
struct MatrixLookup {
  uint32_t epoch;
  uint32_t generation;
  MatrixKey key;
  bool found;
  MatrixOffset offset;
};
MatrixLookup g_matrixLookup = {};
uint32_t g_matrixGeneration = 1;

// Precise positions of the GTE's SXY0..SXY2 (emulation thread)
struct ShadowVertex {
  uint32_t word;
  float x;
  float y;
  bool valid;
};
ShadowVertex g_shadowFifo[3] = {};

struct ProjectionSetup {
  double rotation[9];
  double translation[3];
  double offsetX;
  double offsetY;
  double h;
  double ratio;
  double irMin;
};

struct Projection {
  double x;
  double y;
  double z;
};

// GPF/GPL chain of the skinner's two-joint blend (emulation thread): the
// MAC1..3 the last GPF or GPL left, its value from the integer points, its
// precise value, and the number of its queued result
struct BlendChain {
  bool valid;
  bool precise;
  bool queued;
  uint32_t result;
  int32_t mac[3];
  double integer[3];
  double value[3];
};
BlendChain g_blendChain = {};

// GTE inputs of a GPF/GPL, read before it runs
struct BlendInput {
  int32_t weight;
  int32_t vector[3];
  int32_t accumulator[3];
};

struct VectorU32 {
  uint32_t *begin;
  uint32_t *end;
  uint32_t *capacity;
};

struct PolygonRecords {
  uint32_t command;
  uint32_t count;
  bool perspective;
  uint32_t words[kMaxCorners];
  PreciseVertex vertices[kMaxCorners];
};

typedef uint32_t(__thiscall *GteExecCommand_t)(void *gte, uint32_t command);
typedef void(__fastcall *NclipFine_t)(uint8_t *gte);
typedef void(__thiscall *VecInsert_t)(VectorU32 *vector, uint32_t *where,
                                      const uint32_t *first,
                                      const uint32_t *last, uint32_t tag);
typedef uint32_t(__thiscall *GpuCommand_t)(void *gpu, const uint32_t *words,
                                           uint32_t remaining);
typedef uint32_t(__thiscall *GuestRead32_t)(void *region, uint32_t address);

// Guest memory region objects (emulation thread): vtable slot of read32 and
// the region size
constexpr uint32_t kRegionRead32Slot = 3;
constexpr uint32_t kRegionShift = 19;

uintptr_t g_moduleBase = 0;
// Switched by F6 on the emulation thread
volatile bool g_enabled = true;
bool g_subpixelInstalled = false;
bool g_preciseInstalled = false;
volatile bool g_preciseActive = false;

bool g_hotkeyDown = false;
ULONGLONG g_hotkeyPollTime = 0;

GteExecCommand_t g_gteExecCommand = nullptr;
NclipFine_t g_nclipFineOriginal = nullptr;
VecInsert_t g_vecInsert = nullptr;
GpuCommand_t g_gpuCommandOriginal[8] = {};

uint32_t g_scratch[kScratchWords];

// Render thread: records armed by the private command
PolygonRecords g_armedRecords = {};
bool g_recordsArmed = false;

void UpdateActiveParts();

// F6 switches PGXP on and off while the game window has the focus
void PollHotkey() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_hotkeyPollTime < kHotkeyPollMs)
    return;
  g_hotkeyPollTime = now;

  const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
  const bool pressed = down && !g_hotkeyDown;
  g_hotkeyDown = down;
  if (!pressed)
    return;
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  if (process != GetCurrentProcessId())
    return;
  g_enabled = !g_enabled;
  UpdateActiveParts();
  std::cout << "[PGXP] " << (g_enabled ? "on" : "off") << std::endl;
}

// Records the oldest pending result unless a blend consumed it
void RecordOldestResult() {
  const PendingResult &item = g_pending.items[g_pending.begin];
  g_pending.begin = (g_pending.begin + 1) % kPendingResults;
  --g_pending.count;
  if (item.consumed)
    return;
  CountInsertion(g_skinTable.Insert(item.key, item.candidate, g_epoch),
                 &g_stats.skinConflicts, &g_stats.skinRenewed);
}

void RecordPendingResults() {
  while (g_pending.count)
    RecordOldestResult();
}

// Queues a skinning result under its integer point; returns its number
uint32_t QueueResult(uint64_t key, const SkinOffset &offset) {
  if (g_pending.count == kPendingResults)
    RecordOldestResult();
  const uint32_t sequence = g_skinSequence++;
  g_pending.items[(g_pending.begin + g_pending.count) % kPendingResults] = {
      key, {offset, sequence, sequence}, false};
  ++g_pending.count;
  return sequence;
}

// The first capture after an OT walk starts a new frame
void BeginCapture() {
  if (g_walkSinceCapture) {
    g_walkSinceCapture = false;
    RecordPendingResults();
    ++g_epoch;
    ++g_stats.frames;
  }
}

void PushShadow(uint32_t word, float x, float y, bool valid) {
  g_shadowFifo[0] = g_shadowFifo[1];
  g_shadowFifo[1] = g_shadowFifo[2];
  g_shadowFifo[2] = {word, x, y, valid};
}

template <typename T> T GteRead(const uint8_t *gte, uintptr_t offset) {
  return *reinterpret_cast<const T *>(gte + offset);
}

uint64_t SkinKey(int32_t x, int32_t y, int32_t z) {
  return static_cast<uint64_t>(static_cast<uint16_t>(x)) |
         (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16) |
         (static_cast<uint64_t>(static_cast<uint16_t>(z)) << 32);
}

bool FitsInt16(int32_t value) { return value >= -32768 && value <= 32767; }

MatrixKey GteMatrixKey(const uint8_t *gte) {
  MatrixKey key = {};
  for (int i = 0; i < 9; ++i)
    key.rotation[i] = GteRead<int16_t>(gte, rde::gte_rotation + i * 2);
  for (int i = 0; i < 3; ++i)
    key.translation[i] =
        GteRead<int32_t>(gte, rde::gte_translation + i * kGteWordStride);
  return key;
}

// The composition record of the GTE's rotation and translation, or nullptr
const MatrixOffset *GteMatrixOffset(const uint8_t *gte) {
  const MatrixKey key = GteMatrixKey(gte);
  MatrixLookup &lookup = g_matrixLookup;
  if (lookup.epoch != g_epoch || lookup.generation != g_matrixGeneration ||
      !(lookup.key == key)) {
    lookup.epoch = g_epoch;
    lookup.generation = g_matrixGeneration;
    lookup.key = key;
    const MatrixOffset *offset = nullptr;
    const Lookup result = g_matrixTable.Use(key, g_epoch, &offset);
    lookup.found = result == Lookup::kFound;
    if (lookup.found)
      lookup.offset = *offset;
    else if (result == Lookup::kAmbiguous)
      ++g_stats.matrixAmbiguous;
  }
  return lookup.found ? &lookup.offset : nullptr;
}

ProjectionSetup ReadProjectionSetup(const uint8_t *gte, uint32_t command) {
  ProjectionSetup setup;
  for (int i = 0; i < 9; ++i)
    setup.rotation[i] = GteRead<int16_t>(gte, rde::gte_rotation + i * 2);
  for (int i = 0; i < 3; ++i)
    setup.translation[i] =
        GteRead<int32_t>(gte, rde::gte_translation + i * kGteWordStride);
  setup.offsetX = GteRead<int32_t>(gte, rde::gte_ofx) / 65536.0;
  setup.offsetY = GteRead<int32_t>(gte, rde::gte_ofy) / 65536.0;
  setup.h = GteRead<uint16_t>(gte, rde::gte_h);
  setup.ratio = GetCurrentWidescreenRatio();
  setup.irMin = (command & kGteLimitFlag) ? 0.0 : -32768.0;
  return setup;
}

// setup with the precise matrix when the GTE's matrix has a composition
// record; false otherwise
bool PreciseMatrixSetup(const uint8_t *gte, const ProjectionSetup &setup,
                        ProjectionSetup *out) {
  const MatrixOffset *offset = GteMatrixOffset(gte);
  if (!offset)
    return false;
  *out = setup;
  for (int i = 0; i < 9; ++i)
    out->rotation[i] += offset->rotation[i];
  for (int i = 0; i < 3; ++i)
    out->translation[i] += offset->translation[i];
  return true;
}

// The projection of a model point; false behind the camera. Products of the
// 16-bit inputs and their sums stay exact in double.
bool Project(const ProjectionSetup &setup, double vx, double vy, double vz,
             Projection *out) {
  const double *r = setup.rotation;
  const double mac1 =
      setup.translation[0] * 4096.0 + r[0] * vx + r[1] * vy + r[2] * vz;
  const double mac2 =
      setup.translation[1] * 4096.0 + r[3] * vx + r[4] * vy + r[5] * vz;
  const double mac3 =
      setup.translation[2] * 4096.0 + r[6] * vx + r[7] * vy + r[8] * vz;
  const double ir1 = std::clamp(mac1 / 4096.0, setup.irMin, 32767.0);
  const double ir2 = std::clamp(mac2 / 4096.0, setup.irMin, 32767.0);
  const double z =
      (std::max)((std::min)(mac3 / 4096.0, 65535.0), setup.h * 0.5);
  if (!(z > 0.0))
    return false;
  const double scale = setup.h / z;
  out->x = setup.offsetX + setup.ratio * ir1 * scale;
  out->y = setup.offsetY + ir2 * scale;
  out->z = z;
  return true;
}

// What one projection recorded under each of its two words
struct CapturedVertex {
  bool fineValid;
  bool integerValid;
  uint32_t fineWord;
  uint32_t integerWord;
  PreciseVertex fine;
  PreciseVertex integer;
};

// One projected vertex. The projection of the integer vertex must agree with
// what RDE computed (the integer projection error is at most about
// (H + offset from the centre) / z pixels); the stored position is that of
// the precise skinned point when the vertex has a skin fraction (skin),
// projected with preciseSetup when the GTE matrix has a composition record.
void CaptureVertex(const uint8_t *gte, const ProjectionSetup &setup,
                   const ProjectionSetup *preciseSetup, int vertexIndex,
                   const SkinOffset *skin, uint32_t sxyWord, uint32_t fineWord,
                   uint32_t sequence, CapturedVertex *out) {
  out->fineValid = false;
  out->integerValid = false;
  ++g_stats.projections;
  const int16_t *vertex = reinterpret_cast<const int16_t *>(
      gte + rde::gte_vector0 + vertexIndex * kGteVectorStride);
  Projection base;
  if (!Project(setup, vertex[0], vertex[1], vertex[2], &base)) {
    ++g_stats.rejected;
    PushShadow(sxyWord, 0.0f, 0.0f, false);
    return;
  }

  const double tolerance =
      1.25 + (setup.h + std::fabs(base.x - setup.offsetX) +
              std::fabs(base.y - setup.offsetY)) /
                 base.z;
  const double fineX = static_cast<int16_t>(fineWord & 0xFFFF) / 8.0;
  const double fineY = static_cast<int16_t>(fineWord >> 16) / 8.0;
  const bool fineValid = std::fabs(base.x - fineX) <= tolerance &&
                         std::fabs(base.y - fineY) <= tolerance;
  const double screenX = static_cast<int16_t>(sxyWord & 0xFFFF);
  const double screenY = static_cast<int16_t>(sxyWord >> 16);
  const double baseX = std::clamp(base.x, -1024.0, 1023.0);
  const double baseY = std::clamp(base.y, -1024.0, 1023.0);
  const bool integerValid = std::fabs(baseX - screenX) <= tolerance + 1.0 &&
                            std::fabs(baseY - screenY) <= tolerance + 1.0;
  if (!fineValid && !integerValid) {
    ++g_stats.rejected;
    PushShadow(sxyWord, 0.0f, 0.0f, false);
    return;
  }

  double point[3] = {static_cast<double>(vertex[0]),
                     static_cast<double>(vertex[1]),
                     static_cast<double>(vertex[2])};
  if (skin) {
    for (int i = 0; i < 3; ++i)
      point[i] += skin->d[i];
  }
  Projection precise = base;
  if (skin || preciseSetup)
    Project(preciseSetup ? *preciseSetup : setup, point[0], point[1], point[2],
            &precise);

  const float w = static_cast<float>(precise.z);
  const VertexCandidate fine = {
      {static_cast<float>(precise.x), static_cast<float>(precise.y), w},
      sequence,
      sequence};
  if (fineValid)
    CountInsertion(g_fineTable.Insert(fineWord, fine, g_epoch),
                   &g_stats.fine.crowdedWords, nullptr);

  const float clampedX = static_cast<float>(std::clamp(precise.x, -1024.0, 1023.0));
  const float clampedY = static_cast<float>(std::clamp(precise.y, -1024.0, 1023.0));
  const VertexCandidate integer = {{clampedX, clampedY, w}, sequence, sequence};
  if (integerValid)
    CountInsertion(g_integerTable.Insert(sxyWord, integer, g_epoch),
                   &g_stats.integer.crowdedWords, nullptr);
  PushShadow(sxyWord, clampedX, clampedY, integerValid);
  *out = {fineValid, integerValid, fineWord, sxyWord, fine.vertex,
          integer.vertex};
}

// The batch key of the corner words, and for each of its words the corner it
// came from
FaceKey MakeFaceKey(const uint32_t *words, uint32_t corners, uint32_t *order) {
  FaceKey key = {corners};
  for (uint32_t i = 0; i < corners; ++i) {
    uint32_t at = i;
    while (at > 0 && key.words[at - 1] > words[i]) {
      key.words[at] = key.words[at - 1];
      order[at] = order[at - 1];
      --at;
    }
    key.words[at] = words[i];
    order[at] = i;
  }
  return key;
}

// Records the positions of one projection batch under its corner words
void InsertFace(FaceTable *table, const uint32_t *words,
                const PreciseVertex *vertices, uint32_t corners,
                uint32_t *conflicts) {
  uint32_t order[kMaxCorners];
  const FaceKey key = MakeFaceKey(words, corners, order);
  FaceRecord record = {};
  for (uint32_t i = 0; i < corners; ++i)
    record.vertices[i] = vertices[order[i]];
  CountInsertion(table->Insert(key, record, g_epoch), conflicts, nullptr);
}

// Face records of a batch whose corners all have a record of that kind
void InsertFaces(const CapturedVertex *captured, uint32_t corners) {
  bool fine = true;
  bool integer = true;
  uint32_t fineWords[kMaxCorners];
  uint32_t integerWords[kMaxCorners];
  PreciseVertex fineVertices[kMaxCorners];
  PreciseVertex integerVertices[kMaxCorners];
  for (uint32_t i = 0; i < corners; ++i) {
    fine = fine && captured[i].fineValid;
    integer = integer && captured[i].integerValid;
    fineWords[i] = captured[i].fineWord;
    integerWords[i] = captured[i].integerWord;
    fineVertices[i] = captured[i].fine;
    integerVertices[i] = captured[i].integer;
  }
  if (fine)
    InsertFace(&g_fineFaces, fineWords, fineVertices, corners,
               &g_stats.fine.faceConflicts);
  if (integer)
    InsertFace(&g_integerFaces, integerWords, integerVertices, corners,
               &g_stats.integer.faceConflicts);
}

// The last RTPT (emulation thread): an RTPS right after it (the next
// sequence number) is the fourth corner of a quad
struct ProjectionBatch {
  bool valid;
  uint32_t next;
  CapturedVertex vertices[3];
};
ProjectionBatch g_lastBatch = {};

// Face records of a projection batch: an RTPT is a triangle, and with the
// RTPS that follows it a quad
void CaptureFaces(const CapturedVertex *captured, int count,
                  uint32_t sequence) {
  if (count == 3) {
    InsertFaces(captured, 3);
    g_lastBatch.valid = true;
    g_lastBatch.next = sequence + 3;
    memcpy(g_lastBatch.vertices, captured, sizeof(g_lastBatch.vertices));
    return;
  }
  if (g_lastBatch.valid && g_lastBatch.next == sequence) {
    CapturedVertex quad[kMaxCorners];
    memcpy(quad, g_lastBatch.vertices, sizeof(g_lastBatch.vertices));
    quad[3] = captured[0];
    InsertFaces(quad, kMaxCorners);
  }
  g_lastBatch.valid = false;
}

// How far apart two ranges of sequence numbers lie; 0 when they overlap
uint32_t RangeDistance(uint32_t firstA, uint32_t lastA, uint32_t firstB,
                       uint32_t lastB) {
  if (Later(firstB, lastA))
    return firstB - lastA;
  if (Later(firstA, lastB))
    return firstA - lastB;
  return 0;
}

// Picks a fraction for each skinned vertex of a batch (choice) whose point
// has several: the combination in which the vertices lie closest in skinning
// order to their nearest other skinned vertex (summed over the batch), or,
// for a single skinned vertex, the fraction closest to the anchor. Returns
// the vertices (bit i for entries[i]) left without a pick: equally close
// combinations disagree on them, or their pick lies more than kSkinSpread
// from the others.
uint32_t ChooseSkin(const SkinEntry *const *entries, uint32_t count,
                    uint32_t *choice) {
  uint32_t several = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (entries[i]->count > 1)
      several |= 1u << i;
  }
  if (count == 1) {
    if (!g_skinAnchor.valid)
      return several;
    uint32_t best = 0;
    bool tied = false;
    for (uint32_t c = 0; c < entries[0]->count; ++c) {
      const SkinCandidate &candidate = entries[0]->candidates[c];
      const uint32_t distance =
          RangeDistance(candidate.first, candidate.last, g_skinAnchor.first,
                        g_skinAnchor.last);
      if (c == 0 || distance < best) {
        best = distance;
        tied = false;
        choice[0] = c;
      } else if (distance == best) {
        tied = true;
      }
    }
    return tied || best > kSkinSpread ? several : 0;
  }

  uint32_t combinations = 1;
  for (uint32_t i = 0; i < count; ++i)
    combinations *= entries[i]->count;
  uint32_t bestScore = 0;
  uint32_t bestNearest[3] = {};
  uint32_t disputed = 0;
  for (uint32_t combination = 0; combination < combinations; ++combination) {
    uint32_t pick[3];
    uint32_t rest = combination;
    for (uint32_t i = 0; i < count; ++i) {
      pick[i] = rest % entries[i]->count;
      rest /= entries[i]->count;
    }
    uint32_t score = 0;
    uint32_t nearest[3];
    for (uint32_t i = 0; i < count; ++i) {
      const SkinCandidate &a = entries[i]->candidates[pick[i]];
      nearest[i] = UINT32_MAX;
      for (uint32_t j = 0; j < count; ++j) {
        if (j == i)
          continue;
        const SkinCandidate &b = entries[j]->candidates[pick[j]];
        nearest[i] = (std::min)(
            nearest[i], RangeDistance(a.first, a.last, b.first, b.last));
      }
      score += (std::min)(nearest[i], kSkinSpread + 1);
    }
    if (combination == 0 || score < bestScore) {
      bestScore = score;
      disputed = 0;
      memcpy(choice, pick, count * sizeof(pick[0]));
      memcpy(bestNearest, nearest, count * sizeof(nearest[0]));
    } else if (score == bestScore) {
      for (uint32_t i = 0; i < count; ++i) {
        if (pick[i] != choice[i])
          disputed |= 1u << i;
      }
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (bestNearest[i] > kSkinSpread)
      disputed |= several & (1u << i);
  }
  return disputed;
}

// Skin fractions of a projection batch's input vertices (out[i], nullptr
// without one): a vertex whose point has one fraction takes it, one whose
// point has several takes the pick of ChooseSkin. Marks the fractions taken
// as used and makes them the anchor.
void ResolveSkin(const uint8_t *gte, int count, const SkinOffset **out) {
  SkinEntry *entries[3];
  int vertices[3];
  uint32_t found = 0;
  bool several = false;
  for (int i = 0; i < count; ++i) {
    out[i] = nullptr;
    const int16_t *vertex = reinterpret_cast<const int16_t *>(
        gte + rde::gte_vector0 + i * kGteVectorStride);
    SkinEntry *entry = nullptr;
    const Lookup result = g_skinTable.Find(
        SkinKey(vertex[0], vertex[1], vertex[2]), g_epoch, &entry);
    if (result == Lookup::kAmbiguous)
      ++g_stats.skinAmbiguous;
    if (result != Lookup::kFound)
      continue;
    entries[found] = entry;
    vertices[found] = i;
    ++found;
    several = several || entry->count > 1;
  }
  uint32_t choice[3] = {};
  const uint32_t left = several ? ChooseSkin(entries, found, choice) : 0;
  SkinAnchor anchor = {};
  for (uint32_t i = 0; i < found; ++i) {
    SkinEntry &entry = *entries[i];
    if (left & (1u << i)) {
      ++g_stats.skinAmbiguous;
      continue;
    }
    if (entry.count > 1)
      ++g_stats.skinResolved;
    entry.used |= 1u << choice[i];
    const SkinCandidate &candidate = entry.candidates[choice[i]];
    out[vertices[i]] = &candidate.offset;
    if (!anchor.valid || Later(anchor.first, candidate.first))
      anchor.first = candidate.first;
    if (!anchor.valid || Later(candidate.last, anchor.last))
      anchor.last = candidate.last;
    anchor.valid = true;
  }
  if (anchor.valid)
    g_skinAnchor = anchor;
}

// RTPS (count 1) or RTPT (count 3) has just run
void CaptureProjections(const uint8_t *gte, uint32_t command, int count) {
  BeginCapture();
  RecordPendingResults();
  const uint32_t sequence = g_sequence;
  g_sequence += count;
  if (!(command & kGteShiftFlag)) {
    g_lastBatch.valid = false;
    for (int i = 0; i < count; ++i)
      PushShadow(0, 0.0f, 0.0f, false);
    return;
  }

  const ProjectionSetup setup = ReadProjectionSetup(gte, command);
  ProjectionSetup matrixSetup;
  const ProjectionSetup *preciseSetup =
      PreciseMatrixSetup(gte, setup, &matrixSetup) ? &matrixSetup : nullptr;
  const SkinOffset *skin[3];
  ResolveSkin(gte, count, skin);
  const uint32_t ringCount = GteRead<uint32_t>(gte, rde::gte_fine_ring_count);
  CapturedVertex captured[3];
  for (int i = 0; i < count; ++i) {
    const uintptr_t sxy =
        rde::gte_sxy0 + (count == 1 ? 2 : i) * kGteWordStride;
    const uint32_t ringIndex = (ringCount - (count - 1) + i) & 3;
    CaptureVertex(gte, setup, preciseSetup, i, skin[i],
                  GteRead<uint32_t>(gte, sxy),
                  GteRead<uint32_t>(gte, rde::gte_fine_ring + ringIndex * 4),
                  sequence + i, &captured[i]);
  }
  CaptureFaces(captured, count, sequence);
}

// MVMVA rotation * V0..V2 + translation with the 12-bit shift has just run:
// queue what each MAC dropped (with the precise matrix when it has a
// composition record), keyed by the integer point
void CaptureTransform(const uint8_t *gte, uint32_t command) {
  const uint32_t vectorIndex = (command >> kGteMvmvaVectorShift) & 3;
  if ((command & kGteMvmvaMatrixAndTranslation) || !(command & kGteShiftFlag) ||
      vectorIndex == kGteMvmvaVectorIr)
    return;
  BeginCapture();

  const int16_t *vertex = reinterpret_cast<const int16_t *>(
      gte + rde::gte_vector0 + vectorIndex * kGteVectorStride);
  const MatrixOffset *matrix = GteMatrixOffset(gte);
  int32_t point[3];
  SkinOffset offset;
  for (int row = 0; row < 3; ++row) {
    const int16_t *r = reinterpret_cast<const int16_t *>(
        gte + rde::gte_rotation + row * 6);
    const int64_t raw =
        static_cast<int64_t>(
            GteRead<int32_t>(gte, rde::gte_translation + row * kGteWordStride)) *
            4096 +
        static_cast<int64_t>(r[0]) * vertex[0] +
        static_cast<int64_t>(r[1]) * vertex[1] +
        static_cast<int64_t>(r[2]) * vertex[2];
    const int32_t mac =
        GteRead<int32_t>(gte, rde::gte_mac1 + row * kGteWordStride);
    if ((raw >> 12) != mac || !FitsInt16(mac))
      return;
    point[row] = mac;
    double precise = static_cast<double>(raw);
    if (matrix) {
      precise += matrix->translation[row] * 4096.0;
      for (int i = 0; i < 3; ++i)
        precise += matrix->rotation[row * 3 + i] * vertex[i];
    }
    offset.d[row] = static_cast<float>(precise / 4096.0 - mac);
  }
  QueueResult(SkinKey(point[0], point[1], point[2]), offset);
}

// The fraction of the latest pending result of the point a blend reads,
// which the blend consumes; nullptr when there is none
const SkinOffset *ConsumeResult(const int32_t *point) {
  const uint64_t key = SkinKey(point[0], point[1], point[2]);
  for (uint32_t i = g_pending.count; i-- > 0;) {
    PendingResult &item =
        g_pending.items[(g_pending.begin + i) % kPendingResults];
    if (item.consumed || item.key != key)
      continue;
    item.consumed = true;
    ++g_stats.skinConsumed;
    return &item.candidate.offset;
  }
  return nullptr;
}

// Drops the pending partial sum of a blend chain that continues
void RetractResult(uint32_t sequence) {
  for (uint32_t i = g_pending.count; i-- > 0;) {
    PendingResult &item =
        g_pending.items[(g_pending.begin + i) % kPendingResults];
    if (item.candidate.first == sequence) {
      item.consumed = true;
      return;
    }
  }
}

BlendInput ReadBlendInput(const uint8_t *gte) {
  BlendInput input;
  input.weight = GteRead<int16_t>(gte, rde::gte_ir0);
  for (int i = 0; i < 3; ++i) {
    input.vector[i] = GteRead<int16_t>(gte, rde::gte_ir1 + i * kGteWordStride);
    input.accumulator[i] =
        GteRead<int32_t>(gte, rde::gte_mac1 + i * kGteWordStride);
  }
  return input;
}

// GPF (start) or GPL (accumulate) with the 12-bit shift has just run on
// input: weight * point / 4096 summed over a chain. Each point consumes its
// pending transform; the sum of a chain with such a transform among its
// points is recorded as a skinning result.
void CaptureBlend(const uint8_t *gte, uint32_t operation,
                  const BlendInput &input) {
  BeginCapture();
  BlendChain chain = {};
  if (operation == kGteGpl) {
    if (!g_blendChain.valid ||
        memcmp(g_blendChain.mac, input.accumulator, sizeof(input.accumulator))) {
      g_blendChain.valid = false;
      ++g_stats.blendsBroken;
      return;
    }
    chain = g_blendChain;
    if (chain.queued)
      RetractResult(chain.result);
    chain.queued = false;
  }

  static const SkinOffset kNoOffset = {};
  const SkinOffset *offset = ConsumeResult(input.vector);
  if (offset)
    chain.precise = true;
  else
    offset = &kNoOffset;

  bool fits = true;
  for (int i = 0; i < 3; ++i) {
    chain.integer[i] += input.weight * input.vector[i] / 4096.0;
    chain.value[i] +=
        input.weight * (input.vector[i] + offset->d[i]) / 4096.0;
    chain.mac[i] = GteRead<int32_t>(gte, rde::gte_mac1 + i * kGteWordStride);
    if (std::fabs(chain.integer[i] - chain.mac[i]) > 4.0) {
      g_blendChain.valid = false;
      ++g_stats.blendsBroken;
      return;
    }
    fits = fits && FitsInt16(chain.mac[i]);
  }
  chain.valid = true;
  if (chain.precise && fits) {
    SkinOffset blended;
    for (int i = 0; i < 3; ++i)
      blended.d[i] = static_cast<float>(chain.value[i] - chain.mac[i]);
    chain.queued = true;
    chain.result =
        QueueResult(SkinKey(chain.mac[0], chain.mac[1], chain.mac[2]), blended);
  }
  g_blendChain = chain;
}

// MAC0 from the precise SXY0..SXY2 when all three have one; a result that
// would truncate to 0 keeps RDE's own value
void CorrectNclip(uint8_t *gte) {
  double x[3];
  double y[3];
  for (int i = 0; i < 3; ++i) {
    const uint32_t word = GteRead<uint32_t>(gte, rde::gte_sxy0 + i * kGteWordStride);
    const ShadowVertex &shadow = g_shadowFifo[i];
    const VertexEntry *entry = nullptr;
    if (shadow.valid && shadow.word == word) {
      x[i] = shadow.x;
      y[i] = shadow.y;
    } else if (g_integerTable.Find(word, g_epoch, &entry) == Lookup::kFound &&
               entry->count == 1) {
      x[i] = entry->candidates[0].vertex.x;
      y[i] = entry->candidates[0].vertex.y;
    } else {
      return;
    }
  }

  double area = x[0] * (y[1] - y[2]) + x[1] * (y[2] - y[0]) +
                x[2] * (y[0] - y[1]);
  const double magnitude = std::fabs(area);
  if (magnitude > 0.1 && magnitude < 1.0)
    area += area < 0.0 ? -1.0 : 1.0;
  if (std::fabs(area) < 1.0)
    return;
  area = std::clamp(area, -2147483648.0, 2147483647.0);
  *reinterpret_cast<int32_t *>(gte + rde::gte_mac0) = static_cast<int32_t>(area);
}

uint32_t FineToPolygon(uint32_t word) {
  return ((word >> 1) & kGp0Gouraud) + (word & 0x0FFFFFFF) + 0x20000000;
}

bool IsPolygon(uint32_t word) {
  const uint32_t range = word >> 29;
  return range == 1 || (range >= 6 && (word & kGp0FineFlag));
}

uint32_t PolygonLength(uint32_t polygon) {
  const uint32_t corners = (polygon & kGp0Quad) ? 4 : 3;
  const uint32_t perCorner = (polygon & kGp0Textured) ? 2 : 1;
  return 1 + corners * perCorner + ((polygon & kGp0Gouraud) ? corners - 1 : 0);
}

uint32_t PolylineLength(const uint32_t *words, uint32_t available,
                        uint32_t perVertex) {
  uint32_t length = 2 + perVertex;
  while (length < available) {
    const uint32_t word = words[length++];
    if ((word & kGp0PolylineEndMask) == kGp0PolylineEnd)
      return length;
    length += perVertex - 1;
  }
  return 0;
}

// Words of the GP0 command at words, as RDE's GP0 handlers consume them; 0
// when unknown (commands with inline data) or not complete in the packet
uint32_t Gp0CommandLength(const uint32_t *words, uint32_t available) {
  const uint32_t word = words[0];
  uint32_t length = 0;
  switch (word >> 29) {
  case 0:
    length = (word >> 24) == kGp0FillCommand ? 3 : 1;
    break;
  case 1:
    length = PolygonLength(word);
    break;
  case 2: {
    const uint32_t perVertex = (word & kGp0Gouraud) ? 2 : 1;
    if (word & kGp0PolylineFlag)
      return PolylineLength(words, available, perVertex);
    length = 1 + 2 * perVertex;
    break;
  }
  case 3:
    length = 2 + ((word & kGp0Textured) ? 1 : 0) +
             ((word & kGp0RectSizeMask) ? 0 : 1);
    break;
  case 4:
    length = 4;
    break;
  case 6:
    if (!(word & kGp0FineFlag))
      return 0;
    length = PolygonLength(FineToPolygon(word));
    break;
  case 7:
    length = (word & kGp0FineFlag) ? PolygonLength(FineToPolygon(word)) : 1;
    break;
  default:
    return 0;
  }
  return length <= available ? length : 0;
}

void StoreFloat(uint32_t *out, float value) {
  memcpy(out, &value, sizeof(value));
}

float LoadFloat(const uint32_t *in) {
  float value;
  memcpy(&value, in, sizeof(value));
  return value;
}

// Picks one candidate per corner so that their projections lie as close
// together as possible; spread = how far apart they are then. Returns the
// corners (bit i for corner i) on which equally close picks disagree.
uint32_t ChooseCandidates(const VertexEntry *const *entries, uint32_t count,
                          uint32_t *choice, uint32_t *spread) {
  uint32_t combinations = 1;
  for (uint32_t i = 0; i < count; ++i)
    combinations *= entries[i]->count;
  uint32_t disputed = 0;
  for (uint32_t combination = 0; combination < combinations; ++combination) {
    uint32_t pick[kMaxCorners];
    uint32_t rest = combination;
    uint32_t latestFirst = 0;
    uint32_t earliestLast = 0;
    for (uint32_t i = 0; i < count; ++i) {
      pick[i] = rest % entries[i]->count;
      rest /= entries[i]->count;
      const VertexCandidate &candidate = entries[i]->candidates[pick[i]];
      if (i == 0 || Later(candidate.first, latestFirst))
        latestFirst = candidate.first;
      if (i == 0 || Later(earliestLast, candidate.last))
        earliestLast = candidate.last;
    }
    const uint32_t distance =
        Later(latestFirst, earliestLast) ? latestFirst - earliestLast : 0;
    if (combination == 0 || distance < *spread) {
      *spread = distance;
      disputed = 0;
      memcpy(choice, pick, count * sizeof(pick[0]));
    } else if (distance == *spread) {
      for (uint32_t i = 0; i < count; ++i) {
        if (pick[i] != choice[i])
          disputed |= 1u << i;
      }
    }
  }
  return disputed;
}

// Every corner word is among the recorded words
bool CoversCorners(const uint32_t *cornerWords, uint32_t corners,
                   const uint32_t *words, uint32_t count) {
  for (uint32_t i = 0; i < corners; ++i) {
    if (std::find(words, words + count, cornerWords[i]) == words + count)
      return false;
  }
  return true;
}

// Positions for the private command, one per distinct corner word
struct MarkerRecords {
  uint32_t count;
  uint32_t words[kMaxCorners];
  PreciseVertex vertices[kMaxCorners];
};

// The record of the projection batch whose words are exactly the polygon's
// corner words, from the current frame or else the previous one; false when
// there is none or it is ambiguous
bool FindFace(const FaceTable &table, const uint32_t *cornerWords,
              uint32_t corners, PolygonStats *stats, MarkerRecords *out) {
  uint32_t order[kMaxCorners];
  const FaceKey key = MakeFaceKey(cornerWords, corners, order);
  for (uint32_t age = 0; age < 2; ++age) {
    const FaceRecord *record = nullptr;
    const Lookup result = table.FindIn(key, g_epoch - age, &record);
    if (result == Lookup::kAmbiguous)
      return false;
    if (result == Lookup::kAbsent)
      continue;
    out->count = 0;
    for (uint32_t i = 0; i < corners; ++i) {
      if (i > 0 && key.words[i] == key.words[i - 1])
        continue;
      out->words[out->count] = key.words[i];
      out->vertices[out->count] = record->vertices[i];
      ++out->count;
    }
    if (age)
      ++stats->previous;
    return true;
  }
  return false;
}

// Word records of a polygon's distinct corner words in one frame; the found
// ones first
struct CornerRecords {
  uint32_t distinct;
  uint32_t ambiguous;
  uint32_t found;
  uint32_t words[kMaxCorners];
  const VertexEntry *entries[kMaxCorners];
};

void FindCorners(const VertexTable &table, const uint32_t *cornerWords,
                 uint32_t corners, uint32_t epoch, CornerRecords *out) {
  uint32_t seen[kMaxCorners];
  out->distinct = 0;
  out->ambiguous = 0;
  out->found = 0;
  for (uint32_t i = 0; i < corners; ++i) {
    const uint32_t word = cornerWords[i];
    if (std::find(seen, seen + out->distinct, word) != seen + out->distinct)
      continue;
    seen[out->distinct++] = word;
    const VertexEntry *entry = nullptr;
    const Lookup result = table.FindIn(word, epoch, &entry);
    if (result == Lookup::kAmbiguous)
      ++out->ambiguous;
    if (result != Lookup::kFound)
      continue;
    out->words[out->found] = word;
    out->entries[out->found] = entry;
    ++out->found;
  }
}

// Records from the word tables, all from one frame: the one in which more
// corner words are known (the current one on a tie). A fine corner whose
// word has several positions takes the one whose projections lie closest to
// those of the other corners, and none where equally close picks disagree.
// An integer polygon needs every corner word known, at most one of them
// ambiguous (which every polygon of the frame then leaves uncorrected), and
// the others found, undisputed and projected close together. False when
// nothing applies.
bool FindWords(bool fine, const uint32_t *cornerWords, uint32_t corners,
               PolygonStats *stats, MarkerRecords *out) {
  const VertexTable &table = fine ? g_fineTable : g_integerTable;
  CornerRecords lookups[2];
  FindCorners(table, cornerWords, corners, g_epoch, &lookups[0]);
  uint32_t age = 0;
  const uint32_t known = lookups[0].found + lookups[0].ambiguous;
  if (known < lookups[0].distinct) {
    FindCorners(table, cornerWords, corners, g_epoch - 1, &lookups[1]);
    const uint32_t knownBefore = lookups[1].found + lookups[1].ambiguous;
    if (known && knownBefore)
      ++stats->split;
    if (knownBefore > known)
      age = 1;
  }
  const CornerRecords &records = lookups[age];
  if (records.found == 0 && records.ambiguous == 0)
    return false;
  ++stats->polygons;
  const uint32_t absent = records.distinct - records.found - records.ambiguous;
  stats->absent += absent;
  stats->ambiguous += records.ambiguous;

  bool several = false;
  for (uint32_t i = 0; i < records.found; ++i)
    several = several || records.entries[i]->count > 1;
  uint32_t choice[kMaxCorners] = {};
  uint32_t spread = 0;
  uint32_t disputed = 0;
  if (!fine || several)
    disputed = ChooseCandidates(records.entries, records.found, choice, &spread);
  for (uint32_t i = 0; i < records.found; ++i) {
    if (disputed & (1u << i))
      ++stats->ties;
  }

  if (!fine && spread > kIntegerSpread)
    ++stats->spread;
  if (records.found == 0 ||
      (!fine && (absent || records.ambiguous > 1 ||
                 (records.ambiguous && records.found < 2) || disputed ||
                 spread > kIntegerSpread))) {
    ++stats->rejected;
    return false;
  }

  out->count = 0;
  for (uint32_t i = 0; i < records.found; ++i) {
    if (disputed & (1u << i))
      continue;
    out->words[out->count] = records.words[i];
    out->vertices[out->count] = records.entries[i]->candidates[choice[i]].vertex;
    ++out->count;
  }
  if (out->count == 0) {
    ++stats->rejected;
    return false;
  }
  if (age)
    ++stats->previous;
  if (out->count == records.distinct)
    ++stats->complete;
  else
    ++stats->partial;
  return true;
}

// The private command for the polygon at packet; 0 words when nothing
// applies. Records come from the polygon's projection batch when its corners
// were projected together, else from the records of the single words.
uint32_t BuildMarker(const uint32_t *packet, uint32_t *out) {
  const uint32_t command = packet[0];
  const bool fine = (command >> 29) != 1;
  const uint32_t polygon = fine ? FineToPolygon(command) : command;
  const uint32_t corners = (polygon & kGp0Quad) ? 4 : 3;
  const uint32_t perCorner = (polygon & kGp0Textured) ? 2 : 1;
  const bool gouraud = (polygon & kGp0Gouraud) != 0;
  uint32_t cornerWords[kMaxCorners];
  for (uint32_t i = 0; i < corners; ++i)
    cornerWords[i] = packet[1 + i * perCorner + (gouraud ? i : 0)];

  PolygonStats *stats = fine ? &g_stats.fine : &g_stats.integer;
  MarkerRecords records;
  if (FindFace(fine ? g_fineFaces : g_integerFaces, cornerWords, corners,
               stats, &records)) {
    ++stats->polygons;
    ++stats->faces;
  } else if (!FindWords(fine, cornerWords, corners, stats, &records)) {
    return 0;
  }

  for (uint32_t i = 0; i < records.count; ++i) {
    const PreciseVertex &vertex = records.vertices[i];
    uint32_t *record = out + kMarkerHeaderWords + i * kRecordWords;
    record[0] = records.words[i];
    StoreFloat(record + 1, vertex.x);
    StoreFloat(record + 2, vertex.y);
    StoreFloat(record + 3, vertex.w);
  }
  const bool complete =
      CoversCorners(cornerWords, corners, records.words, records.count);
  out[0] = kMarkerTag | (complete ? kMarkerPerspective : 0) | records.count;
  out[1] = command;
  return kMarkerHeaderWords + records.count * kRecordWords;
}

// Replaces the OT walk's append of one packet: the same words, with the
// private command in front of each polygon that has records
void __fastcall OtAppendHook(VectorU32 *vector, void *, uint32_t *,
                             const uint32_t *first, const uint32_t *last,
                             uint32_t tag) {
  PollHotkey();
  if (vector->begin == vector->end) {
    ++g_stats.walks;
    ReportStats();
  }
  g_walkSinceCapture = true;
  if (!g_preciseActive) {
    g_vecInsert(vector, vector->end, first, last, tag);
    return;
  }

  const uint32_t count = static_cast<uint32_t>(last - first);
  uint32_t copied = 0;
  uint32_t used = 0;
  uint32_t index = 0;
  while (index < count) {
    const uint32_t length = Gp0CommandLength(first + index, count - index);
    if (length == 0)
      break;
    if (IsPolygon(first[index])) {
      uint32_t marker[kMarkerHeaderWords + kMaxCorners * kRecordWords];
      const uint32_t markerWords = BuildMarker(first + index, marker);
      if (markerWords) {
        memcpy(g_scratch + used, first + copied, (index - copied) * 4);
        used += index - copied;
        copied = index;
        memcpy(g_scratch + used, marker, markerWords * 4);
        used += markerWords;
      }
    }
    index += length;
  }

  if (used == 0) {
    g_vecInsert(vector, vector->end, first, last, tag);
    return;
  }
  memcpy(g_scratch + used, first + copied, (count - copied) * 4);
  used += count - copied;
  g_vecInsert(vector, vector->end, g_scratch, g_scratch + used, tag);
}

uint32_t ReadMarker(const uint32_t *words, uint32_t remaining) {
  const uint32_t count = words[0] & kMarkerCountMask;
  const uint32_t length = kMarkerHeaderWords + count * kRecordWords;
  if (remaining < length)
    return 0;
  g_armedRecords.command = words[1];
  g_armedRecords.count = (std::min)(count, kMaxCorners);
  g_armedRecords.perspective = (words[0] & kMarkerPerspective) != 0;
  for (uint32_t i = 0; i < g_armedRecords.count; ++i) {
    const uint32_t *record = words + kMarkerHeaderWords + i * kRecordWords;
    g_armedRecords.words[i] = record[0];
    g_armedRecords.vertices[i] = {LoadFloat(record + 1), LoadFloat(record + 2),
                                  LoadFloat(record + 3)};
  }
  g_recordsArmed = g_armedRecords.count > 0;
  return length;
}

// A matrix in guest memory (PS1 layout: nine 16-bit rotation entries, a pad,
// three 32-bit translations), read through the guest's memory regions
bool ReadGuestMatrix(uint32_t address, MatrixKey *out) {
  uint32_t words[sizeof(MatrixKey) / 4];
  void *const *regions =
      reinterpret_cast<void *const *>(g_moduleBase + rde::guest_region_table);
  for (uint32_t i = 0; i < sizeof(words) / 4; ++i) {
    const uint32_t wordAddress = address + i * 4;
    void *region = regions[wordAddress >> kRegionShift];
    if (!region)
      return false;
    const GuestRead32_t read32 =
        (*reinterpret_cast<GuestRead32_t *const *>(region))[kRegionRead32Slot];
    words[i] = read32(region, wordAddress);
  }
  memcpy(out, words, sizeof(words));
  out->pad = 0;
  return true;
}

// The composition record of a matrix, or zero offsets
const MatrixOffset &CompositionOffset(const MatrixKey &matrix) {
  static const MatrixOffset kNoOffset = {};
  const MatrixOffset *offset = nullptr;
  const Lookup result = g_matrixTable.Use(matrix, g_epoch, &offset);
  if (result == Lookup::kAmbiguous)
    ++g_stats.matrixAmbiguous;
  return result == Lookup::kFound ? *offset : kNoOffset;
}

// CompMatrix(parent, local) has just stored result: the rotation is
// parent * local, each entry the sum of its products shifted right by 12 and
// saturated to 16 bits (MVMVA's IR without lm), and the translation parent *
// local translation (its low 16 bits) shifted right by 12 (MAC, which holds
// it whole) + parent translation, wrapping at 32 bits. Recomputes both from
// the precise parent and local, saturated the same way, and keeps what was
// dropped (from the unwrapped translation). A result that the integer inputs do not reproduce exactly is not
// a composition of them and gets no record; one they do reproduce is exact
// given the records of its inputs, whatever the size of the dropped part.
void CaptureComposition(const MatrixKey &parent, const MatrixKey &local,
                        uint32_t resultAddress) {
  MatrixKey result;
  if (!ReadGuestMatrix(resultAddress, &result))
    return;
  BeginCapture();
  const MatrixOffset &parentOffset = CompositionOffset(parent);
  const MatrixOffset &localOffset = CompositionOffset(local);
  double parentRotation[9];
  double localRotation[9];
  for (int i = 0; i < 9; ++i) {
    parentRotation[i] = parent.rotation[i] + parentOffset.rotation[i];
    localRotation[i] = local.rotation[i] + localOffset.rotation[i];
  }
  int32_t shortTranslation[3];
  double localTranslation[3];
  for (int i = 0; i < 3; ++i) {
    shortTranslation[i] = static_cast<int16_t>(local.translation[i]);
    localTranslation[i] = shortTranslation[i];
    if (shortTranslation[i] == local.translation[i])
      localTranslation[i] += localOffset.translation[i];
  }

  MatrixOffset offset;
  bool saturated = false;
  bool large = false;
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      int64_t product = 0;
      double precise = 0.0;
      for (int k = 0; k < 3; ++k) {
        product += static_cast<int64_t>(parent.rotation[row * 3 + k]) *
                   local.rotation[k * 3 + column];
        precise += parentRotation[row * 3 + k] * localRotation[k * 3 + column];
      }
      const int64_t shifted = product >> 12;
      const int16_t entry = result.rotation[row * 3 + column];
      if (std::clamp<int64_t>(shifted, -32768, 32767) != entry) {
        ++g_stats.matrixRejected;
        return;
      }
      saturated = saturated || !FitsInt16(static_cast<int32_t>(shifted));
      const double dropped =
          std::clamp(precise / 4096.0, -32768.0, 32767.0) - entry;
      large = large || std::fabs(dropped) > kLargeRotationOffset;
      offset.rotation[row * 3 + column] = static_cast<float>(dropped);
    }
    int64_t product = 0;
    double precise = 0.0;
    for (int k = 0; k < 3; ++k) {
      product += static_cast<int64_t>(parent.rotation[row * 3 + k]) *
                 shortTranslation[k];
      precise += parentRotation[row * 3 + k] * localTranslation[k];
    }
    const int64_t sum = (product >> 12) + parent.translation[row];
    const int32_t translation =
        static_cast<int32_t>(static_cast<uint32_t>(sum));
    const double dropped = precise / 4096.0 + parent.translation[row] +
                           parentOffset.translation[row] -
                           static_cast<double>(sum);
    if (translation != result.translation[row]) {
      ++g_stats.matrixRejected;
      return;
    }
    large = large || std::fabs(dropped) > kLargeTranslationOffset;
    offset.translation[row] = static_cast<float>(dropped);
  }
  if (saturated)
    ++g_stats.matrixSaturated;
  if (large)
    ++g_stats.matrixLarge;
  ++g_matrixGeneration;
  CountInsertion(g_matrixTable.Insert(result, offset, g_epoch),
                 &g_stats.matrixConflicts, &g_stats.matrixRenewed);
}

} // namespace

// Referenced from the assembly stubs below (file scope names)
static const PolygonRecords *g_activeRecords = nullptr;
static void *g_vec3Make = nullptr;
static void *g_roundf = nullptr;
static void *g_compMatrixOriginal = nullptr;
static volatile bool g_subpixelActive = false;
static volatile bool g_batchBoundsActive = false;

namespace {
void UpdateActiveParts() {
  const bool preciseWasActive = g_preciseActive;
  g_subpixelActive = g_enabled && g_subpixelInstalled;
  g_preciseActive = g_enabled && g_preciseInstalled;
  g_batchBoundsActive = g_enabled;
  // Records captured before PGXP was switched off are stale
  if (g_preciseActive && !preciseWasActive)
    g_epoch += 2;
  g_blendChain.valid = false;
  g_pending.count = 0;
}
} // namespace

static uint32_t __cdecl GteExecute(uint8_t *gte, uint32_t command) {
  if (!g_preciseActive)
    return g_gteExecCommand(gte, command);

  const uint32_t operation = command & 0x3F;
  const bool blend = (operation == kGteGpf || operation == kGteGpl) &&
                     (command & kGteShiftFlag);
  BlendInput blendInput = {};
  if (blend)
    blendInput = ReadBlendInput(gte);
  const uint32_t result = g_gteExecCommand(gte, command);
  switch (operation) {
  case kGteRtpsAlias:
  case kGteRtps:
    CaptureProjections(gte, command, 1);
    break;
  case kGteNclip:
    CorrectNclip(gte);
    break;
  case kGteMvmva:
    CaptureTransform(gte, command);
    break;
  case kGteRtpt:
    CaptureProjections(gte, command, 3);
    break;
  case kGteGpf:
  case kGteGpl:
    if (blend)
      CaptureBlend(gte, operation, blendInput);
    break;
  }
  return result;
}

static void __cdecl NclipFine(uint8_t *gte) {
  g_nclipFineOriginal(gte);
  if (g_preciseActive)
    CorrectNclip(gte);
}

// Replaces the call of gte_exec_command (thiscall: ecx = GTE, one stack
// argument). The original touches no SSE register, so all are preserved.
__declspec(naked) static void GteExecCommandHook() {
  __asm {
    push ebp
    mov ebp, esp
    sub esp, 0x80
    and esp, 0xFFFFFFF0
    movaps xmmword ptr [esp], xmm0
    movaps xmmword ptr [esp + 0x10], xmm1
    movaps xmmword ptr [esp + 0x20], xmm2
    movaps xmmword ptr [esp + 0x30], xmm3
    movaps xmmword ptr [esp + 0x40], xmm4
    movaps xmmword ptr [esp + 0x50], xmm5
    movaps xmmword ptr [esp + 0x60], xmm6
    movaps xmmword ptr [esp + 0x70], xmm7
    push dword ptr [ebp + 8]
    push ecx
    call GteExecute
    add esp, 8
    movaps xmm0, xmmword ptr [esp]
    movaps xmm1, xmmword ptr [esp + 0x10]
    movaps xmm2, xmmword ptr [esp + 0x20]
    movaps xmm3, xmmword ptr [esp + 0x30]
    movaps xmm4, xmmword ptr [esp + 0x40]
    movaps xmm5, xmmword ptr [esp + 0x50]
    movaps xmm6, xmmword ptr [esp + 0x60]
    movaps xmm7, xmmword ptr [esp + 0x70]
    mov esp, ebp
    pop ebp
    ret 4
  }
}

// Replaces the entry of gte_nclip_fine (fastcall: ecx = GTE)
__declspec(naked) static void NclipFineHook() {
  __asm {
    push ebp
    mov ebp, esp
    sub esp, 0x80
    and esp, 0xFFFFFFF0
    movaps xmmword ptr [esp], xmm0
    movaps xmmword ptr [esp + 0x10], xmm1
    movaps xmmword ptr [esp + 0x20], xmm2
    movaps xmmword ptr [esp + 0x30], xmm3
    movaps xmmword ptr [esp + 0x40], xmm4
    movaps xmmword ptr [esp + 0x50], xmm5
    movaps xmmword ptr [esp + 0x60], xmm6
    movaps xmmword ptr [esp + 0x70], xmm7
    push ecx
    call NclipFine
    add esp, 4
    movaps xmm0, xmmword ptr [esp]
    movaps xmm1, xmmword ptr [esp + 0x10]
    movaps xmm2, xmmword ptr [esp + 0x20]
    movaps xmm3, xmmword ptr [esp + 0x30]
    movaps xmm4, xmmword ptr [esp + 0x40]
    movaps xmm5, xmmword ptr [esp + 0x50]
    movaps xmm6, xmmword ptr [esp + 0x60]
    movaps xmm7, xmmword ptr [esp + 0x70]
    mov esp, ebp
    pop ebp
    ret
  }
}

// Runs the original CompMatrix port: ecx = parent, edx = local, one stack
// argument (the result) the caller removes
__declspec(naked) static uint32_t __cdecl CallCompMatrix(uint32_t, uint32_t,
                                                         uint32_t) {
  __asm {
    mov ecx, dword ptr [esp + 4]
    mov edx, dword ptr [esp + 8]
    push dword ptr [esp + 12]
    call dword ptr [g_compMatrixOriginal]
    add esp, 4
    ret
  }
}

// Guest matrix addresses of CompMatrix(parent, local, result)
static uint32_t __cdecl CompMatrix(uint32_t parent, uint32_t local,
                                   uint32_t result) {
  if (!g_preciseActive)
    return CallCompMatrix(parent, local, result);
  MatrixKey parentMatrix;
  MatrixKey localMatrix;
  const bool inputs = ReadGuestMatrix(parent, &parentMatrix) &&
                      ReadGuestMatrix(local, &localMatrix);
  const uint32_t value = CallCompMatrix(parent, local, result);
  if (inputs)
    CaptureComposition(parentMatrix, localMatrix, result);
  return value;
}

// Replaces the entry of the CompMatrix port (ecx = parent, edx = local,
// [esp + 4] = result; the caller removes it). Callers may keep SSE values
// across the call, so all are preserved.
__declspec(naked) static void CompMatrixHook() {
  __asm {
    push ebp
    mov ebp, esp
    sub esp, 0x80
    and esp, 0xFFFFFFF0
    movaps xmmword ptr [esp], xmm0
    movaps xmmword ptr [esp + 0x10], xmm1
    movaps xmmword ptr [esp + 0x20], xmm2
    movaps xmmword ptr [esp + 0x30], xmm3
    movaps xmmword ptr [esp + 0x40], xmm4
    movaps xmmword ptr [esp + 0x50], xmm5
    movaps xmmword ptr [esp + 0x60], xmm6
    movaps xmmword ptr [esp + 0x70], xmm7
    push dword ptr [ebp + 8]
    push edx
    push ecx
    call CompMatrix
    add esp, 12
    movaps xmm0, xmmword ptr [esp]
    movaps xmm1, xmmword ptr [esp + 0x10]
    movaps xmm2, xmmword ptr [esp + 0x20]
    movaps xmm3, xmmword ptr [esp + 0x30]
    movaps xmm4, xmmword ptr [esp + 0x40]
    movaps xmm5, xmmword ptr [esp + 0x50]
    movaps xmm6, xmmword ptr [esp + 0x60]
    movaps xmm7, xmmword ptr [esp + 0x70]
    mov esp, ebp
    pop ebp
    ret
  }
}

// Replaces the two roundf calls of the fine vertex emit: the unrounded
// argument while PGXP is on, else roundf
__declspec(naked) static void RoundFineHook() {
  __asm {
    cmp byte ptr [g_subpixelActive], 0
    je use_roundf
    fld dword ptr [esp + 4]
    ret
  use_roundf:
    jmp dword ptr [g_roundf]
  }
}

// Replaces "mov eax, [esp+14h]; cvttss2si edi, xmm2" of gpu_batch_flush,
// where xmm4 / xmm1 hold the bounding box's maximum x / y: while PGXP is on,
// rounds both up to whole render pixels, so the truncation that follows keeps
// them
__declspec(naked) static void BatchBoundsHook() {
  __asm {
    cmp byte ptr [g_batchBoundsActive], 0
    je replaced
    cvttss2si eax, xmm4
    cvtsi2ss xmm0, eax
    comiss xmm0, xmm4
    jae x_done
    inc eax
  x_done:
    cvtsi2ss xmm4, eax
    cvttss2si eax, xmm1
    cvtsi2ss xmm0, eax
    comiss xmm0, xmm1
    jae y_done
    inc eax
  y_done:
    cvtsi2ss xmm1, eax
  replaced:
    mov eax, dword ptr [esp + 0x18]
    cvttss2si edi, xmm2
    ret
  }
}

// position = {x, y, z} about to be passed to vec3_make by a vertex emit
static void __cdecl AdjustEmitPosition(const uint8_t *gpu, float *position) {
  const PolygonRecords *records = g_activeRecords;
  const uint32_t word = *reinterpret_cast<const uint32_t *>(gpu + rde::gpu_vertex_xy);
  for (uint32_t i = 0; i < records->count; ++i) {
    if (records->words[i] != word)
      continue;
    const uint32_t offset =
        *reinterpret_cast<const uint32_t *>(gpu + rde::gpu_draw_offset);
    const int32_t offsetX = static_cast<int32_t>(offset << 21) >> 21;
    const int32_t offsetY = static_cast<int32_t>(offset << 10) >> 21;
    const PreciseVertex &vertex = records->vertices[i];
    position[0] = (static_cast<float>(offsetX) + vertex.x) * 4.0f;
    position[1] = (static_cast<float>(offsetY) + vertex.y) * 4.0f;
    position[2] = records->perspective ? vertex.w : 0.0f;
    return;
  }
}

// Shared tail of the emit stubs: [esp] = GPU object, [esp + 4] = return
// address into the emit; xmm1..xmm3 = position. Ends in vec3_make.
__declspec(naked) static void EmitPositionCommon() {
  __asm {
    cmp g_activeRecords, 0
    je done
    pushad
    mov ebx, esp
    sub esp, 0x60
    and esp, 0xFFFFFFF0
    movaps xmmword ptr [esp], xmm0
    movaps xmmword ptr [esp + 0x10], xmm4
    movaps xmmword ptr [esp + 0x20], xmm5
    movaps xmmword ptr [esp + 0x30], xmm6
    movaps xmmword ptr [esp + 0x40], xmm7
    movss dword ptr [esp + 0x50], xmm1
    movss dword ptr [esp + 0x54], xmm2
    movss dword ptr [esp + 0x58], xmm3
    lea eax, [esp + 0x50]
    push eax
    push dword ptr [ebx + 0x20]
    call AdjustEmitPosition
    add esp, 8
    movaps xmm0, xmmword ptr [esp]
    movaps xmm4, xmmword ptr [esp + 0x10]
    movaps xmm5, xmmword ptr [esp + 0x20]
    movaps xmm6, xmmword ptr [esp + 0x30]
    movaps xmm7, xmmword ptr [esp + 0x40]
    movss xmm1, dword ptr [esp + 0x50]
    movss xmm2, dword ptr [esp + 0x54]
    movss xmm3, dword ptr [esp + 0x58]
    mov esp, ebx
    popad
  done:
    add esp, 4
    jmp dword ptr [g_vec3Make]
  }
}

// gpu_emit_vertex: GPU object in edi
__declspec(naked) static void EmitPositionHookInteger() {
  __asm {
    push edi
    jmp EmitPositionCommon
  }
}

// gpu_emit_vertex_fine: GPU object in esi
__declspec(naked) static void EmitPositionHookFine() {
  __asm {
    push esi
    jmp EmitPositionCommon
  }
}

// Wraps a GP0 handler slot: the private command arms records, and a polygon
// whose command word matches the armed records is parsed with them active
template <int kSlot>
static uint32_t __fastcall GpuCommandHook(void *gpu, void *,
                                          const uint32_t *words,
                                          uint32_t remaining) {
  const uint32_t word = words[0];
  if (kSlot == rde::gpu_cmd_table_env_or_fine &&
      (word & kMarkerMask) == kMarkerTag)
    return ReadMarker(words, remaining);

  const bool armed = g_recordsArmed;
  g_recordsArmed = false;
  const GpuCommand_t original = g_gpuCommandOriginal[kSlot];
  if (!armed || g_armedRecords.command != word || !IsPolygon(word))
    return original(gpu, words, remaining);

  g_activeRecords = &g_armedRecords;
  const uint32_t consumed = original(gpu, words, remaining);
  g_activeRecords = nullptr;
  return consumed;
}

bool PgxpEnabled(const Settings &settings) {
  return settings.GetBool("pgxp_enabled", true);
}

static bool InstallSubpixelFineHooks(uintptr_t base) {
  const uintptr_t sites[2] = {base + rde::gpu_emit_vertex_fine_plus_4c,
                              base + rde::gpu_emit_vertex_fine_plus_8f};
  for (uintptr_t site : sites) {
    if (!VerifyCallTo(site, base + rde::crt_roundf, "PGXP fine vertex rounding"))
      return false;
  }
  g_roundf = reinterpret_cast<void *>(base + rde::crt_roundf);
  for (uintptr_t site : sites) {
    if (!InstallCallHook(site, reinterpret_cast<void *>(RoundFineHook), 5))
      return false;
  }
  return true;
}

static bool InstallBatchBoundsHook(uintptr_t base) {
  // mov eax, [esp+14h]; cvttss2si edi, xmm2
  static const uint8_t kOriginal[8] = {0x8B, 0x44, 0x24, 0x14,
                                       0xF3, 0x0F, 0x2C, 0xFA};
  const uintptr_t site = base + rde::gpu_batch_flush_plus_176;
  return VerifyBytes(site, kOriginal, sizeof(kOriginal), "PGXP batch bounds") &&
         InstallCallHook(site, reinterpret_cast<void *>(BatchBoundsHook),
                         sizeof(kOriginal)) != 0;
}

static bool InstallNclipFineHook(uintptr_t base) {
  static const uint8_t kPrologue[rde::gte_nclip_fine_prologue] = {
      0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x20};
  const uintptr_t entry = base + rde::gte_nclip_fine;
  if (!VerifyBytes(entry, kPrologue, sizeof(kPrologue), "PGXP fine NCLIP"))
    return false;
  g_nclipFineOriginal = reinterpret_cast<NclipFine_t>(
      MakeTrampoline(entry, kPrologue, sizeof(kPrologue)));
  return g_nclipFineOriginal &&
         InstallJmpHook(entry, reinterpret_cast<void *>(NclipFineHook),
                        rde::gte_nclip_fine_prologue) != 0;
}

static bool InstallCompMatrixHook(uintptr_t base) {
  static const uint8_t kPrologue[rde::gte_comp_matrix_impl_prologue] = {
      0x83, 0xEC, 0x18, 0x53, 0x55};
  const uintptr_t entry = base + rde::gte_comp_matrix_impl;
  if (!VerifyBytes(entry, kPrologue, sizeof(kPrologue), "PGXP CompMatrix"))
    return false;
  g_compMatrixOriginal = MakeTrampoline(entry, kPrologue, sizeof(kPrologue));
  return g_compMatrixOriginal &&
         InstallJmpHook(entry, reinterpret_cast<void *>(CompMatrixHook),
                        rde::gte_comp_matrix_impl_prologue) != 0;
}

// GPU side, GTE capture and NCLIP of the precise positions
static bool InstallPreciseHooks(uintptr_t base) {
  const uintptr_t gteSite = base + rde::gte_exec_cop2_plus_18;
  const uintptr_t emitSite = base + rde::gpu_emit_vertex_plus_5e;
  const uintptr_t emitFineSite = base + rde::gpu_emit_vertex_fine_plus_ae;
  const uintptr_t table = base + rde::gpu_cmd_table;
  const uintptr_t polygonSlot = table + rde::gpu_cmd_table_polygon * 4;
  const uintptr_t fineSlotLow =
      table + rde::gpu_cmd_table_vram_to_cpu_or_fine * 4;
  const uintptr_t fineSlotHigh = table + rde::gpu_cmd_table_env_or_fine * 4;
  if (!VerifyCallTo(gteSite, base + rde::gte_exec_command, "PGXP GTE") ||
      !VerifyCallTo(emitSite, base + rde::vec3_make, "PGXP vertex emit") ||
      !VerifyCallTo(emitFineSite, base + rde::vec3_make,
                    "PGXP fine vertex emit") ||
      !VerifyOperand(polygonSlot, base + rde::gpu_cmd_polygon,
                     "PGXP GP0 polygon handler") ||
      !VerifyOperand(fineSlotLow, base + rde::gpu_cmd_vram_to_cpu_or_fine,
                     "PGXP GP0 C0-DF handler") ||
      !VerifyOperand(fineSlotHigh, base + rde::gpu_cmd_env_or_fine,
                     "PGXP GP0 E0-FF handler"))
    return false;

  g_gteExecCommand =
      reinterpret_cast<GteExecCommand_t>(base + rde::gte_exec_command);
  g_vec3Make = reinterpret_cast<void *>(base + rde::vec3_make);
  g_gpuCommandOriginal[rde::gpu_cmd_table_polygon] =
      reinterpret_cast<GpuCommand_t>(base + rde::gpu_cmd_polygon);
  g_gpuCommandOriginal[rde::gpu_cmd_table_vram_to_cpu_or_fine] =
      reinterpret_cast<GpuCommand_t>(base + rde::gpu_cmd_vram_to_cpu_or_fine);
  g_gpuCommandOriginal[rde::gpu_cmd_table_env_or_fine] =
      reinterpret_cast<GpuCommand_t>(base + rde::gpu_cmd_env_or_fine);

  // Render side first, so every private command finds its handler
  if (!RedirectOperand(polygonSlot,
                       reinterpret_cast<void *>(
                           GpuCommandHook<rde::gpu_cmd_table_polygon>)) ||
      !RedirectOperand(
          fineSlotLow,
          reinterpret_cast<void *>(
              GpuCommandHook<rde::gpu_cmd_table_vram_to_cpu_or_fine>)) ||
      !RedirectOperand(fineSlotHigh,
                       reinterpret_cast<void *>(
                           GpuCommandHook<rde::gpu_cmd_table_env_or_fine>)) ||
      !InstallCallHook(emitSite,
                       reinterpret_cast<void *>(EmitPositionHookInteger), 5) ||
      !InstallCallHook(emitFineSite,
                       reinterpret_cast<void *>(EmitPositionHookFine), 5)) {
    std::cout << "[PGXP] Failed to install the GPU hooks" << std::endl;
    return false;
  }

  if (!InstallNclipFineHook(base))
    std::cout << "[PGXP] Fine NCLIP not corrected" << std::endl;
  if (!InstallCompMatrixHook(base))
    std::cout << "[PGXP] Precise matrices not available" << std::endl;

  if (!InstallCallHook(gteSite, reinterpret_cast<void *>(GteExecCommandHook),
                       5)) {
    std::cout << "[PGXP] Failed to install the GTE hook" << std::endl;
    return false;
  }
  return true;
}

bool ApplyPgxpPatch(uintptr_t base, bool stats) {
  g_moduleBase = base;
  g_statsEnabled = stats;

  const uintptr_t walkSite = base + rde::dma_gpu_ot_walk_plus_ba;
  if (!VerifyCallTo(walkSite, base + rde::vec_u32_insert_range,
                    "PGXP OT walk"))
    return false;
  g_vecInsert = reinterpret_cast<VecInsert_t>(base + rde::vec_u32_insert_range);

  g_subpixelInstalled = InstallSubpixelFineHooks(base);
  g_preciseInstalled = InstallPreciseHooks(base);
  if (!g_subpixelInstalled && !g_preciseInstalled)
    return false;
  UpdateActiveParts();

  if (!InstallBatchBoundsHook(base))
    std::cout << "[PGXP] Batch edges not widened (seams may show)"
              << std::endl;
  if (!InstallCallHook(walkSite, reinterpret_cast<void *>(OtAppendHook), 5)) {
    std::cout << "[PGXP] Failed to install the OT hook" << std::endl;
    return false;
  }

  std::cout << "[Mod] PGXP enabled" << std::endl;
  return g_subpixelInstalled && g_preciseInstalled;
}
