#include "tokens.h"
#include <stddef.h>

// Both name tables are keyed by token, so the enums in tokens.h are free to be
// reordered. The sizeof checks at the bottom catch a token appended to an enum
// without a name. Bytes that are not named tokens have no entry here; their
// one-byte strings are installed by intern_ctx_init.

const char *FixedTokenNames[] = {
    [TOK_NULL]              = NULL,

    [TOK_EMPTYSTRING]       = "",

    [TOK_PLUS]              = "+",
    [TOK_MINUS]             = "-",
    [TOK_MUL]               = "*",
    [TOK_SLASH]             = "/",
    [TOK_PERCENT]           = "%",
    [TOK_DOUBLE_PERCENT]    = "%%",
    [TOK_SLASH_TILDE]       = "/~",
    [TOK_SLASH_PERCENT]     = "/%",
    [TOK_TILDE]             = "~",
    [TOK_AMP]               = "&",
    [TOK_PIPE]              = "|",
    [TOK_CARET]             = "^",
    [TOK_LSHIFT]            = "<<",
    [TOK_RSHIFT]            = ">>",

    [TOK_URSHIFT]           = ">>>",
    [TOK_STARSTAR]          = "**",
    [TOK_AND_AND]           = "&&",
    [TOK_OR_OR]             = "||",

    [TOK_LT]                = "<",
    [TOK_GT]                = ">",
    [TOK_LT_EQ]             = "<=",
    [TOK_GT_EQ]             = ">=",
    [TOK_EQ_EQ]             = "==",
    [TOK_NOT_EQ]            = "!=",
    [TOK_EQ_EQ_EQ]          = "===",
    [TOK_NOT_EQ_EQ]         = "!==",

    [TOK_EQ]                = "=",
    [TOK_PLUS_EQ]           = "+=",
    [TOK_MINUS_EQ]          = "-=",
    [TOK_MUL_EQ]            = "*=",
    [TOK_SLASH_EQ]          = "/=",
    [TOK_PERCENT_EQ]        = "%=",
    [TOK_DOUBLE_PERCENT_EQ] = "%%=",
    [TOK_SLASH_TILDE_EQ]    = "/~=",
    [TOK_SLASH_PERCENT_EQ]  = "/%=",
    [TOK_TILDE_EQ]          = "~=",
    [TOK_AMP_EQ]            = "&=",
    [TOK_PIPE_EQ]           = "|=",
    [TOK_CARET_EQ]          = "^=",
    [TOK_LSHIFT_EQ]         = "<<=",
    [TOK_RSHIFT_EQ]         = ">>=",
    [TOK_PLUS_PLUS]         = "++",
    [TOK_MINUS_MINUS]       = "--",

    [TOK_BANG]              = "!",
    [TOK_QMARK]             = "?",
    [TOK_COLON]             = ":",

    [TOK_DOTDOT]            = "..",
    [TOK_DOTDOT_EQ]         = "..=",
    [TOK_DOTDOTDOT]         = "...",

    [TOK_COMMA]             = ",",
    [TOK_DOT]               = ".",
    [TOK_SEMI]              = ";",

    [TOK_LPAREN]            = "(",
    [TOK_LBRACKET]          = "[",
    [TOK_LBRACE]            = "{",
    [TOK_RPAREN]            = ")",
    [TOK_RBRACKET]          = "]",
    [TOK_RBRACE]            = "}",

    [TOK_DQUOTE]            = "\"",
    [TOK_SQUOTE]            = "'",

    [TOK_ARROW_R]           = "->",
    [TOK_BIGARROW_R]        = "-->",
    [TOK_ARROW_F]           = "=>",
    [TOK_ARROW_L]           = "<-",

    [TOK_LINE_COMMENT]      = "//",
    [TOK_BLOCK_OPEN]        = "/*",
    [TOK_BLOCK_CLOSE]       = "*/",

    [TOK_AS]                = "as",
    [TOK_IN]                = "in",
    [TOK_THEN]              = "then",
    [TOK_DO]                = "do",
    [TOK_ELSE]              = "else",

    [TOK_HASH]              = "#",
    [TOK_SPACE]             = " ",
    [TOK_EOF]               = "EOF",  // should not occur as a string
};

#define M(t) [(t) - TOK_MATHS_FIRST]
const char *MathTokenNames[] = {
    M(TOK_SIN)           = "sin",
    M(TOK_COS)           = "cos",
    M(TOK_SIN01)         = "sin01",
    M(TOK_COS01)         = "cos01",
    M(TOK_ATAN2)         = "atan2",
    M(TOK_EXP)           = "exp",
    M(TOK_LOG)           = "log",
    M(TOK_POW)           = "pow",
    M(TOK_IPOW)          = "ipow",
    M(TOK_SQRT)          = "sqrt",

    M(TOK_FLOOR)         = "floor",
    M(TOK_FRAC)          = "frac",
    M(TOK_ABS)           = "abs",
    M(TOK_FMOD)          = "fmod",
    M(TOK_MIN)           = "min",
    M(TOK_MAX)           = "max",
    M(TOK_CLAMP)         = "clamp",

    M(TOK_MIX)           = "mix",
    M(TOK_LINEARSTEP)    = "linearstep",
    M(TOK_SMOOTHSTEP)    = "smoothstep",
    M(TOK_SMOOTHERSTEP)  = "smootherstep",
    M(TOK_LINEARSTEPA)   = "linearstepa",
    M(TOK_SMOOTHSTEPA)   = "smoothstepa",
    M(TOK_SMOOTHERSTEPA) = "smootherstepa",

    M(TOK_I32)           = "i32",
    M(TOK_I64)           = "i64",
    M(TOK_F32)           = "f32",
    M(TOK_F64)           = "f64",
};
#undef M

// These fire when a name table and its enum have drifted in length. They must
// sit after the definitions, so that sizeof sees the initializer count rather
// than the (deliberately unsized) declaration.
typedef char _fixed_tok_count_check[
    (sizeof(FixedTokenNames)/sizeof(FixedTokenNames[0]) == TOK_COUNT) ? 1 : -1];
typedef char _math_tok_count_check[
    (sizeof(MathTokenNames)/sizeof(MathTokenNames[0]) == TOK_MATHS_LAST - TOK_MATHS_FIRST) ? 1 : -1];
