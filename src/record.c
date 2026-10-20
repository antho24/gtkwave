/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#include <config.h>
#include "globals.h"
#include <string.h>
#include "analyzer.h"
#include "baseconvert.h"
#include "record.h"

#define RECORD_SIG_MASK (~(TR_HIGHLIGHT | TR_CLOSED | TR_COLLAPSED | TR_RSVD))

/* synthesized node of a record, the values are stored in a string chunk */
typedef struct
{
    GwNode node; /* must be first */
    GwHistEnt *ents;
    GStringChunk *values;
} RecordNode;

typedef struct
{
    GwTrace *t;
    gint idx; /* current entry (latest with time <= the merge time) */
    gint count;
} RecordField;

/* A record header is a group begin flagged TR_RECORD which is either blank
   (not built yet) or shows a synthesized node.  */
static gboolean is_record_header(GwTrace *t)
{
    return (t->flags & (TR_RECORD | TR_GRP_BEGIN)) == (TR_RECORD | TR_GRP_BEGIN) &&
           (!HasWave(t) || t->record_node_owned);
}

/* Returns the traces that make up the record T: its waveform traces and
   nested records, but not the contents of nested (non record) groups.  */
static GPtrArray *record_get_fields(GwTrace *t)
{
    GPtrArray *fields = g_ptr_array_new();

    if (t->t_match == NULL) {
        return fields;
    }

    for (GwTrace *c = t->t_next; c != NULL && c != t->t_match; c = c->t_next) {
        if (IsGroupBegin(c)) {
            if (HasWave(c)) {
                g_ptr_array_add(fields, c);
            }
            if (c->t_match == NULL) {
                break;
            }
            c = c->t_match;
        } else if (HasWave(c) && !(c->flags & TR_EXCLUDE)) {
            g_ptr_array_add(fields, c);
        }
    }

    return fields;
}

static gboolean record_field_init(RecordField *f, GwTrace *t)
{
    f->t = t;
    f->idx = 0;

    if (t->vector) {
        f->count = t->n.vec->numregions;
    } else {
        f->count = t->n.nd->harray != NULL ? t->n.nd->numhist : 0;
    }

    return f->count > 0;
}

static GwTime record_field_time(RecordField *f, gint idx)
{
    if (f->t->vector) {
        return f->t->n.vec->vectors[idx]->time;
    } else {
        return f->t->n.nd->harray[idx]->time;
    }
}

/* Formats the current value of field F like the signal list does.  */
static void record_field_append_value(RecordField *f, GString *str)
{
    GwTrace *t = f->t;
    char *s;

    if (t->vector) {
        s = convert_ascii(t, t->n.vec->vectors[f->idx]);
    } else {
        GwNode *nd = t->n.nd;
        GwHistEnt *h = nd->harray[f->idx];

        if (!nd->extvals) {
            unsigned char h_val = h->v.h_val;
            if (t->flags & TR_INVERT) {
                h_val = gw_bit_invert(h_val);
            }
            g_string_append_c(str, gw_bit_to_char(h_val));
            return;
        }

        if (h->flags & GW_HIST_ENT_FLAG_REAL) {
            if (!(h->flags & GW_HIST_ENT_FLAG_STRING)) {
                s = convert_ascii_real(t, &h->v.h_double);
            } else {
                s = convert_ascii_string(h->v.h_vector);
            }
        } else {
            s = convert_ascii_vec(t, h->v.h_vector);
        }
    }

    if (s != NULL) {
        g_string_append(str, s);
        free_2(s);
    }
}

static GwNode *record_node_new(GwTrace *t, GPtrArray *fields)
{
    RecordField *f = g_new0(RecordField, fields->len);
    guint n = 0;

    for (guint i = 0; i < fields->len; i++) {
        if (record_field_init(&f[n], g_ptr_array_index(fields, i))) {
            n++;
        }
    }

    if (n == 0) {
        g_free(f);
        return NULL;
    }

    GArray *times = g_array_new(FALSE, FALSE, sizeof(GwTime));
    GPtrArray *values = g_ptr_array_new();
    GStringChunk *chunk = g_string_chunk_new(4096);
    GString *str = g_string_new(NULL);

    for (;;) {
        /* find the next transition of any field */
        gboolean found = FALSE;
        GwTime next = 0;
        for (guint i = 0; i < n; i++) {
            if (f[i].idx + 1 < f[i].count) {
                GwTime tim = record_field_time(&f[i], f[i].idx + 1);
                if (!found || tim < next) {
                    next = tim;
                    found = TRUE;
                }
            }
        }
        if (!found) {
            break;
        }

        for (guint i = 0; i < n; i++) {
            while (f[i].idx + 1 < f[i].count && record_field_time(&f[i], f[i].idx + 1) <= next) {
                f[i].idx++;
            }
        }

        g_string_truncate(str, 0);
        g_string_append_c(str, '{');
        for (guint i = 0; i < n; i++) {
            if (i > 0) {
                g_string_append_c(str, ' ');
            }
            record_field_append_value(&f[i], str);
        }
        g_string_append_c(str, '}');

        if (values->len > 0 && strcmp(g_ptr_array_index(values, values->len - 1), str->str) == 0) {
            continue;
        }

        g_array_append_val(times, next);
        g_ptr_array_add(values, g_string_chunk_insert_const(chunk, str->str));
    }

    g_string_free(str, TRUE);
    g_free(f);

    if (values->len == 0) {
        g_array_free(times, TRUE);
        g_ptr_array_free(values, TRUE);
        g_string_chunk_free(chunk);
        return NULL;
    }

    RecordNode *rn = g_new0(RecordNode, 1);
    GwNode *nd = &rn->node;
    guint count = values->len;
    GwHistEnt *ents = g_new0(GwHistEnt, count);

    rn->ents = ents;
    rn->values = chunk;

    nd->nname = g_strdup(t->name != NULL ? t->name : "record");
    nd->head.time = -2;
    nd->head.flags = GW_HIST_ENT_FLAG_REAL | GW_HIST_ENT_FLAG_STRING;
    nd->head.next = &ents[0];

    nd->harray = g_new(GwHistEnt *, count + 1);
    nd->harray[0] = &nd->head;
    for (guint i = 0; i < count; i++) {
        ents[i].time = g_array_index(times, GwTime, i);
        ents[i].v.h_vector = g_ptr_array_index(values, i);
        ents[i].flags = GW_HIST_ENT_FLAG_REAL | GW_HIST_ENT_FLAG_STRING;
        ents[i].next = (i + 1 < count) ? &ents[i + 1] : NULL;
        nd->harray[i + 1] = &ents[i];
    }
    nd->numhist = count + 1;
    nd->curr = &ents[count - 1];
    nd->extvals = 1;
    nd->vartype = GW_VAR_TYPE_GEN_STRING;

    g_array_free(times, TRUE);
    g_ptr_array_free(values, TRUE);

    return nd;
}

