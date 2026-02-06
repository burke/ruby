/*
 * gem_tar.c — tar/tar.gz extractor for RubyGems, implemented as a C extension.
 *
 * Provides:
 *   GemTar.extract(tar_path, dest_dir)      # extract uncompressed tar
 *   GemTar.extract_gz(targz_path, dest_dir) # extract gzip-compressed tar
 *
 * Both methods release the GVL during I/O so that multiple threads can
 * extract gems in true parallelism.
 *
 * Handles POSIX/ustar tar format including GNU long-name extensions (type 'L'),
 * regular files, directories, and symlinks.  Absolute paths and entries
 * containing ".." are silently skipped as a safety measure.
 */

#include <ruby.h>
#include <ruby/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#include <unistd.h>

#define BLOCK_SIZE 512
#define IO_BUF_SIZE (256 * 1024)

/* ------------------------------------------------------------------ */
/* tar header layout (POSIX / ustar, 512 bytes)                       */
/* ------------------------------------------------------------------ */

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

/* ------------------------------------------------------------------ */
/* I/O abstraction: plain FILE* or gzFile                             */
/* ------------------------------------------------------------------ */

typedef struct {
    gzFile gz;
    FILE  *fp;
    int    use_gz;
} tar_reader;

static int reader_open(tar_reader *r, const char *path, int gz) {
    r->use_gz = gz;
    if (gz) {
        r->gz = gzopen(path, "rb");
        if (!r->gz) return -1;
        gzbuffer(r->gz, IO_BUF_SIZE);
        r->fp = NULL;
    } else {
        r->fp = fopen(path, "rb");
        if (!r->fp) return -1;
        r->gz = NULL;
    }
    return 0;
}

static void reader_close(tar_reader *r) {
    if (r->use_gz && r->gz) gzclose(r->gz);
    if (!r->use_gz && r->fp) fclose(r->fp);
    r->gz = NULL;
    r->fp = NULL;
}

static size_t reader_read(tar_reader *r, void *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        int n;
        if (r->use_gz) {
            n = gzread(r->gz, (char *)buf + total, (unsigned)(size - total));
        } else {
            n = (int)fread((char *)buf + total, 1, size - total, r->fp);
        }
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static long parse_octal(const char *str, int len) {
    long val = 0;
    const char *end = str + len;
    while (str < end && (*str == ' ' || *str == '0')) str++;
    while (str < end && *str >= '0' && *str <= '7') {
        val = val * 8 + (*str - '0');
        str++;
    }
    return val;
}

static void mkdirs_for(const char *path) {
    char *tmp = strdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    free(tmp);
}

static int is_all_zero(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i]) return 0;
    }
    return 1;
}

static void skip_data(tar_reader *reader, char *io_buf, long data_blocks) {
    long pad = data_blocks * BLOCK_SIZE;
    while (pad > 0) {
        long chunk = pad > IO_BUF_SIZE ? IO_BUF_SIZE : pad;
        reader_read(reader, io_buf, (size_t)chunk);
        pad -= chunk;
    }
}

/* ------------------------------------------------------------------ */
/* core extraction (runs WITHOUT the GVL)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *path;
    const char *dest_dir;
    int use_gz;
    int error;          /* 0 = ok, -1 = open failed, -2 = extract error */
    char errmsg[512];
} extract_args;

