
#include "parser.h"
#include "tokens.h"
#include "common/token_iterator.h"
#include "common/arena.h"
#include "common/string_pure.h"


// --- Helper Types & Constants ---

// Recursion guards, well inside a 1MB stack even in a debug build: nested
// brackets / prefix operators, and the depth of the finished tree.
#ifndef PARSE_MAX_NESTING
#define PARSE_MAX_NESTING 500
#endif
#ifndef PARSE_MAX_DEPTH
#define PARSE_MAX_DEPTH 2000
#endif

typedef struct {
    int prec;
    int is_right_assoc;
    int found;
} OpInfo;

typedef struct {
    InternID end_id; 
    int prec;
} BracketInfo;

// --- Helper Functions ---

static ASTNode *create_node(Arena *arena, Tsys *sys) {
    ASTNode *n = (ASTNode*)arena_alloc(arena, sizeof(ASTNode));
    sys->memset(n, 0, sizeof(ASTNode));
    return n;
}

// An integer literal past 2^53 keeps its exact value beside the rounded double.
// Digits beyond the i64 range get no flag; the VM refuses them by magnitude.
static void number_set_exact_i64(ASTNode *e, const char *digits, int neg) {
    if (!digits || !digits[0]) return;
    unsigned long long u = 0;
    for (int i = 0; digits[i]; i++) {
        if (!c_isdigit((unsigned char)digits[i])) return;
        unsigned d = (unsigned)(digits[i] - '0');
        if (u > (0xFFFFFFFFFFFFFFFFULL - d) / 10) return;
        u = u * 10 + d;
    }
    if (u <= (1ULL << 53)) return;
    if (u > (neg ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL)) return;
    e->number_i = neg ? -(long long)(u - 1) - 1 : (long long)u;
    e->number_flags = (NumberFlags)(e->number_flags | NUM_EXACT_I64);
}

// The value and flags of a numeric literal spelled `nstr` (digits, optional
// fraction and exponent; the sign and an `f` suffix are separate tokens).
static void set_number(ASTNode *e, InternID tok, const char *nstr, int neg, int has_f) {
    int is_double = 0, has_exp = 0;
    for (int i = 0; nstr[i]; i++) {
        if (nstr[i] == '.') is_double = 1;
        if (nstr[i] == 'e' || nstr[i] == 'E') { is_double = 1; has_exp = 1; break; }
    }
    double val = s_to_number(nstr);
    e->number = neg ? -val : val;
    e->number_flags = (has_f ? NUM_FLOAT : (is_double ? NUM_DOUBLE : NUM_INTEGER)) |
                      (has_exp ? NUM_EXPONENTIAL_FORM : ((has_f || is_double) ? NUM_DECIMAL_FORM : 0 )) |
                      (neg ? NUM_WAS_PARSED_NEGATIVE : 0);
    if (!has_f && !is_double) number_set_exact_i64(e, nstr, neg);
    e->number_as_token = tok;
}

// Prefix operators all sit at one tier, 21.
//  - `/x` is the reciprocal. Only ever reached in operand position: the binary
//    loop never guesses it from spacing, so `w /2` stays a division.
//  - Prefix ++/--. The postfix forms are handled in parse_expression_r's
//    operator loop (they take a left operand, which a prefix by definition
//    doesn't have).
//  - `...x`, the spread. At the unary tier so the operand still takes `.` (23),
//    a subscript (23) and an implicit call (22) -- `...a.b`, `...a[0]` and
//    `...g(1)[2]` all bind into the splat -- while `...x + 1` parses as
//    `(...x) + 1` and is refused by the compiler with a real message.
static OpInfo get_unary_op(InternID t) {
    OpInfo op = {0, 0, 0};
    if (t == TOK_BANG || t == TOK_MINUS || t == TOK_PLUS || t == TOK_SLASH
        || TOKEN_IS_INC_DEC(t) || t == TOK_DOTDOTDOT) { op.prec = 21; op.found = 1; }
    return op;
}

// The implicit call, `f x`, a juxtaposition written with no operator at all.
#define PREC_CALL 22

// { prec, is_right_assoc, found }, indexed by token.
//  - 'as' binds tighter than every binary arithmetic operator (matching Rust,
//    where `as` sits above `* / %`, `+ -`, etc.) -- '**' is just another
//    operator in that tier, so it follows the same rule: `x ** 2 as f64` reads
//    as `x ** (2 as f64)`, exactly like `a * b as f64` reads as
//    `a * (b as f64)`. Right-associative so a ** b ** c == a ** (b ** c).
//  - Concatenation `~`, at Lua's `..` level: below `+ -` so `"n = " ~ a + b`
//    joins the *sum*, above the comparisons so `"n = " ~ n == s` compares the
//    joined string. Left-associative, so a chain reopens one buffer
//    (vm.c strb_reopen).
//  - Ranges, at Rust's level: below every arithmetic and comparison operator,
//    so `0..n-1` is `0..(n-1)` and needs no parens. `in` sits below the range
//    it binds, and above `,` so `for(i in 0..n)` survives a paren scope.
//  - `a <- b`, the reactive soft write, is shaped like an assignment.
static const OpInfo binary_ops[TOK_COUNT] = {
    [TOK_EMPTYSTRING]       = { PREC_CALL, 0, 1 },
    [TOK_DOT]               = { 23, 0, 1 },
    [TOK_AS]                = { 20, 0, 1 },
    [TOK_STARSTAR]          = { 19, 1, 1 },
    [TOK_MUL]               = { 18, 0, 1 },
    [TOK_SLASH]             = { 18, 0, 1 },
    [TOK_PERCENT]           = { 18, 0, 1 },
    [TOK_DOUBLE_PERCENT]    = { 18, 0, 1 },
    [TOK_SLASH_TILDE]       = { 18, 0, 1 },
    [TOK_SLASH_PERCENT]     = { 18, 0, 1 },
    [TOK_PLUS]              = { 17, 0, 1 },
    [TOK_MINUS]             = { 17, 0, 1 },
    [TOK_TILDE]             = { 16, 0, 1 },
    [TOK_LSHIFT]            = { 15, 0, 1 },
    [TOK_RSHIFT]            = { 15, 0, 1 },
    [TOK_URSHIFT]           = { 15, 0, 1 },
    [TOK_LT]                = { 14, 0, 1 },
    [TOK_LT_EQ]             = { 14, 0, 1 },
    [TOK_GT]                = { 14, 0, 1 },
    [TOK_GT_EQ]             = { 14, 0, 1 },
    [TOK_EQ_EQ]             = { 13, 0, 1 },
    [TOK_NOT_EQ]            = { 13, 0, 1 },
    [TOK_EQ_EQ_EQ]          = { 13, 0, 1 },
    [TOK_NOT_EQ_EQ]         = { 13, 0, 1 },
    [TOK_AMP]               = { 12, 0, 1 },
    [TOK_CARET]             = { 11, 0, 1 },
    [TOK_PIPE]              = { 10, 0, 1 },
    [TOK_AND_AND]           = {  9, 0, 1 },
    [TOK_ARROW_R]           = {  9, 0, 1 },
    [TOK_OR_OR]             = {  8, 0, 1 },
    [TOK_QMARK]             = {  7, 1, 1 },
    [TOK_COLON]             = {  7, 1, 1 },
    [TOK_ARROW_F]           = {  6, 1, 1 },
    [TOK_DOTDOT]            = {  5, 0, 1 },
    [TOK_DOTDOT_EQ]         = {  5, 0, 1 },
    [TOK_BIGARROW_R]        = {  4, 0, 1 },
    [TOK_EQ]                = {  3, 1, 1 },
    [TOK_PLUS_EQ]           = {  3, 1, 1 },
    [TOK_MINUS_EQ]          = {  3, 1, 1 },
    [TOK_MUL_EQ]            = {  3, 1, 1 },
    [TOK_SLASH_EQ]          = {  3, 1, 1 },
    [TOK_PERCENT_EQ]        = {  3, 1, 1 },
    [TOK_DOUBLE_PERCENT_EQ] = {  3, 1, 1 },
    [TOK_SLASH_TILDE_EQ]    = {  3, 1, 1 },
    [TOK_SLASH_PERCENT_EQ]  = {  3, 1, 1 },
    [TOK_TILDE_EQ]          = {  3, 1, 1 },
    [TOK_AMP_EQ]            = {  3, 1, 1 },
    [TOK_PIPE_EQ]           = {  3, 1, 1 },
    [TOK_CARET_EQ]          = {  3, 1, 1 },
    [TOK_LSHIFT_EQ]         = {  3, 1, 1 },
    [TOK_RSHIFT_EQ]         = {  3, 1, 1 },
    [TOK_ARROW_L]           = {  3, 1, 1 },
    [TOK_IN]                = {  2, 0, 1 },
    [TOK_COMMA]             = {  0, 0, 1 },
};

static OpInfo get_binary_op(InternID t) {
    OpInfo none = {0, 0, 0};
    return (t > 0 && t < TOK_COUNT) ? binary_ops[t] : none;
}

static BracketInfo get_bracket_info(InternID t) {
    BracketInfo b = {0, 0};
    // The closing tokens run parallel to the opening ones (see tokens.h).
    if (TOKEN_IS_OPEN_BRACKET(t)) { b.end_id = (InternID)TOKEN_CLOSE_BRACKET_FOR(t); b.prec = 23; }
    return b;
}

