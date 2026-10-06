// ic_complete.c -- the editing aids: a catalog of the names a script can use, the
// call a point sits in, the argument hint, comma readings and Tab completion.
// #included by interactive_coding.c after the overlay layer, so it shares that
// file's statics, and so every build that lists that file gets it.

#define ICC_STACK    64
#define ICC_MAX_ARGS 32

#define ICC_APP(buf, cap, pos, ...) do {                                            \
        int n_ = s_snprintf((buf) + (pos), (size_t)((cap) - (pos)), __VA_ARGS__);   \
        if (n_ > 0) (pos) = (pos) + n_ < (cap) ? (pos) + n_ : (cap) - 1;            \
    } while (0)

static bool icc_ident_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static bool icc_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int icc_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// n characters of src, terminated; dst holds at least n + 1.
static void icc_copy(char *dst, const char *src, int n) {
    s_strncpy(dst, src, (size_t)n + 1);
    dst[n] = '\0';
}

// ===========================================================================
// Scanning text
// ===========================================================================

// Past the string literal or comment that starts at t[i]; i itself when none
// does. *open is set when it runs into `end` unfinished, i.e. `end` is inside it.
static size_t icc_skip_lit(const char *t, size_t i, size_t end, bool *open) {
    char c = t[i];
    if (c == '/' && i + 1 < end && t[i + 1] == '/') {
        size_t j = i + 2;
        while (j < end && t[j] != '\n') j++;
        if (j >= end) *open = true;
        return j;
    }
    if (c == '/' && i + 1 < end && t[i + 1] == '*') {
        size_t j = i + 2;
        while (j + 1 < end && !(t[j] == '*' && t[j + 1] == '/')) j++;
        if (j + 1 >= end) { *open = true; return end; }
        return j + 2;
    }
    if (c == '"' || c == '\'') {
        size_t j = i + 1;
        while (j < end && t[j] != c && t[j] != '\n') {
            if (t[j] == '\\' && j + 1 < end) j++;
            j++;
        }
        if (j >= end) { *open = true; return end; }
        return t[j] == c ? j + 1 : j;       // an unterminated string ends with its line
    }
    return i;
}

// Is `off` inside a string or a comment?
static bool icc_in_literal(const char *t, size_t off) {
    for (size_t i = 0; i < off; ) {
        bool open = false;
        size_t j = icc_skip_lit(t, i, off, &open);
        if (open) return true;
        i = j != i ? j : i + 1;
    }
    return false;
}

// The start of the line holding the nearest column-0 statement at or before
// `off`: the place a bracket count can begin from.
static size_t icc_stmt_start(const char *t, size_t off) {
    size_t i = off;
    for (;;) {
        size_t ls = i;
        while (ls > 0 && t[ls - 1] != '\n') ls--;
        char c = t[ls];
        if (ls == 0) return 0;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ')' && c != ']' && c != '}' && c != '\0')
            return ls;
        i = ls - 1;
    }
}

static bool icc_is_keyword(const char *s, size_t n) {
    static const char *const kw[] = {
        "if", "else", "while", "for", "in", "then", "do", "return", "break", "continue",
        "const", "struct", "ref", "as", "switch", "case", 0
    };
    for (int i = 0; kw[i]; i++)
        if (s_strlen(kw[i]) == n && s_strncmp(s, kw[i], n) == 0) return true;
    return false;
}

// `(` at open_off closes with `)` and is followed by `=>`: a lambda's parameters.
static bool icc_paren_is_decl(const char *t, size_t len, size_t open_off) {
    size_t i = open_off + 1;
    int depth = 1;
    while (i < len && depth > 0 && i - open_off < 4096) {
        bool open = false;
        size_t j = icc_skip_lit(t, i, len, &open);
        if (j != i) { i = j; continue; }
        char c = t[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        i++;
    }
    if (depth != 0) return false;
    while (i < len && (t[i] == ' ' || t[i] == '\t')) i++;
    return i + 1 < len && t[i] == '=' && t[i + 1] == '>';
}

// A statement that starts `name arg, arg`: a call with no parentheses.
static bool icc_paren_less(const char *t, size_t off, ICCallCtx *out) {
    size_t ls = off;
    while (ls > 0 && t[ls - 1] != '\n') ls--;
    size_t p = ls;
    while (p < off && (t[p] == ' ' || t[p] == '\t')) p++;
    if (p >= off || !icc_ident_start(t[p])) return false;
    size_t ie = p;
    while (ie < off && icc_ident_char(t[ie])) ie++;
    if (ie >= off || (t[ie] != ' ' && t[ie] != '\t')) return false;
    if (icc_is_keyword(t + p, ie - p)) return false;
    size_t q = ie;
    while (q < off && (t[q] == ' ' || t[q] == '\t')) q++;
    if (q < off && s_strchr("=+-*/%<>&|^!?:;,.)]}", t[q])) return false;

    int depth = 0, commas = 0;
    for (size_t i = q; i < off; ) {
        bool open = false;
        size_t j = icc_skip_lit(t, i, off, &open);
        if (open) return false;
        if (j != i) { i = j; continue; }
        char c = t[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth == 0) return false; depth--; }
        else if (c == ',' && depth == 0) commas++;
        i++;
    }
    if (depth != 0) return false;
    out->kind = IC_CALL_CALL;
    out->callee_lo = p;
    out->callee_hi = ie;
    out->arg_index = commas;
    out->open_off = ie;
    out->parens = false;
    return true;
}

typedef struct {
    char   open;
    int    commas;
    size_t open_off, callee_lo, callee_hi;
    bool   call, index;
} ICCFrame;

bool ic_call_context(const char *t, size_t len, size_t off, ICCallCtx *out) {
    ICCallCtx zero = {0};
    *out = zero;
    if (off > len) off = len;

    ICCFrame st[ICC_STACK];
    int sp = 0;
    for (size_t i = icc_stmt_start(t, off); i < off; ) {
        bool open = false;
        size_t j = icc_skip_lit(t, i, off, &open);
        if (open) return false;             // the point is inside a string or a comment
        if (j != i) { i = j; continue; }
        char c = t[i];
        if (c == '(' || c == '[' || c == '{') {
            if (sp < ICC_STACK) {
                ICCFrame *f = &st[sp++];
                ICCFrame z = {0};
                *f = z;
                f->open = c;
                f->open_off = i;
                if (c == '(') {
                    size_t lo = i;
                    while (lo > 0 && (icc_ident_char(t[lo - 1]) || t[lo - 1] == '.')) lo--;
                    if (lo < i && icc_ident_start(t[lo]) && !icc_is_keyword(t + lo, i - lo)) {
                        f->call = true;
                        f->callee_lo = lo;
                        f->callee_hi = i;
                    }
                } else if (c == '[') {
                    char prev = i > 0 ? t[i - 1] : 0;
                    f->index = icc_ident_char(prev) || prev == ')' || prev == ']';
                }
            }
        } else if (c == ')' || c == ']' || c == '}') {
            if (sp > 0) sp--;
        } else if (c == ',') {
            if (sp > 0) st[sp - 1].commas++;
        }
        i++;
    }

    if (sp > 0 && st[sp - 1].open != '{') {
        ICCFrame *f = &st[sp - 1];
        out->arg_index = f->commas;
        out->open_off  = f->open_off;
        if (f->open == '(') {
            if (!f->call) {
                out->in_decl = icc_paren_is_decl(t, len, f->open_off);
                return false;
            }
            out->kind      = IC_CALL_CALL;
            out->parens    = true;
            out->callee_lo = f->callee_lo;
            out->callee_hi = f->callee_hi;
            return true;
        }
        out->kind = f->index ? IC_CALL_INDEX : IC_CALL_LITERAL;
        return true;
    }
    return icc_paren_less(t, off, out);
}

// The byte offset of a buffer position in `t`.
static size_t icc_offset(const char *t, int row, int col) {
    size_t i = 0;
    for (int r = 0; r < row && t[i]; i++)
        if (t[i] == '\n') r++;
    for (int c = 0; c < col && t[i] && t[i] != '\n'; c++) i++;
    return i;
}

// ===========================================================================
// The name catalog
// ===========================================================================

static ICName *icc_cat_push(InteractiveCoding *ic, ICCatalog *c) {
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 128;
        ICName *nv = (ICName *)ic->sys->realloc(c->v, (size_t)nc * sizeof(ICName));
        if (!nv) return NULL;
        c->v = nv;
        c->cap = nc;
    }
    ICName *n = &c->v[c->n++];
    ic->sys->memset(n, 0, sizeof(*n));
    n->n_params = -1;
    n->scope_id = -1;
    n->scope_lo = n->scope_hi = -1;
    return n;
}

void ic_catalog_free(InteractiveCoding *ic, ICCatalog *c) {
    if (c->v) ic->sys->free(c->v);
    c->v = NULL;
    c->n = c->cap = 0;
}

typedef struct { InteractiveCoding *ic; ICCatalog *cat; } ICVmNames;

static void icc_vm_name(void *user, const VMNameInfo *i) {
    ICVmNames *k = (ICVmNames *)user;
    ICName *n = icc_cat_push(k->ic, k->cat);
    if (!n) return;
    s_snprintf(n->name, sizeof(n->name), "%s", i->name);
    n->kind       = (int)i->kind;
    n->n_params   = i->n_params;
    n->n_optional = i->n_optional;
    if (i->params)      s_snprintf(n->params, sizeof(n->params), "%s", i->params);
    if (i->param_types) s_snprintf(n->types,  sizeof(n->types),  "%s", i->param_types);
    if (i->ret_type)    s_snprintf(n->ret,    sizeof(n->ret),    "%s", i->ret_type);
}

// What the host registers does not change while it is running, so the names a
// VM enumerates are built once per setup callback and copied from then on: the
// completion hint asks for a catalog on every keypress.
static void icc_add_vm_names(InteractiveCoding *ic, ICCatalog *cat) {
    ICComplete *cp = &ic->cp;
    if (!cp->base_ok || cp->base_setup != ic->on_vm_setup || cp->base_user != ic->on_vm_setup_user) {
        ic_catalog_free(ic, &cp->base);
        VM *vm = vm_create(ic->sys, &ic->parser);
        if (!vm) return;
        if (ic->on_vm_setup) ic->on_vm_setup(vm, ic->on_vm_setup_user);
        ICVmNames k = { ic, &cp->base };
        vm_enumerate_names(vm, icc_vm_name, &k);
        vm_destroy(vm);
        cp->base_ok    = true;
        cp->base_setup = ic->on_vm_setup;
        cp->base_user  = ic->on_vm_setup_user;
    }
    if (cp->base.n == 0) return;
    ICName *nv = (ICName *)ic->sys->realloc(cat->v, (size_t)(cat->n + cp->base.n) * sizeof(ICName));
    if (!nv) return;
    cat->v = nv;
    cat->cap = cat->n + cp->base.n;
    ic->sys->memcpy(cat->v + cat->n, cp->base.v, (size_t)cp->base.n * sizeof(ICName));
    cat->n += cp->base.n;
}

// ---- names the buffer declares ----

typedef struct {
    InteractiveCoding *ic;
    ICCatalog         *cat;
    ASTNode           *scopes[64];
    int                n_scopes;
    int                first;       // where this source's names begin, for dedup
    bool               top_only;    // a prelude: only what it declares at the top
} ICColl;

static ASTNode *icc_pinner(ASTNode *n) {
    while (n && n->left_bracket == TOK_LPAREN && n->items.size == 1)
        n = *(ASTNode **)array_get(&n->items, 0);
    return n;
}

static ASTNode *icc_unwrap(ASTNode *n) {
    while (n && n->token == 0 && !n->number_flags && !n->left && !n->right
           && n->left_bracket == 0 && n->items.size == 1)
        n = *(ASTNode **)array_get(&n->items, 0);
    return n;
}

static const char *icc_ident_name(InteractiveCoding *ic, ASTNode *n) {
    if (!n || !n->token || n->number_flags || n->string || n->left || n->right
        || n->left_bracket || n->items.size) return NULL;
    const char *s = intern_get_cstr(&ic->parser.intern, n->token);
    return s && icc_ident_start((unsigned char)s[0]) ? s : NULL;
}

static void icc_type_text(InteractiveCoding *ic, ASTNode *t, char *out, int cap) {
    out[0] = '\0';
    if (!t) return;
    char *s = unparse_expression(&ic->parser, t);
    if (!s) return;
    const char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\n') a++;
    int n = (int)s_strlen(a);
    while (n > 0 && (a[n - 1] == ' ' || a[n - 1] == '\t' || a[n - 1] == '\n')) n--;
    if (n >= cap) n = cap - 1;
    ic->sys->memcpy(out, a, (size_t)n);
    out[n] = '\0';
    ic->sys->free(s);
}

