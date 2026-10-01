#include "token_iterator.h"
#include "arena.h"
#include "string_pure.h"
#include "parser/tokens.h"

// --- String Utils (Modified) ---

int c_isdigit(int c) {
    return c >= '0' && c <= '9';
}

int c_isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

int c_iswhite(int c) {
    return c == 32 || c == 10 || c == 13 || c == 9;
}

// --- Tokenizer Implementation ---

static int is_symbol(int c) {
    return c_isdigit(c) || c_isalpha(c);
}

struct JoinTable {
    InternID *pairs;              // (A, result) pairs, grouped by B
    unsigned short start[257];    // byte B's pairs are [start[B], start[B + 1])
};

// The token A + B joins into, or 0. B is always one byte.
static InternID try_join(const JoinTable *t, InternID a, unsigned char b) {
    for (int i = t->start[b]; i < t->start[b + 1]; i++)
        if (t->pairs[2 * i] == a) return t->pairs[2 * i + 1];
    return 0;
}

JoinTable *create_join_table(const char **joins, InternOwner *owner, Tsys *sys) {
    JoinTable *t = (JoinTable *)S_MALLOC(sys, sizeof(JoinTable));
    sys->memset(t, 0, sizeof(*t));
    int n = 0;
    for (const char **p = joins; p && *p; p++) if (s_strlen(*p) >= 2) n++;
    t->pairs = (InternID *)S_MALLOC(sys, sizeof(InternID) * 2 * (size_t)(n ? n : 1));
    // Counting sort on the last byte: count, prefix sum, then place.
    for (const char **p = joins; p && *p; p++) {
        size_t len = s_strlen(*p);
        if (len >= 2) t->start[(unsigned char)(*p)[len - 1] + 1]++;
    }
    for (int b = 0; b < 256; b++) t->start[b + 1] += t->start[b];
    unsigned short fill[256];
    for (int b = 0; b < 256; b++) fill[b] = t->start[b];
    for (const char **p = joins; p && *p; p++) {
        const char *s = *p;
        size_t len = s_strlen(s);
        if (len < 2) continue;
        int i = fill[(unsigned char)s[len - 1]]++;
        t->pairs[2 * i]     = intern_string(owner, sv_from_parts(s, len - 1));
        t->pairs[2 * i + 1] = intern_c_string(owner, s);
    }
    return t;
}

void free_join_table(JoinTable *t, Tsys *sys) {
    if (!t) return;
    S_FREE(sys, t->pairs);
    S_FREE(sys, t);
}

// Advance line/col tracking over a span of raw source characters.
static void advance_pos(const char *src, size_t start, size_t end, int *line, int *col) {
    for (size_t i = start; i < end; i++) {
        if (src[i] == '\n') { (*line)++; *col = 0; }
        else if (src[i] != '\r') { (*col)++; }
    }
}

Array parse_to_tokens(const char *input, Arena *arena, Tsys *sys, InternOwner *owner, const JoinTable *joins, Array *out_positions) {
    Array tokens;
    array_init(&tokens, sizeof(InternID));
    if (out_positions) array_init(out_positions, sizeof(TokenPos));
    
    if (!input) return tokens;
    
    const char *src = input;
    size_t len = s_strlen(src);

    // Track line/col as we scan.  Assumed to start at (0,0).
    int line = 0, col = 0;
    // pos_scan is the last position we've tracked through
    size_t pos_scan = 0;
    
    size_t i = 0;
    
    while (i < len) {
        // Sync line/col tracking up to i
        advance_pos(src, pos_scan, i, &line, &col);
        pos_scan = i;
        
        int tok_line = line;
        int tok_col  = col;
        
        size_t start = i;
        unsigned char c = (unsigned char)src[i];
        
        if (c_isdigit(c)) {
            while (i < len && c_isdigit((unsigned char)src[i])) i++;
            if (i < len && src[i] == '.') {
                 if (i+1 < len && (c_isdigit((unsigned char)src[i+1]) || src[i+1] == 'e' || src[i+1] == 'E')) {
                     i++; 
                     while (i < len && c_isdigit((unsigned char)src[i])) i++; 
                 }
            }
            if (i < len && (src[i] == 'e' || src[i] == 'E')) {
                size_t e_pos = i;
                i++;
                if (i < len && (src[i] == '+' || src[i] == '-')) i++;
                size_t param_start = i;
                while (i < len && c_isdigit((unsigned char)src[i])) i++;
                if (i == param_start) i = e_pos;
            }
        } else if (is_symbol(c)) {
             while (i < len && is_symbol((unsigned char)src[i])) i++;
        } else {
             i++;
        }
        
        // Sync line/col tracking through this token's characters
        advance_pos(src, pos_scan, i, &line, &col);
        pos_scan = i;
        
        size_t tok_len = i - start;
        // A one-byte token is its own id (tokens.h). Only such a token can
        // complete a join, and only a byte some join ends with.
        InternID new_id = tok_len == 1 ? (InternID)c : intern_string(owner, sv_from_parts(src + start, tok_len));
        int may_join = tok_len == 1 && joins && joins->start[c] != joins->start[c + 1];

        // Record position before any merge
        TokenPos tpos = TOKENPOS_MAKE(tok_line, tok_col);

        // Merge check
        if (may_join && tokens.size > 0) {
            InternID *last = (InternID*)array_back(&tokens);
            InternID merged = try_join(joins, *last, c);
            
            if (merged != 0) {
                *last = merged; // Replace last (position stays from first token of the merge)
                continue; // Skip push
            }
        }
        
        array_push(&tokens, &new_id, sys);
        if (out_positions) array_push(out_positions, &tpos, sys);
    }
    
    return tokens;
}