static int is_end_bracket(InternID t) {
    return TOKEN_IS_CLOSE_BRACKET(t);
}

static int is_terminator(InternID t) {
    return t == TOK_SEMI || t == TOK_COMMA;
}

// A one-line block opener ends the paren-less argument it follows, the same way
// `,` does: in `if x > 0 then y = 1` the condition is `x > 0`, and
// without this the whole tail would be swallowed as further arguments of `0`.
// Handing the opener back to the level that owns the head is what makes the
// paren-less and parenthesised forms produce the same shape.
//
// Takes the iterator because that is feature-dependent: with one-line blocks
// off, `then` / `do` / `else` are ordinary words again and must chain as
// arguments, exactly as they did before the feature existed.
static int is_injected_bracket_terminator(TokenIterator *it, InternID t) {
    if (t == TOK_COMMA) return 1;
    return it->oneline_mode && (t == TOK_THEN || t == TOK_DO || t == TOK_ELSE);
}

// A one-line block opener: `then` and `do` open a block that the end of the
// statement closes, and `else` opens the second one on an `if`.  See TOK_THEN
// in tokens.h -- they are brackets, so they sit in the left_bracket slot.
static int is_oneline_opener(InternID t) {
    return t == TOK_THEN || t == TOK_DO;
}

// Could `t` begin an operand? Deliberately one-sided: it only rules out the
// tokens that definitely cannot, which is all the postfix-vs-prefix ++/--
// disambiguation in parse_expression_r needs.
static int can_start_operand(InternID t) {
    return t != 0 && !is_terminator(t) && !is_end_bracket(t);
}

// --- Parser Forward Decls ---

static ASTNode *parse_expression_r(TokenIterator *it, int prec, int is_functor_arg, int injected_bracket_scope);
static ASTNode *maybe_attach_indent_block(TokenIterator *it, ASTNode *stmt, int stmt_col);
static int stmt_indent_col(TokenIterator *it);
static ASTNode *parse_indent_block(TokenIterator *it, int body_col, int parent_col);
static ASTNode *attach_oneline_block(TokenIterator *it, ASTNode *head, InternID opener);

// Returns 1 when `call` has the C-style form  x() { ... }
// i.e. an implicit-call node whose left is itself an implicit-call ending
// in a tight parenthesised bracket, and whose right is a brace block that
// heads the expression (no further left child).
//
// ctx is needed to resolve ws1: ti_proceed always interns a string (even ""
// for no-whitespace), so ws1 is never a raw 0.  We distinguish tight `x(`
// from spaced `x (` by checking that the whitespace string has length == 0.
// `x () {}` is intentionally excluded per spec.
static int is_c_style_call(ASTNode *call, InternCtx *ctx) {
    if (!call || call->token != TOK_EMPTYSTRING) return 0;
    ASTNode *L = call->left, *R = call->right;
    if (!R) return 0;
    if (L->token != TOK_EMPTYSTRING) return 0;
    if (L->right->left_bracket != TOK_LPAREN) return 0;
    // Tight-paren check: ws1 must be an empty-string token (no actual whitespace).
    // A space between x and ( produces a ws1 whose interned string has length > 0.
    if (intern_get(ctx, L->right->ws1).length > 0) return 0;
    // Right must be the injected-bracket wrapper whose first item is a `{...}` block.
    // When a brace is parsed as an injected (bracketless) arg, parse_expression_r
    // returns a wrapper with left_bracket==0; the real {...} node lives in items[0]
    // and carries left_bracket==TOK_LBRACE.
    if (R->left != NULL) return 0;
    if (R->left_bracket != 0) return 0;       // must be the injected wrapper
    if (R->items.size == 0) return 0;
    ASTNode *first = *(ASTNode**)array_get(&R->items, 0);
    if (!first || first->left_bracket != TOK_LBRACE) return 0;
    return 1;
}

// A sub-scope attachment: `head { body }`, where the body is a block rather
// than an argument.  Built by giving the attachment node the NULL token
// instead of TOK_EMPTYSTRING, so the shape survives into the compiler --
// is_c_style_call is the trigger, this is the answer it records.
// Both tokens unparse to nothing, so the round-trip is unaffected.
//
// Returns the block, or NULL when `n` is not a sub-scope.  The block sits inside
// the injected-bracket wrapper the implicit-call arg was parsed into, and is
// either a real `{...}` or an indent block (empty-string brackets).  Requiring
// one of those on the right is what keeps this apart from the other NULL-token
// pair, `[N]type`, whose right side is an element-type name.
static int is_block_node(ASTNode *n) {
    if (!n) return 0;
    return n->left_bracket == TOK_LBRACE || n->left_bracket == TOK_EMPTYSTRING
        || n->left_bracket == TOK_THEN || n->left_bracket == TOK_DO
        || n->left_bracket == TOK_ELSE;
}

static ASTNode *subscope_block(ASTNode *n) {
    if (!n || n->token != 0 || !n->left || !n->right) return NULL;
    ASTNode *R = n->right;
    if (is_block_node(R)) return R;
    if (R->left_bracket != 0 || R->left || R->items.size == 0) return NULL;
    ASTNode *first = *(ASTNode**)array_get(&R->items, 0);
    return is_block_node(first) ? first : NULL;
}

static int is_subscope(ASTNode *n) { return subscope_block(n) != NULL; }

// Record a parse error. First error wins, so later cascading failures don't
// overwrite the original. The message is interned in the iterator's owner.
// The position (line, col) is captured from the current token.
static void set_error(TokenIterator *it, const char *msg) {
    if (it->error) return;
    it->error = intern_c_string(it->owner, msg);
    it->error_row = ti_current_line(it);
    it->error_col = ti_current_col(it);
}

// Consume the token that follows `n` (a divider, an operator). Whitespace still
// pending in front of it -- what an indent block scanned to find its dedent --
// goes to n->ws2, or unparse would drop it: `f\n    x, // c\n, 1` lost its
// comment, `g = i =>\n    v;\n~=t` its newline.
static void consume_keep_ws(TokenIterator *it, ASTNode *n) {
    InternID ws = ti_proceed(it, 1);
    StringView pend = intern_get(it->intern, ws);
    if (n && pend.length > 0) {
        StringView have = intern_get(it->intern, n->ws2);
        if (have.length == 0) {
            n->ws2 = ws;
        } else {
            char *buf = (char*)arena_alloc(it->arena, have.length + pend.length);
            it->sys->memcpy(buf, have.start, have.length);
            it->sys->memcpy(buf + have.length, pend.start, pend.length);
            n->ws2 = intern_string(it->owner, sv_from_parts(buf, have.length + pend.length));
        }
    }
    ti_proceed_ignore_ws(it, 0);
}

// --- Parser Implementation ---

// this normalized assignments so that the left hand side always has a token (which is the key)
// this leads to some strange things: 
//    {  0.23 = 2  }  // fine, number is also accessible in number field
//    { -0.23 = 2  }  // not parsed as assignment
//    {  0.23f = 2 }  // not parsed as assignment

static ASTNode *try_to_parse_assignment(TokenIterator *it) {
    InternID tok_id = ti_peek(it);
    if (!tok_id) return NULL;
    
    Tsys *sys = it->sys;
    InternCtx *ctx = it->intern;
    StringView tok_sv = intern_get(ctx, tok_id);
    ASTNode *k_tmp = NULL;
    
    unsigned char c = (unsigned char)tok_sv.start[0];
    int tok_is_string = TOKEN_IS_QUOTE(tok_id);
    int tok_is_digit = c_isdigit(c); 
    
    // Also alpha is allowed for identifier
    if (c_isalpha(c) || tok_is_digit || tok_is_string) {
        size_t pos = ti_get_token_pos(it);
        
        InternID ws1 = 0;
        InternID final_token = 0;
        ASTNode *data_node = NULL; 
        InternID num_tok = 0;
        
        if (tok_is_string) {
            ws1 = ti_proceed(it, 1);
            InternID parsed_id = ti_parse_quoted_string(it); // content ID
            
            data_node = create_node(it->arena, sys);
            data_node->string = parsed_id;
            data_node->string_quoting = tok_id; 
            
            // The UNQUOTED content id, so a quoted string used as a key behaves
            // like an identifier.
            final_token = parsed_id;
            

        } else if (tok_is_digit) {
            // Converted only once the `=` shows this really is a key.
            num_tok = tok_id;
            ws1 = ti_proceed(it, 0); 
            final_token = tok_id;
        } else {
            ws1 = ti_proceed(it, 0); 
            final_token = tok_id;
        }
        
        InternID next = ti_peek(it);
        if (next == TOK_EQ) {
            InternID ws2 = ti_proceed(it, 0); // Consume '='
            
            k_tmp = create_node(it->arena, sys);
            k_tmp->token = final_token;
            k_tmp->ws1 = ws1;
            k_tmp->ws2 = ws2;
            
            if (num_tok) set_number(k_tmp, num_tok, intern_get_cstr(ctx, num_tok), 0, 0);
            // The quote too, or unparse writes the key back without it.
            if (data_node) {
                k_tmp->string = data_node->string;
                k_tmp->string_quoting = data_node->string_quoting;
            }
        } else {
            // Backtrack
            ti_set_token_pos(it, pos);
        }
    }
    
    return k_tmp;
}

