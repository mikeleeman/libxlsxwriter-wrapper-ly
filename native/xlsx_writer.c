/*
 * xlsx_writer.c
 *
 * Reads a flat NDJSON command stream (one JSON object per line) and
 * writes it out as an .xlsx file using libxlsxwriter.
 *
 * Usage:
 *   xlsx_writer <input.ndjson> <output.xlsx>
 *
 * Exit codes:
 *   0  success
 *   1  usage / file open error
 *   2  malformed input line (bad JSON, unknown op, etc.)
 *   3  libxlsxwriter error (write/merge/close failure)
 *   4  input stream ended without a terminating {"op":"end"} line
 *
 * On any non-zero exit, a one-line human-readable reason is written to
 * stderr. The caller (Laravel) should treat any non-zero exit as failure
 * and leave the input NDJSON file in place for inspection.
 *
 * --- NDJSON command reference -------------------------------------
 *
 * Every line is a FLAT JSON object: {"op": "...", ...scalar fields}.
 * No nested objects. The only arrays allowed are flat arrays of
 * numbers (col widths, page-break rows). This is deliberate so the
 * parser below can stay a simple single-pass tokenizer instead of a
 * general recursive-descent JSON parser.
 *
 * {"op":"workbook","sheet":"Daily Report"}
 *     Must be the first line. Output path comes from argv[2], not
 *     from the stream, so the C binary is never trusted to choose
 *     where it writes on disk.
 *
 * {"op":"page_setup","orientation":"landscape","fit_w":1,"fit_h":1}
 *     orientation is "landscape" or "portrait" (default portrait if
 *     omitted). fit_w/fit_h map to worksheet_fit_to_pages().
 *
 * {"op":"col_widths","w":[8,15,8,11,...]}
 *     One width per column, 0-indexed, applied in order.
 *
 * {"op":"style","id":"standard_body","sz":7,"b":false,
 *   "ba":true,"bt":false,"br":false,"bl":false,"bb":false,
 *   "bc":"#000000","wrap":true,"align":"center","valign":"center"}
 *     Defines a named style. Must appear before any "cell" line that
 *     references it. "ba" (border-all) implies all four sides; the
 *     per-side bt/br/bl/bb flags are used when ba is false/omitted.
 *     align/valign: "center" | "left" | "right" (align only) |
 *     "top" | "center" | "bottom" (valign only). Omit for default.
 *
 * {"op":"row_height","row":12,"h":25}
 *     0-indexed row.
 *
 * {"op":"cell","r1":0,"c1":0,"r2":0,"c2":5,"v":"COMMUNITY NAME: Foo","s":"header"}
 *     Writes v into the r1,c1..r2,c2 range (0-indexed, inclusive).
 *     If the range covers more than one cell it is merged first, then
 *     the value is written into the top-left anchor cell (matches how
 *     the previous FastExcelWriter-based code merged ranges). "v" may
 *     be a JSON string or a JSON number; omit "v" (or use null) for an
 *     empty styled cell. "s" is optional; omit for no format.
 *
 * {"op":"page_breaks","rows":[40,81,122]}
 *     0-indexed row numbers, mapped to worksheet_set_h_pagebreaks().
 *     NOTE: verify these land in the same visual place as the old
 *     raw-XML <rowBreaks> output before relying on this in production
 *     — libxlsxwriter treats the break as occurring *after* the given
 *     row, which should match the existing getAccumulatedRowIndex()
 *     math, but confirm against a known-good export once.
 *
 * {"op":"end"}
 *     Must be the last line. Its absence is treated as a truncated
 *     file (worker died mid-write) and the whole run fails (exit 4)
 *     rather than silently producing a partial .xlsx.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

#include <xlsxwriter.h>

#define MAX_LINE        (1 << 20)   /* 1 MB per NDJSON line, generous */
#define MAX_FIELDS      32
#define MAX_KEY_LEN     16
#define MAX_STR_LEN     8192
#define MAX_STYLES      128
#define MAX_STYLE_ID_LEN 64
#define MAX_ARRAY_NUMS  4096

