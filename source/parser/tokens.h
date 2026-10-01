#ifndef TOKENS_H
#define TOKENS_H

// Token ids are intern ids. Ids 1..255 are fixed: the id of a one-byte token IS
// the byte, so `,` is 44 and the identifier `x` is 120, and interning, looking
// up or comparing one needs no hash. 256 is the empty string, and named tokens
// of more than one byte follow it. Id 0 is TOK_NULL, "no token" (a NUL byte
// never reaches the parser).
//
// A consequence: `id < TOK_COUNT` no longer means "operator or punctuation" --
// a one-letter name is below it too. token_is_named() is that test.

#define TOKEN_IS_BASIC_OPERATOR(t)   ((t) == TOK_PLUS || (t) == TOK_MINUS || (t) == TOK_MUL || (t) == TOK_SLASH)
#define TOKEN_IS_BITWISE_OPERATOR(t) ((t) == TOK_AMP || (t) == TOK_PIPE || (t) == TOK_CARET \
    || (t) == TOK_LSHIFT || (t) == TOK_RSHIFT || (t) == TOK_URSHIFT)
#define TOKEN_IS_BOOLEAN_OPERATOR(t) ((t) == TOK_AND_AND || (t) == TOK_OR_OR)
#define TOKEN_IS_OPERATOR_EXCEPT_EQ(t) (TOKEN_IS_BASIC_OPERATOR(t) \
    || TOKEN_IS_BITWISE_OPERATOR(t) || TOKEN_IS_BOOLEAN_OPERATOR(t) \
    || (t) == TOK_PERCENT || (t) == TOK_DOUBLE_PERCENT \
    || (t) == TOK_SLASH_TILDE || (t) == TOK_SLASH_PERCENT)

// Compound assignment (`a += b`). Plain TOK_EQ is deliberately excluded --
// these are the tokens that desugar to `a = a <op> b`, and
// TOKEN_COMPOUND_ASSIGN_BINOP is the <op> they stand for. The TOK_*_EQ run
// below must stay contiguous and in the order of token_compound_binop's table.
#define TOKEN_IS_COMPOUND_ASSIGN(t)    ((t) >= TOK_PLUS_EQ && (t) <= TOK_RSHIFT_EQ)
#define TOKEN_COMPOUND_ASSIGN_BINOP(t) token_compound_binop(t)

// ++ / -- , in either prefix or postfix position.
#define TOKEN_IS_INC_DEC(t) ((t) == TOK_PLUS_PLUS || (t) == TOK_MINUS_MINUS)

// Brackets. TOKEN_CLOSE_BRACKET_FOR maps `(` `[` `{` to `)` `]` `}`: ASCII puts
// `]` and `}` two after their opener, `)` one.
#define TOKEN_IS_OPEN_BRACKET(t)   ((t) == TOK_LPAREN || (t) == TOK_LBRACKET || (t) == TOK_LBRACE)
#define TOKEN_IS_CLOSE_BRACKET(t)  ((t) == TOK_RPAREN || (t) == TOK_RBRACKET || (t) == TOK_RBRACE)
#define TOKEN_IS_BRACKET(t)        (TOKEN_IS_OPEN_BRACKET(t) || TOKEN_IS_CLOSE_BRACKET(t))
#define TOKEN_CLOSE_BRACKET_FOR(t) ((t) == TOK_LPAREN ? TOK_RPAREN : (t) + 2)

// The quote a string literal was written with.
#define TOKEN_IS_QUOTE(t) ((t) == TOK_DQUOTE || (t) == TOK_SQUOTE)

enum FixedToken {
    TOK_NULL = 0,

    // ---- one byte: the id is the byte ------------------------------------
    TOK_PLUS      = '+',
    TOK_MINUS     = '-',
    TOK_MUL       = '*',
    TOK_SLASH     = '/',
    TOK_PERCENT   = '%',    // C remainder, pairs with /~
    TOK_TILDE     = '~',    // string concat (not bitwise-not), see TOK_TILDE_EQ
    TOK_AMP       = '&',
    TOK_PIPE      = '|',
    TOK_CARET     = '^',
    TOK_LT        = '<',
    TOK_GT        = '>',
    TOK_EQ        = '=',
    TOK_BANG      = '!',
    TOK_QMARK     = '?',
    TOK_COLON     = ':',
    TOK_COMMA     = ',',
    TOK_DOT       = '.',
    TOK_SEMI      = ';',
    TOK_LPAREN    = '(',
    TOK_LBRACKET  = '[',
    TOK_LBRACE    = '{',
    TOK_RPAREN    = ')',
    TOK_RBRACKET  = ']',
    TOK_RBRACE    = '}',
    TOK_DQUOTE    = '"',
    TOK_SQUOTE    = '\'',
    TOK_HASH      = '#',    // compile-directive prefix
    TOK_SPACE     = ' ',

    TOK_BYTE_LAST = 255,

    // ---- zero bytes: the first id past the bytes -------------------------
    TOK_EMPTYSTRING,    // (empty string "", used for implicit call/application nodes)

    // ---- more than one byte ----------------------------------------------
    TOK_DOUBLE_PERCENT, // %%  floored remainder, pairs with /%
    TOK_SLASH_TILDE,    // /~  truncating division (C's integer /)
    TOK_SLASH_PERCENT,  // /%  floored division
    TOK_LSHIFT,         // <<
    TOK_RSHIFT,         // >>
    TOK_URSHIFT,        // >>>
    TOK_STARSTAR,       // ** (JS-style exponentiation operator)
    TOK_AND_AND,        // &&
    TOK_OR_OR,          // ||