// --- Iterator Implementation ---

static void parse_internal(TokenIterator *it) {
    if (it->ws != 0) return;
    InternCtx *ctx = it->intern; 
    if (!ctx) return; 
    
    Array *tokens = &it->tokens;
    size_t scan_pos = it->pos;
    size_t total_len = 0;
    int count = 0;

    // Indentation of the line the upcoming token sits on, for the indent-block
    // rule. `run` counts whitespace since the last linefeed; `valid` says a
    // linefeed has been seen with nothing but whitespace after it, so the next
    // item is this line's first content; `committed` means that first content
    // was a comment, which fixes the indent even though the token itself sits
    // further right. A block comment carrying a linefeed clears both: whatever
    // trails its closing marker is a continuation, not a line start.
    // `mixed` marks a leading run holding both tabs and spaces: a tab counts as
    // one column here, so mixing the two would measure an indent nobody can see.
    // Such a line makes no indentation decision at all, leaving it a plain
    // sibling statement as it was before this syntax existed.
    int run = 0, valid = 0, committed = 0, indent = 0;
    int saw_tab = 0, saw_space = 0, mixed = 0;

    // Pass 1: scan for whitespace block
    while (scan_pos < tokens->size) {
        InternID *id_ptr = (InternID*)array_get(tokens, scan_pos);
        // The tokenizer emits whitespace one byte per token.
        InternID id = *id_ptr;
        int wc = (id == ' ' || id == '\n' || id == '\t' || id == '\r') ? id : 0;
        if (wc) {
            if (wc == '\n') {
                it->parsed_new_line = 1;
                run = 0; valid = 1; committed = 0;
                saw_tab = saw_space = mixed = 0;
            } else if (wc != '\r') {
                if (valid && !committed) {
                    if (wc == '\t') saw_tab = 1; else saw_space = 1;
                    if (saw_tab && saw_space) mixed = 1;
                }
                run++;
            }
            total_len++;
            count++;
            scan_pos++;
            continue;
        }
        if (id != TOK_LINE_COMMENT && id != TOK_BLOCK_OPEN) break;
        StringView sv = intern_get(ctx, *id_ptr);

        if (id == TOK_LINE_COMMENT) {
            // Line comment
            if (valid && !committed) { indent = run; committed = 1; }
            total_len += sv.length;
            count++;
            scan_pos++;
            
            while(scan_pos < tokens->size) {
                InternID *t = (InternID*)array_get(tokens, scan_pos);
                StringView tsv = intern_get(ctx, *t);
                if (*t == '\n') break;
                total_len += tsv.length;
                count++;
                scan_pos++;
            }
            if (scan_pos < tokens->size) {
                InternID *t = (InternID*)array_get(tokens, scan_pos); // \n
                StringView tsv = intern_get(ctx, *t);
                total_len += tsv.length;
                count++;
                
                it->parsed_new_line = 1;
                run = 0; valid = 1; committed = 0;
                saw_tab = saw_space = mixed = 0;

                scan_pos++;
            }
        } else if (id == TOK_BLOCK_OPEN) {
            // Block comment
            if (valid && !committed) { indent = run; committed = 1; }
            int had_nl = 0;
            total_len += sv.length;
            count++;
            scan_pos++;
            
            while(scan_pos < tokens->size) {
                InternID *t = (InternID*)array_get(tokens, scan_pos);
                StringView tsv = intern_get(ctx, *t);
                if (*t == TOK_BLOCK_CLOSE) break;
                if (*t == '\n') { it->parsed_new_line = 1; had_nl = 1; }
                total_len += tsv.length;
                count++;
                scan_pos++;
            }
            if (scan_pos < tokens->size) {
                InternID *t = (InternID*)array_get(tokens, scan_pos); // */
                StringView tsv = intern_get(ctx, *t);
                total_len += tsv.length;
                count++;
                scan_pos++;
            }
            if (had_nl) { valid = 0; committed = 0; run = 0; saw_tab = saw_space = mixed = 0; }
        }
    }
    
    // Only recorded when this call actually scanned something: after a
    // ti_proceed(only_ws) the whitespace is gone from it->ws but pos still sits
    // on the same token, and the re-scan must not wipe what it described. Same
    // rule as parsed_new_line, which is likewise only ever set here.
    if ((committed || valid) && !mixed) {
        it->line_indent = committed ? indent : run;
        it->line_indent_valid = 1;
    }

    if (count == 0) {
        // The empty string rather than 0 -- 0 is TOK_NULL, "no whitespace scanned".
        it->ws = TOK_EMPTYSTRING;
        return;
    }
    
    // Optimize: if count is 1, just return the ID!
    if (count == 1) {
        InternID *tok = (InternID*)array_get(tokens, it->pos);
        it->ws = *tok;
        it->pos++;
        return;
    }
    
    // Consolidate
    char *buf = (char*)arena_alloc(it->arena, total_len + 1);
    size_t curr = 0;
    
    for(size_t i = it->pos; i < scan_pos; i++) {
        InternID *id_ptr = (InternID*)array_get(tokens, i);
        StringView sv = intern_get(ctx, *id_ptr);
        for(size_t k=0; k<sv.length; k++) buf[curr++] = sv.start[k];
    }
    buf[curr] = '\0';
    
    it->ws = intern_c_string(it->owner, buf); // Intern the aggregated whitespace
    it->pos = scan_pos;
}