// Check if the current token (possibly a '-' followed by a digit) starts a number.
static int ti_peek_is_number(TokenIterator *it) {
    InternCtx *ctx = it->intern;
    InternID t = ti_peek(it);
    if (!t) return 0;
    StringView sv = intern_get(ctx, t);
    if (sv.length == 0) return 0;
    unsigned char c = (unsigned char)sv.start[0];
    if (c_isdigit(c)) return 1;
    if (t == TOK_MINUS) {
        InternID next = ti_raw_peek(it, 1);
        if (!next) return 0;
        StringView next_sv = intern_get(ctx, next);
        if (next_sv.length > 0 && c_isdigit((unsigned char)next_sv.start[0]))
            return 1;
    }
    return 0;
}

// Parse a numeric literal from the token iterator into the given AST node.
// Handles optional '-', integer/double/float suffixes ('f'), and exponent notation.
static void parse_number(TokenIterator *it, ASTNode *e) {
    InternCtx *ctx = it->intern;
    InternID t = ti_peek(it);

    int is_neg = (t == TOK_MINUS);
    int offset = is_neg ? 1 : 0;
    InternID num_tok = is_neg ? ti_raw_peek(it, 1) : t;
    InternID sfx     = ti_raw_peek(it, offset + 1);
    StringView sfx_sv = intern_get(ctx, sfx);
    int has_f = (sfx_sv.length == 1 && sfx_sv.start[0] == 'f');

    set_number(e, num_tok, intern_get_cstr(ctx, num_tok), is_neg, has_f);

    if (is_neg) ti_proceed_ignore_ws(it, 0);
    ti_proceed_ignore_ws(it, 0);
    if (has_f)  ti_proceed_ignore_ws(it, 0);

    e->is_literal = 1;
}

static ASTNode *parse_expression_body(TokenIterator *it, int prec, int is_functor_arg, int injected_bracket_scope);

static ASTNode *parse_expression_r(TokenIterator *it, int prec, int is_functor_arg, int injected_bracket_scope) {
    if (it->depth >= PARSE_MAX_NESTING) {
        set_error(it, "expression nested too deeply");
        return NULL;
    }
    it->depth++;
    ASTNode *e = parse_expression_body(it, prec, is_functor_arg, injected_bracket_scope);
    it->depth--;
    return e;
}