static void record_node_free(GwNode *nd)
{
    RecordNode *rn = (RecordNode *)nd;

    g_string_chunk_free(rn->values);
    g_free(rn->ents);
    g_free(nd->harray);
    g_free(nd->nname);
    g_free(rn);
}

static guint64 record_signature(GPtrArray *fields)
{
    guint64 sig = UINT64_C(1469598103934665603);

#define RECORD_SIG_MIX(x) sig = (sig ^ (guint64)(x)) * UINT64_C(1099511628211)
    for (guint i = 0; i < fields->len; i++) {
        GwTrace *c = g_ptr_array_index(fields, i);
        RECORD_SIG_MIX((guintptr)c);
        RECORD_SIG_MIX((guintptr)c->n.nd);
        RECORD_SIG_MIX(c->flags & RECORD_SIG_MASK);
        RECORD_SIG_MIX(c->e_filter);
        RECORD_SIG_MIX(c->f_filter);
        RECORD_SIG_MIX(c->p_filter);
        RECORD_SIG_MIX(c->t_filter);
        RECORD_SIG_MIX(c->t_fpdecshift);
    }
#undef RECORD_SIG_MIX

    return sig;
}

static void record_trace_build_with_fields(GwTrace *t, GPtrArray *fields)
{
    GwNode *nd = record_node_new(t, fields);

    record_trace_free(t);

    if (t->asciivalue) {
        free_2(t->asciivalue);
        t->asciivalue = NULL;
    }

    t->record_sig = record_signature(fields);

    if (nd == NULL) {
        t->flags |= TR_BLANK;
        return;
    }

    t->vector = FALSE;
    t->n.nd = nd;
    t->record_node_owned = 1;
    t->flags &= ~TR_BLANK;

    /* like composite groups the end of the group isn't shown */
    if (t->t_match != NULL) {
        t->t_match->flags |= TR_COLLAPSED;
    }

    GLOBALS->signalwindow_width_dirty = 1;
}

/* (Re)builds the composite waveform of the record group begin trace T.  */
void record_trace_build(GwTrace *t)
{
    if (!is_record_header(t)) {
        return;
    }

    GPtrArray *fields = record_get_fields(t);
    record_trace_build_with_fields(t, fields);
    g_ptr_array_free(fields, TRUE);
}

/* Frees the synthesized node of the record trace T.  */
void record_trace_free(GwTrace *t)
{
    if (t->record_node_owned) {
        record_node_free(t->n.nd);
        t->n.nd = NULL;
        t->record_node_owned = 0;
        t->flags |= TR_BLANK;
    }
}

/* Sets the nesting level of the traces in record trees: 1 for the top level
   record header, 2 for its fields and so on.  */
static void record_traces_update_levels(void)
{
    GPtrArray *ends = g_ptr_array_new();

    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        guint depth = ends->len;
        guint8 level;

        if (depth > 0 && t == g_ptr_array_index(ends, depth - 1)) {
            g_ptr_array_set_size(ends, depth - 1);
            level = MIN(depth, G_MAXUINT8);
        } else if (IsGroupBegin(t) && ((t->flags & TR_RECORD) || depth > 0)) {
            level = MIN(depth + 1, G_MAXUINT8);
            if (t->t_match != NULL) {
                g_ptr_array_add(ends, t->t_match);
            }
        } else {
            level = MIN(depth + (depth > 0 ? 1 : 0), G_MAXUINT8);
        }

        /* the indentation changes the width of the names */
        if (t->record_level != level) {
            t->record_level = level;
            GLOBALS->signalwindow_width_dirty = 1;
        }
    }

    g_ptr_array_free(ends, TRUE);
}

/* Rebuilds the records whose fields changed (format, filters, moved traces).  */
void record_traces_refresh(void)
{
    /* backwards, so that nested records are updated before their parents */
    for (GwTrace *t = GLOBALS->traces.last; t != NULL; t = t->t_prev) {
        if (!is_record_header(t)) {
            continue;
        }

        GPtrArray *fields = record_get_fields(t);
        if (record_signature(fields) != t->record_sig) {
            record_trace_build_with_fields(t, fields);
        }
        g_ptr_array_free(fields, TRUE);
    }

    record_traces_update_levels();
}

/* Flags to save for T: records are saved as blank group begins (or as a
   comment when the record was ungrouped), as the synthesized node can't be
   looked up by name.  */
TraceFlagsType record_trace_save_flags(GwTrace *t)
{
    if (is_record_header(t) || t->record_node_owned) {
        return t->flags | TR_BLANK;
    }
    return t->flags;
}