static ICName *icc_add_name(ICColl *k, const char *name, int kind, int scope) {
    if (k->top_only && scope != -1) return NULL;
    for (int i = k->first; i < k->cat->n; i++) {
        ICName *e = &k->cat->v[i];
        if (e->scope_id == scope && e->kind == kind && s_strcmp(e->name, name) == 0) return NULL;
    }
    ICName *n = icc_cat_push(k->ic, k->cat);
    if (!n) return NULL;
    s_snprintf(n->name, sizeof(n->name), "%s", name);
    n->kind     = kind;
    n->scope_id = scope;
    return n;
}

static int icc_scope_add(ICColl *k, ASTNode *arrow) {
    if (k->n_scopes >= 64) return -1;
    k->scopes[k->n_scopes] = arrow;
    return k->n_scopes++;
}

static bool icc_blank(ASTNode *n) {
    return !n || (n->token == 0 && !n->string && !n->number_flags && !n->left && !n->right
                  && n->left_bracket == 0 && n->items.size == 0);
}

// The parameters of a `(a, b: f64, c = 1) =>` list. Fills `fn` with their text
// when it is given, and adds each as a name bound inside `scope`.
static void icc_params(ICColl *k, ASTNode *pl, int scope, ICName *fn) {
    InteractiveCoding *ic = k->ic;
    ASTNode *items[ICC_MAX_ARGS];
    int ni = 0;
    ASTNode *p = icc_unwrap(pl);
    if (p && p->left_bracket == TOK_LPAREN) {
        for (size_t i = 0; i < p->items.size && ni < ICC_MAX_ARGS; i++) {
            ASTNode *it = *(ASTNode **)array_get(&p->items, i);
            if (!icc_blank(it)) items[ni++] = it;
        }
    } else if (p) {
        items[ni++] = p;
    }

    int  pos = 0, opt = 0;
    char text[IC_PARAMS_MAX];
    text[0] = '\0';
    for (int i = 0; i < ni; i++) {
        ASTNode *it = icc_pinner(icc_unwrap(items[i]));
        if (it && it->token == TOK_EQ && it->left && it->right) { opt++; it = icc_pinner(it->left); }
        bool rest = false;
        if (it && it->token == TOK_DOTDOTDOT && !it->left && it->right) { rest = true; it = icc_pinner(it->right); }
        ASTNode *tnode = NULL;
        if (it && it->token == TOK_COLON && it->left && it->right) {
            tnode = it->right;
            it = icc_pinner(it->left);
            if (it && it->token == TOK_DOTDOTDOT && !it->left && it->right) { rest = true; it = icc_pinner(it->right); }
        }
        const char *nm = icc_ident_name(ic, it);
        if (!nm) continue;
        char type[IC_TYPES_MAX];
        icc_type_text(ic, tnode, type, sizeof(type));
        if (fn) {
            ICC_APP(text, (int)sizeof(text), pos, "%s%s%s", pos ? ", " : "", rest ? "..." : "", nm);
            if (type[0]) ICC_APP(text, (int)sizeof(text), pos, ": %s", type);
        }
        ICName *n = icc_add_name(k, nm, ICN_PARAM, scope);
        if (n && type[0]) s_snprintf(n->types, sizeof(n->types), "%s", type);
    }
    if (fn) {
        s_snprintf(fn->params, sizeof(fn->params), "%s", text);
        fn->n_params   = ni;
        fn->n_optional = opt;
    }
}

static void icc_walk(ICColl *k, ASTNode *n, int scope, int depth) {
    if (!n || depth > 200) return;
    InteractiveCoding *ic = k->ic;
    int inner = scope;

    if (n->token == TOK_ARROW_F && n->left) {
        inner = icc_scope_add(k, n);
        icc_params(k, n->left, inner, NULL);
    } else if (n->token == TOK_EMPTYSTRING && n->left && n->right) {
        const char *w = icc_ident_name(ic, n->left);
        ASTNode *rhs = icc_pinner(icc_unwrap(n->right));
        if (w && s_strcmp(w, "const") == 0 && rhs && rhs->token == TOK_EQ && rhs->left) {
            ASTNode *nm = icc_pinner(rhs->left);
            if (nm && nm->token == TOK_COLON && nm->left) nm = icc_pinner(nm->left);
            const char *cn = icc_ident_name(ic, nm);
            if (cn) icc_add_name(k, cn, ICN_CONSTDECL, scope);
        }
    } else if ((n->token == TOK_EQ || TOKEN_IS_COMPOUND_ASSIGN(n->token)) && n->left && n->right) {
        ASTNode *lhs = icc_pinner(n->left);
        ASTNode *tnode = NULL;
        if (lhs && lhs->token == TOK_COLON && lhs->left && lhs->right) {
            tnode = lhs->right;
            lhs = icc_pinner(lhs->left);
        }
        const char *nm = icc_ident_name(ic, lhs);
        if (nm) {
            ASTNode *rhs = icc_pinner(n->right);
            if (n->token == TOK_EQ && rhs && rhs->token == TOK_ARROW_F && rhs->left) {
                ICName *f = icc_add_name(k, nm, ICN_FUNC, scope);
                if (f) icc_params(k, rhs->left, -2, f);
            } else {
                ICName *v = icc_add_name(k, nm, ICN_VAR, scope);
                if (v && tnode) icc_type_text(ic, tnode, v->types, (int)sizeof(v->types));
            }
        }
    } else if (n->token == TOK_IN && n->left) {
        const char *nm = icc_ident_name(ic, icc_pinner(icc_unwrap(n->left)));
        if (nm) icc_add_name(k, nm, ICN_LOOPVAR, scope);
    } else if (n->token == 0 && n->items.size == 2) {
        ASTNode *a0 = icc_pinner(icc_unwrap(*(ASTNode **)array_get(&n->items, 0)));
        ASTNode *a1 = icc_pinner(icc_unwrap(*(ASTNode **)array_get(&n->items, 1)));
        const char *nm = a1 && a1->token == TOK_IN ? icc_ident_name(ic, a0) : NULL;
        if (nm) icc_add_name(k, nm, ICN_LOOPVAR, scope);
    }

    icc_walk(k, n->left, inner, depth + 1);
    icc_walk(k, n->right, inner, depth + 1);
    for (size_t i = 0; i < n->items.size; i++)
        icc_walk(k, *(ASTNode **)array_get(&n->items, i), inner, depth + 1);
}

// The rows each registered lambda covers, in one pass over the position map.
static void icc_scope_rows(ICColl *k, ParseResult *pr, int *lo, int *hi) {
    for (int i = 0; i < k->n_scopes; i++) { lo[i] = -1; hi[i] = -1; }
    if (k->n_scopes == 0 || !pr->txt_to_ref) return;
    int    row = 0;
    size_t next_line = pr->num_lines_offsets > 0 ? pr->line_offsets[0] : (size_t)-1;
    for (size_t o = 0; o < pr->num_txt_to_ref; o++) {
        while (o >= next_line) {
            row++;
            next_line = row < pr->num_lines_offsets ? pr->line_offsets[row] : (size_t)-1;
        }
        for (ASTNode *p = pr->txt_to_ref[o]; p; p = p->parent) {
            if (p->token != TOK_ARROW_F) continue;
            for (int i = 0; i < k->n_scopes; i++) {
                if (k->scopes[i] != p) continue;
                if (lo[i] < 0 || row < lo[i]) lo[i] = row;
                if (row > hi[i]) hi[i] = row;
                break;
            }
        }
    }
}