typedef enum { JV_STRING, JV_NUMBER, JV_BOOL, JV_NULL, JV_ARRAY } jval_type;

typedef struct {
    char key[MAX_KEY_LEN];
    jval_type type;
    char str[MAX_STR_LEN];   /* used when type == JV_STRING */
    double num;              /* used when type == JV_NUMBER */
    int boolean;             /* used when type == JV_BOOL */
    double arr[MAX_ARRAY_NUMS];
    int arr_len;              /* used when type == JV_ARRAY */
} jfield;

typedef struct {
    jfield fields[MAX_FIELDS];
    int nfields;
} jline;

typedef struct {
    char id[MAX_STYLE_ID_LEN];
    lxw_format *format;
} style_entry;

static style_entry g_styles[MAX_STYLES];
static int g_nstyles = 0;

/* pageOrientationLandscape()/pageFitToWidth()/pageFitToHeight() on the
 * PHP side each emit their OWN "page_setup" line (to mirror the old
 * FastExcelWriter call shape with minimal changes at the call site).
 * fit_w and fit_h therefore arrive on separate lines. worksheet_fit_to_
 * pages() takes both at once and isn't additive across calls, so calling
 * it immediately per-line meant the last call (fit_h, with no fit_w key,
 * defaulting to 0) silently overwrote an earlier fit_w=1 with 0 — real
 * bug, caught by inspecting actual output ("fitToWidth=\"0\"" in a real
 * generated file instead of the expected "1"). Fixed by accumulating
 * across every "page_setup" line seen and applying once, right before
 * workbook_close(). */
static int g_fit_w = 0;
static int g_fit_h = 0;
static int g_fit_pages_set = 0;

static int g_line_no = 0;

static void die(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "xlsx_writer: line %d: ", g_line_no);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(code);
}

/* ---------------------------------------------------------------- *
 * Minimal flat-JSON-object tokenizer.
 *
 * Only handles: {"key":"string"|number|true|false|null|[num,num,...]}
 * repeated, comma-separated. No nested objects, no arrays of
 * non-numbers, no \u escapes (only \" \\ \/ \n \t \r pass through).
 * This is intentionally not a general JSON parser — the NDJSON
 * contract above is flat by design so this stays simple and fast.
 * ---------------------------------------------------------------- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *parse_quoted_string(const char *p, char *out, size_t outsz) {
    size_t oi = 0;
    if (*p != '"') die(2, "expected '\"' at start of string");
    p++;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\') {
            p++;
            switch (*p) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default: c = *p; break; /* best-effort; no \u support */
            }
        }
        if (oi + 1 < outsz) out[oi++] = c;
        p++;
    }
    if (*p != '"') die(2, "unterminated string");
    p++;
    out[oi] = '\0';
    return p;
}

static const char *parse_key(const char *p, char *out, size_t outsz) {
    p = skip_ws(p);
    return parse_quoted_string(p, out, outsz);
}

/* Parses one JSON value into `f`. Returns pointer past the value. */
static const char *parse_value(const char *p, jfield *f) {
    p = skip_ws(p);
    if (*p == '"') {
        f->type = JV_STRING;
        p = parse_quoted_string(p, f->str, sizeof(f->str));
    } else if (*p == '[') {
        f->type = JV_ARRAY;
        f->arr_len = 0;
        p++;
        p = skip_ws(p);
        while (*p != ']') {
            if (f->arr_len >= MAX_ARRAY_NUMS) die(2, "array too long (max %d)", MAX_ARRAY_NUMS);
            char *endptr;
            f->arr[f->arr_len++] = strtod(p, &endptr);
            if (endptr == p) die(2, "expected number in array");
            p = endptr;
            p = skip_ws(p);
            if (*p == ',') { p++; p = skip_ws(p); }
        }
        p++; /* skip ']' */
    } else if (*p == 't' && strncmp(p, "true", 4) == 0) {
        f->type = JV_BOOL; f->boolean = 1; p += 4;
    } else if (*p == 'f' && strncmp(p, "false", 5) == 0) {
        f->type = JV_BOOL; f->boolean = 0; p += 5;
    } else if (*p == 'n' && strncmp(p, "null", 4) == 0) {
        f->type = JV_NULL; p += 4;
    } else if (*p == '-' || isdigit((unsigned char)*p)) {
        char *endptr;
        f->type = JV_NUMBER;
        f->num = strtod(p, &endptr);
        if (endptr == p) die(2, "invalid number");
        p = endptr;
    } else {
        die(2, "unexpected character '%c' while parsing value", *p);
    }
    return p;
}