static ASTNode *parse_expression_body(TokenIterator *it, int prec, int is_functor_arg, int injected_bracket_scope) {
    Tsys *sys = it->sys;
    InternCtx *ctx = it->intern;

    if (it->error) return NULL;

    InternID t = ti_peek(it);
    
    if (!t) {
        set_error(it, "unexpected end of expression");
        return NULL;
    }
    
    if (is_end_bracket(t)) {
        set_error(it, "unexpected closing bracket");
        return NULL;
    }
    
    ASTNode *e = create_node(it->arena, sys);
    
    // ti_raw_peek sees the unfiltered token array, so the previous token may be
    // whitespace -- test it rather than assuming the iterator skipped it.
    InternID prev_id = ti_raw_peek(it, -1);
    StringView prev_sv = intern_get(ctx, prev_id);
    int prev_is_space = (prev_sv.length > 0 && c_iswhite(prev_sv.start[0]));
    
    int inject_bracket = is_functor_arg && (prev_is_space || ((t != TOK_LPAREN) && (t != TOK_LBRACKET)));
    
    if (!inject_bracket) {
        InternID ws = ti_proceed(it, 1);
        if (ws) e->ws1 = ws;
    }
    
    BracketInfo brack = get_bracket_info(t);

    // Postfix index / call: an UNSPACED ( or [ directly after an operand, i.e.
    // is_functor_arg with no preceding space -- exactly the case inject_bracket
    // excludes just above. It is a SUFFIX on whatever the caller has already
    // built, so it must not go on to absorb operators of its own: '.' binds
    // tighter (22) than implicit application (21), so `a[i].f` would otherwise
    // group as `a([i].f)` and silently evaluate `[i].f`. Parsed as a bare
    // bracket group and handed straight back, leaving the '.' to the caller's
    // loop -- giving `(a[i]).f`, as in C, where postfix [] () and . share a tier.
    int postfix_bracket = is_functor_arg && !inject_bracket && brack.end_id;

    if (inject_bracket || (brack.end_id && prec < brack.prec)) {
        // Newline-termination is suppressed inside ( ... ) so it can serve as
        // the line-continuation escape hatch; braces/brackets keep it on.
        // Injected (parenless) scopes inherit the surrounding state. Saved and
        // restored per entry so `( ... { ... } ... )` re-enables inside braces.
        int saved_nl_suppress = it->nl_suppressed;
        int saved_stmt_col = it->stmt_col;
        int saved_oneline = it->in_oneline_block;
        it->stmt_col = -1;
        // A real bracket leaves the one-line body behind: inside `{ ... }` the
        // ordinary `else` chaining rules apply again, whatever opened the body
        // this bracket sits in. (An injected scope inherits, as for nl.)
        if (!inject_bracket) {
            it->nl_suppressed = (t == TOK_LPAREN);
            it->in_oneline_block = 0;
        }

        if (!inject_bracket) {
             ti_proceed_ignore_ws(it, 0); // Consume bracket
        }
        
        ASTNode *tmp = NULL;
        Array items_list; 
        int has_items = 0;
        
        InternID peek = ti_peek(it);
        if (!inject_bracket && peek == brack.end_id) {
             // Empty brackets
             StringView peek_sv = intern_get(ctx, peek);
             (void)peek_sv;
             if (!is_functor_arg && t == TOK_LPAREN && ti_raw_peek(it, 2) != TOK_ARROW_F) {
                 set_error(it, "unexpected empty parentheses");
             }
             InternID bws = ti_proceed(it, 1);
             e->ws_empty_bracket = bws;
        } else {
            array_init(&items_list, sizeof(ASTNode*));
            
            while (1) {
                // Statement context for an indent block: only a real `{ ... }`
                // holds statements, and only there does a deeper line mean a
                // nested block. Parens and brackets hold expressions.
                int is_stmt_scope = !inject_bracket && t == TOK_LBRACE;
                int item_col = -1;
                if (is_stmt_scope) {
                    ti_peek(it);
                    item_col = stmt_indent_col(it);
                }
                it->stmt_col = item_col;

                ASTNode *k_tmp = try_to_parse_assignment(it);
                tmp = parse_expression_r(it, 0, 0, inject_bracket);
                
                if (it->error) break;

                InternID next_tok = ti_peek(it);
                if (next_tok == TOK_EQ) {
                    if (k_tmp) set_error(it, "unexpected '=' after assignment key");
                    k_tmp = tmp;
                    ti_proceed_ignore_ws(it, 0); 
                    ASTNode *val = parse_expression_r(it, 0, 0, inject_bracket);

                    ASTNode *assign = create_node(it->arena, sys);
                    assign->left = k_tmp;
                    assign->token = TOK_EQ;
                    assign->right = val;
                    tmp = assign;
                }
                
                if (k_tmp) {
                    ASTNode *assign = create_node(it->arena, sys);
                    assign->left = k_tmp;
                    assign->token = TOK_EQ;
                    assign->right = tmp;
                    tmp = assign;
                }

                if (is_stmt_scope) tmp = maybe_attach_indent_block(it, tmp, item_col);

                InternID div_tok = ti_peek(it);
                // `;` terminates back to the closest explicit-bracket items list;
                // it never extends an injected (parenless) call chain.
                int more_items = (div_tok == TOK_COMMA
                                  || (div_tok == TOK_SEMI && !inject_bracket));
                // A C-style x(){} item inside an explicit bracket (e.g. `{ while(){}
                // if(){} }`) also acts as an implicit separator, just like `;` would.
                // Only applies inside real brackets (not injected scopes) so that
                // injected-scope chaining is not affected.
                int is_csc = !inject_bracket && is_subscope(tmp);
                // A linefeed before the next item acts as an implicit divider
                // (divider stays 0, so unparse emits nothing; the newline already
                // lives in the previous item's ws2). Only when there really is a
                // next item token (not the closer) after the newline.
                int nl_divide = !inject_bracket && div_tok && div_tok != brack.end_id
                                && !more_items && ti_linefeed_terminates(it);
                if (more_items || k_tmp || inject_bracket || is_csc || nl_divide) {
                    has_items = 1;
                }

                if (has_items) {
                    if (more_items) tmp->divider = div_tok;
                    array_push(&items_list, &tmp, sys);
                }

                if (more_items) {
                    consume_keep_ws(it, tmp);
                    // Allow a trailing divider before the closing bracket:
                    // e.g. `{ a(); s(); }` or `(1, 2,)`. Capture any
                    // whitespace between the trailing divider and the
                    // closing bracket so unparse roundtrips.
                    InternID after = ti_peek(it);
                    if (after == brack.end_id || !after) {
                        InternID tail_ws = ti_proceed(it, 1);
                        if (tail_ws) e->ws_empty_bracket = tail_ws;
                        break;
                    }
                } else if (nl_divide) {
                    // Implicit divider at the linefeed: nothing to consume,
                    // loop straight into the next item.
                } else if (is_csc) {
                    // C-style call: peek to decide whether more items follow.
                    // Capture any trailing whitespace before the closing bracket
                    // so that unparse can roundtrip e.g. `{ while(){} }` intact.
                    InternID after = ti_peek(it);
                    if (!after || is_end_bracket(after)) {
                        InternID tail_ws = ti_proceed(it, 1);
                        if (tail_ws) e->ws_empty_bracket = tail_ws;
                        break;
                    }
                    // More items follow: continue loop without consuming a separator.
                } else {
                    break;
                }
            }
            
            if (has_items) {
                e->items = items_list;
            } else if (tmp) {
                e->items = items_list;
                array_push(&e->items, &tmp, sys);
            } else {
                e->items = items_list;
            }
        }
        
        if (!inject_bracket) {
            InternID curr = ti_peek(it);
            if (curr != brack.end_id) {
                 set_error(it, "expected closing bracket");
            }
             ti_proceed_ignore_ws(it, 0); 
             e->left_bracket = t;
             e->right_bracket = brack.end_id;
        } else {
            // IDs 0 for empty?
            e->left_bracket = 0;
            e->right_bracket = 0;
        }

        it->nl_suppressed = saved_nl_suppress; // restore before returning to caller
        it->stmt_col = saved_stmt_col;
        it->in_oneline_block = saved_oneline;

    } else {
        // Literals or Prefix Ops
        if (TOKEN_IS_QUOTE(t)) {
            InternID parsed_id = ti_parse_quoted_string(it);
            e->string = parsed_id;
            e->string_quoting = t;
            e->is_literal = 1;
            
        } else if (ti_peek_is_number(it)) {
            parse_number(it, e);
        } else {
             OpInfo una = get_unary_op(t);
             
             if (una.found && una.prec > prec) {
                 e->token = t;
                 ti_proceed_ignore_ws(it, 0);
                 // One below the prefix tier, so the operand may itself open
                 // with a prefix operator: `-/x`, `-!x`. No binary operator
                 // sits at that level, so nothing else binds differently.
                 e->right = parse_expression_r(it, una.prec - 1, 0, inject_bracket);
             } else {
                 e->token = t;
                 ti_proceed_ignore_ws(it, 0);
             }
        }
    }
    
    InternID ws2 = ti_proceed(it, 1);
    if (ws2) e->ws2 = ws2;

    // See postfix_bracket above. Returns only AFTER the trailing-whitespace
    // capture: returning any earlier loses the space after the ')' in
    // `f(a) / b` and unparse stops round-tripping.
    if (postfix_bracket) return e;

    // Binary Op Loop
    while (1) {
        if (it->error) return e;
        t = ti_peek(it);
        if (!t || is_terminator(t) || is_end_bracket(t) || (injected_bracket_scope && is_injected_bracket_terminator(it, t))) {
            return e;
        }
        // In an injected-bracket scope a `{...}` block terminates the current
        // expression.  Without this, `{b} if(){...}` inside a functor-arg
        // scope would chain `if(){...}` as another implicit-call on `{b}`
        // instead of stopping at the `}` and letting the outer level handle
        // the next C-style construct.
        if (injected_bracket_scope && e->left_bracket == TOK_LBRACE) {
            return e;
        }

        // Inside a one-line block body, `else` closes the body -- the caller
        // then attaches it as the second block. Without this, `if c then a else
        // b` would chain `else b` onto `a` as an implicit-call argument.
        //
        // Only inside such a body: everywhere else an `else` reached from here
        // is the tail of a brace chain, where `if(a){} else if(b){} else {}`
        // needs it to keep chaining as an argument of the preceding `else`
        // (see compile_else_tail in vm.c). In the brace and indent forms `else`
        // heads its own statement, so it is the operand this loop starts from
        // and never a token the loop meets at all.
        if (t == TOK_ELSE && it->in_oneline_block) break;

        // One-line block: `head then stmt` / `head do stmt`, the single-statement
        // twin of `head { stmt }`. Requires ASI, since the end of the line is
        // what closes the block; with it off the words stay plain identifiers
        // and chain as they always did.
        //
        // Only the statement level (prec 0) builds it. Every deeper level ends
        // its operand instead and hands the opener back up: an operand of a
        // binary operator is parsed at that operator's precedence, so in
        // `if x > 0 then y = 1` the `0` would otherwise take the block itself,
        // and the paren-less argument scope is parsed at the implicit-call
        // precedence, so the opener passes straight through it too. Both land
        // here, at the level where the head chain is complete.
        // (A paren-less scope at prec 0 is caught by the terminator test above.)
        if (is_oneline_opener(t) && it->oneline_mode) {
            if (prec != 0) break;
            e = attach_oneline_block(it, e, t);
            // `if c then a else b`: the second block, on the same statement.
            // Only `then` takes one -- an `else` after `do` is left to become a
            // sibling statement, where vm.c reports it has no matching `if`.
            if (!it->error && t == TOK_THEN
                && ti_peek(it) == TOK_ELSE && !ti_linefeed_terminates(it))
                e = attach_oneline_block(it, e, TOK_ELSE);
            break;
        }

        OpInfo op2 = get_binary_op(t);

        OpInfo unary_check = get_unary_op(t);

        InternID prev_id = ti_raw_peek(it, -1);
        StringView prev_sv = intern_get(ctx, prev_id);
        int prev_is_space = (prev_sv.length > 0 && c_iswhite(prev_sv.start[0]));

        InternID next_id = ti_raw_peek(it, 1);
        StringView next_sv = intern_get(ctx, next_id);
        int next_is_space = (next_sv.length > 0 && c_iswhite(next_sv.start[0]));

        int guess_unary = (op2.found && unary_check.found && t != TOK_SLASH &&
            prev_is_space && next_id && !next_is_space);

        // Postfix ++/--: a suffix on whatever we've built so far. Node shape is
        // left=operand, token=++, right=NULL -- the mirror image of the prefix
        // form, which is how the two are told apart downstream.
        //
        // It sits in the implicit-call/index tier (like C, where postfix ++ and
        // [] share a precedence level), so it binds tighter than every binary
        // operator -- `a++ * 2` is `(a++) * 2` -- while still letting `a[i]++`
        // group as `(a[i])++`: the `[i]` arg is parsed at exactly that
        // precedence, so the `++` is left for the level that owns the whole
        // index expression rather than being swallowed by the subscript.
        //
        // `++` after a complete operand is genuinely ambiguous here, because a
        // bare identifier followed by an argument is an implicit call: in
        // `return ++i` the `++` has to be a prefix on `i`, not a postfix on
        // `return`. Resolved with the same spacing convention this parser
        // already uses to tell binary `a - b` from unary `a -b` (guess_unary
        // above) -- space before but not after means prefix -- except that a
        // token which cannot start an operand at all (`;`, `)`, `,`, `}`) leaves
        // postfix as the only reading, so `i ++ ;` still works.
        //
        // A linefeed in between ends the expression instead (JS does the same),
        // so `x = 1` followed by `++i` on the next line stays two statements.
        int guess_prefix_incdec = prev_is_space && !next_is_space
                               && can_start_operand(next_id);
        if (TOKEN_IS_INC_DEC(t) && !guess_prefix_incdec
            && prec < PREC_CALL
            && !ti_linefeed_terminates(it)) {
            consume_keep_ws(it, e);
            ASTNode *new_e = create_node(it->arena, sys);
            new_e->left = e;
            new_e->token = t;
            // No right child to soak up the trailing whitespace, so take it here.
            InternID pws = ti_proceed(it, 1);
            if (pws) new_e->ws2 = pws;
            e = new_e;
            continue;
        }

        if (op2.found && !guess_unary) {
            if (op2.prec > prec || (op2.is_right_assoc && op2.prec == prec)) {
                int next_prec = op2.prec - (!op2.is_right_assoc && op2.prec == prec ? 1 : 0);
                consume_keep_ws(it, e);
                
                ASTNode *new_e = create_node(it->arena, sys);
                new_e->left = e;
                new_e->token = t;
                // `f = (a, b) =>` with the body indented on the lines below.
                // Only `=>` takes a block this way: doing it for other operators
                // would turn an ordinary indented continuation line into one.
                int took_block = 0;
                if (t == TOK_ARROW_F && it->indent_mode && it->stmt_col >= 0
                    && ti_peek(it) && ti_linefeed_terminates(it)) {
                    int bcol;
                    if (ti_line_indent(it, &bcol) && bcol > it->stmt_col) {
                        new_e->right = parse_indent_block(it, bcol, it->stmt_col);
                        took_block = 1;
                    }
                }
                if (!took_block) new_e->right = parse_expression_r(it, next_prec, 0, 0);
                e = new_e;
            } else {
                break;
            }
        } else if (prec < PREC_CALL) { // Functor implicit call
             // A linefeed here is a virtual end-of-expression: stop chaining.
             // (Explicit binary operators are not guarded, so `+` at line end or
             // line start still continues the expression across the newline.)
             if (ti_linefeed_terminates(it)) break;
             // Block call from a bracket unless followed by another bracket.
             // This prevents [3]i32 from being parsed as call([3], i32) which
             // would swallow the element type into an injected-bracket scope.
             // [3][3] still works because '[' is a bracket opener.
             int is_bracket_expr = (e->right_bracket != 0);
             int next_is_bracket = TOKEN_IS_OPEN_BRACKET(t);
             if (is_bracket_expr && !next_is_bracket) {
                 ASTNode *new_e = create_node(it->arena, sys);
                 new_e->left = e;
                 new_e->token = 0;
                 new_e->right = parse_expression_r(it, PREC_CALL, 0, 0);
                 e = new_e;
             } else {
                 ASTNode *new_e = create_node(it->arena, sys);
                 new_e->left = e;
                 new_e->token = TOK_EMPTYSTRING; 
                 new_e->right = parse_expression_r(it, PREC_CALL, 1, 0);
                 e = new_e;
                 // C-style x() { ... } detection: the recursive call has just
                 // finished consuming the `}`.  If the node we built has the
                 // shape  call( call(x,()), {block} )  treat the closing brace
                 // as an implicit statement terminator so that a following
                 // if(){} / while(){} etc. becomes a sibling instead of another
                 // nested implicit-call arg.
                 // The NULL token marks it as a sub-scope rather than an
                 // implicit call, so the compiler does not have to re-derive
                 // what was just decided here. Unparses to nothing, same as
                 // the TOK_EMPTYSTRING it replaces.
                 if (is_c_style_call(e, ctx)) { e->token = 0; break; }
             }
        } else {
            break;
        }
    }
    
    return e;
}