// Names from `#define NAME` lines, which the parse does not see as declarations.
static void icc_defines(ICColl *k, const char *src) {
    for (const char *p = src; *p; ) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (s_strncmp(q, "#define", 7) == 0 && (q[7] == ' ' || q[7] == '\t')) {
            q += 7;
            while (*q == ' ' || *q == '\t') q++;
            const char *e = q;
            while (icc_ident_char((unsigned char)*e)) e++;
            if (e > q && icc_ident_start((unsigned char)*q)) {
                char nm[IC_NAME_MAX];
                int n = (int)(e - q);
                if (n >= IC_NAME_MAX) n = IC_NAME_MAX - 1;
                icc_copy(nm, q, n);
                nm[n] = '\0';
                icc_add_name(k, nm, ICN_DEFINE, -1);
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

// Collect the names `src` declares. `first_row_offset` is unused by callers that
// pass NULL scopes; prelude text is read for what it declares at the top only.
static void icc_collect_source(InteractiveCoding *ic, ICCatalog *cat, const char *src, bool top_only) {
    if (!src || !src[0]) return;
    ParseResult pr = parse_to_asts(&ic->parser, src, PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
    if (!parser_get_error(&pr) && pr.code_tree) {
        ICColl *k = (ICColl *)ic->sys->malloc(sizeof(ICColl));
        if (k) {
            ic->sys->memset(k, 0, sizeof(*k));
            k->ic = ic;
            k->cat = cat;
            k->first = cat->n;
            k->top_only = top_only;
            icc_walk(k, pr.code_tree, -1, 0);
            int lo[64], hi[64];
            icc_scope_rows(k, &pr, lo, hi);
            for (int i = k->first; i < cat->n; i++) {
                ICName *n = &cat->v[i];
                if (n->scope_id < 0 || n->scope_id >= k->n_scopes) continue;
                n->scope_lo = lo[n->scope_id];
                n->scope_hi = hi[n->scope_id];
            }
            icc_defines(k, src);
            ic->sys->free(k);
        }
    }
    free_parse_result(&ic->parser, &pr);
}

// The wrap's parameters: every body in the editor can name them.
static void icc_add_wrap_params(InteractiveCoding *ic, ICEditor *ed, ICCatalog *cat) {
    const char *p = ed->wrap_params;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *e = p;
        while (icc_ident_char((unsigned char)*e)) e++;
        if (e > p && icc_ident_start((unsigned char)*p)) {
            ICName *n = icc_cat_push(ic, cat);
            if (!n) return;
            int len = (int)(e - p);
            if (len >= IC_NAME_MAX) len = IC_NAME_MAX - 1;
            icc_copy(n->name, p, len);
            n->name[len] = '\0';
            n->kind = ICN_WRAP;
            const char *t = e;
            while (*t == ' ') t++;
            if (*t == ':') {
                t++;
                while (*t == ' ') t++;
                const char *te = t;
                while (*te && *te != ',' && *te != '=') te++;
                while (te > t && te[-1] == ' ') te--;
                int tl = (int)(te - t);
                if (tl >= IC_TYPES_MAX) tl = IC_TYPES_MAX - 1;
                icc_copy(n->types, t, tl);
                n->types[tl] = '\0';
            }
        }
        while (*p && *p != ',') p++;
    }
}

void ic_catalog_build(InteractiveCoding *ic, ICEditor *ed, ICCatalog *out) {
    out->v = NULL;
    out->n = out->cap = 0;
    icc_add_vm_names(ic, out);
    if (!ed) return;
    if (ed->prelude) icc_collect_source(ic, out, ed->prelude, true);
    icc_add_wrap_params(ic, ed, out);
    icc_collect_source(ic, out, ed->good_src, false);
}

static int icc_split_list(const char *s, int *st, int *ln, int max);

// ---- inferred types ----

// The parameter list of the first `name = (...) => ...` in the tree.
static ASTNode *icc_find_func_params(InteractiveCoding *ic, ASTNode *n, const char *name, int depth) {
    if (!n || depth > 200) return NULL;
    if (n->token == TOK_EQ && n->left && n->right) {
        ASTNode *rhs = icc_pinner(n->right);
        const char *nm = icc_ident_name(ic, icc_pinner(n->left));
        if (nm && rhs && rhs->token == TOK_ARROW_F && rhs->left && s_strcmp(nm, name) == 0) return rhs->left;
    }
    ASTNode *f = icc_find_func_params(ic, n->left, name, depth + 1);
    if (!f) f = icc_find_func_params(ic, n->right, name, depth + 1);
    for (size_t i = 0; !f && i < n->items.size; i++)
        f = icc_find_func_params(ic, *(ASTNode **)array_get(&n->items, i), name, depth + 1);
    return f;
}

// "(a: i32, b: f64) => f64" as "i32, f64" and "f64".
static void icc_label_types(const char *label, char *types, int tcap, char *ret, int rcap) {
    types[0] = ret[0] = '\0';
    if (label[0] != '(') return;
    int depth = 0, close = -1;
    for (int i = 0; label[i]; i++) {
        if (label[i] == '(') depth++;
        else if (label[i] == ')' && --depth == 0) { close = i; break; }
    }
    if (close < 0) return;
    char inner[IC_PARAMS_MAX];
    int il = close - 1 < (int)sizeof(inner) - 1 ? close - 1 : (int)sizeof(inner) - 1;
    icc_copy(inner, label + 1, il);
    int st[ICC_MAX_ARGS], ln[ICC_MAX_ARGS];
    int n = icc_split_list(inner, st, ln, ICC_MAX_ARGS);
    int pos = 0;
    for (int i = 0; i < n; i++) {
        int c = -1;
        for (int k = 0; k < ln[i]; k++) if (inner[st[i] + k] == ':') { c = k; break; }
        if (c < 0) { types[0] = '\0'; return; }
        int a = st[i] + c + 1, b = st[i] + ln[i];
        while (a < b && inner[a] == ' ') a++;
        char ty[IC_TYPES_MAX];
        int tl = b - a < (int)sizeof(ty) - 1 ? b - a : (int)sizeof(ty) - 1;
        icc_copy(ty, inner + a, tl);
        ICC_APP(types, tcap, pos, "%s%s", i ? ", " : "", ty);
    }
    const char *arrow = s_strstr(label + close, "=> ");
    if (arrow) s_snprintf(ret, (size_t)rcap, "%s", arrow + 3);
}

// Fill in the types of the buffer function `name` when it has exactly one
// specialisation -- several mean the types depend on the call, so the names are
// all that is true. A compile only: nothing runs.
static void icc_infer_types(InteractiveCoding *ic, ICEditor *ed, ICName *target) {
    if (!ed || !ed->good_src || !target || target->kind != ICN_FUNC) return;
    const char *name = target->name;

    char *wrapped = build_wrapped_source(ic, ed, ed->good_src, NULL, NULL);
    ParseResult res = parse_to_asts(&ic->parser, wrapped ? wrapped : ed->good_src, 0);
    if (!parser_get_error(&res) && res.code_tree) {
        ASTNode *params = icc_find_func_params(ic, res.code_tree, name, 0);
        VM *vm = vm_create(ic->sys, &ic->parser);
        if (vm) {
            if (ic->on_vm_setup) ic->on_vm_setup(vm, ic->on_vm_setup_user);
            func_create(vm, res.code_tree, &res);
            char labels[512];
            if (params && vm_signature_labels(vm, params, labels, (int)sizeof(labels)) == 1)
                icc_label_types(labels, target->types, (int)sizeof(target->types),
                                target->ret, (int)sizeof(target->ret));
            vm_destroy(vm);
        }
    }
    free_parse_result(&ic->parser, &res);
    if (wrapped) ic->sys->free(wrapped);
}

// ---- querying ----

// Lower is better. A name bound by the lambda the caret is in first, then the
// wrap's parameters, the buffer's own top-level names, what the host binds, the
// buffer's functions, built-ins, intrinsics -- and last, names local to some
// other function.
static int icc_rank(const ICName *n, int caret_row) {
    bool local = n->scope_id >= 0 || n->scope_lo >= 0;
    if (local) {
        if (n->scope_lo >= 0 && caret_row >= n->scope_lo && caret_row <= n->scope_hi) return 0;
        return 7;
    }
    switch (n->kind) {
    case ICN_WRAP:      return 1;
    case ICN_VAR: case ICN_PARAM: case ICN_LOOPVAR: case ICN_CONSTDECL: case ICN_DEFINE: return 2;
    case ICN_GLOBAL: case ICN_CONST: case ICN_HOST_BUF: return 3;
    case ICN_FUNC:      return 4;
    case ICN_NATIVE: case ICN_LIB: return 5;
    case ICN_OPTION_ALT: return 7;
    default:            return 6;
    }
}

// What a menu button shows: an option that already holds by default is in parens.
static void icc_label(const ICName *n, char *out, int cap) {
    s_snprintf(out, (size_t)cap, n->kind == ICN_OPTION_ALT ? "(%s)" : "%s", n->name);
}

// 0 = prefix as typed, 1 = prefix ignoring case, 2 = anywhere, -1 = no match.
static int icc_match_tier(const char *name, const char *q, int qlen) {
    int i = 0;
    while (i < qlen && name[i] == q[i]) i++;
    if (i == qlen) return 0;
    i = 0;
    while (i < qlen && name[i] && icc_lower(name[i]) == icc_lower(q[i])) i++;
    if (i == qlen) return 1;
    if (qlen < 2) return -1;
    int nlen = (int)s_strlen(name);
    for (int s = 1; s + qlen <= nlen; s++) {
        int k = 0;
        while (k < qlen && icc_lower(name[s + k]) == icc_lower(q[k])) k++;
        if (k == qlen) return 2;
    }
    return -1;
}

typedef struct { int idx, tier, rank, len; } ICHit;

static bool icc_hit_before(const ICCatalog *c, const ICHit *a, const ICHit *b) {
    if (a->tier != b->tier) return a->tier < b->tier;
    if (a->rank != b->rank) return a->rank < b->rank;
    if (a->len  != b->len)  return a->len < b->len;
    return s_strcmp(c->v[a->idx].name, c->v[b->idx].name) < 0;
}

int ic_catalog_query(const ICCatalog *c, const char *q, int qlen, int caret_row,
                     int *hits, int max_hits) {
    int n = 0;
    for (int i = 0; i < c->n; i++)
        if ((int)s_strlen(c->v[i].name) == qlen && s_strncmp(c->v[i].name, q, (size_t)qlen) == 0) return 0;
    // On the stack: the catalog is a few hundred names at most.
    ICHit tmp[512];
    for (int i = 0; i < c->n && n < 512; i++) {
        const ICName *nm = &c->v[i];
        int tier = icc_match_tier(nm->name, q, qlen);
        if (tier < 0) continue;
        ICHit h = { i, tier, icc_rank(nm, caret_row), (int)s_strlen(nm->name) };
        // One entry per name: the better-ranked declaration stands for it.
        int dup = -1;
        for (int j = 0; j < n; j++)
            if (s_strcmp(c->v[tmp[j].idx].name, nm->name) == 0) { dup = j; break; }
        if (dup >= 0) {
            if (icc_hit_before(c, &h, &tmp[dup])) tmp[dup] = h;
            continue;
        }
        tmp[n++] = h;
    }
    for (int i = 1; i < n; i++) {
        ICHit h = tmp[i];
        int j = i - 1;
        while (j >= 0 && icc_hit_before(c, &h, &tmp[j])) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = h;
    }
    if (n > max_hits) n = max_hits;
    for (int i = 0; i < n; i++) hits[i] = tmp[i].idx;
    return n;
}

const ICName *ic_catalog_find(const ICCatalog *c, const char *name, int caret_row) {
    const ICName *best = NULL;
    int best_rank = 99;
    for (int i = 0; i < c->n; i++) {
        const ICName *n = &c->v[i];
        if (s_strcmp(n->name, name) != 0) continue;
        int r = icc_rank(n, caret_row);
        // A callable beats a plain variable of the same name.
        if (n->n_params < 0) r += 10;
        if (!best || r < best_rank) { best = n; best_rank = r; }
    }
    return best;
}

// ===========================================================================
// Parameter text
// ===========================================================================

// Split "a: f64, b = 1, ...c" at its top-level commas into trimmed entries.
static int icc_split_list(const char *s, int *st, int *ln, int max) {
    int n = 0, depth = 0;
    int start = 0;
    int i = 0;
    for (;; i++) {
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        if ((c == ',' && depth == 0) || c == '\0') {
            int a = start, b = i;
            while (a < b && s[a] == ' ') a++;
            while (b > a && s[b - 1] == ' ') b--;
            if (b > a && n < max) { st[n] = a; ln[n] = b - a; n++; }
            start = i + 1;
            if (c == '\0') break;
        }
    }
    return n;
}

// One entry as the label shows it: `name`, or `name: type` when `typed` and the
// entry carries its own annotation or the catalog has a type for the slot. A
// default is never shown; the brackets around the slot say it is optional.
static void icc_entry_text(const char *params, int st, int ln, const char *type, int type_len,
                           bool typed, char *out, int cap) {
    int pos = 0;
    int n = ln;
    for (int i = 0; i < ln; i++)
        if (params[st + i] == '=') { n = i; break; }
    while (n > 0 && params[st + n - 1] == ' ') n--;
    bool has_colon = false;
    for (int i = 0; i < n; i++) if (params[st + i] == ':') { has_colon = true; if (!typed) n = i; break; }
    while (n > 0 && params[st + n - 1] == ' ') n--;
    if (n >= cap) n = cap - 1;
    for (int i = 0; i < n; i++) out[pos++] = params[st + i];
    out[pos] = '\0';
    if (typed && !has_colon && type_len > 0) {
        char ty[IC_TYPES_MAX];
        if (type_len >= (int)sizeof(ty)) type_len = (int)sizeof(ty) - 1;
        icc_copy(ty, type, type_len);
        ICC_APP(out, cap, pos, ": %s", ty);
    }
}

// The parameter slots of a callee, with the optional ones counted from the end.
typedef struct {
    int         n;
    bool        variadic;
    int         pst[ICC_MAX_ARGS], pln[ICC_MAX_ARGS];
    int         tst[ICC_MAX_ARGS], tln[ICC_MAX_ARGS];
    int         nt;
    char        synth[ICC_MAX_ARGS * 3 + 1];
    const char *params;
} ICSlots;

static void icc_slots(const ICName *sig, ICSlots *s) {
    s->params = sig->params;
    s->n = icc_split_list(sig->params, s->pst, s->pln, ICC_MAX_ARGS);
    s->nt = icc_split_list(sig->types, s->tst, s->tln, ICC_MAX_ARGS);
    s->variadic = s_strstr(sig->params, "...") != NULL;
    if (s->n == 0 && sig->n_params > 0) {
        // Arity is known even when the names are not: `_, _, _`.
        int n = sig->n_params < ICC_MAX_ARGS ? sig->n_params : ICC_MAX_ARGS;
        int pos = 0;
        for (int i = 0; i < n; i++) {
            if (i) { s->synth[pos++] = ','; s->synth[pos++] = ' '; }
            s->synth[pos++] = '_';
            s->pst[i] = pos - 1;
            s->pln[i] = 1;
        }
        s->synth[pos] = '\0';
        s->params = s->synth;
        s->n = n;
    }
}

static void icc_slot_text(const ICSlots *s, const ICName *sig, int i, bool typed, char *out, int cap) {
    const char *ty = NULL;
    int tl = 0;
    if (i < s->nt) { ty = sig->types + s->tst[i]; tl = s->tln[i]; }
    icc_entry_text(s->params, s->pst[i], s->pln[i], ty, tl, typed, out, cap);
}

// The text of the argument hint for a caret in argument `arg`. `warn` is set
// when the call already has more arguments than the callee takes.
static void icc_build_hint(const ICName *sig, int arg, bool parens,
                           char *text, int tcap, char *alt, int acap, bool *warn) {
    *warn = false;
    text[0] = alt[0] = '\0';
    if (sig->n_params < 0) return;
    ICSlots s;
    icc_slots(sig, &s);
    if (arg >= s.n && !s.variadic) {
        s_snprintf(text, (size_t)tcap, "too many args for %s", sig->name);
        s_snprintf(alt, (size_t)acap, "too many args for %s", sig->name);
        *warn = true;
        return;
    }
    int tp = 0, ap = 0;
    int opt_from = s.n - sig->n_optional;
    for (int i = arg < s.n ? arg : s.n - 1; i < s.n; i++) {
        if (i < 0) break;
        char a[96], b[96];
        icc_slot_text(&s, sig, i, true, a, sizeof(a));
        icc_slot_text(&s, sig, i, false, b, sizeof(b));
        bool optional = sig->n_optional > 0 && i >= opt_from;
        ICC_APP(text, tcap, tp, "%s%s%s%s", i > (arg < s.n ? arg : s.n - 1) ? ", " : "", optional ? "[" : "", a, optional ? "]" : "");
        ICC_APP(alt,  acap, ap, "%s%s%s%s", i > (arg < s.n ? arg : s.n - 1) ? ", " : "", optional ? "[" : "", b, optional ? "]" : "");
    }
    if (parens) {
        ICC_APP(text, tcap, tp, ")");
        ICC_APP(alt,  acap, ap, ")");
    }
    if (sig->ret[0]) ICC_APP(text, tcap, tp, " => %s", sig->ret);
}

// `x0 , y0` -- the names either side of the comma that ends argument `arg`. The
// long form carries types; `*at` / `*alt_at` are where the comma sits in each.
static bool icc_build_comma(const ICName *sig, int arg, char *text, int tcap, char *alt, int acap,
                            int *at, int *alt_at) {
    text[0] = alt[0] = '\0';
    if (sig->n_params < 0) return false;
    ICSlots s;
    icc_slots(sig, &s);
    if (arg >= s.n && !s.variadic) return false;
    char l1[96], l2[96], r1[96], r2[96];
    int li = arg < s.n ? arg : s.n - 1;
    if (li < 0) return false;
    icc_slot_text(&s, sig, li, true, l1, sizeof(l1));
    icc_slot_text(&s, sig, li, false, l2, sizeof(l2));
    r1[0] = r2[0] = '\0';
    if (li + 1 < s.n) {
        icc_slot_text(&s, sig, li + 1, true, r1, sizeof(r1));
        icc_slot_text(&s, sig, li + 1, false, r2, sizeof(r2));
    }
    int tp = 0, ap = 0;
    ICC_APP(text, tcap, tp, "%s ,", l1);
    *at = tp - 1;
    if (r1[0]) ICC_APP(text, tcap, tp, " %s", r1);
    ICC_APP(alt, acap, ap, "%s ,", l2);
    *alt_at = ap - 1;
    if (r2[0]) ICC_APP(alt, acap, ap, " %s", r2);
    return true;
}

// ===========================================================================
// Editor state shared by the three aids
// ===========================================================================

static bool icc_editor_focused(UIContext *ui, ICEditor *ed) {
    return ed && ed->ta && ui->focus_id == ui_id_from_ptr(ed->ta);
}

static void icc_drop_key(UIContext *ui, int i) {
    for (int k = i + 1; k < ui->num_key_events; k++) {
        ui->key_events[k - 1] = ui->key_events[k];
        ui->key_flags[k - 1]  = ui->key_flags[k];
    }
    ui->num_key_events--;
}

static void icc_drop_char(UIContext *ui, int i) {
    for (int k = i + 1; k < ui->num_char_events; k++) {
        ui->char_events[k - 1] = ui->char_events[k];
        ui->char_flags[k - 1]  = ui->char_flags[k];
    }
    ui->num_char_events--;
}

static bool icc_mouse_pressed(UIContext *ui) {
    return ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT] || ui->mouse_pressed[UI_MOUSE_BUTTON_RIGHT];
}

// The text, and the caret's offset in it. Caller frees.
static char *icc_text_and_caret(ICEditor *ed, size_t *caret) {
    char *t = ui_textarea_get_text(ed->ta);
    if (!t) return NULL;
    int r, c;
    ui_textarea_get_cursor(ed->ta, &r, &c);
    *caret = icc_offset(t, r, c);
    return t;
}

// ===========================================================================
// Feature A: the remaining parameters after a typed comma or `(`
// ===========================================================================

// Keywords whose clause has a shape worth showing, like a call's parameters.
static const struct { const char *kw, *hint, *alt; } icc_kw_hints[] = {
    { "for",   "i in a..b  |  x in xs  |  i, x in xs", "i in a..b" },
    { "while", "cond  [do stmt]", "cond" },
    { "if",    "cond  [then stmt]", "cond" },
    { "const", "name = value", NULL },
};
#define ICC_KW_N ((int)(sizeof(icc_kw_hints) / sizeof(icc_kw_hints[0])))

// Which keyword occupies columns [c0, c1) of the row, when it is the first thing
// on it. -1 for none.
static int icc_kw_index(ICEditor *ed, int row, int c1) {
    int c0 = c1;
    while (c0 > 0 && icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, row, c0 - 1))) c0--;
    for (int c = 0; c < c0; c++) {
        char ch = ui_textarea_get_char_at(ed->ta, row, c);
        if (ch != ' ' && ch != '\t') return -1;
    }
    for (int i = 0; i < ICC_KW_N; i++) {
        int n = (int)s_strlen(icc_kw_hints[i].kw);
        if (n != c1 - c0) continue;
        int k = 0;
        while (k < n && ui_textarea_get_char_at(ed->ta, row, c0 + k) == icc_kw_hints[i].kw[k]) k++;
        if (k == n) return i;
    }
    return -1;
}