static void parse_line(const char *line, jline *out) {
    out->nfields = 0;
    const char *p = skip_ws(line);
    if (*p == '\0') return; /* blank line, treated as no-op by caller */
    if (*p != '{') die(2, "expected '{' at start of line");
    p++;
    p = skip_ws(p);
    if (*p == '}') return; /* empty object */
    for (;;) {
        if (out->nfields >= MAX_FIELDS) die(2, "too many fields on one line (max %d)", MAX_FIELDS);
        jfield *f = &out->fields[out->nfields];
        p = parse_key(p, f->key, sizeof(f->key));
        p = skip_ws(p);
        if (*p != ':') die(2, "expected ':' after key '%s'", f->key);
        p++;
        p = parse_value(p, f);
        out->nfields++;
        p = skip_ws(p);
        if (*p == ',') { p++; p = skip_ws(p); continue; }
        if (*p == '}') { p++; break; }
        die(2, "expected ',' or '}' after value for key '%s'", f->key);
    }
}

static jfield *find_field(jline *jl, const char *key) {
    for (int i = 0; i < jl->nfields; i++) {
        if (strcmp(jl->fields[i].key, key) == 0) return &jl->fields[i];
    }
    return NULL;
}

static const char *field_str(jline *jl, const char *key, const char *dflt) {
    jfield *f = find_field(jl, key);
    if (!f || f->type != JV_STRING) return dflt;
    return f->str;
}

static double field_num(jline *jl, const char *key, double dflt) {
    jfield *f = find_field(jl, key);
    if (!f || f->type != JV_NUMBER) return dflt;
    return f->num;
}

static int field_bool(jline *jl, const char *key, int dflt) {
    jfield *f = find_field(jl, key);
    if (!f || f->type != JV_BOOL) return dflt;
    return f->boolean;
}

/* ---------------------------------------------------------------- *
 * Style registry
 * ---------------------------------------------------------------- */

static lxw_color_t parse_hex_color(const char *s) {
    if (!s) return 0x000000;
    if (s[0] == '#') s++;
    return (lxw_color_t) strtol(s, NULL, 16);
}

static void handle_style(lxw_workbook *wb, jline *jl) {
    const char *id = field_str(jl, "id", NULL);
    if (!id) die(2, "\"style\" op missing \"id\"");
    if (g_nstyles >= MAX_STYLES) die(2, "too many styles (max %d)", MAX_STYLES);

    lxw_format *fmt = workbook_add_format(wb);

    double sz = field_num(jl, "sz", 0);
    if (sz > 0) format_set_font_size(fmt, sz);

    if (field_bool(jl, "b", 0)) format_set_bold(fmt);
    if (field_bool(jl, "wrap", 0)) format_set_text_wrap(fmt);

    const char *align = field_str(jl, "align", NULL);
    if (align) {
        if (strcmp(align, "center") == 0) format_set_align(fmt, LXW_ALIGN_CENTER);
        else if (strcmp(align, "left") == 0) format_set_align(fmt, LXW_ALIGN_LEFT);
        else if (strcmp(align, "right") == 0) format_set_align(fmt, LXW_ALIGN_RIGHT);
    }
    const char *valign = field_str(jl, "valign", NULL);
    if (valign) {
        if (strcmp(valign, "center") == 0) format_set_align(fmt, LXW_ALIGN_VERTICAL_CENTER);
        else if (strcmp(valign, "top") == 0) format_set_align(fmt, LXW_ALIGN_VERTICAL_TOP);
        else if (strcmp(valign, "bottom") == 0) format_set_align(fmt, LXW_ALIGN_VERTICAL_BOTTOM);
    }

    lxw_color_t bc = parse_hex_color(field_str(jl, "bc", "#000000"));

    if (field_bool(jl, "ba", 0)) {
        format_set_border(fmt, LXW_BORDER_THIN);
        format_set_border_color(fmt, bc);
    } else {
        if (field_bool(jl, "bt", 0)) { format_set_top(fmt, LXW_BORDER_THIN); format_set_top_color(fmt, bc); }
        if (field_bool(jl, "br", 0)) { format_set_right(fmt, LXW_BORDER_THIN); format_set_right_color(fmt, bc); }
        if (field_bool(jl, "bl", 0)) { format_set_left(fmt, LXW_BORDER_THIN); format_set_left_color(fmt, bc); }
        if (field_bool(jl, "bb", 0)) { format_set_bottom(fmt, LXW_BORDER_THIN); format_set_bottom_color(fmt, bc); }
    }

    strncpy(g_styles[g_nstyles].id, id, MAX_STYLE_ID_LEN - 1);
    g_styles[g_nstyles].id[MAX_STYLE_ID_LEN - 1] = '\0';
    g_styles[g_nstyles].format = fmt;
    g_nstyles++;
}

