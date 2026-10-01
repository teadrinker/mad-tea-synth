#ifndef PARSER_H
#define PARSER_H

/*

Initially ported from https://teadrinker.net/spine

The parser is inspired by JSON and designed to have a generic/loose syntax.
It uses a single ASTNode output struct, and leaves validation / interpretation
up to the next abstraction level. 

Like in CoffeeScript, argument parenthesis can be omitted. 

Like Lua, there is a single a container structure, supporting order and optional keys.
Brackets () [] {}, all have the same behaviour, except () for items <= 1,
which are special to support infix notation etc.


Main changes from js version:
  * Handling number types (ASTNode::number_flags)
  * Mapping from text input character index -> output ASTNode (used for syntax highlighting)
  * ASI (Automatic Semicolon Insertion) Support (not relevant for Spine as lines are already separated earlier)
  * More hardcoded (get_binary_op is C function rather than data)
  * Compound operators and more C alignment in general 
  * A hack to detect typical C contructions ( x() {} )
    to make it easier for compiler to digest, I see this
    as a temporary hack, and plan to allow cleaner syntax
    later.


*/

#include "common/tsys.h"
#include "common/array.h"
#include "common/string_view.h"
#include "common/intern.h"
#include "tokens.h"

struct Arena; // Forward declaration

typedef struct Parser Parser;
struct Parser {
    InternCtx intern;
    InternOwner fixed_owner; // Owner for fixed tokens/joins
    struct JoinTable *joins; // token joins (`<` `=` -> `<=`), see token_iterator.h
    Tsys *sys;
};

void parser_init(Parser *p, Tsys *sys);
void parser_deinit(Parser *p);

typedef enum {
    NUM_NONE    = 0,
    NUM_INTEGER = 1,   // e.g. 4
    NUM_DOUBLE  = 2,   // e.g. 4.0
    NUM_FLOAT   = 3,   // e.g. 4f or 4.0f
    NUM_TYPE    = 7,   
    NUM_WAS_PARSED_NEGATIVE = 8,  
    NUM_EXPONENTIAL_FORM = 16,  
    NUM_DECIMAL_FORM = 32,
    NUM_EXACT_I64 = 64,  // integer literal past 2^53: number_i holds it exactly, number only approximately
} NumberFlags;

typedef struct ASTNode ASTNode;
struct ASTNode {
    InternID token; // Usually an operator or identifier
    
    double number;
    long long number_i;       // valid only under NUM_EXACT_I64; a writer of `number` must clear that flag
    NumberFlags number_flags; // NUM_NONE when not a numeric literal
    InternID string;   // cases like "Hello world" and {"key with spaces" : 10} 

    union {
        InternID number_as_token; // note: sign is parsed separately (number_flags & NUM_WAS_PARSED_NEGATIVE)
        InternID string_quoting; // " or '
    };
    

    ASTNode *left;
    ASTNode *right;
    ASTNode *parent; 
    Array items; // Array of ASTNode*
    
    InternID ws1;
    InternID ws2;
    
    InternID left_bracket;
    InternID right_bracket;
    InternID ws_empty_bracket;
        
    InternID divider; 
    
    int is_literal; // a number or string literal, as opposed to a name or operator
};

static inline int ast_has_exact_i64(const ASTNode *n) {
    return n && (n->number_flags & NUM_EXACT_I64) && (n->number_flags & NUM_TYPE) == NUM_INTEGER;
}

typedef struct {
    ASTNode *code_tree; // ASTNode with items (statements); no left/right bracket
    InternID error;     // 0 on success; otherwise an interned error message
    int error_row;      // 0-based line of the parse error, -1 if unknown
    int error_col;      // 0-based column of the parse error, -1 if unknown
    struct Arena *arena;
    InternOwner owner; 
    ASTNode **txt_to_ref;   // optional PARSE_OUTPUT_REFS: per-position mapping (1 slot per char of unparsed output)
    size_t  num_txt_to_ref; 
    size_t *line_offsets;   // optional PARSE_OUTPUT_LINE_OFFSETS: byte offsets of each \n in unparsed output
    int num_lines_offsets;
} ParseResult;

#define PARSE_VERIFY              (1<<0)
#define PARSE_OUTPUT_REFS         (1<<1)
#define PARSE_OUTPUT_LINE_OFFSETS (1<<2)
// Automatic statement termination (ASI) is ON by default: a whitespace block
// containing a linefeed terminates an expression wherever it would otherwise
// chain via implicit call / item continuation. Parens ( ... ) suppress it;
// braces/brackets keep it on. Round-trip safe. Pass PARSE_DISABLE_ASI to opt out
// and get the legacy behavior where newlines chain as implicit calls.
#define PARSE_DISABLE_ASI  (1<<3)
// Indentation blocks are ON by default: a run of lines indented deeper than the
// statement before them becomes that statement's block, the same shape a
// `{ ... }` body produces. Depends on ASI, so PARSE_DISABLE_ASI turns it off
// too. Round-trip safe. Pass PARSE_DISABLE_INDENT_BLOCKS to opt out.
#define PARSE_DISABLE_INDENT_BLOCKS (1<<4)
// One-line blocks are ON by default: `if c then stmt`, `while c do stmt` and
// `if c then a else b`, the single-statement twin of a `{ ... }` body. Depends
// on ASI too -- the end of the line is what closes the block. Round-trip safe.
// Pass PARSE_DISABLE_ONE_LINE_BLOCKS to opt out and get the legacy behavior
// where `then`, `do` and `else` chain as ordinary implicit-call arguments.
#define PARSE_DISABLE_ONE_LINE_BLOCKS (1<<5)

ParseResult parse_to_asts(Parser *p, const char *txt, int flags); 
char *unparse_asts(Parser *p, ParseResult *res);
char *unparse_expression(Parser *p, ASTNode *node);

// Debug visualization: returns a multi-line string. Row 0 is the full
// unparse of `node` (same as unparse_expression). Each subsequent row
// shows the nodes at that depth, horizontally aligned to the column of
// their primary token in row 0. The given `node` is depth 1.
// Caller owns the returned string (free via sys->free).
char *parser_debugrows(Parser *p, ASTNode *node);

// Returns NULL when there is no error. The returned C string lives as
// long as the ParseResult (it is interned in res->owner).
const char *parser_get_error(ParseResult *res);

void free_parse_result(Parser *p, ParseResult *res); 
void free_ast_node(ASTNode *node, Tsys *sys);

#endif