static void ic_on_insert_text(void *user, UITextArea *ta, int row, int col, int length) {
    InteractiveCoding *ic = (InteractiveCoding *)user;
    if (!ic || !ic->cp.on || length != 1) return;
    ICEditor *ed = find_editor_for_ta(ic, ta);
    if (!ed) return;
    char c = ui_textarea_get_char_at(ta, row, col);
    if (c == ' ') {
        int k = icc_kw_index(ed, row, col);
        if (k >= 0) { ic->cp.kw_ed = ed; ic->cp.kw_idx = k + 1; return; }
        ic->cp.hint_ed    = ed;
        ic->cp.hint_armed = true;
        return;
    }
    if (c != ',' && c != '(') return;
    ic->cp.kw_idx     = 0;
    ic->cp.hint_ed    = ed;
    ic->cp.hint_armed = true;
}

static void icc_hint_off(ICComplete *cp) {
    cp->hint_on = false;
    cp->hint_armed = false;
    cp->hint_have = false;
}

// Resolve the callee of a context against the catalog, copying what the hint
// needs into `sig`. False when there is nothing callable by that name.
static bool icc_lookup_callee(InteractiveCoding *ic, ICEditor *ed, const char *text,
                              const ICCallCtx *ctx, int caret_row, ICName *sig) {
    char name[IC_NAME_MAX];
    size_t n = ctx->callee_hi - ctx->callee_lo;
    if (n == 0 || n >= sizeof(name)) return false;
    ic->sys->memcpy(name, text + ctx->callee_lo, n);
    name[n] = '\0';
    ICCatalog cat;
    ic_catalog_build(ic, ed, &cat);
    ICName *found = (ICName *)ic_catalog_find(&cat, name, caret_row);
    if (found && found->kind == ICN_FUNC) icc_infer_types(ic, ed, found);
    bool ok = found && found->n_params >= 0;
    if (ok) *sig = *found;
    ic_catalog_free(ic, &cat);
    return ok;
}

// Once a frame, before the textarea reads this frame's keys.
static void icc_hint_update(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    if (!cp->on) { icc_hint_off(cp); return; }
    ICEditor *ed = cp->hint_ed;
    if (!ed || !ed->ta || !icc_editor_focused(ui, ed)) {
        if (cp->hint_on || cp->hint_armed) icc_hint_off(cp);
        return;
    }
    if (icc_mouse_pressed(ui)) { icc_hint_off(cp); return; }
    if (ed->needs_parse && !cp->hint_armed && !cp->hint_on) return;

    // Escape dismisses it, and is consumed only while it is up.
    if (cp->hint_on) {
        for (int i = 0; i < ui->num_key_events; i++) {
            if (ui->key_events[i] != (int)PLATFORM_KEY_ESCAPE || (ui->key_flags[i] & (UI_FLAGS_CTRL | UI_FLAGS_SHIFT))) continue;
            icc_drop_key(ui, i);
            icc_hint_off(cp);
            ui->wants_escape = 1;
            return;
        }
    }
    if (!cp->hint_on && !cp->hint_armed) return;

    int row, col;
    ui_textarea_get_cursor(ed->ta, &row, &col);
    if (cp->hint_have && !cp->hint_armed && cp->hint_serial == ed->edit_serial &&
        cp->hint_row == row && cp->hint_col == col) { ui->wants_escape = 1; return; }

    size_t caret;
    char *text = icc_text_and_caret(ed, &caret);
    if (!text) { icc_hint_off(cp); return; }
    ICCallCtx ctx;
    bool ok = ic_call_context(text, s_strlen(text), caret, &ctx) && ctx.kind == IC_CALL_CALL;

    if (ok && cp->hint_armed) {
        // A new trigger inside the call already shown just advances it.
        if (!cp->hint_on || cp->hint_open_off != ctx.open_off) {
            ICName sig;
            ok = icc_lookup_callee(ic, ed, text, &ctx, row, &sig);
            if (ok) { cp->hint_sig = sig; cp->hint_open_off = ctx.open_off; cp->hint_on = true; }
        }
        cp->hint_armed = false;
    }
    if (ok && (!cp->hint_on || cp->hint_open_off != ctx.open_off)) ok = false;
    if (!ok) {
        ic->sys->free(text);
        icc_hint_off(cp);
        return;
    }
    cp->hint_arg    = ctx.arg_index;
    cp->hint_parens = ctx.parens;
    cp->hint_row    = row;
    cp->hint_col    = col;
    cp->hint_serial = ed->edit_serial;
    icc_build_hint(&cp->hint_sig, ctx.arg_index, ctx.parens, cp->hint_text, IC_HINT_TEXT,
                   cp->hint_alt, IC_HINT_TEXT, &cp->hint_warn);
    cp->hint_have = cp->hint_text[0] != '\0';
    ic->sys->free(text);
    ui->wants_escape = 1;
}

// Once a frame, before the textarea reads this frame's keys: the clause hint
// lives while the caret stays on the keyword's row, still after `kw `.
static void icc_kw_update(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    if (!cp->kw_idx) return;
    ICEditor *ed = cp->kw_ed;
    if (!cp->on || !ed || !ed->ta || !icc_editor_focused(ui, ed) || icc_mouse_pressed(ui) || cp->hint_on || cp->menu_open) {
        cp->kw_idx = 0;
        return;
    }
    for (int i = 0; i < ui->num_key_events; i++) {
        if (ui->key_events[i] != (int)PLATFORM_KEY_ESCAPE || (ui->key_flags[i] & (UI_FLAGS_CTRL | UI_FLAGS_SHIFT))) continue;
        icc_drop_key(ui, i);
        cp->kw_idx = 0;
        ui->wants_escape = 1;
        return;
    }
    int row, col;
    ui_textarea_get_cursor(ed->ta, &row, &col);
    int e = 0;
    while (ui_textarea_get_char_at(ed->ta, row, e) == ' ' || ui_textarea_get_char_at(ed->ta, row, e) == '\t') e++;
    while (icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, row, e))) e++;
    if (icc_kw_index(ed, row, e) != cp->kw_idx - 1 || ui_textarea_get_char_at(ed->ta, row, e) != ' ' || col <= e) {
        cp->kw_idx = 0;
        return;
    }
    // Gone as soon as anything but blanks follows the keyword.
    for (int c = e; c < col; c++)
        if (ui_textarea_get_char_at(ed->ta, row, c) != ' ') { cp->kw_idx = 0; return; }
    cp->kw_row = row;
    cp->kw_col = col;
    ui->wants_escape = 1;
}

static void ic_complete_add_chint(InteractiveCoding *ic, UIContext *ui);

static void ic_complete_add_hint(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    ic->ov.hint_added = true;
    if (cp->kw_idx && !cp->hint_on && cp->kw_ed && cp->kw_ed->ta) {
        const char *hint = icc_kw_hints[cp->kw_idx - 1].hint, *alt = icc_kw_hints[cp->kw_idx - 1].alt;
        int kcol = cp->kw_col > 0 ? cp->kw_col - 1 : 0;
        ICOverlayLabel kl = { IC_OV_HINT, hint, alt, cp->kw_row, kcol, cp->kw_row, kcol, -1.0f, -1, 0,
                              IC_OV_SAME_ROW | IC_OV_ONE_ROW, NULL, NULL, 0 };
        ic_overlay_add(ic, ui, cp->kw_ed->ta, &kl);
        return;
    }
    if (!cp->hint_on || !cp->hint_have || !cp->hint_ed || !cp->hint_ed->ta) {
        ic_complete_add_chint(ic, ui);
        return;
    }
    int col = cp->hint_col > 0 ? cp->hint_col - 1 : 0;
    ICOverlayLabel l = { cp->hint_warn ? IC_OV_WARN : IC_OV_HINT, cp->hint_text,
                         cp->hint_alt[0] && s_strcmp(cp->hint_alt, cp->hint_text) != 0 ? cp->hint_alt : NULL,
                         cp->hint_row, col, cp->hint_row, col, -1.0f, -1, 0,
                         IC_OV_SAME_ROW | IC_OV_ONE_ROW, NULL, NULL, 0 };
    ic_overlay_add(ic, ui, cp->hint_ed->ta, &l);
}

// ===========================================================================
// Feature B: a hovered comma
// ===========================================================================

