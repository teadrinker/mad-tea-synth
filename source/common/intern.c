#include "intern.h"
#include "parser/tokens.h"

typedef struct {
    char *data;
    size_t len;
    int ref_count;
    // Serial of the owner that last took a reference (0 = none). An owner only
    // ever releases all of its references at once, so a second take by the same
    // owner can be skipped. A serial, not a pointer: owners are copied by value
    // (ParseResult is returned by value), and a later owner may reuse an address.
    unsigned holder;
} InternEntry;

#ifdef INTERN_HASH_SUPPORT

typedef struct {
    unsigned int hash;
    InternID id; // -1 empty, -2 tombstone, else >=0
} HashSlot;

#define HASH_SLOT_EMPTY     ((InternID)-1)
#define HASH_SLOT_TOMBSTONE ((InternID)-2)
#define HASH_INITIAL_CAP    64u

static unsigned int hash_bytes(const char *s, size_t len) {
    // FNV-1a 32-bit.
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static void hash_alloc(InternCtx *ctx, size_t cap) {
    HashSlot *slots = (HashSlot*)S_MALLOC(ctx->sys, cap * sizeof(HashSlot));
    for (size_t i = 0; i < cap; ++i) {
        slots[i].hash = 0;
        slots[i].id = HASH_SLOT_EMPTY;
    }
    ctx->hash_slots = slots;
    ctx->hash_cap = cap;
    ctx->hash_used = 0;
    ctx->hash_live = 0;
}

static void hash_insert_id(InternCtx *ctx, unsigned int h, InternID id);

static void hash_rehash(InternCtx *ctx, size_t new_cap) {
    HashSlot *old = (HashSlot*)ctx->hash_slots;
    size_t old_cap = ctx->hash_cap;
    hash_alloc(ctx, new_cap);
    if (!old) return;
    for (size_t i = 0; i < old_cap; ++i) {
        if (old[i].id >= 0) {
            hash_insert_id(ctx, old[i].hash, old[i].id);
        }
    }
    S_FREE(ctx->sys, old);
}

static void hash_insert_id(InternCtx *ctx, unsigned int h, InternID id) {
    HashSlot *slots = (HashSlot*)ctx->hash_slots;
    size_t cap = ctx->hash_cap;
    size_t mask = cap - 1;
    size_t i = h & mask;
    while (slots[i].id >= 0) {
        i = (i + 1) & mask;
    }
    // If this slot was empty (not tombstone) we are consuming a fresh cell.
    if (slots[i].id == HASH_SLOT_EMPTY) ctx->hash_used++;
    ctx->hash_live++;
    slots[i].hash = h;
    slots[i].id = id;
}

// Lookup returns >=0 if found, else -1. Out param *insert_pos receives the
// index where a new entry should go (first tombstone or empty along the probe).
static InternID hash_lookup(InternCtx *ctx, unsigned int h, const char *bytes,
                            size_t len, size_t *insert_pos) {
    HashSlot *slots = (HashSlot*)ctx->hash_slots;
    size_t cap = ctx->hash_cap;
    size_t mask = cap - 1;
    size_t i = h & mask;
    size_t first_free = (size_t)-1;
    for (;;) {
        InternID id = slots[i].id;
        if (id == HASH_SLOT_EMPTY) {
            if (insert_pos) *insert_pos = (first_free != (size_t)-1) ? first_free : i;
            return -1;
        }
        if (id == HASH_SLOT_TOMBSTONE) {
            if (first_free == (size_t)-1) first_free = i;
        } else if (slots[i].hash == h) {
            InternEntry *e = (InternEntry*)array_get(&ctx->entries, (size_t)id);
            if (e && e->data && e->len == len) {
                size_t k;
                for (k = 0; k < len; ++k) if (e->data[k] != bytes[k]) break;
                if (k == len) {
                    if (insert_pos) *insert_pos = i;
                    return id;
                }
            }
        }
        i = (i + 1) & mask;
    }
}

static void hash_remove_bytes(InternCtx *ctx, const char *bytes, size_t len) {
    if (!ctx->hash_slots) return;
    unsigned int h = hash_bytes(bytes, len);
    size_t pos;
    InternID id = hash_lookup(ctx, h, bytes, len, &pos);
    if (id >= 0) {
        HashSlot *slots = (HashSlot*)ctx->hash_slots;
        slots[pos].id = HASH_SLOT_TOMBSTONE;
        ctx->hash_live--;
        // hash_used unchanged: tombstones still count toward load.
    }
}

static void hash_maybe_grow(InternCtx *ctx) {
    if (!ctx->hash_slots) {
        hash_alloc(ctx, HASH_INITIAL_CAP);
        return;
    }
    // Rehash when load >= 0.75, but only grow when the live entries need it:
    // a table that is mostly tombstones (parse after parse in an editor) is
    // cleaned at the same size rather than doubled forever.
    if ((ctx->hash_used + 1) * 4 >= ctx->hash_cap * 3) {
        hash_rehash(ctx, (ctx->hash_live + 1) * 2 >= ctx->hash_cap ? ctx->hash_cap * 2 : ctx->hash_cap);
    }
}

#endif // INTERN_HASH_SUPPORT

void intern_ctx_init(InternCtx *ctx, Tsys *sys) {
    array_init(&ctx->entries, sizeof(InternEntry));
    ctx->sys = sys;
    // The one-byte strings behind ids 1..255 and the empty string at 256 (see
    // tokens.h), in one block owned by the context: never hashed, owned,
    // refcounted or freed on their own. Byte 0's slot is "\0\0", which is where
    // the empty string points.
    ctx->byte_strings = (char*)S_MALLOC(sys, 256 * 2);
    for (int i = 0; i < 256; ++i) {
        char *s = ctx->byte_strings + 2 * i;
        s[0] = (char)i;
        s[1] = '\0';
        InternEntry e = { i ? s : NULL, i ? 1u : 0u, i ? 1 : 0, 0 };
        array_push(&ctx->entries, &e, sys);
    }
    InternEntry empty = { ctx->byte_strings, 0, 1, 0 };
    array_push(&ctx->entries, &empty, sys);
#ifdef INTERN_HASH_SUPPORT
    ctx->hash_slots = NULL;
    ctx->hash_cap = 0;
    ctx->hash_used = 0;
    ctx->hash_live = 0;
#endif
    array_init(&ctx->free_ids, sizeof(InternID));
    ctx->owner_serial = 0;
}

void intern_ctx_deinit(InternCtx *ctx) {
    for (size_t i = TOK_EMPTYSTRING + 1; i < ctx->entries.size; ++i) {
        InternEntry *e = (InternEntry*)array_get(&ctx->entries, i);
        if (e->data) {
            S_FREE(ctx->sys, e->data);
        }
    }
    array_free(&ctx->entries, ctx->sys);
    array_free(&ctx->free_ids, ctx->sys);
    S_FREE(ctx->sys, ctx->byte_strings);
    ctx->byte_strings = NULL;
#ifdef INTERN_HASH_SUPPORT
    if (ctx->hash_slots) {
        S_FREE(ctx->sys, ctx->hash_slots);
        ctx->hash_slots = NULL;
        ctx->hash_cap = 0;
        ctx->hash_used = 0;
    }
#endif
}

void intern_owner_init(InternOwner *owner, InternCtx *ctx) {
    owner->ctx = ctx;
    owner->serial = ++ctx->owner_serial;
    if (!owner->serial) owner->serial = ++ctx->owner_serial;
    array_init(&owner->owned_ids, sizeof(InternID));
}

static void release_id(InternCtx *ctx, InternID id) {
    if (id < 0 || id >= (InternID)ctx->entries.size) return;
    InternEntry *e = (InternEntry*)array_get(&ctx->entries, id);
    if (e->data) {
        e->ref_count--;
        if (e->ref_count <= 0) {
#ifdef INTERN_HASH_SUPPORT
            hash_remove_bytes(ctx, e->data, e->len);
#endif
            S_FREE(ctx->sys, e->data);
            e->data = NULL; // Slot is now free
            e->len = 0;
            e->ref_count = 0;
            e->holder = 0;
            // A gap below TOK_COUNT is never reused: an unregistered fixed
            // token must keep its id.
            if (id >= TOK_COUNT) array_push(&ctx->free_ids, &id, ctx->sys);
        }
    }
}

void intern_owner_deinit(InternOwner *owner) {
    if(owner->ctx == NULL)
        return;
    
    InternCtx *ctx = owner->ctx;
    for (size_t i = 0; i < owner->owned_ids.size; ++i) {
        InternID id = ((InternID*)owner->owned_ids.data)[i];
        // Forget the mark before releasing: the next owner may well be
        // allocated at this same address.
        InternEntry *e = (InternEntry*)array_get(&ctx->entries, id);
        if (e && e->holder == owner->serial) e->holder = 0;
        release_id(ctx, id);
    }
    array_free(&owner->owned_ids, ctx->sys);
}

static InternID intern_hold(InternOwner *owner, InternID id) {
    InternEntry *e = (InternEntry*)array_get(&owner->ctx->entries, (size_t)id);
    if (owner->serial && e->holder == owner->serial) return id;
    e->ref_count++;
    e->holder = owner->serial;
    array_push(&owner->owned_ids, &id, owner->ctx->sys);
    return id;
}

InternID intern_string(InternOwner *owner, StringView sv) {
    InternCtx *ctx = owner->ctx;
    if (sv.length == 1 && (unsigned char)sv.start[0]) return (InternID)(unsigned char)sv.start[0];
    if (sv.length == 0) return TOK_EMPTYSTRING;

#ifdef INTERN_HASH_SUPPORT
    unsigned int h = hash_bytes(sv.start, sv.length);
    if (ctx->hash_slots) {
        InternID found = hash_lookup(ctx, h, sv.start, sv.length, NULL);
        if (found >= 0) return intern_hold(owner, found);
    }
#else
    for (size_t i = 0; i < ctx->entries.size; ++i) {
        InternEntry *e = (InternEntry*)array_get(&ctx->entries, i);
        if (!e->data || e->len != sv.length) continue;
        size_t k = 0;
        while (k < sv.length && e->data[k] == sv.start[k]) k++;
        if (k == sv.length) return intern_hold(owner, (InternID)i);
    }
#endif

    InternEntry new_e;
    new_e.len = sv.length;
    new_e.ref_count = 1;
    new_e.holder = owner->serial;
    new_e.data = (char*)S_MALLOC(ctx->sys, sv.length + 1);
    ctx->sys->memcpy(new_e.data, sv.start, sv.length);
    new_e.data[sv.length] = '\0';
    
    InternID ret_id;
    if (ctx->free_ids.size > 0) {
        ret_id = ((InternID*)ctx->free_ids.data)[--ctx->free_ids.size];
        array_set(&ctx->entries, (size_t)ret_id, &new_e);
    } else {
        ret_id = (InternID)ctx->entries.size;
        // Pad past the fixed-token block so a dynamic id never collides with one.
        while (ret_id < TOK_COUNT) {
             InternEntry pad = { NULL, 0, 0, 0 };
             array_push(&ctx->entries, &pad, ctx->sys);
             ret_id++;
        }
        
        array_push(&ctx->entries, &new_e, ctx->sys);
    }

#ifdef INTERN_HASH_SUPPORT
    hash_maybe_grow(ctx);
    hash_insert_id(ctx, h, ret_id);
#endif

    array_push(&owner->owned_ids, &ret_id, ctx->sys);
    return ret_id;
}

InternID intern_c_string(InternOwner *owner, const char *str) {
    if (!str) return -1;
    return intern_string(owner, sv_from_cstring(str));
}

void intern_string_at_fixed_id(InternCtx *ctx, InternID id, const char *str) {
    // Ensure array is big enough
    while ((InternID)ctx->entries.size <= id) {
        InternEntry empty = { NULL, 0, 0, 0 };
        array_push(&ctx->entries, &empty, ctx->sys);
    }
    
    InternEntry *e = (InternEntry*)array_get(&ctx->entries, id);
    if (e->data) {
        // Fixed ids are installed before any dynamic one, so an occupied slot
        // here is always a double-registration.
        if (ctx->sys->error) ctx->sys->error("intern_string_at_fixed_id: Slot validation failed (occupied)");
        return;
    }
    
    size_t len = 0; while(str[len]) len++;
    e->len = len;
    // No InternOwner ever tracks a fixed id, and release_id is only reached
    // through intern_owner_deinit -- so a count of 1 is never decremented and
    // the string is permanent.
    e->ref_count = 1;

    e->data = (char*)S_MALLOC(ctx->sys, len + 1);
    ctx->sys->memcpy(e->data, str, len);
    e->data[len] = '\0';

#ifdef INTERN_HASH_SUPPORT
    hash_maybe_grow(ctx);
    {
        unsigned int h = hash_bytes(e->data, e->len);
        hash_insert_id(ctx, h, id);
    }
#endif
}

StringView intern_get(InternCtx *ctx, InternID id) {
     if (id < 0 || id >= (InternID)ctx->entries.size) return sv_from_parts(NULL, 0);
     InternEntry *e = (InternEntry*)array_get(&ctx->entries, id);
     if (!e->data) return sv_from_parts(NULL, 0);
     return sv_from_parts(e->data, e->len);
}

const char *intern_get_cstr(InternCtx *ctx, InternID id) {
     if (id < 0 || id >= (InternID)ctx->entries.size) return "";
     InternEntry *e = (InternEntry*)array_get(&ctx->entries, id);
     if (!e->data) return "";
     return e->data;
}

void intern_init_fixed_tokens(InternCtx *ctx) {
     // The bytes (1..255) and the empty string (256) come from intern_ctx_init.
     for (int i = TOK_EMPTYSTRING + 1; i < TOK_COUNT; ++i) {
         if (FixedTokenNames[i]) {
             intern_string_at_fixed_id(ctx, (InternID)i, FixedTokenNames[i]);
         }
     }
}
