/*
 * TermmiK
 * Copyright (C) 2026 SfymmiK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Minimal font lookup — replaces the fontconfig dependency.
 *
 * TermmiK needs exactly two things from fontconfig: turn a family name into
 * a list of font files, and supply fallback faces for glyphs the primary
 * font lacks. Fontconfig is a full font-matching engine with its own XML
 * configuration language, caches and a FreeType dependency — far more than
 * needed. This module does the two needed things directly and with no extra
 * libraries:
 *
 *   1. scan the standard font directories (XDG + system),
 *   2. read each face's name/OS-2/post tables with tiny zero-copy reads,
 *   3. score faces against the requested family/style,
 *   4. return the best matches first, then the useful fallback faces.
 */

#include "font_list.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SCAN_FACES 4096
#define MAX_SCAN_DEPTH 6

typedef struct {
    FontMatch m;
    int order;
} Candidate;

static uint16_t rd16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int is_font_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    dot++;
    return (strcasecmp(dot, "ttf") == 0 || strcasecmp(dot, "ttc") == 0 ||
            strcasecmp(dot, "otf") == 0 || strcasecmp(dot, "otc") == 0);
}

// Decode a name-table string into UTF-8. Windows records are UTF-16BE, Mac
// records are byte strings (usually ASCII/Latin-1).
static void decode_name(const unsigned char *src, int len, int utf16, char *out, size_t cap) {
    size_t o = 0;
    if (utf16) {
        for (int i = 0; i + 1 < len && o + 4 < cap; i += 2) {
            uint32_t cp = (uint32_t)(src[i] << 8) | src[i + 1];
            if (cp == 0) break;
            if (cp < 0x80) {
                out[o++] = (char)cp;
            } else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
        }
    } else {
        for (int i = 0; i < len && o + 1 < cap; i++) {
            unsigned char c = src[i];
            if (c == 0) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

// Locate a table record (tag) in a face. Returns 0 when absent.
static int find_table(const unsigned char *buf, size_t len, size_t face_off,
                      const char *tag, const unsigned char **out, size_t *out_len) {
    if (face_off + 12 > len) return 0;
    int num = rd16(buf + face_off + 4);
    if (num < 0 || (size_t)num > (len - face_off - 12) / 16) {
        num = (int)((len - face_off - 12) / 16);
    }
    for (int t = 0; t < num; t++) {
        const unsigned char *rec = buf + face_off + 12 + (size_t)t * 16;
        if (memcmp(rec, tag, 4) == 0) {
            size_t off = rd32(rec + 8);
            size_t tlen = rd32(rec + 12);
            if (off >= len) continue;
            if (tlen > len - off) tlen = len - off;
            *out = buf + off;
            *out_len = tlen;
            return 1;
        }
    }
    return 0;
}

// Read family (nameID 1/16), style (nameID 2) and the monospace hints from
// one face of a font file. Returns 0 when the face has no usable name table.
static int face_names(const unsigned char *buf, size_t len, int face,
                      char *family, size_t fam_cap, char *style, size_t style_cap,
                      int *monospace, int *outlines) {
    size_t face_off = 0;
    if (len >= 16 && memcmp(buf, "ttcf", 4) == 0) {
        uint32_t faces = rd32(buf + 8);
        if (face < 0 || (uint32_t)face >= faces || 12 + 4u * face + 4 > len) return 0;
        face_off = rd32(buf + 12 + 4u * face);
    } else if (face != 0) {
        return 0;
    }
    if (face_off + 12 > len) return 0;

    family[0] = '\0';
    style[0] = '\0';
    *monospace = 0;

    const unsigned char *name = NULL, *os2 = NULL, *post = NULL;
    const unsigned char *tmp_tbl = NULL;
    size_t name_len = 0, os2_len = 0, post_len = 0, tmp_len = 0;
    find_table(buf, len, face_off, "name", &name, &name_len);
    find_table(buf, len, face_off, "OS/2", &os2, &os2_len);
    find_table(buf, len, face_off, "post", &post, &post_len);

    // Bitmap-only faces (color emoji, CBDT/CBLC) have no outline table and
    // can never be rasterized by stb_truetype, so they are not candidates.
    if (outlines) {
        *outlines = find_table(buf, len, face_off, "glyf", &tmp_tbl, &tmp_len) ||
                    find_table(buf, len, face_off, "CFF ", &tmp_tbl, &tmp_len) ||
                    find_table(buf, len, face_off, "CFF2", &tmp_tbl, &tmp_len);
    }

    if (post && post_len >= 16) {
        if (rd32(post + 12) != 0) *monospace = 1; // isFixedPitch
    }
    if (os2 && os2_len >= 42) {
        if (os2[32 + 3] == 9) *monospace = 1; // panose proportion=monospaced
    }

    if (name && name_len >= 6) {
        int count = rd16(name + 2);
        int str_off = rd16(name + 4);
        if (count < 0 || (size_t)(6 + 12 * count) > name_len) {
            count = (int)((name_len - 6) / 12);
        }
        int fam_best = -1, sty_best = -1;
        for (int i = 0; i < count; i++) {
            const unsigned char *rec = name + 6 + (size_t)i * 12;
            int platform = rd16(rec);
            int encoding = rd16(rec + 2);
            int lang = rd16(rec + 4);
            int name_id = rd16(rec + 6);
            int nlen = rd16(rec + 8);
            int noff = rd16(rec + 10);
            if (name_id != 1 && name_id != 16 && name_id != 2) continue;
            if ((size_t)str_off + noff + nlen > name_len) continue;
            int utf16 = (platform == 3 && encoding == 1);
            // Preference: Windows English > Windows > Mac Roman > anything.
            int pref = 0;
            if (platform == 3 && lang == 0x409) pref = 4;
            else if (platform == 3) pref = 3;
            else if (platform == 1 && lang == 0) pref = 2;
            else if (platform == 1) pref = 1;
            if (pref == 0) continue;
            const unsigned char *s = name + str_off + noff;
            if (name_id == 2) {
                if (pref > sty_best) { sty_best = pref; decode_name(s, nlen, utf16, style, style_cap); }
            } else {
                if (pref > fam_best) { fam_best = pref; decode_name(s, nlen, utf16, family, fam_cap); }
            }
        }
    }
    return family[0] != '\0';
}

// --------------------------------------------------------------------------
// Scoring
// --------------------------------------------------------------------------

// Lowercase and drop everything that is not a letter or digit, so
// "Noto Sans Mono", "NotoSansMono" and "noto-sans-mono" all compare equal.
static void normalize(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
    }
    out[o] = '\0';
}

// Strip trailing style words from a normalized family so "Noto Sans Mono
// Light" dedupes against "Noto Sans Mono" (the same coverage).
static void family_root(const char *norm, char *out, size_t cap) {
    static const char *styles[] = {
        "black", "extrabold", "semibold", "demibold", "ultrabold", "heavy",
        "extrablack", "ultralight", "extralight", "semilight", "demilight",
        "light", "thin", "medium", "book", "roman", "regular", "normal",
        "bolditalic", "boldoblique", "bold", "italic", "oblique", "condensed",
        // Abbreviations used in font names ("Noto Sans Arabic Blk", ...).
        "blk", "med", "lt", "th", "bd", "rg", "md", "ext", "obl", "demi", "semi",
        // Nerd Font packaging variants share one glyph set.
        "nerdfontmono", "nerdfontpropo", "nerdfont",
    };
    size_t len = strlen(norm);
    if (len >= cap) len = cap - 1;
    memcpy(out, norm, len);
    out[len] = '\0';
    int changed = 1;
    while (changed && len > 0) {
        changed = 0;
        for (unsigned i = 0; i < sizeof(styles) / sizeof(*styles); i++) {
            size_t sl = strlen(styles[i]);
            if (len > sl && strcmp(out + len - sl, styles[i]) == 0) {
                len -= sl;
                out[len] = '\0';
                changed = 1;
                break;
            }
        }
    }
    if (len == 0) { // a family that is nothing but a style word
        snprintf(out, cap, "%s", norm);
    }
}

static int str_contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

// Well-known terminal fonts, in the order they should be preferred when the
// request is generic ("monospace") or when the requested family is not
// installed. Keeps the default stable across systems instead of depending on
// directory scan order.
static const char *known_mono[] = {
    // Common distro defaults first (so "monospace" resolves like the system
    // would), then popular installable terminal fonts, then the rest.
    "notosansmono", "dejavusansmono", "liberationmono", "ubuntumono",
    "jetbrainsmono", "firacode", "firamono", "hack", "cascadiacode",
    "cascadiamono", "sourcecodepro", "adwaitamono", "bitstreamverasansmono",
};

static int known_mono_rank(const char *face_family) {
    char n[96];
    normalize(face_family, n, sizeof(n));
    for (unsigned i = 0; i < sizeof(known_mono) / sizeof(*known_mono); i++) {
        if (strcmp(n, known_mono[i]) == 0) return (int)i;
    }
    return -1;
}

static int fallback_bonus(const char *family);

// Fonts that add glyph coverage a terminal actually needs (icons, CJK,
// symbols). Promoting them inside the generic-match ranking means a missing
// CJK/icon glyph is found after a couple of font loads instead of walking
// every unrelated monospace face first.
static int coverage_bonus(const char *family) {
    char n[96];
    normalize(family, n, sizeof(n));
    if (str_contains(n, "unifont")) return 130;
    if (str_contains(n, "nerdfont")) return 120;
    if (str_contains(n, "powerline") || str_contains(n, "symbol")) return 110;
    if (str_contains(n, "emoji")) return 105;
    if (str_contains(n, "cjk") || str_contains(n, "hans") || str_contains(n, "hant") ||
        str_contains(n, "japanese") || str_contains(n, "korean") ||
        str_contains(n, "notosansjp") || str_contains(n, "notosanskr") ||
        str_contains(n, "notosanssc") || str_contains(n, "notosanstc")) return 100;
    return 0;
}

// How well a face fits "family:options". Higher is better, 0 = fallback.
static int score_face(const char *family_key, const char *opts, int want_bold,
                      int want_italic, int want_regular,
                      const char *face_family, const char *face_style, int mono) {
    char nfam[96], nface[96], nstyle[64];
    normalize(family_key, nfam, sizeof(nfam));
    normalize(face_family, nface, sizeof(nface));
    normalize(face_style, nstyle, sizeof(nstyle));

    int match = 0;
    int generic = 0;
    int mono_generic = 0;
    if (nfam[0] == '\0' || strcmp(nfam, "monospace") == 0 || strcmp(nfam, "mono") == 0 ||
        strcmp(nfam, "fixed") == 0) {
        generic = 1;
        mono_generic = 1;
        match = mono ? 800 : 0;
    } else if (strcmp(nfam, "sansserif") == 0 || strcmp(nfam, "sans") == 0) {
        generic = 1;
        if (str_contains(nface, "sans") && !str_contains(nface, "serif")) match = 500;
        if (mono) match -= 50; // prefer a proportional face when asked for one
    } else if (strcmp(nfam, "serif") == 0) {
        generic = 1;
        if (str_contains(nface, "serif") && !str_contains(nface, "sans")) match = 480;
        if (mono) match -= 50;
    } else if (strcmp(nface, nfam) == 0) {
        match = 1000;
    } else if (str_contains(nface, nfam) || str_contains(nfam, nface)) {
        match = 650;
    } else {
        // Family lists: "First, Second" — any later entry is a weaker match.
        const char *comma = strchr(family_key, ',');
        while (comma) {
            char sub[96];
            const char *nxt = strchr(comma + 1, ',');
            size_t len = nxt ? (size_t)(nxt - comma - 1) : strlen(comma + 1);
            if (len >= sizeof(sub)) len = sizeof(sub) - 1;
            memcpy(sub, comma + 1, len);
            sub[len] = '\0';
            char nsub[96];
            normalize(sub, nsub, sizeof(nsub));
            if (nsub[0] && strcmp(nface, nsub) == 0) { match = 550; break; }
            comma = nxt;
        }
    }

    int face_bold = str_contains(nstyle, "bold") || str_contains(nstyle, "heavy") ||
                    str_contains(nstyle, "black") || str_contains(nstyle, "semibold");
    int face_italic = str_contains(nstyle, "italic") || str_contains(nstyle, "oblique");
    int face_regular = !face_bold && !face_italic;

    // Families that are a weight variant of another family ("... Black",
    // "... Light", "... Thin" — common in the Noto families) sort after the
    // base family, so dedupe keeps the regular face when both are installed.
    char fam_root[96];
    family_root(nface, fam_root, sizeof(fam_root));
    int weight_variant = strcmp(fam_root, nface) != 0;

    if (match == 0) {
        // Nothing about the family matched. Well-known terminal fonts are
        // promoted so a missing family still lands on a usable font; the
        // rest of the list is ordered for glyph coverage.
        int score = fallback_bonus(face_family);
        if (mono) {
            int rank = known_mono_rank(face_family);
            if (rank >= 0) score += 300 - rank;
        }
        if (want_regular && face_regular) score += 20;
        if (want_regular && face_bold) score -= 30;
        if (want_regular && face_italic) score -= 20;
        if (want_bold && face_bold) score += 20;
        if (want_italic && face_italic) score += 20;
        if (weight_variant) score -= 10;
        return score;
    }

    // Among generic monospace matches prefer the well-known terminal fonts,
    // then coverage fonts — a real family match always wins over both.
    if (mono_generic && mono) {
        int rank = known_mono_rank(face_family);
        if (rank >= 0) match += 300 - rank;
        else match += coverage_bonus(face_family);
    }

    if (want_bold && face_bold) match += 30;
    if (want_italic && face_italic) match += 30;
    if (want_regular && face_regular) match += 20;
    if (want_regular && face_bold) match -= 15;
    if (want_regular && face_italic) match -= 10;
    if (generic && face_regular) match += 5;
    if (weight_variant) match -= 10;
    if (mono) match += 2; // tiebreaker: terminals want fixed widths
    (void)opts;
    return match;
}

// Fallback faces are appended after real matches; bias them toward fonts
// that typically cover the glyphs a terminal is missing (the primary font
// already covers Latin, so scripts and icon fonts matter more than yet
// another Latin face) since glyph lookup walks this list in order.
static int fallback_bonus(const char *family) {
    char n[96];
    normalize(family, n, sizeof(n));
    if (str_contains(n, "unifont")) return 80;
    if (str_contains(n, "emoji")) return 70;
    if (str_contains(n, "symbols") || str_contains(n, "symbol")) return 60;
    if (str_contains(n, "cjk") || str_contains(n, "hans") || str_contains(n, "hant") ||
        str_contains(n, "japanese") || str_contains(n, "korean") ||
        str_contains(n, "notosansjp") || str_contains(n, "notosanskr") ||
        str_contains(n, "notosanssc") || str_contains(n, "notosanstc")) return 55;
    if (str_contains(n, "arab") || str_contains(n, "hebrew") || str_contains(n, "devanagari") ||
        str_contains(n, "thai") || str_contains(n, "hangul") || str_contains(n, "armenian") ||
        str_contains(n, "georgian") || str_contains(n, "ethiopic") || str_contains(n, "khmer") ||
        str_contains(n, "myanmar") || str_contains(n, "sinhala") || str_contains(n, "tamil") ||
        str_contains(n, "telugu") || str_contains(n, "kannada") || str_contains(n, "malayalam") ||
        str_contains(n, "bengali") || str_contains(n, "gujarati") || str_contains(n, "gurmukhi") ||
        str_contains(n, "oriya") || str_contains(n, "syriac") || str_contains(n, "tibetan") ||
        str_contains(n, "mongolian") || str_contains(n, "cherokee") || str_contains(n, "javanese") ||
        str_contains(n, "balinese") || str_contains(n, "adlam")) return 45;
    if (str_contains(n, "notosans") || str_contains(n, "dejavusans") ||
        str_contains(n, "notoserif") || str_contains(n, "dejavuserif")) return 30;
    if (str_contains(n, "sans")) return 10;
    return 0;
}

// --------------------------------------------------------------------------
// Directory scanning
// --------------------------------------------------------------------------

typedef struct {
    const char *family_key;
    const char *opts;
    int want_bold, want_italic, want_regular;
    Candidate *cands;
    int count;
    int max;
    int visited_dirs;
} ScanCtx;

static void scan_file(ScanCtx *ctx, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 4) { close(fd); return; }
    size_t len = (size_t)st.st_size;
    const unsigned char *buf = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) return;

    int faces = 1;
    if (len >= 16 && memcmp(buf, "ttcf", 4) == 0) {
        uint32_t n = rd32(buf + 8);
        faces = (n > 32) ? 32 : (int)n; // sanity cap for collections
    }

    char family[96], style[64];
    int mono, outlines;
    for (int f = 0; f < faces && ctx->count < ctx->max; f++) {
        if (!face_names(buf, len, f, family, sizeof(family), style, sizeof(style), &mono, &outlines)) continue;
        if (!outlines) continue; // unrenderable bitmap face (color emoji etc.)
        int score = score_face(ctx->family_key, ctx->opts, ctx->want_bold,
                               ctx->want_italic, ctx->want_regular, family, style, mono);
        if (score == 0) score = fallback_bonus(family); // fallback ordering only
        Candidate *c = &ctx->cands[ctx->count++];
        c->m.score = score;
        c->order = ctx->count;
        c->m.face = f;
        c->m.monospace = mono;
        c->m.outlines = outlines;
        snprintf(c->m.path, sizeof(c->m.path), "%s", path);
        snprintf(c->m.family, sizeof(c->m.family), "%s", family);
        snprintf(c->m.style, sizeof(c->m.style), "%s", style);
    }
    munmap((void *)buf, len);
}