static void icc_comma_clear(ICComplete *cp) {
    cp->cmh_ed = NULL;
    cp->cmh_kind = 0;
    cp->cmh_built = false;
    cp->cmh_suppress = false;
}

// Once a frame, before the value hover reads the pointer. Sets cmh_suppress
// while the pointer rests on a comma that has something of its own to say, so
// the value hover stands down.
static void icc_comma_update(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    cp->cmh_suppress = false;
    if (!cp->on || !ic->hover_inspect || ic->rc_drag_target || cp->menu_open) { icc_comma_clear(cp); return; }

    float mx = m_floorf(ui->mouse_x), my = m_floorf(ui->mouse_y);
    ICEditor *ed = NULL;
    int mr = 0, mc = 0;
    for (int i = 0; i < ic->num_editors; i++) {
        ICEditor *e = ic->editors[i];
        if (e->ta && ui_textarea_screen_to_pos(e->ta, mx, my, &mr, &mc)) { ed = e; break; }
    }
    if (!ed || ed->needs_parse || ui_textarea_get_char_at(ed->ta, mr, mc) != ',') { icc_comma_clear(cp); return; }

    if (cp->cmh_ed != ed || cp->cmh_row != mr || cp->cmh_col != mc || cp->cmh_serial != ed->edit_serial) {
        bool moved = cp->cmh_ed != ed || cp->cmh_row != mr || cp->cmh_col != mc;
        cp->cmh_ed = ed; cp->cmh_row = mr; cp->cmh_col = mc; cp->cmh_serial = ed->edit_serial;
        if (moved) cp->cmh_since = ui->time;
        cp->cmh_built = false;
        cp->cmh_kind = 0;
        char *text = ui_textarea_get_text(ed->ta);
        if (text) {
            size_t off = icc_offset(text, mr, mc);
            ICCallCtx ctx;
            ic_call_context(text, s_strlen(text), off, &ctx);
            if (ctx.kind == IC_CALL_CALL && !ctx.in_decl) {
                size_t n = ctx.callee_hi - ctx.callee_lo;
                if (n > 0 && n < sizeof(cp->cmh_callee)) {
                    ic->sys->memcpy(cp->cmh_callee, text + ctx.callee_lo, n);
                    cp->cmh_callee[n] = '\0';
                    cp->cmh_kind = 1;
                }
            } else if (ctx.kind == IC_CALL_LITERAL) {
                cp->cmh_kind = 2;
            }
            cp->cmh_arg = ctx.arg_index;
            ic->sys->free(text);
        }
    }
    if (cp->cmh_kind == 0) return;
    cp->cmh_suppress = true;

    float delay = cp->cmh_kind == 2 ? IC_INSPECT_INDEX_DELAY : IC_INSPECT_HOVER_DELAY;
    if (cp->cmh_built || ui->time - cp->cmh_since < delay) return;
    cp->cmh_built = true;
    cp->cmh_text[0] = cp->cmh_alt[0] = '\0';
    if (cp->cmh_kind == 2) {
        s_snprintf(cp->cmh_text, sizeof(cp->cmh_text), "id=%d", cp->cmh_arg + 1);
        cp->cmh_align = cp->cmh_align_alt = (int)s_strlen(cp->cmh_text) / 2;
        return;
    }
    ICCatalog cat;
    ic_catalog_build(ic, ed, &cat);
    ICName *sig = (ICName *)ic_catalog_find(&cat, cp->cmh_callee, cp->cmh_row);
    if (sig && sig->kind == ICN_FUNC) icc_infer_types(ic, ed, sig);
    if (sig) icc_build_comma(sig, cp->cmh_arg, cp->cmh_text, (int)sizeof(cp->cmh_text),
                             cp->cmh_alt, (int)sizeof(cp->cmh_alt), &cp->cmh_align, &cp->cmh_align_alt);
    ic_catalog_free(ic, &cat);
}

static void ic_complete_add_comma(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    if (!cp->cmh_suppress || !cp->cmh_built || !cp->cmh_text[0] || !cp->cmh_ed) return;
    ICOverlayLabel l = { IC_OV_HOVER, cp->cmh_text,
                         cp->cmh_alt[0] && s_strcmp(cp->cmh_alt, cp->cmh_text) != 0 ? cp->cmh_alt : NULL,
                         cp->cmh_row, cp->cmh_col, cp->cmh_row, cp->cmh_col, -1.0f,
                         cp->cmh_col, cp->cmh_align, IC_OV_ONE_ROW, NULL, NULL, cp->cmh_align_alt };
    ic_overlay_add(ic, ui, cp->cmh_ed->ta, &l);
}

// ===========================================================================
// Feature C: Tab completion with a numbered menu
// ===========================================================================

typedef struct {
    int  row, c0, caret, c1;        // word start, caret, word end (columns)
    char prefix[IC_NAME_MAX];       // word start .. caret
    int  qlen;
} ICWord;

static bool icc_blank_char(char ch) { return ch == ' ' || ch == '\t'; }

// Column c of row r is the command slot just after a leading `#` (1), the option
// slot of `#enable` (2) / `#disable` (3), or neither (0).
static int icc_directive_slot(ICEditor *ed, int r, int c) {
    int i = 0;
    while (i < c && icc_blank_char(ui_textarea_get_char_at(ed->ta, r, i))) i++;
    if (i >= c || ui_textarea_get_char_at(ed->ta, r, i++) != '#') return 0;
    if (i == c) return 1;
    char cmd[8];
    int n = 0;
    while (i < c && n < 7 && icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, i))) cmd[n++] = ui_textarea_get_char_at(ed->ta, r, i++);
    cmd[n] = '\0';
    bool enable = s_strcmp(cmd, "enable") == 0;
    if (!enable && s_strcmp(cmd, "disable") != 0) return 0;
    if (i >= c || !icc_blank_char(ui_textarea_get_char_at(ed->ta, r, i))) return 0;
    while (i < c && icc_blank_char(ui_textarea_get_char_at(ed->ta, r, i))) i++;
    for (int k = i; k < c; k++) if (!icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, k))) return 0;
    return enable ? 2 : 3;
}

static const struct { const char *name, *help; } icc_directives[] = {
    { "push",           NULL },
    { "pop",            NULL },
    { "rewire",         "f64 -> f32  (from -> to, `number` = any numeric type)" },
    { "rewire literal", "f64 -> f32  (from -> to, number literals only)" },
    { "rewire type",    "f64 -> f32  (from -> to, declared types only)" },
    { "define",         NULL },
    { "enable",         NULL },
    { "disable",        NULL },
};

// A row that is just `#rewire`, `#rewire literal` or `#rewire type` with the
// caret at its end: the text that says what is to be written next.
static const char *icc_rewire_help(ICEditor *ed, int r, int c) {
    char row[32];
    int n = 0, i = 0;
    while (i < c && icc_blank_char(ui_textarea_get_char_at(ed->ta, r, i))) i++;
    for (; i < c && n < 30; i++) row[n++] = ui_textarea_get_char_at(ed->ta, r, i);
    if (i < c || ui_textarea_get_char_at(ed->ta, r, c) != '\0') return NULL;
    while (n > 0 && icc_blank_char(row[n - 1])) n--;
    row[n] = '\0';
    if (row[0] != '#') return NULL;
    for (int k = 0; k < (int)(sizeof(icc_directives) / sizeof(icc_directives[0])); k++)
        if (icc_directives[k].help && s_strncmp(row + 1, icc_directives[k].name, s_strlen(icc_directives[k].name) + 1) == 0)
            return icc_directives[k].help;
    return NULL;
}

static bool icc_word_at_caret(ICEditor *ed, ICWord *w) {
    int r1, c1, r2, c2;
    if (ui_textarea_get_selection(ed->ta, &r1, &c1, &r2, &c2)) return false;
    int r, c;
    ui_textarea_get_cursor(ed->ta, &r, &c);
    if (c > 0 && !icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, c - 1)) && icc_directive_slot(ed, r, c) &&
        !icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, c))) {
        w->row = r; w->c0 = w->caret = w->c1 = c;
        w->prefix[0] = '\0';
        w->qlen = 0;
        return true;
    }
    if (c <= 0 || !icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, c - 1))) return false;
    int c0 = c;
    while (c0 > 0 && icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, c0 - 1))) c0--;
    char first = ui_textarea_get_char_at(ed->ta, r, c0);
    if (first >= '0' && first <= '9') return false;
    int e = c;
    while (icc_ident_char((unsigned char)ui_textarea_get_char_at(ed->ta, r, e))) e++;
    if (c - c0 >= IC_NAME_MAX) return false;
    w->row = r; w->c0 = c0; w->caret = c; w->c1 = e;
    for (int i = 0; i < c - c0; i++) w->prefix[i] = ui_textarea_get_char_at(ed->ta, r, c0 + i);
    w->prefix[c - c0] = '\0';
    w->qlen = c - c0;
    return true;
}

// Replace the whole word -- through its end, not just to the caret -- as one
// undo step, leaving the caret after it.
static void icc_replace_word(ICEditor *ed, int row, int c0, int c1, const char *name) {
    ui_textarea_undo_begin_batch(ed->ta);
    ui_textarea_delete_range(ed->ta, row, c0, row, c1);
    ui_textarea_insert_string(ed->ta, row, c0, name);
    ui_textarea_undo_end_batch(ed->ta);
    ui_textarea_set_cursor(ed->ta, row, c0 + (int)s_strlen(name));
}

// Complete to `pick`. A function is made a call and its argument hint shown:
// one known to return nothing gets a space, a statement of its own written
// paren-less; anything else a `(`. Either is kept, not doubled, when already there.
static void icc_complete_name(InteractiveCoding *ic, ICEditor *ed, int row, int c0, int c1, const ICName *pick) {
    if (pick->kind == ICN_DIRECTIVE && s_strcmp(pick->name, "pop") != 0) {
        char text[IC_NAME_MAX + 2];
        s_snprintf(text, sizeof(text), "%s%s", pick->name, icc_blank_char(ui_textarea_get_char_at(ed->ta, row, c1)) ? "" : " ");
        icc_replace_word(ed, row, c0, c1, text);
        return;
    }
    if (pick->n_params < 0) { icc_replace_word(ed, row, c0, c1, pick->name); return; }
    ICName n = *pick;
    if (n.kind == ICN_FUNC && !n.ret[0]) icc_infer_types(ic, ed, &n);
    char open = s_strcmp(n.ret, "void") == 0 ? ' ' : '(';
    char text[IC_NAME_MAX + 2];
    int len = (int)s_strlen(n.name);
    s_snprintf(text, sizeof(text), "%s", n.name);
    if (ui_textarea_get_char_at(ed->ta, row, c1) != open) { text[len] = open; text[len + 1] = '\0'; }
    icc_replace_word(ed, row, c0, c1, text);
    ui_textarea_set_cursor(ed->ta, row, c0 + len + 1);
    ic->cp.kw_idx     = 0;
    ic->cp.hint_ed    = ed;
    ic->cp.hint_armed = true;
}

static void icc_menu_close(InteractiveCoding *ic) {
    ICComplete *cp = &ic->cp;
    if (!cp->menu_open) return;
    cp->menu_open = false;
    cp->menu_ed = NULL;
    ic_catalog_free(ic, &cp->menu_cat);
    // A pin placed while the menu covered its cell is re-placed now.
    ic->ins_place_todo = 2;
}

#define IC_MENU_BODY 64

static bool icc_menu_more(const ICComplete *cp) {
    return cp->menu_first + cp->menu_per < cp->menu_n;
}

// The whole list, short enough that Tab has nothing to turn: Tab picks entry 1.
static bool icc_menu_lone(const ICComplete *cp) {
    return cp->menu_first == 0 && !icc_menu_more(cp) && cp->menu_per <= IC_MENU_DIGITS;
}

// Button `i` of the page. Nine entries from menu_num on carry the digit that
// picks them, the rest two blanks in its place; after the entries, "..more".
// "(tab)" sits beside what Tab does when that is not turning the numbers.
static void icc_menu_body(const ICComplete *cp, int i, char *out, int cap) {
    if (i >= cp->menu_per) {
        s_snprintf(out, (size_t)cap, "%s", cp->menu_per <= IC_MENU_DIGITS ? "..more (tab)" : "..more");
        return;
    }
    char name[IC_NAME_MAX + 4];
    icc_label(&cp->menu_cat.v[cp->menu_hits[cp->menu_first + i]], name, (int)sizeof(name));
    const char *tab = i == 0 && icc_menu_lone(cp) ? " (tab)" : "";
    int d = i - cp->menu_num;
    if (d >= 0 && d < IC_MENU_DIGITS) s_snprintf(out, (size_t)cap, "%d %s%s", d + 1, name, tab);
    else                              s_snprintf(out, (size_t)cap, "  %s%s", name, tab);
}

