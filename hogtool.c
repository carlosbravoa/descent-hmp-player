/*
 * hogtool - HOG archive utility for Descent 1 & 2 on Linux
 *
 * HOG format (from DXX-Rebirth source and original Descent):
 *   [3 bytes]  magic: "DHF"
 *   Repeated until EOF:
 *     [13 bytes] filename (null-padded, max 12 chars + null)
 *     [4 bytes]  file length (little-endian uint32)
 *     [N bytes]  file data
 *
 * Usage:
 *   hogtool list    <archive.hog>
 *   hogtool extract <archive.hog> [-o outdir] [file1 file2 ...]
 *   hogtool create  <archive.hog> <file1> [file2 ...]
 *   hogtool help
 *
 * Build:
 *   gcc -O2 -o hogtool hogtool.c
 *
 * HOG format documented via DXX-Rebirth utilities by Josh Cogliati,
 * Bradley Bell (GPL v2+). This tool is new work.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>   /* basename() */

/* ── portability ─────────────────────────────────────────────────────────── */

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif

/* HOG format constants */
#define HOG_MAGIC       "DHF"
#define HOG_MAGIC_LEN   3
#define HOG_NAME_LEN    13   /* 12 chars + null terminator */
#define HOG_LEN_BYTES   4

/* read/write helpers (little-endian uint32) */
static uint32_t read_le32(FILE *f) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int write_le32(FILE *f, uint32_t v) {
    unsigned char b[4] = {
        (unsigned char)(v),
        (unsigned char)(v >> 8),
        (unsigned char)(v >> 16),
        (unsigned char)(v >> 24)
    };
    return fwrite(b, 1, 4, f) == 4;
}

/* ── shared: open HOG and verify magic ──────────────────────────────────── */

static FILE *open_hog(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "hogtool: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    char magic[HOG_MAGIC_LEN];
    if (fread(magic, 1, HOG_MAGIC_LEN, f) != HOG_MAGIC_LEN ||
        memcmp(magic, HOG_MAGIC, HOG_MAGIC_LEN) != 0) {
        fprintf(stderr, "hogtool: '%s' is not a valid HOG archive (bad magic)\n", path);
        fclose(f);
        return NULL;
    }
    return f;
}

/* ── mkdir -p (create all intermediate dirs) ────────────────────────────── */

static int mkdirp(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t len = strlen(buf);
    if (len && buf[len-1] == '/')
        buf[--len] = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            MKDIR(buf); /* ignore errors; final mkdir below will catch real failures */
            *p = '/';
        }
    }
    if (MKDIR(buf) != 0 && errno != EEXIST) {
        fprintf(stderr, "hogtool: cannot create directory '%s': %s\n", buf, strerror(errno));
        return 0;
    }
    return 1;
}

/* ── copy N bytes from src → dst using a stack buffer ───────────────────── */

static int copy_bytes(FILE *src, FILE *dst, uint32_t n) {
    char buf[65536];
    while (n > 0) {
        size_t chunk = n < sizeof(buf) ? n : sizeof(buf);
        size_t got = fread(buf, 1, chunk, src);
        if (got == 0) return 0;
        if (fwrite(buf, 1, got, dst) != got) return 0;
        n -= (uint32_t)got;
    }
    return 1;
}

/* ── skip N bytes in file ───────────────────────────────────────────────── */