static ASTNode *parse_statement(TokenIterator *it) {
    if (!ti_peek(it)) return NULL;
    
    ASTNode *k_tmp = try_to_parse_assignment(it);
    
    ASTNode *tmp = parse_expression_r(it, 0, 0, 0);
    
    if (k_tmp) {
        ASTNode *assign = create_node(it->arena, it->sys);
        assign->left = k_tmp;
        assign->token = TOK_EQ;
        assign->right = tmp;
        tmp = assign;
    }

    return tmp;
}

// --- Indentation blocks ---
//
// A run of lines indented deeper than the statement before them is that
// statement's block. The block node writes itself with empty-string brackets,
// so it emits nothing -- the newline and indentation that stand in for braces
// are already in the surrounding whitespace, and the round-trip stays byte
// exact with no special case in either unparser.
//
// The parser attaches unconditionally, with no idea what the head means. A
// block on something that cannot take one is the compiler's error to report,
// which is what keeps keywords out of here.

static ASTNode *parse_indent_block(TokenIterator *it, int body_col, int parent_col);

// The column an indent block under this statement would be measured against.
static int stmt_indent_col(TokenIterator *it) {
    int ind;
    if (ti_line_indent(it, &ind)) return ind;
    return ti_current_col(it);
}

// head + { block }, in the shape a C-style `x() { ... }` produces: the
// NULL-token attachment node over an injected-bracket wrapper.
static ASTNode *make_subscope(TokenIterator *it, ASTNode *head, ASTNode *blk) {
    Tsys *sys = it->sys;
    ASTNode *w = create_node(it->arena, sys);
    array_init(&w->items, sizeof(ASTNode*));
    array_push(&w->items, &blk, sys);

    ASTNode *n = create_node(it->arena, sys);
    n->token = 0;
    n->left = head;
    n->right = w;
    return n;
}

// The token a chained head starts from: `else` in `else if c`, `#` in
// `#enable x`, `if` in `if sin a`. Walks past the implicit-call spine to the
// operand that opened it.
static InternID chain_base_token(ASTNode *n) {
    while (n && n->token == TOK_EMPTYSTRING && n->left) n = n->left;
    return n ? n->token : 0;
}

// Which node an indent block attaches to. Normally the statement itself, but a
// trailing chained head takes it instead: in `else if c` the block is the if's,
// not the else's -- which is also the shape `else if(c) { ... }` produces.
// On a descent, *out_slot points at the slot holding the target, so the
// caller can put the attachment back where the target was.
//
// through_arrow descends past a `=>` into its body as well. A one-line block
// needs that: `=>` is prec 6 and `then` is only taken at prec 0, so in
// `f = n => if c then x` the head this is called with is the whole arrow, and
// without the descent the block lands on the arrow instead of on the `if` --
// which the VM then rejects with "a block is not a value here". The arrow's
// right *is* the function body, so an opener met after it always belongs
// inside. The indent form never needs this: an indented body is taken at the
// `=>` itself, in parse_expression_r, so descending there would only change
// shapes that rule already settled.
static ASTNode *indent_attach_target(ASTNode *stmt, ASTNode ***out_slot, int through_arrow) {
    ASTNode *cur = stmt;
    *out_slot = NULL;
    for (;;) {
        if (through_arrow && cur && cur->token == TOK_ARROW_F && cur->right) {
            *out_slot = &cur->right;
            cur = cur->right;
            continue;
        }
        if (!cur || cur->token != TOK_EMPTYSTRING || !cur->right) break;
        // Only a head written with one of the parser's OWN tokens hands its
        // block on: `else` gives it to the `if` it chains, `#` to the directive
        // word chain under it. Every other head is a name -- `if`, `while`, a
        // user function -- and a name followed by arguments is a call, so the
        // trailing step is that call's ARGUMENT, not an inner statement: in
        // `if sin a then b` the block is the if's, not `sin a`'s. Still no
        // keywords here; the parser only tells its own tokens from names.
        InternID base = chain_base_token(cur);
        if (base != TOK_ELSE && base != TOK_HASH) break;
        ASTNode *w = cur->right;
        if (w->left || w->left_bracket != 0 || w->items.size == 0) break;
        ASTNode **slot = (ASTNode**)array_get(&w->items, w->items.size - 1);
        ASTNode *last = *slot;
        // Only another call chains a block; a condition or a bare name ends it.
        if (!last || last->token != TOK_EMPTYSTRING || !last->left || !last->right) break;
        // A step whose right is a real bracket is a finished operand, not a
        // chain step: `if c()` is one call, not `if` applied to `c` applied to
        // `()`. Descending into it would hang the block on the condition, and
        // the head would then be left with no body at all.
        if (last->right->left_bracket != 0) break;
        *out_slot = slot;
        cur = last;
    }
    return cur;
}

// --- One-line blocks ---
//
// `if c then stmt`, `while c do stmt`, `if c then a else b`. The opener is a
// bracket (tokens.h, TOK_THEN) that the end of the statement closes, so the
// block node carries the opener in left_bracket and an empty string in
// right_bracket -- exactly the trick an indent block uses to write itself as
// nothing, one slot along.
//
// The whitespace needs no new field: unparse_expression_r emits left,
// left_bracket, items, so the space before `then` is already the head's ws2 and
// the space after it is the body's ws1.
//
// The body is exactly ONE statement, so `if c then x = 1; y = 2` leaves `y = 2`
// outside the if, as in C. The `;` is not consumed here -- the enclosing
// statement list takes it as the divider of the whole construct, which is what
// keeps the round-trip exact.

static ASTNode *parse_oneline_block(TokenIterator *it, InternID opener) {
    Tsys *sys = it->sys;
    ASTNode *b = create_node(it->arena, sys);
    b->left_bracket  = opener;
    b->right_bracket = TOK_EMPTYSTRING;
    array_init(&b->items, sizeof(ASTNode*));

    ti_proceed_ignore_ws(it, 0); // consume the opener

    // A one-line block ends at the end of the line even inside parens, where
    // newline termination is otherwise suppressed -- same rule as an indent
    // block. stmt_col goes to -1: nothing here can take an indent block.
    int saved_nl  = it->nl_suppressed;
    int saved_col = it->stmt_col;
    int saved_ol  = it->in_oneline_block;
    it->nl_suppressed = 0;
    it->stmt_col = -1;
    it->in_oneline_block = 1;

    if (!ti_peek(it) || ti_linefeed_terminates(it)) {
        set_error(it, "expected a statement after a one-line block opener");
    } else {
        ASTNode *st = parse_statement(it);
        if (st) array_push(&b->items, &st, sys);
    }

    it->nl_suppressed = saved_nl;
    it->stmt_col = saved_col;
    it->in_oneline_block = saved_ol;
    return b;
}

// Attach a one-line block to `head`, giving the same node shape a `{ ... }`
// body produces. The target is chosen by indent_attach_target for the same
// reason an indent block's is: in `else if c() then e()` the block belongs to
// the `if`, not to the `else` that chains it.
static ASTNode *attach_oneline_block(TokenIterator *it, ASTNode *head, InternID opener) {
    ASTNode *blk = parse_oneline_block(it, opener);
    ASTNode **slot;
    ASTNode *target = indent_attach_target(head, &slot, 1);
    ASTNode *sub = make_subscope(it, target, blk);
    if (slot) { *slot = sub; return head; }
    return sub;
}

// Called after a statement in a statement list: if the next line is indented
// deeper than `stmt_col`, take it (and its followers) as the statement's block.
static ASTNode *maybe_attach_indent_block(TokenIterator *it, ASTNode *stmt, int stmt_col) {
    if (!it->indent_mode || !stmt || it->error) return stmt;
    if (!ti_peek(it)) return stmt;
    if (!ti_linefeed_terminates(it)) return stmt;
    int col;
    if (!ti_line_indent(it, &col)) return stmt;
    if (col <= stmt_col) return stmt;
    ASTNode *blk = parse_indent_block(it, col, stmt_col);

    ASTNode **slot;
    ASTNode *target = indent_attach_target(stmt, &slot, 0);
    ASTNode *sub = make_subscope(it, target, blk);
    if (slot) { *slot = sub; return stmt; }
    return sub;
}