// `nb` buttons `len` wide as up to `cols` columns filled top to bottom, a cell
// apart: each column's width (when col_w) and the block's. Returns the rows.
static int icc_menu_grid(const int *len, int nb, int cols, int *col_w, int *out_w) {
    if (nb <= 0) { *out_w = 0; return 0; }
    int rows = (nb + cols - 1) / cols;
    cols = (nb + rows - 1) / rows;
    int w = 0;
    for (int j = 0; j < cols; j++) {
        int cw = 0;
        for (int k = 0; k < rows && j * rows + k < nb; k++)
            if (len[j * rows + k] > cw) cw = len[j * rows + k];
        if (col_w) col_w[j] = cw;
        w += cw + (j ? 1 : 0);
    }
    *out_w = w;
    return rows;
}

static void icc_menu_seg(ICComplete *cp, int f, int off, int len, int row, int col, int idx) {
    if (cp->menu_nseg[f] >= IC_MENU_SEG_MAX) return;
    ICMenuSeg *s = &cp->menu_seg[f][cp->menu_nseg[f]++];
    s->off = off; s->len = len; s->row = row; s->col = col; s->idx = idx;
}

static void icc_pad_to(Tsys *sys, char *buf, int cap, int *pos, int target) {
    int pad = target - *pos;
    if (pad <= 0 || target >= cap) return;
    sys->memset(buf + *pos, ' ', (size_t)pad);
    *pos = target;
    buf[*pos] = '\0';
}

// Form 0 is one row of buttons a cell apart; form 1 the grid of menu_cols
// columns, every button padded to its column's width.
static void icc_menu_text(InteractiveCoding *ic) {
    ICComplete *cp = &ic->cp;
    const int cap = IC_OV_TEXT_MAX;
    char body[IC_MENU_BODY];
    int nb = cp->menu_per + (icc_menu_more(cp) ? 1 : 0);
    int pos = 0;
    cp->menu_text[0] = cp->menu_col[0] = '\0';
    cp->menu_nseg[0] = cp->menu_nseg[1] = 0;
    // Down to one: no buttons, but what Tab would write, as the completion hint
    // shows it -- the menu stays open, so Backspace brings the choices back.
    if (cp->menu_n == 1) {
        const ICName *n = &cp->menu_cat.v[cp->menu_hits[0]];
        s_snprintf(cp->menu_col, (size_t)cap, "%s (tab)", n->name);
        if (n->n_params >= 0 && n->params[0]) s_snprintf(cp->menu_text, (size_t)cap, "%s(%s) (tab)", n->name, n->params);
        else                                  s_snprintf(cp->menu_text, (size_t)cap, "%s", cp->menu_col);
        return;
    }
    for (int i = 0; i < nb; i++) {
        icc_menu_body(cp, i, body, sizeof(body));
        cp->menu_len[i] = (int)s_strlen(body) + 2;
        if (pos > 0) ICC_APP(cp->menu_text, cap, pos, " ");
        int at = pos;
        ICC_APP(cp->menu_text, cap, pos, " %s ", body);
        icc_menu_seg(cp, 0, at, pos - at, 0, at, i < cp->menu_per ? i : -1);
    }
    int col_w[IC_MENU_MAX_HITS + 1], w;
    int rows = icc_menu_grid(cp->menu_len, nb, cp->menu_cols, col_w, &w);
    pos = 0;
    for (int k = 0; k < rows; k++) {
        if (k) ICC_APP(cp->menu_col, cap, pos, "\n");
        int row0 = pos, x = 0;
        for (int j = 0; j * rows + k < nb; j++) {
            int i = j * rows + k;
            icc_menu_body(cp, i, body, sizeof(body));
            icc_pad_to(ic->sys, cp->menu_col, cap, &pos, row0 + x);
            int at = pos;
            ICC_APP(cp->menu_col, cap, pos, " %s ", body);
            icc_pad_to(ic->sys, cp->menu_col, cap, &pos, at + col_w[j]);
            icc_menu_seg(cp, 1, at, pos - at, k, x, i < cp->menu_per ? i : -1);
            x += col_w[j] + 1;
        }
    }
}

static bool icc_menu_over_button(const ICComplete *cp, UIContext *ui) {
    for (int i = 0; i < cp->menu_nbtn; i++) {
        const ICMenuBtn *b = &cp->menu_btn[i];
        if (ui->mouse_y >= b->y && ui->mouse_y < b->y + 1.0f && ui->mouse_x >= b->x && ui->mouse_x < b->x + (float)b->w) return true;
    }
    return false;
}

static void icc_menu_pick(InteractiveCoding *ic, int idx) {
    ICComplete *cp = &ic->cp;
    ICWord w;
    if (!cp->menu_ed || !icc_word_at_caret(cp->menu_ed, &w)) { icc_menu_close(ic); return; }
    ICName pick = cp->menu_cat.v[cp->menu_hits[idx]];
    ICEditor *ed = cp->menu_ed;
    icc_menu_close(ic);
    icc_complete_name(ic, ed, w.row, w.c0, w.c1, &pick);
}

// The page from entry `first`, numbered from its top. How many entries it
// holds is the layout's to say; until then, at most a digit's worth.
static void icc_menu_show_from(InteractiveCoding *ic, int first) {
    ICComplete *cp = &ic->cp;
    int left = cp->menu_n - first;
    cp->menu_first = first;
    cp->menu_num   = 0;
    cp->menu_per   = left < IC_MENU_DIGITS ? left : IC_MENU_DIGITS;
    cp->menu_dirty = true;
    icc_menu_text(ic);
}

static void icc_menu_next_page(InteractiveCoding *ic) {
    ICComplete *cp = &ic->cp;
    icc_menu_show_from(ic, icc_menu_more(cp) ? cp->menu_first + cp->menu_per : 0);
}

// Tab: the digits move on to the next nine of the page; past its last, the
// page turns. A lone page has nothing to turn, so Tab takes its first entry.
static void icc_menu_tab(InteractiveCoding *ic) {
    ICComplete *cp = &ic->cp;
    if (cp->menu_num + IC_MENU_DIGITS < cp->menu_per) {
        cp->menu_num += IC_MENU_DIGITS;
        icc_menu_text(ic);
    } else if (icc_menu_lone(cp)) {
        icc_menu_pick(ic, cp->menu_first);
    } else {
        icc_menu_next_page(ic);
    }
}

static void icc_menu_requery(InteractiveCoding *ic, const char *q, int qlen, int row) {
    ICComplete *cp = &ic->cp;
    cp->menu_n = ic_catalog_query(&cp->menu_cat, q, qlen, row, cp->menu_hits, IC_MENU_MAX_HITS);
    s_snprintf(cp->menu_q, sizeof(cp->menu_q), "%s", q);
    icc_menu_show_from(ic, 0);
}

// Shell behaviour: the characters every prefix-matching candidate agrees on.
static int icc_common_prefix(const ICCatalog *c, const int *hits, int n, const char *q, int qlen,
                             char *out, int cap) {
    const char *first = NULL;
    int len = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = c->v[hits[i]].name;
        if (icc_match_tier(nm, q, qlen) != 0) return 0;
        if (!first) { first = nm; len = (int)s_strlen(nm); continue; }
        int k = 0;
        while (k < len && nm[k] && nm[k] == first[k]) k++;
        len = k;
    }
    if (!first || len <= qlen || len >= cap) return 0;
    icc_copy(out, first, len);
    out[len] = '\0';
    return len;
}

// A keyword that completes to a whole clause, rather than to a name. `%v` is a
// loop variable no name in scope already has.
static const struct { const char *kw, *tail; } icc_snippets[] = {
    { "for", " %v in 0..8" },
};

static bool icc_name_in_scope(const ICCatalog *cat, const char *name, int row) {
    for (int i = 0; i < cat->n; i++)
        if (s_strcmp(cat->v[i].name, name) == 0 && icc_rank(&cat->v[i], row) < 7) return true;
    return false;
}

static void icc_fresh_loop_var(const ICCatalog *cat, int row, char *out, int cap) {
    static const char letters[] = "ijklmn";
    for (int round = 0; round < 100; round++)
        for (int k = 0; letters[k]; k++) {
            if (round == 0) s_snprintf(out, (size_t)cap, "%c", letters[k]);
            else            s_snprintf(out, (size_t)cap, "%c%d", letters[k], round + 1);
            if (!icc_name_in_scope(cat, out, row)) return;
        }
}

// The snippet a bare keyword at the caret stands for, or -1: the word ends at
// the caret, opens the row, and nothing but blanks follows. *end is where the
// row's text ends.
static int icc_snippet_at(ICEditor *ed, const ICWord *w, int *end) {
    if (w->caret != w->c1) return -1;
    int k = -1;
    for (int i = 0; i < (int)(sizeof(icc_snippets) / sizeof(icc_snippets[0])); i++)
        if (s_strcmp(w->prefix, icc_snippets[i].kw) == 0) k = i;
    if (k < 0) return -1;
    for (int c = 0; c < w->c0; c++) {
        char ch = ui_textarea_get_char_at(ed->ta, w->row, c);
        if (ch != ' ' && ch != '\t') return -1;
    }
    int e = w->c1;
    while (ui_textarea_get_char_at(ed->ta, w->row, e) != '\0') {
        char ch = ui_textarea_get_char_at(ed->ta, w->row, e);
        if (ch != ' ' && ch != '\t') return -1;
        e++;
    }
    *end = e;
    return k;
}

// The text snippet `k` writes after its keyword, with a loop variable that
// nothing in scope at `row` has.
static int icc_snippet_tail(InteractiveCoding *ic, ICEditor *ed, int row, int k, char *tail, int cap) {
    ICCatalog cat;
    ic_catalog_build(ic, ed, &cat);
    char var[IC_NAME_MAX];
    icc_fresh_loop_var(&cat, row, var, sizeof(var));
    ic_catalog_free(ic, &cat);
    int n = 0;
    for (const char *p = icc_snippets[k].tail; *p && n < cap - IC_NAME_MAX; p++) {
        if (p[0] == '%' && p[1] == 'v') { s_snprintf(tail + n, (size_t)(cap - n), "%s", var); n += (int)s_strlen(var); p++; }
        else tail[n++] = *p;
    }
    tail[n] = '\0';
    return n;
}

// Tab right after a bare keyword that opens a statement: write the clause.
static bool icc_try_snippet(InteractiveCoding *ic, ICEditor *ed, const ICWord *w) {
    int end;
    int k = icc_snippet_at(ed, w, &end);
    if (k < 0) return false;
    char tail[64];
    int n = icc_snippet_tail(ic, ed, w->row, k, tail, (int)sizeof(tail));
    ui_textarea_undo_begin_batch(ed->ta);
    if (end > w->c1) ui_textarea_delete_range(ed->ta, w->row, w->c1, w->row, end);
    ui_textarea_insert_string(ed->ta, w->row, w->c1, tail);
    ui_textarea_undo_end_batch(ed->ta);
    ui_textarea_set_cursor(ed->ta, w->row, w->c1 + n);
    return true;
}

static void icc_directive_catalog(InteractiveCoding *ic, ICCatalog *cat, int slot) {
    cat->v = NULL;
    cat->n = cat->cap = 0;
    if (slot == 1) {
        for (int i = 0; i < (int)(sizeof(icc_directives) / sizeof(icc_directives[0])); i++) {
            ICName *n = icc_cat_push(ic, cat);
            if (!n) return;
            s_snprintf(n->name, sizeof(n->name), "%s", icc_directives[i].name);
            n->kind = ICN_DIRECTIVE;
        }
        return;
    }
    for (int i = 0; vm_directive_option_name(i); i++) {
        ICName *n = icc_cat_push(ic, cat);
        if (!n) return;
        s_snprintf(n->name, sizeof(n->name), "%s", vm_directive_option_name(i));
        bool noop = (vm_directive_option_default(i) != 0) == (slot == 2);
        n->kind = noop ? ICN_OPTION_ALT : ICN_OPTION;
    }
}

static bool icc_try_rewire_help(InteractiveCoding *ic, ICEditor *ed) {
    ICComplete *cp = &ic->cp;
    int r, c;
    ui_textarea_get_cursor(ed->ta, &r, &c);
    const char *help = icc_rewire_help(ed, r, c);
    if (!help) return false;
    s_snprintf(cp->ch_text, sizeof(cp->ch_text), "%s", help);
    cp->ch_alt[0] = '\0';
    cp->ch_ed   = ed;
    cp->ch_row  = r;
    cp->ch_col  = c;
    cp->ch_have = true;
    cp->ch_tab  = true;
    return true;
}