static int skip_bytes(FILE *f, uint32_t n) {
    return fseek(f, (long)n, SEEK_CUR) == 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * COMMAND: list
 * ───────────────────────────────────────────────────────────────────────── */

static int cmd_list(const char *hog_path) {
    FILE *f = open_hog(hog_path);
    if (!f) return 1;

    printf("%-13s  %10s\n", "Filename", "Size");
    printf("%-13s  %10s\n", "-------------", "----------");

    char name[HOG_NAME_LEN];
    uint32_t total_files = 0, total_bytes = 0;

    while (1) {
        size_t got = fread(name, 1, HOG_NAME_LEN, f);
        if (got == 0) break;               /* clean EOF */
        if (got < HOG_NAME_LEN) {
            fprintf(stderr, "hogtool: truncated entry header\n");
            fclose(f);
            return 1;
        }
        name[HOG_NAME_LEN - 1] = '\0';    /* safety */

        uint32_t len = read_le32(f);
        if (ferror(f)) {
            fprintf(stderr, "hogtool: read error\n");
            fclose(f);
            return 1;
        }

        printf("%-13s  %10u\n", name, len);
        total_files++;
        total_bytes += len;

        if (!skip_bytes(f, len)) {
            fprintf(stderr, "hogtool: unexpected end of file in '%s'\n", name);
            fclose(f);
            return 1;
        }
    }

    printf("\n%u file(s), %u bytes total\n", total_files, total_bytes);
    fclose(f);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * COMMAND: extract
 * ───────────────────────────────────────────────────────────────────────── */

static int cmd_extract(const char *hog_path, const char *outdir,
                       int nfilters, char **filters) {
    FILE *f = open_hog(hog_path);
    if (!f) return 1;

    /* create output directory if needed */
    if (outdir && strcmp(outdir, ".") != 0) {
        if (!mkdirp(outdir)) {
            fclose(f);
            return 1;
        }
    }

    char name[HOG_NAME_LEN];
    int extracted = 0, skipped = 0, errors = 0;

    while (1) {
        size_t got = fread(name, 1, HOG_NAME_LEN, f);
        if (got == 0) break;
        if (got < HOG_NAME_LEN) {
            fprintf(stderr, "hogtool: truncated entry header\n");
            errors++;
            break;
        }
        name[HOG_NAME_LEN - 1] = '\0';

        uint32_t len = read_le32(f);
        if (ferror(f)) {
            fprintf(stderr, "hogtool: read error\n");
            errors++;
            break;
        }

        /* check filter list */
        int wanted = 1;
        if (nfilters > 0) {
            wanted = 0;
            for (int i = 0; i < nfilters; i++) {
                /* case-insensitive compare for DOS filenames */
                if (strcasecmp(filters[i], name) == 0) {
                    wanted = 1;
                    break;
                }
            }
        }

        if (!wanted) {
            skip_bytes(f, len);
            skipped++;
            continue;
        }

        /* build output path */
        char outpath[4096];
        if (outdir)
            snprintf(outpath, sizeof(outpath), "%s/%s", outdir, name);
        else
            snprintf(outpath, sizeof(outpath), "%s", name);

        /* check for existing file */
        struct stat st;
        if (stat(outpath, &st) == 0) {
            fprintf(stderr, "hogtool: warning: overwriting existing '%s'\n", outpath);
        }

        FILE *out = fopen(outpath, "wb");
        if (!out) {
            fprintf(stderr, "hogtool: cannot write '%s': %s\n", outpath, strerror(errno));
            skip_bytes(f, len);
            errors++;
            continue;
        }

        if (!copy_bytes(f, out, len)) {
            fprintf(stderr, "hogtool: error extracting '%s'\n", name);
            fclose(out);
            errors++;
            continue;
        }

        fclose(out);
        printf("  %s  (%u bytes)\n", outpath, len);
        extracted++;
    }

    fclose(f);
    printf("\n%d extracted, %d skipped, %d error(s)\n", extracted, skipped, errors);
    return errors ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * COMMAND: create
 * ───────────────────────────────────────────────────────────────────────── */

static int cmd_create(const char *hog_path, int nfiles, char **files) {
    if (nfiles == 0) {
        fprintf(stderr, "hogtool: create requires at least one input file\n");
        return 1;
    }

    FILE *out = fopen(hog_path, "wb");
    if (!out) {
        fprintf(stderr, "hogtool: cannot create '%s': %s\n", hog_path, strerror(errno));
        return 1;
    }

    /* write magic */
    if (fwrite(HOG_MAGIC, 1, HOG_MAGIC_LEN, out) != HOG_MAGIC_LEN) {
        fprintf(stderr, "hogtool: write error\n");
        fclose(out);
        return 1;
    }

    int added = 0, errors = 0;

    for (int i = 0; i < nfiles; i++) {
        const char *fpath = files[i];

        /* get just the filename (no directory component) */
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s", fpath);
        const char *fname = basename(tmp);

        /* stat the input first so missing files give a clear error */
        struct stat st;
        if (stat(fpath, &st) != 0) {
            fprintf(stderr, "hogtool: cannot open '%s': %s\n", fpath, strerror(errno));
            errors++;
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "hogtool: '%s' is not a regular file, skipping\n", fpath);
            errors++;
            continue;
        }

        /* validate filename length */
        if (strlen(fname) > 12) {
            fprintf(stderr, "hogtool: '%s': filename too long (max 12 chars), skipping\n", fname);
            errors++;
            continue;
        }

        uint32_t fsize = (uint32_t)st.st_size;

        FILE *in = fopen(fpath, "rb");
        if (!in) {
            fprintf(stderr, "hogtool: cannot open '%s': %s\n", fpath, strerror(errno));
            errors++;
            continue;
        }

        /* write 13-byte null-padded name */
        char name[HOG_NAME_LEN];
        memset(name, 0, HOG_NAME_LEN);
        strncpy(name, fname, 12);
        if (fwrite(name, 1, HOG_NAME_LEN, out) != HOG_NAME_LEN) {
            fprintf(stderr, "hogtool: write error on name for '%s'\n", fname);
            fclose(in);
            errors++;
            break;
        }

        /* write 4-byte little-endian size */
        if (!write_le32(out, fsize)) {
            fprintf(stderr, "hogtool: write error on size for '%s'\n", fname);
            fclose(in);
            errors++;
            break;
        }

        /* copy file data */
        if (!copy_bytes(in, out, fsize)) {
            fprintf(stderr, "hogtool: error copying data for '%s'\n", fname);
            fclose(in);
            errors++;
            break;
        }

        fclose(in);
        printf("  added %-13s  (%u bytes)\n", name, fsize);
        added++;
    }

    fclose(out);

    if (errors && added == 0) {
        /* nothing was written; remove incomplete archive */
        remove(hog_path);
        fprintf(stderr, "hogtool: no files added, archive removed\n");
        return 1;
    }

    printf("\n%d file(s) added to '%s'", added, hog_path);
    if (errors) printf(", %d error(s)", errors);
    printf("\n");
    return errors ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * main / argument dispatch
 * ───────────────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s list    <archive.hog>\n"
        "  %s extract <archive.hog> [-o outdir] [file1 file2 ...]\n"
        "  %s create  <archive.hog> <file1> [file2 ...]\n"
        "  %s help\n"
        "\n"
        "Commands:\n"
        "  list      List files inside a HOG archive\n"
        "  extract   Extract all files (or named subset) from a HOG archive\n"
        "              -o outdir   Write extracted files to this directory\n"
        "                          (created if it does not exist)\n"
        "  create    Create a new HOG archive from a list of files\n"
        "              Filenames are taken from the basename of each path;\n"
        "              max 12 characters each.\n"
        "\n"
        "Examples:\n"
        "  %s list descent.hog\n"
        "  %s extract descent.hog -o ./out\n"
        "  %s extract descent.hog -o ./out game0.hmp briefing.hmp\n"
        "  %s create mymission.hog level01.rdl level01.rl2\n"
        "\n"
        "Build:  gcc -O2 -o hogtool hogtool.c\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(argv[0]);
        return 0;
    }

    /* ── list ────────────────────────────────────────────────────────────── */
    if (!strcmp(cmd, "list") || !strcmp(cmd, "l") || !strcmp(cmd, "ls")) {
        if (argc < 3) {
            fprintf(stderr, "hogtool: 'list' requires an archive path\n");
            return 1;
        }
        return cmd_list(argv[2]);
    }

    /* ── extract ─────────────────────────────────────────────────────────── */
    if (!strcmp(cmd, "extract") || !strcmp(cmd, "x") || !strcmp(cmd, "e")) {
        if (argc < 3) {
            fprintf(stderr, "hogtool: 'extract' requires an archive path\n");
            return 1;
        }
        const char *hog_path = argv[2];
        const char *outdir = ".";
        int file_start = 3;

        /* parse -o outdir */
        if (argc > 3 && !strcmp(argv[3], "-o")) {
            if (argc < 5) {
                fprintf(stderr, "hogtool: '-o' requires a directory argument\n");
                return 1;
            }
            outdir = argv[4];
            file_start = 5;
        }

        int nfilters = argc - file_start;
        char **filters = (nfilters > 0) ? argv + file_start : NULL;
        return cmd_extract(hog_path, outdir, nfilters, filters);
    }

    /* ── create ──────────────────────────────────────────────────────────── */
    if (!strcmp(cmd, "create") || !strcmp(cmd, "c")) {
        if (argc < 4) {
            fprintf(stderr, "hogtool: 'create' requires an archive path and at least one file\n");
            return 1;
        }
        return cmd_create(argv[2], argc - 3, argv + 3);
    }

    fprintf(stderr, "hogtool: unknown command '%s'\n", cmd);
    usage(argv[0]);
    return 1;
}