static ASTNode *parse_indent_block(TokenIterator *it, int body_col, int parent_col) {
    Tsys *sys = it->sys;
    ASTNode *b = create_node(it->arena, sys);
    // Empty-string brackets: they unparse to nothing, which is what makes the
    // block invisible, and they are still non-zero, which keeps the node from
    // being mistaken for a transparent chain wrapper and unwrapped away.
    // Nothing else in the parser ever puts TOK_EMPTYSTRING in a bracket slot,
    // so this doubles as the mark that tells an indent block from a braced one.
    b->left_bracket  = TOK_EMPTYSTRING;
    b->right_bracket = TOK_EMPTYSTRING;
    array_init(&b->items, sizeof(ASTNode*));

    // A block always ends at a newline, even where the enclosing scope had
    // suppressed that (inside parens).
    int saved_nl = it->nl_suppressed;
    int saved_col = it->stmt_col;
    it->nl_suppressed = 0;

    while (1) {
        if (it->error) break;
        if (!ti_peek(it)) break;

        int col = stmt_indent_col(it);
        it->stmt_col = col;
        ASTNode *st = parse_statement(it);
        if (!st) break;
        st = maybe_attach_indent_block(it, st, col);
        array_push(&b->items, &st, sys);
        if (it->error) break;

        InternID tok = ti_peek(it);
        int had_divider = 0;
        if (tok == TOK_SEMI || tok == TOK_COMMA) {
            st->divider = tok;
            consume_keep_ws(it, st);
            had_divider = 1;
            tok = ti_peek(it);
        }
        if (!tok) break;
        // The `}` of an enclosing brace block is not a dedent error.
        if (is_end_bracket(tok)) break;

        if (!ti_linefeed_terminates(it)) {
            if (had_divider || is_subscope(st)) continue;
            set_error(it, "expected end of statement");
            break;
        }

        // A line whose content begins inside a comment opened earlier says
        // nothing about indentation; keep it in the block rather than guess.
        int next_col;
        if (!ti_line_indent(it, &next_col)) continue;
        if (next_col == body_col) continue;            // next statement in this block
        // Only dedents get here: a deeper line was already taken as a block on
        // the statement above, which the compiler rejects if it takes none.
        // Hand the dedent back to whichever level it matches. Landing between
        // two levels matches none of them, so say so here.
        if (next_col > parent_col) { set_error(it, "inconsistent dedent"); break; }
        break;
    }

    it->nl_suppressed = saved_nl;
    it->stmt_col = saved_col;
    return b;
}

// Returns an ASTNode whose items are the parsed statements. No left/right bracket.
// Leading whitespace (when the input is whitespace-only, or just leading WS) is
// stored in the returned node's ws1, so unparse_expression_r emits it naturally.
static ASTNode *parse_statements(TokenIterator *it, Tsys *sys) {
    ASTNode *node = create_node(it->arena, sys);
    array_init(&node->items, sizeof(ASTNode*));

    if (!ti_peek(it)) {
        InternID ws = ti_proceed(it, 0);
        if (ws) node->ws1 = ws;
    } else {
        while (1) {
            if (it->error) break;
            ti_peek(it);
            int stmt_col = stmt_indent_col(it);
            it->stmt_col = stmt_col;
            ASTNode *code_tree = parse_statement(it);
            if (!code_tree) break;
            code_tree = maybe_attach_indent_block(it, code_tree, stmt_col);

            array_push(&node->items, &code_tree, sys);

            InternID token = ti_peek(it);
            if (token == TOK_SEMI || token == TOK_COMMA) {
                code_tree->divider = token;
                consume_keep_ws(it, code_tree);
            } else {
                // For C-style x(){...} constructs the closing `}` acts as an
                // implicit statement separator: continue parsing siblings
                // without requiring an explicit `;`.
                if (!ti_peek(it)) break;
                // A linefeed acts as a virtual `;` between statements.
                if (ti_linefeed_terminates(it)) continue;
                if (!is_subscope(code_tree)) {
                    // With the feature on, tokens remaining here with no linefeed
                    // and no c-style separator are genuine junk after the
                    // statement — surface it rather than dropping it silently.
                    if (it->nl_term_mode) set_error(it, "expected end of statement");
                    break;
                }
                // C-style call: fall through to parse the next sibling.
            }
        }
        // Capture any trailing whitespace left in it->ws (e.g. "6; " with
        // trailing space after the semicolon) so that unparse roundtrips.
        InternID ws = it->ws;
        if (ws) {
            node->ws2 = ws;
        }
    }
    return node;
}

typedef struct UnparseBuf UnparseBuf;
static void upb_init(UnparseBuf *sb, Tsys *sys, InternCtx *intern);
static char *upb_finish(UnparseBuf *sb);
static void upb_reserve(UnparseBuf *sb, size_t n);
static void unparse_expression_r(ASTNode *t, UnparseBuf *sb);

// Sets every parent link and returns the tree's depth. Iterative: a long
// `a + b + c ...` chain is as deep as it is long, whatever the parse recursed.
typedef struct { ASTNode *n; int depth; } LinkItem;

static void link_push(Array *stack, ASTNode *child, ASTNode *parent, int depth, Tsys *sys) {
    if (!child) return;
    child->parent = parent;
    LinkItem li = { child, depth };
    array_push(stack, &li, sys);
}

static int create_parent_links(ASTNode *root, Tsys *sys) {
    if (!root) return 0;
    int max_depth = 0;
    Array stack;
    array_init(&stack, sizeof(LinkItem));
    link_push(&stack, root, NULL, 1, sys);
    while (stack.size > 0) {
        LinkItem li = ((LinkItem*)stack.data)[--stack.size];
        ASTNode *n = li.n;
        if (li.depth > max_depth) max_depth = li.depth;
        link_push(&stack, n->left, n, li.depth + 1, sys);
        link_push(&stack, n->right, n, li.depth + 1, sys);
        for (size_t i = 0; i < n->items.size; i++)
            link_push(&stack, ((ASTNode**)n->items.data)[i], n, li.depth + 1, sys);
    }
    array_free(&stack, sys);
    return max_depth;
}


const char *token_joins[] = {
    "++", "/*", "*/", "//", "!=", "==", "===", "!==", "~=",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    ">=", "<<", ">>", ">>>", "<=", "%%", "**",
    "/~", "/%", "%%=", "/~=", "/%=",
    "&&", "||",
    "??", "???", "..", "..=", "...", "....", "#{", "#}", "->", "|>", "<|",
    "=>", "<=>", "--", "-->", "<-", NULL
};

void parser_init(Parser *p, Tsys *sys) {
    p->sys = sys;
    intern_ctx_init(&p->intern, sys);
    intern_init_fixed_tokens(&p->intern);
    intern_owner_init(&p->fixed_owner, &p->intern);
    
    p->joins = create_join_table(token_joins, &p->fixed_owner, sys);
}

void parser_deinit(Parser *p) {
    free_join_table(p->joins, p->sys);
    p->joins = 0;
    intern_owner_deinit(&p->fixed_owner);
    intern_ctx_deinit(&p->intern);
}

// unparser state
struct UnparseBuf {
    char *data;
    size_t len;
    size_t cap;
    ASTNode** dataRef; 
    size_t *lineOffsets;
    size_t lineOffsetsCap;
    size_t lineCount;
    Tsys *sys;
    InternCtx *intern;
    int flags;
};

