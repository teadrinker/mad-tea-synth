#ifndef INTERN_H
#define INTERN_H

#include "tsys.h"
#include "array.h"
#include "string_view.h"

typedef int InternID;

// Define INTERN_HASH_SUPPORT to compile in an open-addressed hash index for
// intern lookups. Without it, lookups are linear (smaller binary, fine for
// small string tables). With it, intern_string is amortized O(1) and scales
// to large definition files.

#define INTERN_HASH_SUPPORT 1

typedef struct InternCtx {
    Array entries; // Array of InternEntry
    Tsys *sys;
#ifdef INTERN_HASH_SUPPORT
    // Open-addressed hash table mapping interned bytes -> InternID.
    // Slots are (hash, id). id == -1 empty, id == -2 tombstone, else live.
    void *hash_slots;       // allocated via sys->malloc; NULL until first use
    size_t hash_cap;        // power of two, 0 when uninitialized
    size_t hash_used;       // live + tombstone count (for load factor)
    size_t hash_live;       // live count; a rehash only grows when this needs it
#endif
    Array free_ids;         // released dynamic ids, reused before the table grows
    char *byte_strings;     // storage of the one-byte strings behind ids 1..255
    unsigned owner_serial;  // last serial handed to an InternOwner
} InternCtx;

typedef struct InternOwner {
    InternCtx *ctx;
    Array owned_ids; // Array of InternID
    unsigned serial; // unique per ctx, survives the owner being copied
} InternOwner;

// Context Management
void intern_ctx_init(InternCtx *ctx, Tsys *sys);
void intern_ctx_deinit(InternCtx *ctx);

// Owner Management
// An owner tracks a list of IDs it holds references to.
void intern_owner_init(InternOwner *owner, InternCtx *ctx);
void intern_owner_deinit(InternOwner *owner);

// Operations
// Interns a string. 
// If it exists in the pool, refcount is incremented.
// If not, it is added.
// The resulting ID is also added to the 'owner' logic (to be released on owner_deinit).
InternID intern_string(InternOwner *owner, StringView sv);
InternID intern_c_string(InternOwner *owner, const char *str);
void intern_string_at_fixed_id(InternCtx *ctx, InternID id, const char *str);
void intern_init_fixed_tokens(InternCtx *ctx);

// Retrieve string by ID.
// Returns empty view if ID is invalid or released.
StringView intern_get(InternCtx *ctx, InternID id);

// Retrieve C-string. Returns "" if invalid.
const char *intern_get_cstr(InternCtx *ctx, InternID id);

#endif