static void scan_dir(ScanCtx *ctx, const char *dir, int depth) {
    if (depth > MAX_SCAN_DEPTH || ctx->count >= ctx->max) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    char path[512];
    while ((ent = readdir(d)) != NULL && ctx->count < ctx->max) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(ctx, path, depth + 1);
        } else if (S_ISREG(st.st_mode) && is_font_ext(ent->d_name)) {
            scan_file(ctx, path);
        }
    }
    closedir(d);
}

static int cand_cmp(const void *a, const void *b) {
    const Candidate *ca = a, *cb = b;
    if (ca->m.score != cb->m.score) return cb->m.score - ca->m.score;
    if (ca->m.monospace != cb->m.monospace) return cb->m.monospace - ca->m.monospace;
    return ca->order - cb->order;
}

// Add a directory to the scan list (deduplicated).
static void add_dir(char dirs[][512], int *n, int max, const char *dir) {
    if (!dir || !*dir || *n >= max) return;
    for (int i = 0; i < *n; i++) {
        if (strcmp(dirs[i], dir) == 0) return;
    }
    snprintf(dirs[*n], 512, "%s", dir);
    (*n)++;
}

// --------------------------------------------------------------------------
// Public entry point
// --------------------------------------------------------------------------

int font_list_match(const char *pattern, FontMatch *out, int max) {
    if (!pattern || !out || max <= 0) return 0;

    // Split "Family:option=value:..." — only the family and the style hints
    // are interpreted; other fontconfig keys are ignored.
    char family_key[128];
    const char *colon = strchr(pattern, ':');
    size_t fam_len = colon ? (size_t)(colon - pattern) : strlen(pattern);
    if (fam_len >= sizeof(family_key)) fam_len = sizeof(family_key) - 1;
    memcpy(family_key, pattern, fam_len);
    family_key[fam_len] = '\0';

    // Trim whitespace.
    char *fam = family_key;
    while (*fam == ' ' || *fam == '\t') fam++;
    size_t fl = strlen(fam);
    while (fl > 0 && (fam[fl - 1] == ' ' || fam[fl - 1] == '\t')) fam[--fl] = '\0';

    const char *opts = colon ? colon + 1 : "";
    int has_bold = 0, has_italic = 0;
    {
        char nopts[256];
        normalize(opts, nopts, sizeof(nopts));
        has_bold = str_contains(nopts, "bold") || str_contains(nopts, "weight=2") ||
                   str_contains(nopts, "weight=bold") || str_contains(nopts, "heavy");
        has_italic = str_contains(nopts, "italic") || str_contains(nopts, "oblique") ||
                     str_contains(nopts, "slant=2");
    }

    // An explicit file path is used as-is (the renderer maps it directly).
    if (fam[0] == '/' ) {
        struct stat st;
        if (stat(fam, &st) == 0 && S_ISREG(st.st_mode)) {
            out[0].face = 0;
            out[0].score = 1000;
            out[0].monospace = 0;
            out[0].outlines = 1; // explicit path: let the loader try it
            snprintf(out[0].path, sizeof(out[0].path), "%s", fam);
            char style[64];
            int mono, outlines;
            // Best effort: read the family for display/fallback purposes.
            int fd = open(fam, O_RDONLY);
            if (fd >= 0) {
                const unsigned char *buf = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (buf != MAP_FAILED) {
                    face_names(buf, (size_t)st.st_size, 0, out[0].family, sizeof(out[0].family),
                               style, sizeof(style), &mono, &outlines);
                    out[0].monospace = mono;
                    out[0].outlines = outlines;
                    munmap((void *)buf, (size_t)st.st_size);
                }
            }
            if (!out[0].family[0]) snprintf(out[0].family, sizeof(out[0].family), "Custom");
            if (!out[0].style[0]) snprintf(out[0].style, sizeof(out[0].style), "Regular");
            return 1;
        }
    }

    // Font directories: user fonts first, then system-wide ones.
    char dirs[32][512];
    int ndirs = 0;
    char path[512];
    const char *home = getenv("HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && *xdg_data) {
        snprintf(path, sizeof(path), "%s/fonts", xdg_data);
        add_dir(dirs, &ndirs, 32, path);
    } else if (home && *home) {
        snprintf(path, sizeof(path), "%s/.local/share/fonts", home);
        add_dir(dirs, &ndirs, 32, path);
    }
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.fonts", home);
        add_dir(dirs, &ndirs, 32, path);
    }
    add_dir(dirs, &ndirs, 32, "/usr/local/share/fonts");
    add_dir(dirs, &ndirs, 32, "/usr/share/fonts");
    const char *xdg_dirs = getenv("XDG_DATA_DIRS");
    if (!xdg_dirs || !*xdg_dirs) xdg_dirs = "/usr/local/share:/usr/share";
    {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", xdg_dirs);
        char *save = NULL;
        for (char *d = strtok_r(tmp, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            snprintf(path, sizeof(path), "%s/fonts", d);
            add_dir(dirs, &ndirs, 32, path);
        }
    }

    Candidate *cands = calloc(MAX_SCAN_FACES, sizeof(*cands));
    if (!cands) return 0;

    ScanCtx ctx = {
        .family_key = fam,
        .opts = opts,
        .want_bold = has_bold,
        .want_italic = has_italic,
        .want_regular = !has_bold && !has_italic,
        .cands = cands,
        .count = 0,
        .max = MAX_SCAN_FACES,
    };
    for (int i = 0; i < ndirs && ctx.count < MAX_SCAN_FACES; i++) {
        scan_dir(&ctx, dirs[i], 0);
    }

    qsort(cands, (size_t)ctx.count, sizeof(*cands), cand_cmp);

    // One face per family: the renderer only uses the extra fonts for glyphs
    // the primary lacks, so Bold/Italic/Light variants of a family add no
    // coverage — deduplicating leaves room for emoji/CJK/symbol faces.
    char *seen = calloc((size_t)max, 96);
    int n = 0;
    for (int i = 0; i < ctx.count && n < max; i++) {
        char nrm[96], nf[96];
        normalize(cands[i].m.family, nrm, sizeof(nrm));
        family_root(nrm, nf, sizeof(nf));
        int dup = 0;
        for (int k = 0; k < n; k++) {
            if (strcmp(seen + (size_t)k * 96, nf) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        if (seen) memcpy(seen + (size_t)n * 96, nf, sizeof(nf));
        out[n++] = cands[i].m;
    }
    free(seen);
    free(cands);
    return n;
}