ParseResult parse_to_asts(Parser *p, const char *txt, int flags) {
    
    ParseResult res;
    res.code_tree = NULL;
    res.error = 0;
    res.error_row = -1;
    res.error_col = -1;
    res.txt_to_ref = NULL;
    res.num_txt_to_ref = 0;
    res.line_offsets = NULL;
    res.num_lines_offsets = 0;
    
    // Arena creation
    res.arena = (Arena*)S_MALLOC(p->sys, sizeof(Arena));
    arena_init(res.arena, p->sys);
    
    // Result owner for dynamic tokens found in THIS parse
    intern_owner_init(&res.owner, &p->intern);
    
    Array token_positions;
    Array tokens = parse_to_tokens(txt, res.arena, p->sys, &res.owner, p->joins, &token_positions);

    
    TokenIterator it = create_token_iterator(tokens, token_positions, res.arena, p->sys, &p->intern, &res.owner);
    it.nl_term_mode = (flags & PARSE_DISABLE_ASI) == 0;
    // Both block forms need a linefeed to know where the block ends, so ASI off
    // turns both off, whatever their own flags say.
    it.indent_mode = it.nl_term_mode && (flags & PARSE_DISABLE_INDENT_BLOCKS) == 0;
    it.oneline_mode = it.nl_term_mode && (flags & PARSE_DISABLE_ONE_LINE_BLOCKS) == 0;

    res.code_tree = parse_statements(&it, p->sys);
    // Deeper than this and the recursive consumers (unparse, the compiler) are
    // the next thing to run out of stack.
    int too_deep = create_parent_links(res.code_tree, p->sys) > PARSE_MAX_DEPTH;
    if (too_deep) set_error(&it, "expression nested too deeply");
    res.error = it.error;
    res.error_row = it.error_row;
    res.error_col = it.error_col;

    if(flags && !too_deep) {
        UnparseBuf sb;
        upb_init(&sb, p->sys, &p->intern);
        sb.flags = flags;
        // A round-trip is exactly as long as the source.
        size_t txt_len = s_strlen(txt);
        upb_reserve(&sb, txt_len);
        unparse_expression_r(res.code_tree, &sb);

        if(!res.error && (flags & PARSE_VERIFY))
        {
            const char *unparsed = upb_finish(&sb);
            if(sb.len != txt_len || s_strncmp(unparsed, txt, txt_len) != 0) {
                set_error(&it, "unparse round-trip verification failed");
                res.error = it.error;
                res.error_row = it.error_row;
                res.error_col = it.error_col;
            }
            S_FREE(p->sys, (void*)unparsed);
            sb.data = NULL; // prevent double-free below
        }
        if((flags & PARSE_OUTPUT_REFS) && sb.dataRef) {
            res.txt_to_ref = (ASTNode**)S_MALLOC(p->sys, sb.len * sizeof(ASTNode*));
            res.num_txt_to_ref = sb.len;
            p->sys->memcpy(res.txt_to_ref, sb.dataRef, sb.len * sizeof(ASTNode*));
        }
        if((flags & PARSE_OUTPUT_LINE_OFFSETS) && sb.lineOffsets) {
            res.line_offsets = (size_t*)S_MALLOC(p->sys, sb.lineCount * sizeof(size_t));
            p->sys->memcpy(res.line_offsets, sb.lineOffsets, sb.lineCount * sizeof(size_t));
            res.num_lines_offsets = (int)sb.lineCount;
        }

        // Clean up sb internals (data already freed if PARSE_VERIFY was set and sb.data set to NULL)
        if(sb.data) S_FREE(p->sys, sb.data);
        if(sb.dataRef) S_FREE(p->sys, sb.dataRef);
        if(sb.lineOffsets) S_FREE(p->sys, sb.lineOffsets);
    }
    
    ti_destroy(&it); 
    
    return res;
}


static void unparse_number(char *dst, double number, NumberFlags flags) {
    if ((flags & NUM_TYPE) == NUM_INTEGER) {
        long long iv = (long long)number;
        if (iv == 0) { dst[0] = '0'; dst[1] = '\0'; return; }
        int neg = (iv < 0);
        unsigned long long uv = neg ? 0ULL - (unsigned long long)iv : (unsigned long long)iv;
        char tmp[24]; int n = 0;
        while (uv > 0) { tmp[n++] = '0' + (int)(uv % 10); uv /= 10; }
        int out = 0;
        if (neg) dst[out++] = '-';
        for (int i = n - 1; i >= 0; i--) dst[out++] = tmp[i];
        dst[out] = '\0';
        return;
    }

    // SHORTEST, not %g's 6 significant digits: this text replaces the literal
    // in the source, so anything it drops is gone from the program. At %g,
    // scrubbing a value to 110566002.1 would write back 110566000.
    int len = s_from_number_flags(number, dst, S_FROM_NUMBER_FLAG_SHORTEST |
                                              (flags & NUM_EXPONENTIAL_FORM ? S_FROM_NUMBER_FLAG_FORCE_EXP_FORM :
                                               flags & NUM_DECIMAL_FORM     ? S_FROM_NUMBER_FLAG_FORCE_DECIMAL_FORM : 0));
    if ((flags & NUM_TYPE) == NUM_DOUBLE) {
        // ensure a decimal point is present
        int has_dot = 0, has_e = 0;
        for (int i = 0; i < len; i++) {
            if (dst[i] == '.') has_dot = 1;
            if (dst[i] == 'e' || dst[i] == 'E') has_e = 1;
        }
        if (!has_dot && !has_e) {
            dst[len++] = '.'; dst[len++] = '0'; dst[len] = '\0';
        }
    } else { // NUM_FLOAT
        dst[len++] = 'f'; dst[len] = '\0';
    }
}

#define UNPARSE_OUTPUT_DATA         (1<<0)
#define UNPARSE_OUTPUT_REFS         (1<<1)
#define UNPARSE_OUTPUT_LINE_OFFSETS (1<<2)

static void upb_init(UnparseBuf *sb, Tsys *sys, InternCtx *intern) {
    sb->data = NULL; sb->len=0; sb->cap=0; sb->sys=sys; sb->intern=intern; sb->flags=UNPARSE_OUTPUT_DATA;
        sb->dataRef = NULL; 
        sb->lineOffsets = NULL; sb->lineOffsetsCap = 0; sb->lineCount = 0;
}
static void memset_ptr(void **dest, void *ptr_value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dest[i] = ptr_value;
    }
}
static void upb_append_sv(UnparseBuf *sb, StringView sv, ASTNode* node) {
    if (sv.length == 0) return;
    if (sb->len + sv.length >= sb->cap) {
        sb->cap = sb->cap==0 ? 64 : sb->cap*2;
        if (sb->cap < sb->len + sv.length + 1) sb->cap = sb->len + sv.length + 64;
        if(sb->flags & UNPARSE_OUTPUT_DATA) sb->data    = (char*)S_REALLOC(sb->sys, sb->data, sb->cap);
        if(sb->flags & UNPARSE_OUTPUT_REFS) sb->dataRef = (ASTNode**)S_REALLOC(sb->sys, sb->dataRef, sb->cap * sizeof(ASTNode*));
    }
    if(sb->flags & UNPARSE_OUTPUT_DATA) sb->sys->memcpy(sb->data    + sb->len, sv.start, sv.length);
    if(sb->flags & UNPARSE_OUTPUT_REFS) memset_ptr((void **)(sb->dataRef + sb->len), node, sv.length);
    if(sb->flags & UNPARSE_OUTPUT_LINE_OFFSETS) {
        for (size_t i = 0; i < sv.length; i++) {
            if (sv.start[i] == '\n') {
                if (sb->lineCount >= sb->lineOffsetsCap) {
                    sb->lineOffsetsCap = sb->lineOffsetsCap == 0 ? 16 : sb->lineOffsetsCap * 2;
                    sb->lineOffsets = (size_t*)S_REALLOC(sb->sys, sb->lineOffsets, sb->lineOffsetsCap * sizeof(size_t));
                }
                sb->lineOffsets[sb->lineCount++] = sb->len + i + 1;
            }
        }
    }
    sb->len += sv.length;
    if(sb->flags & UNPARSE_OUTPUT_DATA) sb->data[sb->len] = '\0';
}
static void upb_reserve(UnparseBuf *sb, size_t n) {
    if (n + 1 <= sb->cap) return;
    sb->cap = n + 1;
    if(sb->flags & UNPARSE_OUTPUT_DATA) sb->data    = (char*)S_REALLOC(sb->sys, sb->data, sb->cap);
    if(sb->flags & UNPARSE_OUTPUT_REFS) sb->dataRef = (ASTNode**)S_REALLOC(sb->sys, sb->dataRef, sb->cap * sizeof(ASTNode*));
}
static char *upb_finish(UnparseBuf *sb) {
    if (!sb->data) return c_strdup("", sb->sys);
    return sb->data; // client need to free this
}
static void upb_process_cstr(UnparseBuf *sb, const char *s, ASTNode* node) {
    if (!s) return;
    upb_append_sv(sb, sv_from_cstring(s), node);
}
static void upb_process_id(UnparseBuf *sb, InternID id, ASTNode* node) {
    if (!id) return;
    InternCtx *ctx = sb->intern; 
    StringView sv = intern_get(ctx, id); 
    upb_append_sv(sb, sv, node);
}
static void upb_append_cstr(UnparseBuf *sb, const char *s) { upb_process_cstr(sb, s,  NULL); }
static void upb_append_id(UnparseBuf *sb, InternID id)     { upb_process_id  (sb, id, NULL); }


void unparse_expression_r(ASTNode *t, UnparseBuf* sb) {
    if (!t) return;
    
    upb_process_id(sb, t->ws1, NULL);
    
    if (t->left) unparse_expression_r(t->left, sb);
    
    if (t->items.size > 0 || (t->left_bracket)) { 
        upb_process_id(sb, t->left_bracket, t);
        
        for(size_t i=0; i<t->items.size; i++) {
            ASTNode *item = *(ASTNode**)array_get(&t->items, i);
            if (!item) continue;
            unparse_expression_r(item, sb);
            if (item->divider) upb_process_id(sb, item->divider, t);
        }
        
        upb_process_id(sb, t->ws_empty_bracket, NULL);
        upb_process_id(sb, t->right_bracket, t);
    }
    
    if (t->number_flags != NUM_NONE) {
        if(t->number_as_token == 0) { // set number_as_token to 0 when you update t->number
            char tmpchars[S_FROM_NUMBER_MAX_CHARS + 2];
            unparse_number(tmpchars, t->number, t->number_flags);
            upb_process_cstr(sb, tmpchars, t);            
        } else {
            if(t->number_flags & NUM_WAS_PARSED_NEGATIVE) upb_process_cstr(sb, "-", t);            
            upb_process_id(sb, t->number_as_token, t);            
            if((t->number_flags & NUM_TYPE) == NUM_FLOAT) upb_process_cstr(sb, "f", t);                      
        }
    } else if (t->string || t->is_literal) {
        upb_process_id(sb, t->string_quoting, t);
        upb_process_id(sb, t->string, t);
        upb_process_id(sb, t->string_quoting, t);
    } else {
        upb_process_id(sb, t->token, t);
    }
    
    if (t->right) unparse_expression_r(t->right, sb);
    
    upb_process_id(sb, t->ws2, NULL);
}