static int do_extract(tar_reader *reader, const char *dest_dir,
                      char *errmsg, size_t errmsg_size) {
    char header_buf[BLOCK_SIZE];
    char *io_buf = malloc(IO_BUF_SIZE);
    if (!io_buf) {
        snprintf(errmsg, errmsg_size, "malloc failed");
        return -2;
    }
    char long_name[4096];
    long_name[0] = '\0';

    while (1) {
        size_t n = reader_read(reader, header_buf, BLOCK_SIZE);
        if (n < BLOCK_SIZE) break;
        if (is_all_zero(header_buf, BLOCK_SIZE)) break;

        struct tar_header *h = (struct tar_header *)header_buf;

        /* Assemble entry name (prefix + name, or long name from prior header) */
        char name[512];
        if (long_name[0]) {
            snprintf(name, sizeof(name), "%s", long_name);
            long_name[0] = '\0';
        } else if (h->prefix[0] && memcmp(h->magic, "ustar", 5) == 0) {
            size_t plen = strnlen(h->prefix, sizeof(h->prefix));
            size_t nlen = strnlen(h->name, sizeof(h->name));
            snprintf(name, sizeof(name), "%.*s/%.*s",
                     (int)plen, h->prefix, (int)nlen, h->name);
        } else {
            size_t nlen = strnlen(h->name, sizeof(h->name));
            snprintf(name, sizeof(name), "%.*s", (int)nlen, h->name);
        }

        long size        = parse_octal(h->size, sizeof(h->size));
        long mode        = parse_octal(h->mode, sizeof(h->mode));
        char type        = h->typeflag;
        long data_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

        /* GNU long-name extension: next header's name is in this entry's data */
        if (type == 'L') {
            long pad = data_blocks * BLOCK_SIZE;
            reader_read(reader, long_name,
                        pad < (long)sizeof(long_name)
                            ? (size_t)pad : sizeof(long_name) - 1);
            long_name[size < (long)sizeof(long_name)
                          ? size : (long)sizeof(long_name) - 1] = '\0';
            continue;
        }

        /* Safety: skip absolute paths and path-traversal attempts */
        if (name[0] == '/' || strstr(name, "..")) {
            skip_data(reader, io_buf, data_blocks);
            continue;
        }

        char dest_path[4096];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, name);

        if (type == '5' ||
            (type == '\0' && strlen(name) > 0 && name[strlen(name) - 1] == '/')) {
            /* Directory */
            mkdirs_for(dest_path);
            mkdir(dest_path, mode ? (mode_t)(mode & 0777) : 0755);

        } else if (type == '2') {
            /* Symlink */
            char target[101];
            memcpy(target, h->linkname, 100);
            target[100] = '\0';
            for (int i = 99; i >= 0 && (target[i] == '\0' || target[i] == ' '); i--)
                target[i] = '\0';
            mkdirs_for(dest_path);
            symlink(target, dest_path);

        } else if (type == '0' || type == '\0') {
            /* Regular file */
            mkdirs_for(dest_path);
            FILE *out = fopen(dest_path, "wb");
            if (!out) {
                snprintf(errmsg, errmsg_size,
                         "cannot open %s: %s", dest_path, strerror(errno));
                free(io_buf);
                return -2;
            }

            long remaining     = size;
            long pad_remaining = data_blocks * BLOCK_SIZE;

            while (pad_remaining > 0) {
                long chunk = pad_remaining > IO_BUF_SIZE ? IO_BUF_SIZE : pad_remaining;
                size_t rd = reader_read(reader, io_buf, (size_t)chunk);
                if ((long)rd < chunk) {
                    fclose(out);
                    snprintf(errmsg, errmsg_size,
                             "unexpected EOF in tar data for %s", name);
                    free(io_buf);
                    return -2;
                }
                long to_write = remaining > chunk ? chunk : remaining;
                if (to_write > 0)
                    fwrite(io_buf, 1, (size_t)to_write, out);
                remaining     -= to_write;
                pad_remaining -= chunk;
            }

            fclose(out);
            if (mode) chmod(dest_path, (mode_t)(mode & 0xFFFF));

        } else {
            /* Unknown type — skip data */
            skip_data(reader, io_buf, data_blocks);
        }
    }

    free(io_buf);
    return 0;
}

/* Wrapper for rb_thread_call_without_gvl — no Ruby API calls here. */
static void *extract_without_gvl(void *arg) {
    extract_args *a = (extract_args *)arg;

    tar_reader reader;
    if (reader_open(&reader, a->path, a->use_gz) < 0) {
        a->error = -1;
        snprintf(a->errmsg, sizeof(a->errmsg), "%s", strerror(errno));
        return NULL;
    }

    a->error = do_extract(&reader, a->dest_dir, a->errmsg, sizeof(a->errmsg));
    reader_close(&reader);
    return NULL;
}

static void extract_ubf(void *arg) { (void)arg; }

/* ------------------------------------------------------------------ */
/* Ruby-visible methods                                               */
/* ------------------------------------------------------------------ */

/*
 * call-seq:
 *   GemTar.extract(tar_path, dest_dir)  -> true
 *
 * Extract an uncompressed tar archive to +dest_dir+.
 */
static VALUE rb_gem_tar_extract(VALUE self, VALUE rb_path, VALUE rb_dest) {
    extract_args args;
    args.path       = StringValueCStr(rb_path);
    args.dest_dir   = StringValueCStr(rb_dest);
    args.use_gz     = 0;
    args.error      = 0;
    args.errmsg[0]  = '\0';

    rb_thread_call_without_gvl(extract_without_gvl, &args, extract_ubf, NULL);

    if (args.error == -1)
        rb_sys_fail(args.path);
    else if (args.error == -2)
        rb_raise(rb_eIOError, "%s", args.errmsg);
    return Qtrue;
}

/*
 * call-seq:
 *   GemTar.extract_gz(targz_path, dest_dir)  -> true
 *
 * Extract a gzip-compressed tar archive to +dest_dir+.
 */
static VALUE rb_gem_tar_extract_gz(VALUE self, VALUE rb_path, VALUE rb_dest) {
    extract_args args;
    args.path       = StringValueCStr(rb_path);
    args.dest_dir   = StringValueCStr(rb_dest);
    args.use_gz     = 1;
    args.error      = 0;
    args.errmsg[0]  = '\0';

    rb_thread_call_without_gvl(extract_without_gvl, &args, extract_ubf, NULL);

    if (args.error == -1)
        rb_sys_fail(args.path);
    else if (args.error == -2)
        rb_raise(rb_eIOError, "%s", args.errmsg);
    return Qtrue;
}

void Init_gem_tar(void) {
    VALUE mGemTar = rb_define_module("GemTar");
    rb_define_module_function(mGemTar, "extract",    rb_gem_tar_extract,    2);
    rb_define_module_function(mGemTar, "extract_gz", rb_gem_tar_extract_gz, 2);
}