TokenIterator create_token_iterator(Array tokens, Array positions, struct Arena *arena, Tsys *sys, InternCtx *intern, InternOwner *owner) {
    TokenIterator it;
    it.tokens = tokens;
    it.positions = positions;
    it.pos = 0;
    it.parsed_new_line = 0;
    it.nl_term_mode = 0;
    it.nl_suppressed = 0;
    it.indent_mode = 0;
    it.oneline_mode = 0;
    it.stmt_col = -1;
    it.in_oneline_block = 0;
    it.line_indent = 0;
    it.line_indent_valid = 0;
    it.depth = 0;
    it.ws = 0;
    it.sys = sys;
    it.intern = intern;
    it.arena = arena;
    it.owner = owner;
    it.wsp = 0;
    it.error = 0;
    it.error_row = -1;
    it.error_col = -1;
    return it;
}

InternID ti_peek(TokenIterator *it) {
    parse_internal(it);
    if (it->pos >= it->tokens.size) return 0; 
    return *(InternID*)array_get(&it->tokens, it->pos);
}

InternID ti_raw_peek(TokenIterator *it, int offset) {
    long idx = (long)it->pos + offset;
    if (idx < 0 || idx >= (long)it->tokens.size) return 0;
    return *(InternID*)array_get(&it->tokens, idx);
}

InternID ti_proceed(TokenIterator *it, int only_ws) {
    parse_internal(it);
    if (!only_ws) {
        it->pos++;
        it->parsed_new_line = 0;
        it->line_indent_valid = 0;
    }
    it->wsp = it->pos;

    InternID res = it->ws;
    it->ws = 0; // Reset
    
    return res;
}