static bool icc_try_open_menu(InteractiveCoding *ic, ICEditor *ed) {
    ICComplete *cp = &ic->cp;
    if (icc_try_rewire_help(ic, ed)) return true;
    ICWord w;
    if (!icc_word_at_caret(ed, &w)) return false;
    char *text = ui_textarea_get_text(ed->ta);
    if (!text) return false;
    bool in_lit = icc_in_literal(text, icc_offset(text, w.row, w.c0));
    ic->sys->free(text);
    if (in_lit) return false;
    if (icc_try_snippet(ic, ed, &w)) return true;

    ICCatalog cat;
    int slot = icc_directive_slot(ed, w.row, w.c0);
    if (slot) icc_directive_catalog(ic, &cat, slot);
    else ic_catalog_build(ic, ed, &cat);
    int hits[IC_MENU_MAX_HITS];
    int n = ic_catalog_query(&cat, w.prefix, w.qlen, w.row, hits, IC_MENU_MAX_HITS);
    if (n == 0) { ic_catalog_free(ic, &cat); return false; }

    if (n == 1) {
        ICName pick = cat.v[hits[0]];
        ic_catalog_free(ic, &cat);
        icc_complete_name(ic, ed, w.row, w.c0, w.c1, &pick);
        return true;
    }

    // Extend to what they all share when the caret is at the word's end.
    char shared[IC_NAME_MAX];
    int  qlen = w.qlen;
    char q[IC_NAME_MAX];
    s_snprintf(q, sizeof(q), "%s", w.prefix);
    if (w.caret == w.c1 && icc_common_prefix(&cat, hits, n, w.prefix, w.qlen, shared, sizeof(shared))) {
        icc_replace_word(ed, w.row, w.c0, w.c1, shared);
        s_snprintf(q, sizeof(q), "%s", shared);
        qlen = (int)s_strlen(shared);
    }

    icc_menu_close(ic);
    cp->menu_open = true;
    cp->menu_ed   = ed;
    cp->menu_row  = w.row;
    cp->menu_c0   = w.c0;
    cp->menu_cat  = cat;
    cp->menu_cols = 1;
    cp->menu_at = false;
    cp->menu_dirty = true;
    icc_menu_requery(ic, q, qlen, w.row);
    ic->ins_place_todo = 2;
    return true;
}

// Once a frame, before the textarea reads this frame's keys.
static void icc_menu_update(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    if (!cp->on) { icc_menu_close(ic); return; }

    if (cp->menu_open) {
        ICEditor *ed = cp->menu_ed;
        ICWord w;
        if (!ed || !ed->ta || !icc_editor_focused(ui, ed) || (icc_mouse_pressed(ui) && !icc_menu_over_button(cp, ui)) ||
            !icc_word_at_caret(ed, &w) || w.row != cp->menu_row || w.c0 != cp->menu_c0 || w.caret != w.c1) {
            icc_menu_close(ic);
            return;
        }
        if (s_strcmp(w.prefix, cp->menu_q) != 0) {
            icc_menu_requery(ic, w.prefix, w.qlen, w.row);
            if (cp->menu_n == 0) { icc_menu_close(ic); return; }
        }
        for (int i = 0; i < ui->num_key_events; i++) {
            int key = ui->key_events[i], kf = ui->key_flags[i];
            bool plain = !(kf & (UI_FLAGS_CTRL | UI_FLAGS_SHIFT | UI_FLAGS_ALT));
            if (key == (int)PLATFORM_KEY_ESCAPE && plain) {
                icc_drop_key(ui, i);
                icc_menu_close(ic);
                ui->wants_escape = 1;
                return;
            }
            if (key == (int)PLATFORM_KEY_TAB && plain) {
                icc_drop_key(ui, i--);
                icc_menu_tab(ic);
                if (!cp->menu_open) return;
                continue;
            }
        }
        // A digit picks the entry it is printed beside. One that names no entry
        // is an ordinary character.
        for (int pass = 0; pass < 2 && cp->menu_open; pass++) {
            int n = pass == 0 ? ui->num_char_events : ui->num_key_events;
            for (int i = 0; i < n; i++) {
                int ch = pass == 0 ? ui->char_events[i] : ui->key_events[i];
                int fl = pass == 0 ? ui->char_flags[i]  : ui->key_flags[i];
                if (ch < '1' || ch > '9' || (fl & (UI_FLAGS_CTRL | UI_FLAGS_ALT))) continue;
                int local = cp->menu_num + (ch - '1');
                if (local >= cp->menu_per || cp->menu_n == 1) continue;
                int idx = cp->menu_first + local;
                // The same press can arrive as a key and as a character.
                for (int k = ui->num_key_events - 1; k >= 0; k--)
                    if (ui->key_events[k] == ch) icc_drop_key(ui, k);
                for (int k = ui->num_char_events - 1; k >= 0; k--)
                    if (ui->char_events[k] == ch) icc_drop_char(ui, k);
                icc_menu_pick(ic, idx);
                break;
            }
        }
        if (cp->menu_open) ui->wants_escape = 1;
        return;
    }

    // Closed: Tab inside a name opens it.
    ICEditor *ed = ic_active_editor(ic);
    if (!ed || !ed->ta || !icc_editor_focused(ui, ed)) return;
    for (int i = 0; i < ui->num_key_events; i++) {
        int kf = ui->key_flags[i];
        if (ui->key_events[i] != (int)PLATFORM_KEY_TAB || (kf & (UI_FLAGS_CTRL | UI_FLAGS_SHIFT | UI_FLAGS_ALT))) continue;
        if (icc_try_open_menu(ic, ed)) icc_drop_key(ui, i);
        break;
    }
}

// The screen around the caret, read a row at a time when first asked (a row is
// IC_MENU_MAP_COLS cell lookups), and the button widths of the page being fitted.
#define IC_MENU_MAP_ROWS 160
#define IC_MENU_MAP_COLS 200
typedef struct {
    InteractiveCoding *ic;
    UIContext         *ui;
    UITextArea        *ta;
    float              ax, ay;          // the word's last cell
    float              x0, y0;          // the map's first cell: the one after the word's, 48 rows up
    bool               have[IC_MENU_MAP_ROWS];
    short              end[IC_MENU_MAP_ROWS];                         // last occupied column, -1 none
    short              bad[IC_MENU_MAP_ROWS][IC_MENU_MAP_COLS + 1];   // first column >= c not free
    int                first, n;        // the page starts at entry `first`; n entries are left, at most
    int                len[IC_MENU_MAX_HITS + 1];
} ICMenuFit;

typedef struct { int cols; float x, y; } ICMenuSpot;

static int icc_fit_row(ICMenuFit *f, float y) {
    int i = (int)(y - f->y0);
    if (i < 0 || i >= IC_MENU_MAP_ROWS) return -1;
    if (f->have[i]) return i;
    short next = IC_MENU_MAP_COLS;
    f->end[i] = -1;
    f->bad[i][IC_MENU_MAP_COLS] = IC_MENU_MAP_COLS;
    for (int c = IC_MENU_MAP_COLS - 1; c >= 0; c--) {
        int kind = ic_ov_cell_kind(f->ic, f->ui, f->ta, f->x0 + (float)c, y);
        if (kind == IC_CELL_TAKEN && f->end[i] < 0) f->end[i] = (short)c;
        if (kind != IC_CELL_FREE) next = (short)c;
        f->bad[i][c] = next;
    }
    f->have[i] = true;
    return i;
}

// Whether the text of a block `w` wide whose top left is (x, y) has every cell
// of `rows` rows free, padding included. Off the map nothing is.
static bool icc_fit_free(ICMenuFit *f, float x, float y, int w, int rows) {
    int c0 = (int)(x - f->x0) - IC_INSPECT_PAD, c1 = (int)(x - f->x0) + w + IC_INSPECT_PAD;
    if (c0 < 0 || c1 > IC_MENU_MAP_COLS) return false;
    for (int k = 0; k < rows; k++) {
        int i = icc_fit_row(f, y + (float)k);
        if (i < 0 || f->bad[i][c0] < c1) return false;
    }
    return true;
}

// The widths of a page of k entries, then of "..more" when entries remain.
// Returns the button count. Mirrors icc_menu_body.
static int icc_fit_lens(ICMenuFit *f, int k) {
    const ICComplete *cp = &f->ic->cp;
    for (int i = 0; i < k; i++) f->len[i] = 4 + (int)s_strlen(cp->menu_cat.v[cp->menu_hits[f->first + i]].name) + (cp->menu_cat.v[cp->menu_hits[f->first + i]].kind == ICN_OPTION_ALT ? 2 : 0);
    if (f->first == 0 && k == cp->menu_n && k <= IC_MENU_DIGITS) f->len[0] += 6;
    if (k == f->n) return k;
    f->len[k] = k <= IC_MENU_DIGITS ? 14 : 8;
    return k + 1;
}

// `cols` columns of nb buttons with their top row at y: beside the caret, or
// right of the code on every row they cover (and never left of the caret).
static bool icc_fit_try(ICMenuFit *f, int nb, int cols, float y, bool at_caret, ICMenuSpot *out) {
    int w, rows = icc_menu_grid(f->len, nb, cols, NULL, &w);
    if (rows > IC_MENU_MAX_ROWS) return false;
    float x = f->ax + 2.0f;
    for (int k = 0; k < rows && !at_caret; k++) {
        int i = icc_fit_row(f, y + (float)k);
        if (i < 0) return false;
        if (f->end[i] >= 0 && f->x0 + (float)f->end[i] + 2.0f > x) x = f->x0 + (float)f->end[i] + 2.0f;
    }
    if (!icc_fit_free(f, x, y, w, rows)) return false;
    out->cols = cols; out->x = x; out->y = y;
    return true;
}

// The column counts whose balanced height falls in lo .. hi rows, fewest first.
static int icc_fit_cols(int nb, int lo, int hi, int *out) {
    int n = 0;
    for (int r = hi; r >= lo; r--) {
        int c = (nb + r - 1) / r;
        if (n == 0 || out[n - 1] != c) out[n++] = c;
    }
    return n;
}

// Step `step` of the order below for nb buttons:
//   1. one column beside the caret, its top row on the caret's;
//   2. columns there, 4 to 6 rows tall, else 3 or 2 when that is the room;
//   3. those columns right of the code, centred on the caret's row;
//   4. the same shifted -3 .. +6 rows;
//   5. one column right of the code, shifted by up to its height;
//   6, 7. as 3 and 4, 7 to 9 rows tall;
//   8, 9. as 3 and 4, 1 to 3 rows tall.
#define IC_MENU_STEPS 9
static bool icc_fit_step(ICMenuFit *f, int step, int nb, ICMenuSpot *s) {
    static const int shifts[] = { 1, -1, 2, -2, 3, -3, 4, 5, 6 };
    static const int lo[] = { 0, 0, 0, 4, 4, 0, 7, 7, 1, 1 }, hi[] = { 0, 0, 0, 6, 6, 0, 9, 9, 3, 3 };
    const int n_shifts = (int)(sizeof(shifts) / sizeof(shifts[0]));
    int cols[8], nc;
    bool centred = step == 3 || step == 6 || step == 8;
    switch (step) {
    case 1:
        return icc_fit_try(f, nb, 1, f->ay, true, s);
    case 2:
        nc = icc_fit_cols(nb, 2, 6, cols);
        for (int i = 0; i < nc; i++)
            if (cols[i] > 1 && icc_fit_try(f, nb, cols[i], f->ay, true, s)) return true;
        return false;
    case 5:
        for (int k = 0; k <= 2 * nb; k++) {
            int d = (k + 1) / 2 * (k % 2 ? 1 : -1);
            if (icc_fit_try(f, nb, 1, f->ay - (float)((nb - 1) / 2) + (float)d, false, s)) return true;
        }
        return false;
    default:
        nc = icc_fit_cols(nb, lo[step], hi[step], cols);
        for (int d = 0; d < (centred ? 1 : n_shifts); d++)
            for (int i = 0; i < nc; i++) {
                int rows = (nb + cols[i] - 1) / cols[i];
                int shift = centred ? 0 : shifts[d];
                if (icc_fit_try(f, nb, cols[i], f->ay - (float)((rows - 1) / 2) + (float)shift, false, s)) return true;
            }
        return false;
    }
}

