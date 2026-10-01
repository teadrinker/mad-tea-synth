#ifndef TOKEN_ITERATOR_H
#define TOKEN_ITERATOR_H

#include <stddef.h>
#include "string_view.h"
#include "array.h"
#include "tsys.h"
#include "intern.h"

// String utils (modified for SV)
char *c_strdup(const char *s, Tsys *sys);
int c_isdigit(int c);
int c_isalpha(int c);
int c_iswhite(int c);

// Tokenizer & Iterator
struct Arena; // Forward decl

// Packed token position: line in the high 32 bits, col in the low 32 (a data
// table on one line easily passes 64K columns). Both are 0-based.
typedef unsigned long long TokenPos;

#define TOKENPOS_LINE(pos)  ((int)((pos) >> 32))
#define TOKENPOS_COL(pos)   ((int)((pos) & 0xFFFFFFFFu))
#define TOKENPOS_MAKE(l,c)  ((TokenPos)(((unsigned long long)(unsigned int)(l) << 32) | (unsigned int)(c)))

typedef struct {
    Array tokens;    // Array of InternID
    Array positions; // Array of TokenPos (parallel to tokens, one per token)
    size_t pos;
    int parsed_new_line;
    int nl_term_mode;   // ASI enabled for this parse (on unless PARSE_DISABLE_ASI)
    int nl_suppressed;  // saved/restored per bracket; nonzero inside ( ... )
    int indent_mode;    // indentation blocks enabled for this parse
    int oneline_mode;   // `then` / `do` one-line blocks enabled for this parse
    int stmt_col;       // column of the current statement's first token, -1 outside one
    int in_oneline_block; // parsing the body of a `then` / `do` block, where an
                          // `else` closes the body rather than chaining onto it
    int line_indent;       // indentation of the line the next token sits on
    int line_indent_valid; // 0 when that line has no meaningful indentation
    int depth;             // parse_expression_r recursion depth
    InternID ws; // current whitespace (interned)
    Tsys *sys;
    InternCtx *intern;
    struct Arena *arena;
    InternOwner *owner;
    size_t wsp; 
    InternID error; // 0 if no error has occurred; first error wins.
    int error_row;  // 0-based line of the error, -1 if unknown
    int error_col;  // 0-based col of the error, -1 if unknown
} TokenIterator;

TokenIterator create_token_iterator(Array tokens, Array positions, struct Arena *arena, Tsys *sys, InternCtx *intern, InternOwner *owner);
InternID ti_peek(TokenIterator *it);
InternID ti_raw_peek(TokenIterator *it, int offset);
InternID ti_proceed(TokenIterator *it, int only_ws); 
void ti_proceed_ignore_ws(TokenIterator *it, int only_ws);
InternID ti_parse_quoted_string(TokenIterator *it); 
InternID ti_simple_get(TokenIterator *it); 

size_t ti_get_token_pos(TokenIterator *it);
void ti_set_token_pos(TokenIterator *it, size_t pos);

// True when the newline-terminator feature is on, we are not inside parens,
// and the whitespace consumed since the last real token contained a linefeed.
// Callers must have done a ti_peek() first so the ws block has been scanned.
int ti_linefeed_terminates(TokenIterator *it);

// Indentation of the line the next token sits on: the column of that line's
// first content, be it the token or a comment that opens on the line.
// Returns 0 when there is none to speak of -- no linefeed since the last
// token, or the line opened inside a block comment that began earlier, whose
// closing marker says nothing about how the line is indented.
// Callers must have done a ti_peek() first so the ws block has been scanned.
int ti_line_indent(TokenIterator *it, int *out_indent);

// Current token position (line/col) for error reporting.
int ti_current_line(TokenIterator *it);
int ti_current_col(TokenIterator *it);

void ti_destroy(TokenIterator *it);

// Joins merge adjacent tokens into one (`<` `=` -> `<=`), keyed by the one
// byte that completes them.
typedef struct JoinTable JoinTable;
JoinTable *create_join_table(const char **joins, InternOwner *owner, Tsys *sys);
void free_join_table(JoinTable *t, Tsys *sys);

// Tokenizes input into tokens + positions.
// If out_positions is non-NULL, the positions array is initialized and filled.
// positions[i] maps to tokens[i] and is packed as TOKENPOS_MAKE(line,col).
Array parse_to_tokens(const char *input, struct Arena *arena, Tsys *sys, InternOwner *owner, const JoinTable *joins, Array *out_positions);

#endif