    TOK_LT_EQ,          // <=
    TOK_GT_EQ,          // >=
    TOK_EQ_EQ,          // ==
    TOK_NOT_EQ,         // !=
    TOK_EQ_EQ_EQ,       // ===
    TOK_NOT_EQ_EQ,      // !==

    // Compound assignment: contiguous, see TOKEN_IS_COMPOUND_ASSIGN.
    TOK_PLUS_EQ,        // +=
    TOK_MINUS_EQ,       // -=
    TOK_MUL_EQ,         // *=
    TOK_SLASH_EQ,       // /=
    TOK_PERCENT_EQ,     // %=
    TOK_DOUBLE_PERCENT_EQ, // %%=
    TOK_SLASH_TILDE_EQ, // /~=
    TOK_SLASH_PERCENT_EQ, // /%=
    TOK_TILDE_EQ,       // ~=    (concat-assign; see TOK_TILDE)
    TOK_AMP_EQ,         // &=
    TOK_PIPE_EQ,        // |=
    TOK_CARET_EQ,       // ^=
    TOK_LSHIFT_EQ,      // <<=
    TOK_RSHIFT_EQ,      // >>=
    TOK_PLUS_PLUS,      // ++
    TOK_MINUS_MINUS,    // --

    TOK_DOTDOT,         // ..     rust/zig style ranges
    TOK_DOTDOT_EQ,      // ..=    rust style inclusive ranges
    TOK_DOTDOTDOT,      // ...    splat / spread, and the rest-parameter marker

    TOK_ARROW_R,        // ->
    TOK_BIGARROW_R,     // -->
    TOK_ARROW_F,        // =>
    TOK_ARROW_L,        // <-  (the reactive engine's soft write; unsupported elsewhere)

    // Comment markers. Consumed as whitespace; never an AST token.
    TOK_LINE_COMMENT,   // //
    TOK_BLOCK_OPEN,     // /*
    TOK_BLOCK_CLOSE,    // */

    // ---- word tokens -----------------------------------------------------
    TOK_AS,             // as (rust-style cast operator)
    TOK_IN,             // in (the loop-variable binder in `for i in 0..n`)
    TOK_THEN,           // then
    TOK_DO,             // do
    TOK_ELSE,           // else

    // ---- not ordinary source text ----------------------------------------
    TOK_EOF,            // Used for bounds? Or just checking?

    TOK_COUNT
};

// Defined in a C file. Declared without a size on purpose: that leaves the
// definition's own initializer count visible to the sizeof check in tokens.c,
// which is what catches a token added to the enum but not to the name table.
// Entries for bytes that are not named tokens are NULL.
extern const char *FixedTokenNames[];

// A named token -- operator, punctuation, keyword -- as opposed to a name or
// literal. Not the same as `t < TOK_COUNT`: see the note at the top.
static inline int token_is_named(int t) {
    return t > 0 && t < TOK_COUNT && FixedTokenNames[t] != 0;
}

static inline int token_compound_binop(int t) {
    static const short ops[] = {
        TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_SLASH, TOK_PERCENT, TOK_DOUBLE_PERCENT,
        TOK_SLASH_TILDE, TOK_SLASH_PERCENT, TOK_TILDE, TOK_AMP, TOK_PIPE, TOK_CARET,
        TOK_LSHIFT, TOK_RSHIFT,
    };
    return ops[t - TOK_PLUS_EQ];
}

// Built-in math function tokens.
// TOK_MATHS_FIRST == TOK_COUNT so they sit just above the fixed tokens.
// User-registered function tokens should start at TOK_MATHS_LAST or higher.
typedef enum {
    TOK_MATHS_FIRST = TOK_COUNT,

    // ---- transcendental --------------------------------------------------
    TOK_SIN   = TOK_MATHS_FIRST,
    TOK_COS,
    TOK_SIN01,       // input in turns: 0..1 == one cycle
    TOK_COS01,
    TOK_ATAN2,
    TOK_EXP,
    TOK_LOG,
    TOK_POW,
    TOK_IPOW,        // integer power: ipow(base, exp) -- exact integer x**n
    TOK_SQRT,

    // ---- rounding, sign, selection ---------------------------------------
    TOK_FLOOR,
    TOK_FRAC,
    TOK_ABS,
    TOK_FMOD,
    TOK_MIN,
    TOK_MAX,
    TOK_CLAMP,

    // ---- interpolation ---------------------------------------------------
    TOK_MIX,         // linear interpolation: mix(a, b, t) == a + t*(b-a)
    TOK_LINEARSTEP,
    TOK_SMOOTHSTEP,
    TOK_SMOOTHERSTEP,
    TOK_LINEARSTEPA,
    TOK_SMOOTHSTEPA,
    TOK_SMOOTHERSTEPA,

    // ---- casts (TOKEN_IS_CAST) -------------------------------------------
    // Compile-time only: these become IR_CVT, they never reach a C dispatch.
    TOK_I32,
    TOK_I64,
    TOK_F32,
    TOK_F64,

    TOK_MATHS_LAST   // exclusive end; user IDs start here
} MathToken;

#define TOKEN_IS_CAST(t) ((t) >= TOK_I32 && (t) <= TOK_F64)

// Script-visible names for built-in math tokens ("sin", "cos", ...).
// Unsized for the same reason as FixedTokenNames above.
extern const char *MathTokenNames[];

#endif
