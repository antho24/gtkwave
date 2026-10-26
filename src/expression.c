#include <config.h>
#include <string.h>
#include "globals.h"
#include "analyzer.h"
#include "entry.h"
#include "expression.h"
#include "signalwindow.h"
#include "simplereq.h"

/*
 * Expression traces
 *
 * An expression combines signals with VHDL and C style operators:
 *
 *   or | || nor   xor ^ xnor   and & && nand       bitwise (|, &) and logical (||, &&)
 *   = == /= !=   < <= > >=                         comparisons, 1 bit result
 *   + - *                                          unsigned arithmetic, signed if an
 *                                                  operand is signed
 *   not ~ ! -                                      unary operators
 *   sig[7:4] sig[3] sig(7 downto 4)                slices, using the index range of sig
 *   {a, b}                                         concatenation
 *   signed(x) unsigned(x)                          change the signedness
 *
 * Literals are decimal (12), hexadecimal (0x1F, x"1F"), binary (0b0101, "0101", b"0101"),
 * bits ('1') and booleans (true, false).  Signals are named like a trace in the wave window
 * or by their full hierarchical name.  A name that isn't a signal can be compared with a
 * signal of an enumeration type, e.g. "state = IDLE".
 *
 * Values are up to 64 bits wide.  A bit that isn't 0 or 1 makes the result of arithmetic and
 * comparisons X, the bitwise and logical operators treat X like VHDL does (0 and X = 0).
 */

#define MAX_WIDTH 64

typedef struct
{
    guint64 v; /* value */
    guint64 x; /* bits that aren't 0 or 1 */
    const gchar *str; /* value of an enumeration or string, otherwise NULL */
} Value;

typedef struct
{
    GwNode *nd;
    GwBitVector *bv;
    gint width;
    gint msi;
    gint lsi;
    gboolean is_signed;
    guint enum_filter;

    /* evaluation state */
    GwHistEnt *hist;
    gint region;
    gchar bits[MAX_WIDTH + 1];
} Operand;

typedef enum
{
    EXPR_NUMBER,
    EXPR_SIGNAL,
    EXPR_NAME,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_SLICE,
    EXPR_CONCAT,
    EXPR_SIGNEDNESS,
} ExprKind;

typedef enum
{
    OP_LOR,
    OP_LAND,
    OP_OR,
    OP_NOR,
    OP_XOR,
    OP_XNOR,
    OP_AND,
    OP_NAND,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_NOT,
    OP_LNOT,
    OP_NEG,
} Op;

typedef struct _Expr Expr;
struct _Expr
{
    ExprKind kind;
    Op op;
    Expr *a;
    Expr *b;
    GPtrArray *items; /* EXPR_CONCAT */
    Value number; /* EXPR_NUMBER */
    gint operand; /* EXPR_SIGNAL */
    gchar *name; /* EXPR_NAME */
    gint msb; /* EXPR_SLICE, bit positions from the LSB */
    gint lsb;
    gint index_msb; /* EXPR_SLICE, as written */
    gint index_lsb;

    gint width;
    gboolean is_signed;
};

typedef struct
{
    const gchar *text;
    const gchar *pos;
    GPtrArray *operands;
    GError *error;
} Parser;

static GQuark expression_error_quark(void)
{
    return g_quark_from_static_string("gtkwave-expression-error");
}

static void parser_error(Parser *p, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static void parser_error(Parser *p, const gchar *format, ...)
{
    if (p->error != NULL) {
        return;
    }

    va_list args;
    va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);

    p->error = g_error_new_literal(expression_error_quark(), 0, message);
    g_free(message);
}

static guint64 width_mask(gint width)
{
    return width >= 64 ? G_MAXUINT64 : (G_GUINT64_CONSTANT(1) << width) - 1;
}

static Expr *expr_new(ExprKind kind)
{
    Expr *e = g_new0(Expr, 1);
    e->kind = kind;
    return e;
}

static void expr_free(Expr *e)
{
    if (e == NULL) {
        return;
    }
    expr_free(e->a);
    expr_free(e->b);
    if (e->items != NULL) {
        for (guint i = 0; i < e->items->len; i++) {
            expr_free(g_ptr_array_index(e->items, i));
        }
        g_ptr_array_free(e->items, TRUE);
    }
    g_free(e->name);
    g_free(e);
}

/* signal lookup */