// Every entry left, at the first step it fits at. Failing that, the step that
// fits the most of them with a button left for "..more" -- the earlier step
// on a tie. Returns the entries placed, 0 when not even two fit.
static int icc_menu_fit(ICMenuFit *f, ICMenuSpot *s) {
    for (int step = 1; step <= IC_MENU_STEPS; step++)
        if (icc_fit_step(f, step, icc_fit_lens(f, f->n), s)) return f->n;
    // Down from the most, not a bisection: a step that makes columns cannot
    // make them of a few entries, so fitting is not monotonic in the count.
    int best = 1, best_step = 0;
    for (int step = 1; step <= IC_MENU_STEPS; step++)
        for (int k = f->n - 1; k > best; k--)
            if (icc_fit_step(f, step, icc_fit_lens(f, k), s)) { best = k; best_step = step; break; }
    if (best >= 2 && icc_fit_step(f, best_step, icc_fit_lens(f, best), s)) return best;
    return 0;
}

// Lays the page out when it or its anchor changed: as many entries as the
// first spot that takes them all, or else as many as any spot can, behind a
// "..more". With no room for two, the old single row of nine wherever the
// overlay finds room.
static void icc_menu_layout(InteractiveCoding *ic, UIContext *ui, int col2) {
    ICComplete *cp = &ic->cp;
    UITextArea *ta = cp->menu_ed->ta;
    float ax, ay;
    if (!ui_textarea_pos_to_screen(ta, cp->menu_row, col2, &ax, &ay)) return;
    ax = (float)m_floor(ax); ay = (float)m_floor(ay);
    if (!cp->menu_dirty && !ic->ov.force_place && ax == cp->menu_lay_ax && ay == cp->menu_lay_ay) return;
    cp->menu_dirty = false;
    cp->menu_lay_ax = ax;
    cp->menu_lay_ay = ay;

    ICMenuFit *f = (ICMenuFit *)ic->sys->malloc(sizeof(ICMenuFit));
    if (!f) return;
    f->ic = ic; f->ui = ui; f->ta = ta; f->ax = ax; f->ay = ay;
    f->x0 = ax + 1.0f;
    f->y0 = ay - 48.0f;
    ic->sys->memset(f->have, 0, sizeof(f->have));
    f->first = cp->menu_first;
    f->n = cp->menu_n - cp->menu_first;
    int left = f->n;
    ICMenuSpot s;
    int per = icc_menu_fit(f, &s);
    ic->sys->free(f);

    cp->menu_at = per > 0;
    if (!cp->menu_at) {
        per = left < IC_MENU_DIGITS ? left : IC_MENU_DIGITS;
        s.cols = 1; s.x = ax; s.y = ay;
    }
    cp->menu_per  = per;
    if (cp->menu_num >= per) cp->menu_num = 0;
    cp->menu_cols = s.cols;
    cp->menu_at_x = s.x;
    cp->menu_at_y = s.y;
    icc_menu_text(ic);
}

static void ic_complete_add_menu(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    if (!cp->menu_open || !cp->menu_ed || !cp->menu_ed->ta || !cp->menu_text[0]) return;
    ICEditor *ed = cp->menu_ed;
    int r, c;
    ui_textarea_get_cursor(ed->ta, &r, &c);
    int col2 = c > cp->menu_c0 ? c - 1 : cp->menu_c0;
    if (cp->menu_n == 1) {
        const char *alt = s_strcmp(cp->menu_col, cp->menu_text) != 0 ? cp->menu_col : NULL;
        ICOverlayLabel one = { IC_OV_HINT, cp->menu_text, alt, cp->menu_row, col2, cp->menu_row, col2, -1.0f, -1, 0,
                               IC_OV_SAME_ROW | IC_OV_ONE_ROW, NULL, NULL, 0 };
        ic_overlay_add(ic, ui, ed->ta, &one);
        return;
    }
    icc_menu_layout(ic, ui, col2);
    const char *first = cp->menu_at ? cp->menu_col : cp->menu_text;
    const char *then  = cp->menu_at ? NULL : cp->menu_col;
    unsigned flags = IC_OV_PREFER_BELOW | IC_OV_NO_SAME_ROW_RIGHT | IC_OV_ONE_ROW | (cp->menu_at ? IC_OV_AT : 0);
    ICOverlayLabel l = { IC_OV_MENU, first, then, cp->menu_row, cp->menu_c0, cp->menu_row, col2, -1.0f, -1, 0,
                         flags, NULL, NULL, 0, cp->menu_at_x, cp->menu_at_y };
    ic_overlay_add(ic, ui, ed->ta, &l);
}

// After the overlay flush: the menu is not painted as text but as buttons at the
// cells its label was given. They are drawn here, before the textarea, so a
// press on one is taken by it (the textarea only takes a press nobody has) and
// the editor keeps its focus.
static void ic_complete_draw_menu(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    cp->menu_nbtn = 0;
    if (!cp->menu_open || !ic->ov.menu_placed) return;
    const char *label = ic->ov.menu_label;
    int f = label == cp->menu_text ? 0 : 1;

    float pen_x = ui->pen_x, pen_y = ui->pen_y, line_x = ui->line_start_x, max_x = ui->content_max_x;
    float lx = ui->last_x, ly = ui->last_y, lw = ui->last_w, lh = ui->last_h;
    ui->content_max_x = 1.0e9f;
    ui_push_id(ui, "ic_menu");
    int picked = -2;
    for (int i = 0; i < cp->menu_nseg[f] && picked == -2; i++) {
        const ICMenuSeg *s = &cp->menu_seg[f][i];
        if (s->col + s->len > ic->ov.menu_avail && f == 0) continue;
        char text[IC_MENU_BODY + 16];
        icc_copy(text, label + s->off, s->len);
        s_snprintf(text + s->len, sizeof(text) - (size_t)s->len, "##m%d", i);
        float x = ic->ov.menu_x + (float)s->col, y = ic->ov.menu_y + (float)s->row;
        ui_set_cursor(ui, x, y);
        cp->menu_btn[cp->menu_nbtn].x = x;
        cp->menu_btn[cp->menu_nbtn].y = y;
        cp->menu_btn[cp->menu_nbtn].w = s->len;
        cp->menu_nbtn++;
        ui_push_item_width(ui, (float)s->len);
        if (ui_button_flags(ui, text, UI_OPTION_NO_MARGIN)) picked = s->idx < 0 ? -1 : cp->menu_first + s->idx;
        ui_pop_item_width(ui);
    }
    ui_pop_id(ui);
    ui->pen_x = pen_x; ui->pen_y = pen_y; ui->line_start_x = line_x; ui->content_max_x = max_x;
    ui->last_x = lx; ui->last_y = ly; ui->last_w = lw; ui->last_h = lh;

    if (picked >= 0) icc_menu_pick(ic, picked);
    else if (picked == -1) icc_menu_next_page(ic);
}

// ===========================================================================
// The completion hint: the one thing Tab would write
// ===========================================================================

static void icc_chint_clear(ICComplete *cp) {
    cp->ch_have = false;
    cp->ch_ed = NULL;
    cp->ch_key[0] = '\0';
}

// Once a frame. With the Hint box on, a word the caret ends, alone on the end
// of its row, that has exactly one completion shows it.
static void icc_chint_update(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    ICEditor *ed = ic_active_editor(ic);
    ICWord w;
    if (cp->ch_tab) {
        int r, c;
        if (cp->on && ed && ed->ta && cp->ch_ed == ed && icc_editor_focused(ui, ed)) {
            ui_textarea_get_cursor(ed->ta, &r, &c);
            if (r == cp->ch_row && c == cp->ch_col) return;
        }
        cp->ch_tab = false;
        icc_chint_clear(cp);
    }
    if (!cp->on || !cp->hints || !ed || !ed->ta || !icc_editor_focused(ui, ed) || cp->menu_open ||
        cp->hint_on || cp->hint_armed || cp->kw_idx || !icc_word_at_caret(ed, &w) || w.caret != w.c1 ||
        ui_textarea_get_char_at(ed->ta, w.row, w.c1) != '\0' || icc_directive_slot(ed, w.row, w.c0)) {
        icc_chint_clear(cp);
        return;
    }
    if (cp->ch_ed == ed && cp->ch_row == w.row && cp->ch_col == w.caret && cp->ch_serial == ed->edit_serial &&
        s_strcmp(cp->ch_key, w.prefix) == 0) return;
    cp->ch_ed = ed;
    cp->ch_row = w.row;
    cp->ch_col = w.caret;
    cp->ch_serial = ed->edit_serial;
    s_snprintf(cp->ch_key, sizeof(cp->ch_key), "%s", w.prefix);
    cp->ch_have = false;

    char *text = ui_textarea_get_text(ed->ta);
    if (!text) return;
    bool in_lit = icc_in_literal(text, icc_offset(text, w.row, w.c0));
    ic->sys->free(text);
    if (in_lit) return;

    int end;
    int k = icc_snippet_at(ed, &w, &end);
    if (k >= 0) {
        char tail[64];
        icc_snippet_tail(ic, ed, w.row, k, tail, (int)sizeof(tail));
        s_snprintf(cp->ch_text, sizeof(cp->ch_text), "%s (tab)", tail + (tail[0] == 32 ? 1 : 0));
        cp->ch_alt[0] = '\0';
        cp->ch_have = true;
        return;
    }
    ICCatalog cat;
    ic_catalog_build(ic, ed, &cat);
    int hits[2];
    if (ic_catalog_query(&cat, w.prefix, w.qlen, w.row, hits, 2) == 1) {
        const ICName *n = &cat.v[hits[0]];
        s_snprintf(cp->ch_alt, sizeof(cp->ch_alt), "%s (tab)", n->name);
        if (n->n_params >= 0 && n->params[0]) s_snprintf(cp->ch_text, sizeof(cp->ch_text), "%s(%s) (tab)", n->name, n->params);
        else s_snprintf(cp->ch_text, sizeof(cp->ch_text), "%s", cp->ch_alt);
        cp->ch_have = true;
    }
    ic_catalog_free(ic, &cat);
}

static void ic_complete_add_chint(InteractiveCoding *ic, UIContext *ui) {
    ICComplete *cp = &ic->cp;
    if (!cp->ch_have || !cp->ch_ed || !cp->ch_ed->ta) return;
    int col = cp->ch_col > 0 ? cp->ch_col - 1 : 0;
    ICOverlayLabel l = { IC_OV_HINT, cp->ch_text,
                         cp->ch_alt[0] && s_strcmp(cp->ch_alt, cp->ch_text) != 0 ? cp->ch_alt : NULL,
                         cp->ch_row, col, cp->ch_row, col, -1.0f, -1, 0,
                         IC_OV_SAME_ROW | IC_OV_ONE_ROW, NULL, NULL, 0 };
    ic_overlay_add(ic, ui, cp->ch_ed->ta, &l);
}

// ===========================================================================
// The per-frame entry points
// ===========================================================================

// First, before the overlay is built and before any textarea draws: this is
// where keys are taken off the queue the textarea is about to read.
static void ic_complete_input(InteractiveCoding *ic, UIContext *ui) {
    icc_menu_update(ic, ui);
    icc_hint_update(ic, ui);
    icc_kw_update(ic, ui);
    icc_chint_update(ic, ui);
    icc_comma_update(ic, ui);
}

// An editor is going away: let go of everything aimed at it.
static void ic_complete_forget_editor(InteractiveCoding *ic, ICEditor *ed) {
    ICComplete *cp = &ic->cp;
    if (cp->menu_ed == ed) icc_menu_close(ic);
    if (cp->hint_ed == ed) { cp->hint_ed = NULL; icc_hint_off(cp); }
    if (cp->kw_ed == ed) cp->kw_idx = 0;
    if (cp->ch_ed == ed) icc_chint_clear(cp);
    if (cp->cmh_ed == ed) icc_comma_clear(cp);
}

static void ic_complete_release(InteractiveCoding *ic) {
    icc_menu_close(ic);
    ic_catalog_free(ic, &ic->cp.base);
    ic->cp.base_ok = false;
}

void interactive_coding_set_hints(InteractiveCoding *ic, bool on) {
    if (!ic) return;
    ic->cp.hints = on;
    if (!on) icc_chint_clear(&ic->cp);
}

void interactive_coding_set_completion(InteractiveCoding *ic, bool on) {
    if (!ic || ic->cp.on == on) return;
    ic->cp.on = on;
    if (!on) {
        icc_menu_close(ic);
        icc_hint_off(&ic->cp);
        icc_comma_clear(&ic->cp);
    }
}