char *unparse_expression(Parser *p, ASTNode *t) {
    UnparseBuf sb;
    upb_init(&sb, p->sys, &p->intern);
    unparse_expression_r(t, &sb);
    return upb_finish(&sb);
}

// --- Debug rows visualization ---

// One per node: the column-range in row 0 occupied by this node's
// "primary token", plus the node's depth. If `custom` is non-zero, that
// character is painted across the range instead of copying from row 0
// (used for synthetic wrapper markers like '/\').
typedef struct {
    int depth;
    size_t col;
    size_t len;
    char custom;
} DbgEntry;

static void debug_unparse_r(ASTNode *t, UnparseBuf *sb, int depth, Array *entries) {
    if (!t) return;

    upb_append_id(sb, t->ws1);

    // Whether this node paints any glyph of its own at `depth` (token,
    // number, string, brackets, or items). A pure wrapper (e.g. a
    // function-call node whose `left` is the name and `right` is the
    // parens) paints nothing -- we'll mark its level with '/\' below
    // so the wrapper depth is still visible / not misleading.
    int emits_glyph = (t->items.size > 0) || t->left_bracket
                   || t->number_flags || t->string || t->is_literal
                   || (t->token && t->token != TOK_EMPTYSTRING);

    size_t left_end = 0;
    if (t->left) {
        debug_unparse_r(t->left, sb, depth + 1, entries);
        left_end = sb->len;
    }

    if (t->items.size > 0 || t->left_bracket) {
        // Anchor on the left bracket (if any); otherwise this is a wrapper
        // (e.g. statements root) which has no glyph of its own.
        if (t->left_bracket) {
            size_t before = sb->len;
            upb_append_id(sb, t->left_bracket);
            DbgEntry e = { depth, before, sb->len - before };
            array_push(entries, &e, sb->sys);
        }

        for (size_t i = 0; i < t->items.size; i++) {
            ASTNode *item = *(ASTNode**)array_get(&t->items, i);
            if (!item) continue;
            debug_unparse_r(item, sb, depth + 1, entries);
            if (item->divider) {
                size_t before = sb->len;
                upb_append_id(sb, item->divider);
                if (sb->len > before) {
                    DbgEntry e = { depth, before, sb->len - before };
                    array_push(entries, &e, sb->sys);
                }
            }
        }

        upb_append_id(sb, t->ws_empty_bracket);
        if (t->right_bracket) {
            size_t before = sb->len;
            upb_append_id(sb, t->right_bracket);
            if (sb->len > before) {
                DbgEntry e = { depth, before, sb->len - before };
                array_push(entries, &e, sb->sys);
            }
        }
    } else {
        size_t before = sb->len;
        if (t->number_flags != NUM_NONE) {
            char num_s[S_FROM_NUMBER_MAX_CHARS + 2];
            unparse_number(num_s, t->number, t->number_flags);
            upb_append_cstr(sb, num_s);
        } else if (t->string || t->is_literal) {
            upb_append_cstr(sb, "\"");
            upb_append_id(sb, t->string);
            upb_append_cstr(sb, "\"");
        } else {
            upb_append_id(sb, t->token);
        }
        if (sb->len > before) {
            DbgEntry e = { depth, before, sb->len - before };
            array_push(entries, &e, sb->sys);
        }
    }

    size_t right_start = sb->len;
    if (t->right) debug_unparse_r(t->right, sb, depth + 1, entries);

    // For a pure-wrapper node with both a left and right child (typical
    // function-call shape: name + parens), draw '/\' at the wrapper's
    // depth to indicate the level isn't empty but joins its two sides.
    if (!emits_glyph && t->left && t->right && left_end > 0
        && right_start >= left_end) {
        DbgEntry e1 = { depth, left_end - 1, 1, '/' };
        DbgEntry e2 = { depth, right_start, 1, '\\' };
        array_push(entries, &e1, sb->sys);
        array_push(entries, &e2, sb->sys);
    }

    upb_append_id(sb, t->ws2);
}

char *parser_debugrows(Parser *p, ASTNode *node) {
    Tsys *sys = p->sys;

    UnparseBuf row0;
    upb_init(&row0, sys, &p->intern);
    Array entries;
    array_init(&entries, sizeof(DbgEntry));

    debug_unparse_r(node, &row0, 1, &entries);

    size_t row_len = row0.len;
    if (!row0.data) {
        // Ensure a valid empty string for row 0.
        row0.data = c_strdup("", sys);
        row0.len = 0;
        row0.cap = 1;
    }

    // Find max depth.
    int max_depth = 1;
    for (size_t i = 0; i < entries.size; i++) {
        DbgEntry *e = (DbgEntry*)array_get(&entries, i);
        if (e->depth > max_depth) max_depth = e->depth;
    }

    // Allocate (max_depth) row buffers (rows 1..max_depth), each filled with spaces.
    // Row 0 is row0.data itself.
    char **rows = (char**)S_MALLOC(sys, sizeof(char*) * (size_t)max_depth);
    for (int d = 0; d < max_depth; d++) {
        rows[d] = (char*)S_MALLOC(sys, row_len + 1);
        sys->memset(rows[d], ' ', row_len);
        rows[d][row_len] = '\0';
    }

    // Paint each entry onto its depth's row, copying the exact glyph slice
    // from row 0 so e.g. multi-char tokens / quoted strings render correctly.
    for (size_t i = 0; i < entries.size; i++) {
        DbgEntry *e = (DbgEntry*)array_get(&entries, i);
        if (e->depth < 1) continue;
        char *dest = rows[e->depth - 1];
        if (e->col + e->len > row_len) continue;
        if (e->custom) {
            sys->memset(dest + e->col, e->custom, e->len);
        } else {
            sys->memcpy(dest + e->col, row0.data + e->col, e->len);
        }
    }

    // Trim trailing spaces on each non-row-0 line.
    for (int d = 0; d < max_depth; d++) {
        size_t n = row_len;
        while (n > 0 && rows[d][n - 1] == ' ') n--;
        rows[d][n] = '\0';
    }

    // Join: row0 + "\n" + row1 + "\n" + ... (no trailing newline)
    size_t total = row0.len;
    for (int d = 0; d < max_depth; d++) {
        size_t rl = s_strlen(rows[d]);
        total += 1 + rl; // +1 for '\n'
    }

    char *out = (char*)S_MALLOC(sys, total + 1);
    size_t off = 0;
    sys->memcpy(out + off, row0.data, row0.len);
    off += row0.len;
    for (int d = 0; d < max_depth; d++) {
        out[off++] = '\n';
        size_t rl = s_strlen(rows[d]);
        sys->memcpy(out + off, rows[d], rl);
        off += rl;
    }
    out[off] = '\0';

    for (int d = 0; d < max_depth; d++) S_FREE(sys, rows[d]);
    S_FREE(sys, rows);
    S_FREE(sys, row0.data);
    array_free(&entries, sys);
    return out;
}

char *unparse_asts(Parser *p, ParseResult *res) {
    UnparseBuf sb;
    upb_init(&sb, p->sys, &p->intern);
    
    unparse_expression_r(res->code_tree, &sb);
    return upb_finish(&sb);
}

const char *parser_get_error(ParseResult *res) {
    if (!res || !res->error) return NULL;
    if (!res->owner.ctx) return NULL;
    return intern_get_cstr(res->owner.ctx, res->error);
}

// The nodes live in the parse arena; only the items arrays are heap memory.
// Iterative, for the same reason as create_parent_links.
void free_ast_node(ASTNode *node, Tsys *sys) {
    if (!node) return;
    Array stack;
    array_init(&stack, sizeof(ASTNode*));
    array_push(&stack, &node, sys);
    while (stack.size > 0) {
        ASTNode *n = ((ASTNode**)stack.data)[--stack.size];
        if (n->left) array_push(&stack, &n->left, sys);
        if (n->right) array_push(&stack, &n->right, sys);
        if (!n->items.data) continue;
        for (size_t i = 0; i < n->items.size; i++) {
            ASTNode *c = ((ASTNode**)n->items.data)[i];
            if (c) array_push(&stack, &c, sys);
        }
        array_free(&n->items, sys);
    }
    array_free(&stack, sys);
}

void free_parse_result(Parser *p, ParseResult *res) {
    Tsys *sys = p->sys;
    if (res->code_tree) {
        free_ast_node(res->code_tree, sys);
        res->code_tree = NULL;
    }
    
    if (res->txt_to_ref) {
        S_FREE(sys, res->txt_to_ref);
        res->txt_to_ref = NULL;
    }
    
    if (res->line_offsets) {
        S_FREE(sys, res->line_offsets);
        res->line_offsets = NULL;
    }
    
    if (res->arena) {
        arena_free_all(res->arena);
        S_FREE(sys, res->arena); 
    }
    
    // Clean up result owner (dynamic tokens from this specific parse)
    // The actual strings might hang around in InternCtx if there's no GC/Prune,
    // but at least we free the owner's list.
    intern_owner_deinit(&res->owner);
    // InternCtx is OWNED BY PARSER now, so we don't free it here.
}