void ti_proceed_ignore_ws(TokenIterator *it, int only_ws) {
    parse_internal(it);
    if (!only_ws) {
        it->pos++;
        it->parsed_new_line = 0;
        it->line_indent_valid = 0;
    }
    it->wsp = it->pos;
    it->ws = 0;
}

InternID ti_simple_get(TokenIterator *it) {
    if (it->pos >= it->tokens.size) return 0;
    InternID *tok = (InternID*)array_get(&it->tokens, it->pos);
    it->pos++;
    it->wsp = it->pos;
    return *tok;
}

static void append_helper(Tsys *sys, char **buf, size_t *cap, size_t *size, const char *s, size_t l) {
    if (*size + l >= *cap) {
        *cap = (*cap == 0) ? 64 : (*cap * 2);
        if (*cap < *size + l + 1) *cap = *size + l + 64;
        *buf = (char *)S_REALLOC(sys, *buf, *cap);
    }
    for(size_t i=0; i<l; i++) (*buf)[(*size)++] = s[i];
}

InternID ti_parse_quoted_string(TokenIterator *it) {
    InternID end_token_id = ti_simple_get(it); 
    if (!end_token_id) return 0;
    
    InternCtx *ctx = it->intern;
    (void)intern_get(ctx, end_token_id);

    size_t cap = 64;
    size_t size = 0;
    char *buf = (char *)S_MALLOC(it->sys, cap);
    
    // append_helper(it->sys, &buf, &cap, &size, end_token.start, end_token.length); // Removed start quote
    
    while (1) {
        InternID t_id = ti_simple_get(it);
        StringView t = intern_get(ctx, t_id);
        
        if (!t.start) {
            if (it->sys->error) it->sys->error("Missing end of quote");
            if (!it->error) {
                it->error = intern_c_string(it->owner, "Missing end of quote");
                it->error_row = ti_current_line(it);
                it->error_col = ti_current_col(it);
            }
            break;
        } else if (t_id == '\\') {
             InternID next_id = ti_simple_get(it);
             StringView next = intern_get(ctx, next_id);
             
             append_helper(it->sys, &buf, &cap, &size, t.start, t.length);
             append_helper(it->sys, &buf, &cap, &size, next.start, next.length);
        } else if (t_id == end_token_id) {
             // append_helper(it->sys, &buf, &cap, &size, t.start, t.length); // Removed end quote
             break;
        } else {
             append_helper(it->sys, &buf, &cap, &size, t.start, t.length);
        }
    }
    
    if (size >= cap) {
        buf = (char*)S_REALLOC(it->sys, buf, size+1);
    }
    buf[size] = '\0';
    
    InternID ret = intern_c_string(it->owner, buf);
    
    S_FREE(it->sys, buf);
    
    return ret;
}

size_t ti_get_token_pos(TokenIterator *it) {
    return it->wsp;
}

void ti_set_token_pos(TokenIterator *it, size_t pos) {
    it->pos = pos;
    it->wsp = pos;
    it->ws = 0;
    // The abandoned lookahead may have set parsed_new_line while scanning ws
    // past the restore point. Clear it; the saved pos points at the start of
    // the whitespace block, so the next ti_peek re-scan recomputes it.
    it->parsed_new_line = 0;
    it->line_indent_valid = 0;
}

int ti_linefeed_terminates(TokenIterator *it) {
    return it->nl_term_mode && !it->nl_suppressed && it->parsed_new_line;
}

int ti_line_indent(TokenIterator *it, int *out_indent) {
    if (!it->line_indent_valid) return 0;
    *out_indent = it->line_indent;
    return 1;
}

void ti_destroy(TokenIterator *it) {
    array_free(&it->tokens, it->sys);
    array_free(&it->positions, it->sys);
}

// ---- position accessors ----

static TokenPos ti_get_pos_at(TokenIterator *it, size_t idx) {
    if (!it->positions.data || idx >= it->positions.size) return 0;
    return *(TokenPos*)array_get(&it->positions, idx);
}

int ti_current_line(TokenIterator *it) {
    return TOKENPOS_LINE(ti_get_pos_at(it, it->pos));
}

int ti_current_col(TokenIterator *it) {
    return TOKENPOS_COL(ti_get_pos_at(it, it->pos));
}

char *c_strdup(const char *s, Tsys *sys) {
    char *d = (char *)S_MALLOC(sys, s_strlen(s) + 1);
    s_strcpy(d, s);
    return d;
}