static gboolean is_signed_node(GwNode *nd)
{
    switch (nd->vardt) {
        case GW_VAR_DATA_TYPE_VHDL_SIGNED:
        case GW_VAR_DATA_TYPE_VHDL_INTEGER:
        case GW_VAR_DATA_TYPE_VHDL_NATURAL:
        case GW_VAR_DATA_TYPE_VHDL_POSITIVE:
        case GW_VAR_DATA_TYPE_VHDL_SFIXED:
            return TRUE;
        default:
            break;
    }

    switch (nd->vartype) {
        case GW_VAR_TYPE_VCD_INTEGER:
        case GW_VAR_TYPE_SV_INT:
        case GW_VAR_TYPE_SV_SHORTINT:
        case GW_VAR_TYPE_SV_LONGINT:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Does NAME equal FULL, or FULL without its bit range ("u" for "u[7:0]")?  */
static gboolean name_matches(const gchar *name, const gchar *full)
{
    if (full == NULL) {
        return FALSE;
    }
    if (strcmp(name, full) == 0) {
        return TRUE;
    }

    gsize len = strlen(name);
    if (strncmp(name, full, len) != 0 || full[len] != '[') {
        return FALSE;
    }
    const gchar *range = full + len;
    return strchr(range, ':') != NULL && strchr(range, ']') == range + strlen(range) - 1;
}

typedef enum
{
    MATCH_NONE,
    MATCH_WITHOUT_RANGE, /* "u" for "u[7:0]" */
    MATCH_EXACT,
} Match;

static Match match_name(const gchar *name, const gchar *full)
{
    if (full == NULL) {
        return MATCH_NONE;
    }
    if (strcmp(name, full) == 0) {
        return MATCH_EXACT;
    }
    return name_matches(name, full) ? MATCH_WITHOUT_RANGE : MATCH_NONE;
}

static Match trace_matches(GwTrace *t, const gchar *name)
{
    if (t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH)) {
        return MATCH_NONE;
    }

    const gchar *full = t->vector ? t->n.vec->bvname : t->n.nd->nname;
    return MAX(match_name(name, t->name),
               MAX(match_name(name, t->name_full), match_name(name, full)));
}

/* Is trace T derived from other signals: an expression, a bit of an expanded vector or a
   vector combined from such bits?  */
static gboolean is_derived_trace(GwTrace *t)
{
    if (!t->vector) {
        return t->n.nd->expansion != NULL || expression_is_node(t->n.nd);
    }

    GwBits *bits = t->n.vec->bits;
    for (gint i = 0; bits != NULL && i < bits->nnbits; i++) {
        if (bits->nodes[i]->expansion != NULL) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Finds the signal NAME in the traces or in the facilities.  An exact name is preferred over a
   name without the bit range and a signal of the dump file over a derived trace.  Returns
   FALSE if there is no such signal, sets an error if the name is ambiguous.  */
static gboolean find_signal(Parser *p, const gchar *name, GwNode **nd, GwBitVector **bv)
{
    *nd = NULL;
    *bv = NULL;

    GwTrace *best = NULL;
    gint best_rank = 0;
    gboolean ambiguous = FALSE;

    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        Match match = trace_matches(t, name);
        if (match == MATCH_NONE) {
            continue;
        }

        gint rank = 2 * match + (is_derived_trace(t) ? 0 : 1);
        gboolean same = best != NULL && t->vector == best->vector &&
                        (t->vector ? t->n.vec == best->n.vec : t->n.nd == best->n.nd);
        if (rank > best_rank) {
            best = t;
            best_rank = rank;
            ambiguous = FALSE;
        } else if (rank == best_rank && !same) {
            ambiguous = TRUE;
        }
    }

    if (ambiguous) {
        parser_error(p, "'%s' matches more than one trace, use the full name", name);
        return FALSE;
    }

    if (best != NULL) {
        *nd = best->vector ? NULL : best->n.nd;
        *bv = best->vector ? best->n.vec : NULL;
        return TRUE;
    }

    GwFacs *facs = gw_dump_file_get_facs(GLOBALS->dump_file);
    GwSymbol *s = gw_facs_lookup(facs, name);
    if (s == NULL) {
        // a vector without its bit range
        for (guint i = 0; i < gw_facs_get_length(facs); i++) {
            GwSymbol *fac = gw_facs_get(facs, i);
            if (name_matches(name, fac->name)) {
                s = fac;
                break;
            }
        }
    }

    if (s == NULL || s->n == NULL) {
        return FALSE;
    }

    *nd = s->n;
    if ((*nd)->mv.mvlfac != NULL) {
        import_trace(*nd);
    }
    return TRUE;
}

static gint add_operand(Parser *p, GwNode *nd, GwBitVector *bv)
{
    for (guint i = 0; i < p->operands->len; i++) {
        Operand *op = g_ptr_array_index(p->operands, i);
        if (op->nd == nd && op->bv == bv) {
            return i;
        }
    }

    Operand *op = g_new0(Operand, 1);
    op->nd = nd;
    op->bv = bv;

    if (bv != NULL) {
        op->width = bv->nbits;
        op->msi = bv->nbits - 1;
        op->lsi = 0;
        GwNode *first = bv->bits != NULL && bv->bits->nnbits > 0 ? bv->bits->nodes[0] : NULL;
        op->is_signed = first != NULL && is_signed_node(first);

        // the index range of a coalesced vector is in its name, e.g. "sf[3:-4]"
        const gchar *lb = strrchr(bv->bvname, '[');
        gint msi, lsi;
        if (lb != NULL && sscanf(lb, "[%d:%d]", &msi, &lsi) == 2 &&
            ABS(msi - lsi) + 1 == bv->nbits) {
            op->msi = msi;
            op->lsi = lsi;
        }
    } else {
        op->width = nd->extvals ? ABS(nd->msi - nd->lsi) + 1 : 1;
        op->msi = nd->extvals ? nd->msi : 0;
        op->lsi = nd->extvals ? nd->lsi : 0;
        op->is_signed = is_signed_node(nd);
        if (GW_IS_FST_FILE(GLOBALS->dump_file)) {
            op->enum_filter = gw_dump_file_get_enum_filter_for_node(GLOBALS->dump_file, nd);
        }
    }

    g_ptr_array_add(p->operands, op);
    return p->operands->len - 1;
}

/* lexer */

static void skip_space(Parser *p)
{
    while (g_ascii_isspace(*p->pos)) {
        p->pos++;
    }
}

static gboolean is_name_start(gchar c)
{
    return g_ascii_isalpha(c) || c == '_' || c == '\\' || c == '$';
}

static gboolean is_name_char(gchar c)
{
    return g_ascii_isalnum(c) || c == '_' || c == '.' || c == '\\' || c == '$';
}

/* Is the word KEYWORD next (case insensitive, not followed by a name character)?  */
static gboolean peek_keyword(Parser *p, const gchar *keyword)
{
    skip_space(p);
    gsize len = strlen(keyword);
    return g_ascii_strncasecmp(p->pos, keyword, len) == 0 && !is_name_char(p->pos[len]);
}

static gboolean accept_keyword(Parser *p, const gchar *keyword)
{
    if (peek_keyword(p, keyword)) {
        p->pos += strlen(keyword);
        return TRUE;
    }
    return FALSE;
}

/* Accepts the operator OP, but not if it's the start of one of the longer operators.  */
static gboolean accept_op(Parser *p, const gchar *op)
{
    skip_space(p);
    gsize len = strlen(op);
    if (strncmp(p->pos, op, len) != 0) {
        return FALSE;
    }

    static const gchar *LONGER[] = {"||", "&&", "==", "/=", "!=", "<=", ">="};
    for (gsize i = 0; i < G_N_ELEMENTS(LONGER); i++) {
        if (strlen(LONGER[i]) > len && strncmp(p->pos, LONGER[i], strlen(LONGER[i])) == 0) {
            return FALSE;
        }
    }

    p->pos += len;
    return TRUE;
}

static void expect_op(Parser *p, const gchar *op)
{
    if (!accept_op(p, op)) {
        parser_error(p, "expected '%s' at '%s'", op, p->pos);
    }
}

static gboolean parse_integer(Parser *p, gint *value)
{
    skip_space(p);
    gchar *end = NULL;
    glong v = strtol(p->pos, &end, 10);
    if (end == p->pos) {
        parser_error(p, "expected a number at '%s'", p->pos);
        return FALSE;
    }
    p->pos = end;
    *value = (gint)v;
    return TRUE;
}

/* parser */

static Expr *parse_expr(Parser *p);

static Expr *number_expr(guint64 v, guint64 x, gint width)
{
    Expr *e = expr_new(EXPR_NUMBER);
    e->number.v = v;
    e->number.x = x;
    e->width = width;
    return e;
}

/* "0101", b"0101", x"1F" or '1' starting at the quote */
static Expr *parse_bit_string(Parser *p, gint bits_per_digit)
{
    gchar quote = *p->pos++;
    guint64 v = 0;
    guint64 x = 0;
    gint width = 0;

    for (; *p->pos != quote; p->pos++) {
        gchar c = g_ascii_tolower(*p->pos);
        if (c == '\0') {
            parser_error(p, "missing closing %c", quote);
            return NULL;
        }
        if (c == '_') {
            continue;
        }

        gint digit;
        gboolean unknown = FALSE;
        if (bits_per_digit == 4 && g_ascii_isxdigit(c)) {
            digit = g_ascii_xdigit_value(c);
        } else if (c == '0' || c == '1') {
            digit = c - '0';
        } else if (strchr("xzuw-", c) != NULL) {
            digit = 0;
            unknown = TRUE;
        } else {
            parser_error(p, "invalid digit '%c' in bit string", *p->pos);
            return NULL;
        }

        width += bits_per_digit;
        if (width > MAX_WIDTH) {
            parser_error(p, "bit strings are limited to %d bits", MAX_WIDTH);
            return NULL;
        }
        v = (v << bits_per_digit) | (guint64)digit;
        x = (x << bits_per_digit) | (unknown ? width_mask(bits_per_digit) : 0);
    }
    p->pos++;

    if (width == 0) {
        parser_error(p, "empty bit string");
        return NULL;
    }
    return number_expr(v, x, width);
}

static Expr *parse_number(Parser *p)
{
    const gchar *s = p->pos;
    guint64 v = 0;
    gint width = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B')) {
        gint base = (s[1] == 'x' || s[1] == 'X') ? 16 : 2;
        gint bits_per_digit = base == 16 ? 4 : 1;
        p->pos += 2;
        for (; g_ascii_isxdigit(*p->pos) || *p->pos == '_'; p->pos++) {
            if (*p->pos == '_') {
                continue;
            }
            gint digit = g_ascii_xdigit_value(*p->pos);
            if (digit >= base) {
                parser_error(p, "invalid digit '%c'", *p->pos);
                return NULL;
            }
            width += bits_per_digit;
            v = (v << bits_per_digit) | (guint64)digit;
        }
        if (width == 0 || width > MAX_WIDTH) {
            parser_error(p, "invalid number '%.*s'", (int)(p->pos - s), s);
            return NULL;
        }
        return number_expr(v, 0, width);
    }

    for (; g_ascii_isdigit(*p->pos) || *p->pos == '_'; p->pos++) {
        if (*p->pos == '_') {
            continue;
        }
        guint64 digit = *p->pos - '0';
        if (v > (G_MAXUINT64 - digit) / 10) {
            parser_error(p, "number '%.*s' is too large", (int)(p->pos - s + 1), s);
            return NULL;
        }
        v = v * 10 + digit;
    }

    width = 1;
    while (width < MAX_WIDTH && (v >> width) != 0) {
        width++;
    }
    return number_expr(v, 0, width);
}

/* Reads a signal name: letters, digits and the characters _.$\ followed by the bracketed
   parts that make it a known signal, e.g. "tb.mem[0][7:0]" (a bracket that doesn't is a
   slice).  */
static Expr *parse_name(Parser *p)
{
    const gchar *start = p->pos;
    while (is_name_char(*p->pos)) {
        p->pos++;
    }

    gchar *name = g_strndup(start, p->pos - start);
    GwNode *nd = NULL;
    GwBitVector *bv = NULL;
    gboolean found = find_signal(p, name, &nd, &bv);

    const gchar *end = p->pos;
    while (*end == '[') {
        const gchar *close = strchr(end, ']');
        if (close == NULL) {
            break;
        }
        gchar *longer = g_strndup(start, close + 1 - start);
        GwNode *longer_nd = NULL;
        GwBitVector *longer_bv = NULL;
        if (!find_signal(p, longer, &longer_nd, &longer_bv)) {
            g_free(longer);
            break;
        }
        g_free(name);
        name = longer;
        nd = longer_nd;
        bv = longer_bv;
        found = TRUE;
        end = close + 1;
        p->pos = end;
    }

    if (p->error != NULL) {
        g_free(name);
        return NULL;
    }

    Expr *e;
    if (found) {
        e = expr_new(EXPR_SIGNAL);
        e->operand = add_operand(p, nd, bv);
        g_free(name);
    } else {
        e = expr_new(EXPR_NAME);
        e->name = name;
    }
    return e;
}

static Expr *parse_primary(Parser *p)
{
    skip_space(p);

    if (accept_op(p, "(")) {
        Expr *e = parse_expr(p);
        expect_op(p, ")");
        return e;
    }

    if (accept_op(p, "{")) {
        Expr *e = expr_new(EXPR_CONCAT);
        e->items = g_ptr_array_new();
        do {
            g_ptr_array_add(e->items, parse_expr(p));
        } while (p->error == NULL && accept_op(p, ","));
        expect_op(p, "}");
        return e;
    }

    gchar c = *p->pos;
    gchar next = c != '\0' ? p->pos[1] : '\0';

    if (c == '"' || c == '\'') {
        return parse_bit_string(p, 1);
    }
    if ((c == 'x' || c == 'X') && next == '"') {
        p->pos++;
        return parse_bit_string(p, 4);
    }
    if ((c == 'b' || c == 'B') && next == '"') {
        p->pos++;
        return parse_bit_string(p, 1);
    }
    if (g_ascii_isdigit(c)) {
        return parse_number(p);
    }

    if (accept_keyword(p, "true")) {
        return number_expr(1, 0, 1);
    }
    if (accept_keyword(p, "false")) {
        return number_expr(0, 0, 1);
    }

    static const gchar *CASTS[] = {"signed", "unsigned"};
    for (gsize i = 0; i < G_N_ELEMENTS(CASTS); i++) {
        const gchar *save = p->pos;
        if (accept_keyword(p, CASTS[i])) {
            if (accept_op(p, "(")) {
                Expr *e = expr_new(EXPR_SIGNEDNESS);
                e->is_signed = i == 0;
                e->a = parse_expr(p);
                expect_op(p, ")");
                return e;
            }
            p->pos = save; // a signal named signed
        }
    }

    if (is_name_start(c)) {
        return parse_name(p);
    }

    if (c == '\0') {
        parser_error(p, "unexpected end of the expression");
    } else {
        parser_error(p, "unexpected '%s'", p->pos);
    }
    return NULL;
}

static Expr *slice_expr(Expr *a, gint index_msb, gint index_lsb)
{
    Expr *e = expr_new(EXPR_SLICE);
    e->a = a;
    e->index_msb = index_msb;
    e->index_lsb = index_lsb;
    return e;
}

static Expr *parse_postfix(Parser *p)
{
    Expr *e = parse_primary(p);

    while (p->error == NULL) {
        gint msb, lsb;

        if (accept_op(p, "[")) {
            if (!parse_integer(p, &msb)) {
                break;
            }
            lsb = msb;
            if (accept_op(p, ":")) {
                parse_integer(p, &lsb);
            }
            expect_op(p, "]");
            e = slice_expr(e, msb, lsb);
        } else if (e != NULL && e->kind == EXPR_SIGNAL && accept_op(p, "(")) {
            if (!parse_integer(p, &msb)) {
                break;
            }
            lsb = msb;
            if (accept_keyword(p, "downto") || accept_keyword(p, "to")) {
                parse_integer(p, &lsb);
            }
            expect_op(p, ")");
            e = slice_expr(e, msb, lsb);
        } else {
            break;
        }
    }

    return e;
}

static Expr *unary_expr(Op op, Expr *a)
{
    Expr *e = expr_new(EXPR_UNARY);
    e->op = op;
    e->a = a;
    return e;
}

static Expr *parse_unary(Parser *p)
{
    if (accept_keyword(p, "not") || accept_op(p, "~")) {
        return unary_expr(OP_NOT, parse_unary(p));
    }
    if (accept_op(p, "!")) {
        return unary_expr(OP_LNOT, parse_unary(p));
    }
    if (accept_op(p, "-")) {
        return unary_expr(OP_NEG, parse_unary(p));
    }
    return parse_postfix(p);
}

typedef struct
{
    const gchar *token;
    gboolean is_keyword;
    Op op;
} OpToken;

static Expr *parse_binary_level(Parser *p, gint level);

static const OpToken *LEVELS[] = {
    (const OpToken[]){{"||", FALSE, OP_LOR}, {NULL, FALSE, 0}},
    (const OpToken[]){{"&&", FALSE, OP_LAND}, {NULL, FALSE, 0}},
    (const OpToken[]){{"or", TRUE, OP_OR},
                      {"nor", TRUE, OP_NOR},
                      {"|", FALSE, OP_OR},
                      {NULL, FALSE, 0}},
    (const OpToken[]){{"xor", TRUE, OP_XOR},
                      {"xnor", TRUE, OP_XNOR},
                      {"^", FALSE, OP_XOR},
                      {NULL, FALSE, 0}},
    (const OpToken[]){{"and", TRUE, OP_AND},
                      {"nand", TRUE, OP_NAND},
                      {"&", FALSE, OP_AND},
                      {NULL, FALSE, 0}},
    (const OpToken[]){{"==", FALSE, OP_EQ},
                      {"/=", FALSE, OP_NE},
                      {"!=", FALSE, OP_NE},
                      {"=", FALSE, OP_EQ},
                      {NULL, FALSE, 0}},
    (const OpToken[]){{"<=", FALSE, OP_LE},
                      {">=", FALSE, OP_GE},
                      {"<", FALSE, OP_LT},
                      {">", FALSE, OP_GT},
                      {NULL, FALSE, 0}},
    (const OpToken[]){{"+", FALSE, OP_ADD}, {"-", FALSE, OP_SUB}, {NULL, FALSE, 0}},
    (const OpToken[]){{"*", FALSE, OP_MUL}, {NULL, FALSE, 0}},
};

static Expr *parse_binary_level(Parser *p, gint level)
{
    if (level == G_N_ELEMENTS(LEVELS)) {
        return parse_unary(p);
    }

    Expr *e = parse_binary_level(p, level + 1);

    while (p->error == NULL) {
        const OpToken *match = NULL;
        for (const OpToken *t = LEVELS[level]; t->token != NULL; t++) {
            if (t->is_keyword ? accept_keyword(p, t->token) : accept_op(p, t->token)) {
                match = t;
                break;
            }
        }
        if (match == NULL) {
            break;
        }

        Expr *b = expr_new(EXPR_BINARY);
        b->op = match->op;
        b->a = e;
        b->b = parse_binary_level(p, level + 1);
        e = b;
    }

    return e;
}

static Expr *parse_expr(Parser *p)
{
    return parse_binary_level(p, 0);
}

/* type check: computes the widths and checks the names */

static gboolean is_comparison(Op op)
{
    return op == OP_EQ || op == OP_NE;
}

static void check_expr(Parser *p, Expr *e, gboolean name_allowed)
{
    if (p->error != NULL || e == NULL) {
        return;
    }

    switch (e->kind) {
        case EXPR_NUMBER:
            break;

        case EXPR_SIGNAL: {
            Operand *op = g_ptr_array_index(p->operands, e->operand);
            if (op->width > MAX_WIDTH) {
                parser_error(p, "signals wider than %d bits are not supported", MAX_WIDTH);
            }
            e->width = MIN(op->width, MAX_WIDTH);
            e->is_signed = op->is_signed;
            break;
        }

        case EXPR_NAME:
            if (!name_allowed) {
                parser_error(p, "unknown signal '%s'", e->name);
            }
            e->width = 1;
            break;

        case EXPR_UNARY:
            check_expr(p, e->a, FALSE);
            e->width = e->op == OP_LNOT ? 1 : e->a->width;
            e->is_signed = e->op == OP_NEG || (e->op == OP_NOT && e->a->is_signed);
            break;

        case EXPR_SIGNEDNESS:
            check_expr(p, e->a, FALSE);
            e->width = e->a->width;
            break;

        case EXPR_BINARY: {
            gboolean names = is_comparison(e->op);
            check_expr(p, e->a, names);
            check_expr(p, e->b, names);
            if (p->error != NULL) {
                break;
            }
            if (e->a->kind == EXPR_NAME && e->b->kind == EXPR_NAME) {
                parser_error(p, "unknown signals '%s' and '%s'", e->a->name, e->b->name);
                break;
            }

            gint wa = e->a->width;
            gint wb = e->b->width;
            switch (e->op) {
                case OP_LOR:
                case OP_LAND:
                case OP_EQ:
                case OP_NE:
                case OP_LT:
                case OP_LE:
                case OP_GT:
                case OP_GE:
                    e->width = 1;
                    break;
                case OP_MUL:
                    e->width = MIN(wa + wb, MAX_WIDTH);
                    e->is_signed = e->a->is_signed || e->b->is_signed;
                    break;
                default:
                    e->width = MAX(wa, wb);
                    e->is_signed = e->a->is_signed || e->b->is_signed;
                    break;
            }
            break;
        }

        case EXPR_SLICE: {
            check_expr(p, e->a, FALSE);
            if (p->error != NULL) {
                break;
            }

            // indices are those of the signal, otherwise bit positions counted from the LSB
            gint msi = e->a->width - 1;
            gint lsi = 0;
            if (e->a->kind == EXPR_SIGNAL) {
                Operand *op = g_ptr_array_index(p->operands, e->a->operand);
                msi = op->msi;
                lsi = op->lsi;
            }

            gint lo = MIN(msi, lsi);
            gint hi = MAX(msi, lsi);
            if (e->index_msb < lo || e->index_msb > hi || e->index_lsb < lo || e->index_lsb > hi) {
                parser_error(p, "index out of the range %d to %d", msi, lsi);
                break;
            }

            e->msb = msi >= lsi ? e->index_msb - lsi : lsi - e->index_msb;
            e->lsb = msi >= lsi ? e->index_lsb - lsi : lsi - e->index_lsb;
            if (e->msb < e->lsb) {
                parser_error(p,
                             "the slice [%d:%d] is in the wrong direction",
                             e->index_msb,
                             e->index_lsb);
                break;
            }
            e->width = e->msb - e->lsb + 1;
            e->is_signed = FALSE;
            break;
        }

        case EXPR_CONCAT:
            e->width = 0;
            for (guint i = 0; i < e->items->len; i++) {
                Expr *item = g_ptr_array_index(e->items, i);
                check_expr(p, item, FALSE);
                if (p->error != NULL) {
                    return;
                }
                e->width += item->width;
            }
            if (e->width > MAX_WIDTH) {
                parser_error(p, "concatenations are limited to %d bits", MAX_WIDTH);
            }
            break;

        default:
            break;
    }
}

/* evaluation */

static Value value_from_bits(const gchar *bits, gint width)
{
    Value r = {0, 0, NULL};
    for (gint i = 0; i < width; i++) {
        r.v <<= 1;
        r.x <<= 1;
        switch (bits[i]) {
            case GW_BIT_1:
            case GW_BIT_H:
                r.v |= 1;
                break;
            case GW_BIT_0:
            case GW_BIT_L:
                break;
            default:
                r.x |= 1;
                break;
        }
    }
    return r;
}

static Value value_unknown(gint width)
{
    Value r = {0, width_mask(width), NULL};
    return r;
}

static Value operand_value(Operand *op)
{
    gint width = MIN(op->width, MAX_WIDTH);

    if (op->bv != NULL) {
        if (op->region < 0 || op->region >= op->bv->numregions) {
            return value_unknown(width);
        }
        return value_from_bits((const gchar *)op->bv->vectors[op->region]->v, width);
    }

    GwHistEnt *h = op->hist;
    if (h == NULL || h->time < -1) {
        return value_unknown(width);
    }

    if (!op->nd->extvals) {
        gchar bit = h->v.h_val;
        return value_from_bits(&bit, 1);
    }

    if (h->flags & GW_HIST_ENT_FLAG_STRING) {
        Value r = {0, 0, h->v.h_vector != NULL ? h->v.h_vector : ""};
        return r;
    }
    if ((h->flags & GW_HIST_ENT_FLAG_REAL) || h->v.h_vector == NULL) {
        return value_unknown(width);
    }

    Value r = value_from_bits(h->v.h_vector, width);

    // an enumeration with a table of names (FST files written by GHDL)
    if (op->enum_filter != 0 && r.x == 0) {
        GwEnumFilterList *filters = gw_dump_file_get_enum_filters(GLOBALS->dump_file);
        GwEnumFilter *filter = gw_enum_filter_list_get(filters, op->enum_filter - 1);
        for (gint i = 0; i < width; i++) {
            op->bits[i] = (r.v >> (width - 1 - i)) & 1 ? '1' : '0';
        }
        op->bits[width] = '\0';
        const gchar *name = filter != NULL ? gw_enum_filter_lookup(filter, op->bits) : NULL;
        if (name != NULL) {
            r.str = name;
        }
    }

    return r;
}

static gint64 signed_value(Value a, gint width, gboolean is_signed)
{
    guint64 v = a.v & width_mask(width);
    if (is_signed && width < 64 && ((v >> (width - 1)) & 1)) {
        v |= ~width_mask(width);
    }
    return (gint64)v;
}

/* extends A of width W to WIDTH bits */
static Value extend(Value a, gint w, gboolean is_signed, gint width)
{
    if (w < width && is_signed && w > 0) {
        guint64 ext = width_mask(width) & ~width_mask(w);
        if ((a.x >> (w - 1)) & 1) {
            a.x |= ext;
        } else if ((a.v >> (w - 1)) & 1) {
            a.v |= ext;
        }
    }
    return a;
}

/* boolean value: 1 if nonzero, 0 if zero, X if unknown */
static Value boolean_value(Value a, gint width)
{
    Value r = {0, 0, NULL};
    guint64 mask = width_mask(width);
    if ((a.v & ~a.x & mask) != 0) {
        r.v = 1;
    } else if ((a.x & mask) != 0) {
        r.x = 1;
    }
    return r;
}

static Value eval_and(Value a, Value b)
{
    Value r;
    guint64 zero = (~a.v & ~a.x) | (~b.v & ~b.x);
    r.v = a.v & b.v & ~a.x & ~b.x;
    r.x = (a.x | b.x) & ~zero;
    r.str = NULL;
    return r;
}

static Value eval_or(Value a, Value b)
{
    Value r;
    guint64 one = (a.v & ~a.x) | (b.v & ~b.x);
    r.v = one;
    r.x = (a.x | b.x) & ~one;
    r.str = NULL;
    return r;
}

static Value eval_xor(Value a, Value b)
{
    Value r;
    r.x = a.x | b.x;
    r.v = (a.v ^ b.v) & ~r.x;
    r.str = NULL;
    return r;
}

static Value eval_not(Value a, gint width)
{
    Value r;
    r.x = a.x;
    r.v = ~a.v & ~a.x & width_mask(width);
    r.str = NULL;
    return r;
}

static Value eval_expr(GPtrArray *operands, Expr *e);

static Value eval_comparison(GPtrArray *operands, Expr *e)
{
    Value a = eval_expr(operands, e->a);
    Value b = eval_expr(operands, e->b);
    if (e->a->kind == EXPR_NAME) {
        a.str = e->a->name;
    }
    if (e->b->kind == EXPR_NAME) {
        b.str = e->b->name;
    }

    Value r = {0, 0, NULL};

    if (a.str != NULL || b.str != NULL) {
        if (a.str == NULL || b.str == NULL) {
            r.x = 1;
        } else {
            gboolean eq = g_ascii_strcasecmp(a.str, b.str) == 0;
            if (e->op == OP_EQ || e->op == OP_NE) {
                r.v = eq == (e->op == OP_EQ);
            } else {
                r.x = 1;
            }
        }
        return r;
    }

    gint wa = e->a->width;
    gint wb = e->b->width;
    if ((a.x & width_mask(wa)) || (b.x & width_mask(wb))) {
        r.x = 1;
        return r;
    }

    gboolean is_signed = e->a->is_signed || e->b->is_signed;
    gint cmp;
    if (is_signed) {
        gint64 sa = signed_value(a, wa, e->a->is_signed);
        gint64 sb = signed_value(b, wb, e->b->is_signed);
        cmp = (sa > sb) - (sa < sb);
    } else {
        guint64 ua = a.v & width_mask(wa);
        guint64 ub = b.v & width_mask(wb);
        cmp = (ua > ub) - (ua < ub);
    }

    switch (e->op) {
        case OP_EQ:
            r.v = cmp == 0;
            break;
        case OP_NE:
            r.v = cmp != 0;
            break;
        case OP_LT:
            r.v = cmp < 0;
            break;
        case OP_LE:
            r.v = cmp <= 0;
            break;
        case OP_GT:
            r.v = cmp > 0;
            break;
        case OP_GE:
            r.v = cmp >= 0;
            break;
        default:
            break;
    }
    return r;
}

static Value eval_expr(GPtrArray *operands, Expr *e)
{
    Value r = {0, 0, NULL};
    gint width = e->width;

    switch (e->kind) {
        case EXPR_NUMBER:
            return e->number;

        case EXPR_SIGNAL:
            return operand_value(g_ptr_array_index(operands, e->operand));

        case EXPR_NAME:
            r.str = e->name;
            return r;

        case EXPR_SIGNEDNESS:
            return eval_expr(operands, e->a);

        case EXPR_UNARY: {
            Value a = eval_expr(operands, e->a);
            if (a.str != NULL) {
                return value_unknown(width);
            }
            switch (e->op) {
                case OP_NOT:
                    return eval_not(a, width);
                case OP_LNOT: {
                    Value b = boolean_value(a, e->a->width);
                    return eval_not(b, 1);
                }
                case OP_NEG:
                    if (a.x & width_mask(width)) {
                        return value_unknown(width);
                    }
                    r.v = (~a.v + 1) & width_mask(width);
                    return r;
                default:
                    return value_unknown(width);
            }
        }

        case EXPR_BINARY: {
            if (e->op >= OP_EQ && e->op <= OP_GE) {
                return eval_comparison(operands, e);
            }

            Value a = eval_expr(operands, e->a);
            Value b = eval_expr(operands, e->b);
            if (a.str != NULL || b.str != NULL) {
                return value_unknown(width);
            }

            if (e->op == OP_LOR || e->op == OP_LAND) {
                Value ba = boolean_value(a, e->a->width);
                Value bb = boolean_value(b, e->b->width);
                return e->op == OP_LOR ? eval_or(ba, bb) : eval_and(ba, bb);
            }

            a = extend(a, e->a->width, e->a->is_signed, width);
            b = extend(b, e->b->width, e->b->is_signed, width);
            guint64 mask = width_mask(width);

            switch (e->op) {
                case OP_AND:
                    r = eval_and(a, b);
                    break;
                case OP_NAND:
                    r = eval_not(eval_and(a, b), width);
                    break;
                case OP_OR:
                    r = eval_or(a, b);
                    break;
                case OP_NOR:
                    r = eval_not(eval_or(a, b), width);
                    break;
                case OP_XOR:
                    r = eval_xor(a, b);
                    break;
                case OP_XNOR:
                    r = eval_not(eval_xor(a, b), width);
                    break;
                case OP_ADD:
                case OP_SUB:
                case OP_MUL:
                    if ((a.x | b.x) & mask) {
                        return value_unknown(width);
                    }
                    if (e->op == OP_ADD) {
                        r.v = a.v + b.v;
                    } else if (e->op == OP_SUB) {
                        r.v = a.v - b.v;
                    } else {
                        r.v = a.v * b.v;
                    }
                    break;
                default:
                    return value_unknown(width);
            }

            r.v &= mask;
            r.x &= mask;
            return r;
        }

        case EXPR_SLICE: {
            Value a = eval_expr(operands, e->a);
            if (a.str != NULL) {
                return value_unknown(width);
            }
            r.v = (a.v >> e->lsb) & width_mask(width);
            r.x = (a.x >> e->lsb) & width_mask(width);
            return r;
        }

        case EXPR_CONCAT:
            for (guint i = 0; i < e->items->len; i++) {
                Expr *item = g_ptr_array_index(e->items, i);
                Value a = eval_expr(operands, item);
                if (a.str != NULL) {
                    a = value_unknown(item->width);
                }
                guint64 mask = width_mask(item->width);
                r.v = (item->width >= 64 ? 0 : r.v << item->width) | (a.v & mask);
                r.x = (item->width >= 64 ? 0 : r.x << item->width) | (a.x & mask);
            }
            return r;

        default:
            break;
    }

    return value_unknown(width);
}

static gint compare_times(gconstpointer a, gconstpointer b)
{
    GwTime ta = *(const GwTime *)a;
    GwTime tb = *(const GwTime *)b;
    return (ta > tb) - (ta < tb);
}

/* the times at which any operand changes */
static GArray *collect_change_times(GPtrArray *operands)
{
    GArray *times = g_array_new(FALSE, FALSE, sizeof(GwTime));
    GwTime initial = -1;
    g_array_append_val(times, initial);

    for (guint i = 0; i < operands->len; i++) {
        Operand *op = g_ptr_array_index(operands, i);

        if (op->bv != NULL) {
            for (gint r = 0; r < op->bv->numregions; r++) {
                GwTime t = op->bv->vectors[r]->time;
                if (t >= -1 && t < GW_TIME_MAX - 1) {
                    g_array_append_val(times, t);
                }
            }
        } else {
            for (GwHistEnt *h = op->nd->head.next; h != NULL; h = h->next) {
                if (h->time >= -1 && h->time < GW_TIME_MAX - 1) {
                    g_array_append_val(times, h->time);
                }
            }
        }
    }

    g_array_sort(times, compare_times);

    guint n = 0;
    for (guint i = 0; i < times->len; i++) {
        GwTime t = g_array_index(times, GwTime, i);
        if (n == 0 || g_array_index(times, GwTime, n - 1) != t) {
            g_array_index(times, GwTime, n++) = t;
        }
    }
    g_array_set_size(times, n);

    return times;
}

static void advance_operand(Operand *op, GwTime t)
{
    if (op->bv != NULL) {
        while (op->region + 1 < op->bv->numregions && op->bv->vectors[op->region + 1]->time <= t) {
            op->region++;
        }
    } else {
        if (op->hist == NULL) {
            op->hist = &op->nd->head;
        }
        while (op->hist->next != NULL && op->hist->next->time <= t) {
            op->hist = op->hist->next;
        }
    }
}

static GwHistEnt *hist_ent_new(GwTime time, Value value, gint width)
{
    GwHistEnt *h = g_new0(GwHistEnt, 1);
    h->time = time;

    if (width == 1) {
        h->v.h_val = (value.x & 1) ? GW_BIT_X : ((value.v & 1) ? GW_BIT_1 : GW_BIT_0);
    } else {
        h->v.h_vector = g_malloc(width);
        for (gint i = 0; i < width; i++) {
            gint bit = width - 1 - i;
            if ((value.x >> bit) & 1) {
                h->v.h_vector[i] = GW_BIT_X;
            } else {
                h->v.h_vector[i] = ((value.v >> bit) & 1) ? GW_BIT_1 : GW_BIT_0;
            }
        }
    }

    return h;
}

static gboolean same_value(Value a, Value b, gint width)
{
    guint64 mask = width_mask(width);
    return (a.x & mask) == (b.x & mask) && (a.v & ~a.x & mask) == (b.v & ~b.x & mask);
}

/* Evaluates E at every change of its operands into the history of a new node.  */
static GwNode *build_node(const gchar *text, Expr *e, GPtrArray *operands)
{
    gint width = e->width;

    GwNode *nd = g_new0(GwNode, 1);
    nd->nname = g_strdup(text);
    if (width > 1) {
        nd->extvals = 1;
        nd->msi = width - 1;
        nd->lsi = 0;
        if (e->is_signed) {
            nd->vardt = GW_VAR_DATA_TYPE_VHDL_SIGNED;
        }
    }

    Value unknown = value_unknown(width);
    GwHistEnt *head_value = hist_ent_new(-2, unknown, width);
    nd->head = *head_value;
    g_free(head_value);

    GwHistEnt *tail = &nd->head;
    gboolean first = TRUE;
    Value previous = unknown;

    GArray *times = collect_change_times(operands);
    for (guint i = 0; i < times->len; i++) {
        GwTime t = g_array_index(times, GwTime, i);
        for (guint j = 0; j < operands->len; j++) {
            advance_operand(g_ptr_array_index(operands, j), t);
        }

        Value value = eval_expr(operands, e);
        if (value.str != NULL) {
            value = unknown;
        }
        if (first || !same_value(value, previous, width)) {
            tail->next = hist_ent_new(t, value, width);
            tail = tail->next;
            previous = value;
            first = FALSE;
        }
    }
    g_array_free(times, TRUE);

    // end caps like the loaders add
    tail->next = hist_ent_new(GW_TIME_MAX - 1, unknown, width);
    tail = tail->next;
    Value z = unknown;
    GwHistEnt *cap = hist_ent_new(GW_TIME_MAX, z, width);
    if (width == 1) {
        cap->v.h_val = GW_BIT_Z;
    } else {
        memset(cap->v.h_vector, GW_BIT_Z, width);
    }
    tail->next = cap;
    nd->curr = cap;

    return nd;
}

static void operand_free(gpointer data)
{
    g_free(data);
}

/**
 * expression_create_node:
 * @text: The expression.
 * @error: Return location for an error.
 *
 * Creates a node with the values of the expression over time.
 *
 * Returns: (transfer none): The node or %NULL if the expression is invalid.
 */
GwNode *expression_create_node(const char *text, GError **error)
{
    if (GLOBALS->dump_file == NULL) {
        g_set_error_literal(error, expression_error_quark(), 0, "no waveform is loaded");
        return NULL;
    }

    Parser p = {
        .text = text,
        .pos = text,
        .operands = g_ptr_array_new_with_free_func(operand_free),
        .error = NULL,
    };

    Expr *e = parse_expr(&p);
    skip_space(&p);
    if (p.error == NULL && *p.pos != '\0') {
        parser_error(&p, "unexpected '%s'", p.pos);
    }
    check_expr(&p, e, FALSE);
    if (p.error == NULL && e != NULL && e->kind == EXPR_NAME) {
        parser_error(&p, "unknown signal '%s'", e->name);
    }

    GwNode *nd = NULL;
    if (p.error == NULL && e != NULL) {
        nd = build_node(text, e, p.operands);

        if (GLOBALS->expression_nodes == NULL) {
            GLOBALS->expression_nodes = g_hash_table_new(g_direct_hash, g_direct_equal);
        }
        g_hash_table_add(GLOBALS->expression_nodes, nd);
    } else if (p.error != NULL) {
        g_propagate_error(error, p.error);
    }

    expr_free(e);
    g_ptr_array_free(p.operands, TRUE);

    return nd;
}

/**
 * expression_is_node:
 * @nd: A node.
 *
 * Returns: %TRUE if @nd was created from an expression.
 */
gboolean expression_is_node(GwNode *nd)
{
    return nd != NULL && GLOBALS->expression_nodes != NULL &&
           g_hash_table_contains(GLOBALS->expression_nodes, nd);
}

/* Moves the last trace behind the highlighted traces, like an inserted comment.  */
static void move_last_trace_to_insert_position(void)
{
    GwTrace *t = GLOBALS->traces.last;
    if (t == NULL || t->t_prev == NULL) {
        return;
    }

    // not inside an expanded signal, behind it
    GwTrace *last_highlighted = NULL;
    for (GwTrace *u = GLOBALS->traces.first; u != t; u = u->t_next) {
        if (u->flags & TR_HIGHLIGHT) {
            last_highlighted = u;
        }
    }
    GwTrace *expanded = find_enclosing_expanded_trace(last_highlighted);

    GLOBALS->traces.last = t->t_prev;
    GLOBALS->traces.last->t_next = NULL;
    t->t_prev = NULL;
    GLOBALS->traces.total--;

    if (expanded != NULL && expanded->t_match != NULL) {
        insert_trace_after(t, expanded->t_match);
        return;
    }

    GwTrace *buffer = GLOBALS->traces.buffer;
    GwTrace *bufferlast = GLOBALS->traces.bufferlast;
    int buffercount = GLOBALS->traces.buffercount;

    GLOBALS->traces.buffer = GLOBALS->traces.bufferlast = t;
    GLOBALS->traces.buffercount = 1;
    PasteBuffer();

    GLOBALS->traces.buffer = buffer;
    GLOBALS->traces.bufferlast = bufferlast;
    GLOBALS->traces.buffercount = buffercount;
}

/**
 * expression_add_trace:
 * @text: The expression.
 * @alias: (nullable): The name of the trace, %NULL to name it after the expression.
 * @insert: %TRUE to insert the trace behind the highlighted traces, %FALSE to append it.
 * @error: Return location for an error.
 *
 * Adds a trace with the values of an expression.
 *
 * Returns: %TRUE if the trace was added.
 */
gboolean expression_add_trace(const char *text, const char *alias, gboolean insert, GError **error)
{
    GwNode *nd = expression_create_node(text, error);
    if (nd == NULL) {
        return FALSE;
    }

    AddNodeTraceReturn(nd, (char *)alias, NULL);
    if (insert) {
        move_last_trace_to_insert_position();
    }
    return TRUE;
}

/* save files */

/**
 * expression_save_file_lines:
 * @t: An expression trace.
 *
 * Returns: The save file lines of the trace, free with g_free().
 */
char *expression_save_file_lines(GwTrace *t)
{
    GString *lines = g_string_new(NULL);
    if (HasAlias(t)) {
        g_string_append_printf(lines, "[expression_name] %s\n", t->name_full);
    }
    g_string_append_printf(lines, "[expression] %s\n", t->n.nd->nname);
    return g_string_free(lines, FALSE);
}

void expression_write_save_file(FILE *wave, GwTrace *t)
{
    gchar *lines = expression_save_file_lines(t);
    fputs(lines, wave);
    g_free(lines);
}

/* Handles the save file lines "[expression_name] name" and "[expression] text".  */
void expression_parse_save_file_line(const char *tag, const char *line)
{
    gchar *text = g_strstrip(g_strdup(line));

    if (strcmp(tag, "expression_name") == 0) {
        g_free(GLOBALS->expression_name);
        GLOBALS->expression_name = text;
        return;
    }

    GError *error = NULL;
    if (!expression_add_trace(text, GLOBALS->expression_name, FALSE, &error)) {
        fprintf(stderr, "GTKWAVE | Cannot add expression '%s': %s\n", text, error->message);
        g_error_free(error);
    }

    g_clear_pointer(&GLOBALS->expression_name, g_free);
    g_free(text);
}

/* Edit/Insert Expression */

static void expression_error_cleanup(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
}

static void expression_trace_cleanup(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    if (GLOBALS->entrybox_text == NULL) {
        return;
    }

    gchar *text = g_strstrip(g_strdup(GLOBALS->entrybox_text));
    free_2(GLOBALS->entrybox_text);
    GLOBALS->entrybox_text = NULL;

    g_free(GLOBALS->last_expression);
    GLOBALS->last_expression = g_strdup(text);

    GError *error = NULL;
    if (text[0] != '\0' && !expression_add_trace(text, NULL, TRUE, &error)) {
        simplereqbox("Invalid Expression",
                     400,
                     error->message,
                     "OK",
                     NULL,
                     G_CALLBACK(expression_error_cleanup),
                     1);
        g_error_free(error);
    } else {
        GLOBALS->signalwindow_width_dirty = 1;
        redraw_signals_and_waves();
    }

    g_free(text);
}

void menu_insert_expression_trace(gpointer null_data, guint callback_action, GtkWidget *widget)
{
    (void)null_data;
    (void)callback_action;
    (void)widget;

    if (GLOBALS->dump_file == NULL) {
        return;
    }

    entrybox("Insert Expression (e.g. valid and ready, cnt = 5, data[7:4])",
             500,
             GLOBALS->last_expression != NULL ? GLOBALS->last_expression : "",
             NULL,
             1024,
             G_CALLBACK(expression_trace_cleanup));
}