static lxw_format *lookup_style(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < g_nstyles; i++) {
        if (strcmp(g_styles[i].id, id) == 0) return g_styles[i].format;
    }
    die(2, "unknown style id \"%s\" (must be defined before use)", id);
    return NULL; /* unreachable */
}

/* ---------------------------------------------------------------- *
 * Main
 * ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.ndjson> <output.xlsx>\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];

    FILE *in = fopen(in_path, "r");
    if (!in) { fprintf(stderr, "xlsx_writer: cannot open input %s\n", in_path); return 1; }

    lxw_workbook *wb = NULL;
    lxw_worksheet *ws = NULL;

    char *line = malloc(MAX_LINE);
    if (!line) { fprintf(stderr, "xlsx_writer: out of memory\n"); return 1; }

    int saw_workbook = 0;
    int saw_end = 0;

    while (fgets(line, MAX_LINE, in)) {
        g_line_no++;

        /* strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue; /* skip blank lines */

        jline jl;
        parse_line(line, &jl);
        if (jl.nfields == 0) continue;

        const char *op = field_str(&jl, "op", NULL);
        if (!op) die(2, "line missing \"op\" field");

        if (strcmp(op, "workbook") == 0) {
            if (saw_workbook) die(2, "duplicate \"workbook\" op");
            wb = workbook_new(out_path);
            if (!wb) die(3, "workbook_new() failed for %s", out_path);
            const char *sheet = field_str(&jl, "sheet", "Sheet1");
            ws = workbook_add_worksheet(wb, sheet);
            if (!ws) die(3, "workbook_add_worksheet() failed");
            saw_workbook = 1;

        } else if (strcmp(op, "page_setup") == 0) {
            if (!ws) die(2, "\"page_setup\" before \"workbook\"");
            const char *orientation = field_str(&jl, "orientation", "portrait");
            if (strcmp(orientation, "landscape") == 0) worksheet_set_landscape(ws);
            /* Only update fit_w/fit_h if THIS line actually specifies
               them — accumulate across calls rather than resetting
               unset fields back to 0 (see comment on the globals above). */
            jfield *fw = find_field(&jl, "fit_w");
            jfield *fh = find_field(&jl, "fit_h");
            if (fw && fw->type == JV_NUMBER) { g_fit_w = (int) fw->num; g_fit_pages_set = 1; }
            if (fh && fh->type == JV_NUMBER) { g_fit_h = (int) fh->num; g_fit_pages_set = 1; }

        } else if (strcmp(op, "col_widths") == 0) {
            if (!ws) die(2, "\"col_widths\" before \"workbook\"");
            jfield *w = find_field(&jl, "w");
            if (!w || w->type != JV_ARRAY) die(2, "\"col_widths\" missing \"w\" array");
            for (int c = 0; c < w->arr_len; c++) {
                worksheet_set_column(ws, c, c, w->arr[c], NULL);
            }

        } else if (strcmp(op, "style") == 0) {
            if (!wb) die(2, "\"style\" before \"workbook\"");
            handle_style(wb, &jl);

        } else if (strcmp(op, "row_height") == 0) {
            if (!ws) die(2, "\"row_height\" before \"workbook\"");
            int row = (int) field_num(&jl, "row", -1);
            double h = field_num(&jl, "h", -1);
            if (row < 0 || h < 0) die(2, "\"row_height\" needs \"row\" and \"h\"");
            worksheet_set_row(ws, (lxw_row_t) row, h, NULL);

        } else if (strcmp(op, "cell") == 0) {
            if (!ws) die(2, "\"cell\" before \"workbook\"");
            int r1 = (int) field_num(&jl, "r1", -1);
            int c1 = (int) field_num(&jl, "c1", -1);
            int r2 = (int) field_num(&jl, "r2", r1);
            int c2 = (int) field_num(&jl, "c2", c1);
            if (r1 < 0 || c1 < 0) die(2, "\"cell\" needs \"r1\" and \"c1\"");

            const char *style_id = field_str(&jl, "s", NULL);
            lxw_format *fmt = style_id ? lookup_style(style_id) : NULL;

            int merged = (r1 != r2 || c1 != c2);
            if (merged) {
                lxw_error err = worksheet_merge_range(ws, r1, c1, r2, c2, "", fmt);
                if (err != LXW_NO_ERROR) die(3, "merge_range failed: %s", lxw_strerror(err));
            }

            jfield *v = find_field(&jl, "v");
            if (v && v->type == JV_STRING) {
                worksheet_write_string(ws, r1, c1, v->str, fmt);
            } else if (v && v->type == JV_NUMBER) {
                worksheet_write_number(ws, r1, c1, v->num, fmt);
            } else if (merged && fmt) {
                /* No value but merged+styled: format was already applied
                   across the range by merge_range; nothing more to write. */
            } else if (!merged && fmt) {
                /* Single empty styled cell — merge_range wasn't called, so
                   write a blank to actually apply the format. */
                worksheet_write_blank(ws, r1, c1, fmt);
            }

        } else if (strcmp(op, "page_breaks") == 0) {
            if (!ws) die(2, "\"page_breaks\" before \"workbook\"");
            jfield *rows = find_field(&jl, "rows");
            if (!rows || rows->type != JV_ARRAY) die(2, "\"page_breaks\" missing \"rows\" array");
            if (rows->arr_len > 0) {
                lxw_row_t *breaks = calloc(rows->arr_len + 1, sizeof(lxw_row_t));
                if (!breaks) die(1, "out of memory building page breaks");
                for (int i = 0; i < rows->arr_len; i++) breaks[i] = (lxw_row_t) rows->arr[i];
                breaks[rows->arr_len] = 0; /* 0-terminated per libxlsxwriter convention */
                lxw_error err = worksheet_set_h_pagebreaks(ws, breaks);
                free(breaks);
                if (err != LXW_NO_ERROR) die(3, "set_h_pagebreaks failed: %s", lxw_strerror(err));
            }

        } else if (strcmp(op, "end") == 0) {
            saw_end = 1;
            break;

        } else {
            die(2, "unknown op \"%s\"", op);
        }
    }

    fclose(in);
    free(line);

    if (!saw_workbook) die(2, "input stream had no \"workbook\" op");
    if (!saw_end) {
        /* Truncated input — likely the producer (Laravel) died mid-write.
           Refuse to emit a partial file as if it were a finished report. */
        if (wb) workbook_close(wb); /* still free libxlsxwriter's memory */
        fprintf(stderr, "xlsx_writer: input ended without terminating {\"op\":\"end\"} — truncated stream\n");
        return 4;
    }

    if (g_fit_pages_set) {
        worksheet_fit_to_pages(ws, g_fit_w, g_fit_h); /* void return */
    }

    lxw_error err = workbook_close(wb);
    if (err != LXW_NO_ERROR) {
        fprintf(stderr, "xlsx_writer: workbook_close failed: %s\n", lxw_strerror(err));
        return 3;
    }

    return 0;
}
