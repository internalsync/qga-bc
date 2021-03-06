/*
 * QEMU disk image utility
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qapi-visit.h"
#include "qapi/qmp-output-visitor.h"
#include "qjson.h"
#include "qemu-common.h"
#include "qemu-option.h"
#include "qemu-error.h"
#include "osdep.h"
#include "block_int.h"
#include <getopt.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct img_cmd_t {
    const char *name;
    int (*handler)(int argc, char **argv);
} img_cmd_t;

enum {
    OPTION_OUTPUT = 256,
    OPTION_BACKING_CHAIN = 257,
};

typedef enum OutputFormat {
    OFORMAT_JSON,
    OFORMAT_HUMAN,
} OutputFormat;

/* Default to cache=writeback as data integrity is not important for qemu-tcg. */
#define BDRV_O_FLAGS BDRV_O_CACHE_WB
#define BDRV_DEFAULT_CACHE "writeback"

static void format_print(void *opaque, const char *name)
{
    printf(" %s", name);
}

/* Please keep in synch with qemu-img.texi */
static void help(void)
{
    printf("qemu-img version " QEMU_VERSION ", Copyright (c) 2004-2008 Fabrice Bellard\n"
           "usage: qemu-img command [command options]\n"
           "QEMU disk image utility\n"
           "\n"
           "Command syntax:\n"
#define DEF(option, callback, arg_string)        \
           "  " arg_string "\n"
#include "qemu-img-cmds.h"
#undef DEF
#undef GEN_DOCS
           "\n"
           "Command parameters:\n"
           "  'filename' is a disk image filename\n"
           "  'fmt' is the disk image format. It is guessed automatically in most cases\n"
           "  'cache' is the cache mode used to write the output disk image, the valid\n"
           "    options are: 'none', 'writeback' (default), 'writethrough', 'directsync'\n"
           "    and 'unsafe'\n"
           "  'size' is the disk image size in bytes. Optional suffixes\n"
           "    'k' or 'K' (kilobyte, 1024), 'M' (megabyte, 1024k), 'G' (gigabyte, 1024M)\n"
           "    and T (terabyte, 1024G) are supported. 'b' is ignored.\n"
           "  'output_filename' is the destination disk image filename\n"
           "  'output_fmt' is the destination format\n"
           "  'options' is a comma separated list of format specific options in a\n"
           "    name=value format. Use -o ? for an overview of the options supported by the\n"
           "    used format\n"
           "  '-c' indicates that target image must be compressed (qcow format only)\n"
           "  '-u' enables unsafe rebasing. It is assumed that old and new backing file\n"
           "       match exactly. The image doesn't need a working backing file before\n"
           "       rebasing in this case (useful for renaming the backing file)\n"
           "  '-h' with or without a command shows this help and lists the supported formats\n"
           "  '-p' show progress of command (only certain commands)\n"
           "  '-S' indicates the consecutive number of bytes that must contain only zeros\n"
           "       for qemu-img to create a sparse image during conversion\n"
           "  '--output' takes the format in which the output must be done (human or json)\n"
           "\n"
           "Parameters to check subcommand:\n"
           "  '-r' tries to repair any inconsistencies that are found during the check.\n"
           "       '-r leaks' repairs only cluster leaks, whereas '-r all' fixes all\n"
           "       kinds of errors, with a higher risk of choosing the wrong fix or\n"
           "       hiding corruption that has already occurred.\n"
           "\n"
           "Parameters to snapshot subcommand:\n"
           "  'snapshot' is the name of the snapshot to create, apply or delete\n"
           "  '-a' applies a snapshot (revert disk to saved state)\n"
           "  '-c' creates a snapshot\n"
           "  '-d' deletes a snapshot\n"
           "  '-l' lists all snapshots in the given image\n"
           "\n"
           "Parameters to compare subcommand:\n"
           "  '-f' first image format\n"
           "  '-F' second image format\n"
           "  '-s' run in Strict mode - fail on different image size or sector allocation\n"
           );
    printf("\nSupported formats:");
    bdrv_iterate_format(format_print, NULL);
    printf("\n");
    exit(1);
}

#if defined(WIN32)
/* XXX: put correct support for win32 */
static int read_password(char *buf, int buf_size)
{
    int c, i;
    printf("Password: ");
    fflush(stdout);
    i = 0;
    for(;;) {
        c = getchar();
        if (c == '\n')
            break;
        if (i < (buf_size - 1))
            buf[i++] = c;
    }
    buf[i] = '\0';
    return 0;
}

#else

#include <termios.h>

static struct termios oldtty;

static void term_exit(void)
{
    tcsetattr (0, TCSANOW, &oldtty);
}

static void term_init(void)
{
    struct termios tty;

    tcgetattr (0, &tty);
    oldtty = tty;

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
    tty.c_cflag &= ~(CSIZE|PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr (0, TCSANOW, &tty);

    atexit(term_exit);
}

static int read_password(char *buf, int buf_size)
{
    uint8_t ch;
    int i, ret;

    printf("password: ");
    fflush(stdout);
    term_init();
    i = 0;
    for(;;) {
        ret = read(0, &ch, 1);
        if (ret == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            } else {
                ret = -1;
                break;
            }
        } else if (ret == 0) {
            ret = -1;
            break;
        } else {
            if (ch == '\r') {
                ret = 0;
                break;
            }
            if (i < (buf_size - 1))
                buf[i++] = ch;
        }
    }
    term_exit();
    buf[i] = '\0';
    printf("\n");
    return ret;
}
#endif

static int print_block_option_help(const char *filename, const char *fmt)
{
    BlockDriver *drv, *proto_drv;
    QEMUOptionParameter *create_options = NULL;
    Error *local_err = NULL;

    /* Find driver and parse its options */
    drv = bdrv_find_format(fmt);
    if (!drv) {
        error_report("Unknown file format '%s'", fmt);
        return 1;
    }

    create_options = append_option_parameters(create_options,
                                              drv->create_options);

    if (filename) {
        proto_drv = bdrv_find_protocol(filename, &local_err);
        if (!proto_drv) {
            qerror_report_err(local_err);
            error_free(local_err);
            return 1;
        }
        create_options = append_option_parameters(create_options,
                                                  proto_drv->create_options);
    }

    print_option_help(create_options);
    free_option_parameters(create_options);
    return 0;
}

static BlockDriverState *bdrv_new_open(const char *filename,
                                       const char *fmt,
                                       int flags,
                                       bool require_io)
{
    BlockDriverState *bs;
    BlockDriver *drv;
    char password[256];
    int ret;

    bs = bdrv_new("image");

    if (fmt) {
        drv = bdrv_find_format(fmt);
        if (!drv) {
            error_report("Unknown file format '%s'", fmt);
            goto fail;
        }
    } else {
        drv = NULL;
    }

    ret = bdrv_open(bs, filename, flags, drv);
    if (ret < 0) {
        error_report("Could not open '%s': %s", filename, strerror(-ret));
        goto fail;
    }

    if (bdrv_is_encrypted(bs) && require_io) {
        printf("Disk image '%s' is encrypted.\n", filename);
        if (read_password(password, sizeof(password)) < 0) {
            error_report("No password given");
            goto fail;
        }
        if (bdrv_set_key(bs, password) < 0) {
            error_report("invalid password");
            goto fail;
        }
    }
    return bs;
fail:
    if (bs) {
        bdrv_delete(bs);
    }
    return NULL;
}

static int add_old_style_options(const char *fmt, QEMUOptionParameter *list,
                                 const char *base_filename,
                                 const char *base_fmt)
{
    if (base_filename) {
        if (set_option_parameter(list, BLOCK_OPT_BACKING_FILE, base_filename)) {
            error_report("Backing file not supported for file format '%s'",
                         fmt);
            return -1;
        }
    }
    if (base_fmt) {
        if (set_option_parameter(list, BLOCK_OPT_BACKING_FMT, base_fmt)) {
            error_report("Backing file format not supported for file "
                         "format '%s'", fmt);
            return -1;
        }
    }
    return 0;
}

static int img_create(int argc, char **argv)
{
    int c;
    uint64_t img_size = -1;
    const char *fmt = "raw";
    const char *base_fmt = NULL;
    const char *filename;
    const char *base_filename = NULL;
    char *options = NULL;
    Error *local_err = NULL;

    for(;;) {
        c = getopt(argc, argv, "F:b:f:he6o:");
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            break;
        case 'F':
            base_fmt = optarg;
            break;
        case 'b':
            base_filename = optarg;
            break;
        case 'f':
            fmt = optarg;
            break;
        case 'e':
            error_report("qemu-img: option -e is deprecated, please use \'-o "
                  "encryption\' instead!");
            goto fail;
        case '6':
            error_report("qemu-img: option -6 is deprecated, please use \'-o "
                  "compat6\' instead!");
            goto fail;
        case 'o':
            if (!is_valid_option_list(optarg)) {
                error_report("Invalid option list: %s", optarg);
                goto fail;
            }
            if (!options) {
                options = g_strdup(optarg);
            } else {
                char *old_options = options;
                options = g_strdup_printf("%s,%s", options, optarg);
                g_free(old_options);
            }
            break;
        }
    }

    /* Get the filename */
    filename = (optind < argc) ? argv[optind] : NULL;
    if (options && has_help_option(options)) {
        g_free(options);
        return print_block_option_help(filename, fmt);
    }

    if (optind >= argc) {
        help();
    }
    optind++;

    /* Get image size, if specified */
    if (optind < argc) {
        int64_t sval;
        char *end;
        sval = strtosz_suffix(argv[optind++], &end, STRTOSZ_DEFSUFFIX_B);
        if (sval < 0 || *end) {
            if (sval == -ERANGE) {
                error_report("Image size must be less than 8 EiB!");
            } else {
                error_report("Invalid image size specified! You may use k, M, "
                      "G or T suffixes for ");
                error_report("kilobytes, megabytes, gigabytes and terabytes.");
            }
            goto fail;
        }
        img_size = (uint64_t)sval;
    }

    bdrv_img_create(filename, fmt, base_filename, base_fmt,
                    options, img_size, BDRV_O_FLAGS, &local_err);
    if (error_is_set(&local_err)) {
        error_report("%s", error_get_pretty(local_err));
        error_free(local_err);
        goto fail;
    }

    g_free(options);
    return 0;

fail:
    g_free(options);
    return 1;
}

static void dump_json_image_check(ImageCheck *check)
{
    Error *errp = NULL;
    QString *str;
    QmpOutputVisitor *ov = qmp_output_visitor_new();
    QObject *obj;
    visit_type_ImageCheck(qmp_output_get_visitor(ov),
                          &check, NULL, &errp);
    obj = qmp_output_get_qobject(ov);
    str = qobject_to_json(obj);
    assert(str != NULL);
    printf("%s\n", qstring_get_str(str));
    qobject_decref(obj);
    qmp_output_visitor_cleanup(ov);
    QDECREF(str);
}

static void dump_human_image_check(ImageCheck *check)
{
    if (!(check->corruptions || check->leaks || check->check_errors)) {
        printf("No errors were found on the image.\n");
    } else {
        if (check->corruptions) {
            printf("\n%" PRId64 " errors were found on the image.\n"
                "Data may be corrupted, or further writes to the image "
                "may corrupt it.\n",
                check->corruptions);
        }

        if (check->leaks) {
            printf("\n%" PRId64 " leaked clusters were found on the image.\n"
                "This means waste of disk space, but no harm to data.\n",
                check->leaks);
        }

        if (check->check_errors) {
            printf("\n%" PRId64 " internal errors have occurred during the check.\n",
                check->check_errors);
        }
    }

    if (check->total_clusters != 0 && check->allocated_clusters != 0) {
        printf("%" PRId64 "/%" PRId64 "= %0.2f%% allocated, %0.2f%% fragmented\n",
        check->allocated_clusters, check->total_clusters,
        check->allocated_clusters * 100.0 / check->total_clusters,
        check->fragmented_clusters * 100.0 / check->allocated_clusters);
    }

    if (check->image_end_offset) {
        printf("Image end offset: %" PRId64 "\n", check->image_end_offset);
    }
}

static int collect_image_check(BlockDriverState *bs,
                   ImageCheck *check,
                   const char *filename,
                   const char *fmt,
                   int fix)
{
    int ret;
    BdrvCheckResult result;

    ret = bdrv_check(bs, &result, fix);
    if (ret < 0) {
        return ret;
    }

    check->filename                 = g_strdup(filename);
    check->format                   = g_strdup(bdrv_get_format_name(bs));
    check->check_errors             = result.check_errors;
    check->corruptions              = result.corruptions;
    check->has_corruptions          = result.corruptions != 0;
    check->leaks                    = result.leaks;
    check->has_leaks                = result.leaks != 0;
    check->corruptions_fixed        = result.corruptions_fixed;
    check->has_corruptions_fixed    = result.corruptions != 0;
    check->leaks_fixed              = result.leaks_fixed;
    check->has_leaks_fixed          = result.leaks != 0;
    check->image_end_offset         = result.image_end_offset;
    check->has_image_end_offset     = result.image_end_offset != 0;
    check->total_clusters           = result.bfi.total_clusters;
    check->has_total_clusters       = result.bfi.total_clusters != 0;
    check->allocated_clusters       = result.bfi.allocated_clusters;
    check->has_allocated_clusters   = result.bfi.allocated_clusters != 0;
    check->fragmented_clusters      = result.bfi.fragmented_clusters;
    check->has_fragmented_clusters  = result.bfi.fragmented_clusters != 0;

    return 0;
}

/*
 * Checks an image for consistency. Exit codes:
 *
 *  0 - Check completed, image is good
 *  1 - Check not completed because of internal errors
 *  2 - Check completed, image is corrupted
 *  3 - Check completed, image has leaked clusters, but is good otherwise
 * 63 - Checks are not supported by the image format
 */
static int img_check(int argc, char **argv)
{
    int c, ret;
    OutputFormat output_format = OFORMAT_HUMAN;
    const char *filename, *fmt, *output, *cache;
    BlockDriverState *bs;
    int fix = 0;
    int flags = BDRV_O_FLAGS | BDRV_O_CHECK;
    ImageCheck *check;

    fmt = NULL;
    output = NULL;
    cache = BDRV_DEFAULT_CACHE;
    for(;;) {
        int option_index = 0;
        static const struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"format", required_argument, 0, 'f'},
            {"repair", no_argument, 0, 'r'},
            {"output", required_argument, 0, OPTION_OUTPUT},
            {0, 0, 0, 0}
        };
        c = getopt_long(argc, argv, "hf:r:T:",
                        long_options, &option_index);
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt = optarg;
            break;
        case 'r':
            flags |= BDRV_O_RDWR;

            if (!strcmp(optarg, "leaks")) {
                fix = BDRV_FIX_LEAKS;
            } else if (!strcmp(optarg, "all")) {
                fix = BDRV_FIX_LEAKS | BDRV_FIX_ERRORS;
            } else {
                help();
            }
            break;
        case OPTION_OUTPUT:
            output = optarg;
            break;
        case 'T':
            cache = optarg;
            break;
        }
    }
    if (optind >= argc) {
        help();
    }
    filename = argv[optind++];

    if (output && !strcmp(output, "json")) {
        output_format = OFORMAT_JSON;
    } else if (output && !strcmp(output, "human")) {
        output_format = OFORMAT_HUMAN;
    } else if (output) {
        error_report("--output must be used with human or json as argument.");
        return 1;
    }

    ret = bdrv_parse_cache_flags(cache, &flags);
    if (ret < 0) {
        error_report("Invalid source cache option: %s", cache);
        return 1;
    }

    bs = bdrv_new_open(filename, fmt, flags, true);
    if (!bs) {
        return 1;
    }

    check = g_new0(ImageCheck, 1);
    ret = collect_image_check(bs, check, filename, fmt, fix);

    if (ret == -ENOTSUP) {
        error_report("This image format does not support checks");
        ret = 63;
        goto fail;
    }

    if (check->corruptions_fixed || check->leaks_fixed) {
        int corruptions_fixed, leaks_fixed;

        leaks_fixed         = check->leaks_fixed;
        corruptions_fixed   = check->corruptions_fixed;

        if (output_format == OFORMAT_HUMAN) {
            printf("The following inconsistencies were found and repaired:\n\n"
                   "    %" PRId64 " leaked clusters\n"
                   "    %" PRId64 " corruptions\n\n"
                   "Double checking the fixed image now...\n",
                   check->leaks_fixed,
                   check->corruptions_fixed);
        }

        ret = collect_image_check(bs, check, filename, fmt, 0);

        check->leaks_fixed          = leaks_fixed;
        check->corruptions_fixed    = corruptions_fixed;
    }

    switch (output_format) {
    case OFORMAT_HUMAN:
        dump_human_image_check(check);
        break;
    case OFORMAT_JSON:
        dump_json_image_check(check);
        break;
    }

    if (ret || check->check_errors) {
        ret = 1;
        goto fail;
    }

    if (check->corruptions) {
        ret = 2;
    } else if (check->leaks) {
        ret = 3;
    } else {
        ret = 0;
    }

fail:
    qapi_free_ImageCheck(check);
    bdrv_delete(bs);

    return ret;
}

static int img_commit(int argc, char **argv)
{
    int c, ret, flags;
    const char *filename, *fmt, *cache;
    BlockDriverState *bs;

    fmt = NULL;
    cache = BDRV_DEFAULT_CACHE;
    for(;;) {
        c = getopt(argc, argv, "f:ht:");
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt = optarg;
            break;
        case 't':
            cache = optarg;
            break;
        }
    }
    if (optind >= argc) {
        help();
    }
    filename = argv[optind++];

    flags = BDRV_O_RDWR;
    ret = bdrv_parse_cache_flags(cache, &flags);
    if (ret < 0) {
        error_report("Invalid cache option: %s\n", cache);
        return -1;
    }

    bs = bdrv_new_open(filename, fmt, flags, true);
    if (!bs) {
        return 1;
    }
    ret = bdrv_commit(bs);
    switch(ret) {
    case 0:
        printf("Image committed.\n");
        break;
    case -ENOENT:
        error_report("No disk inserted");
        break;
    case -EACCES:
        error_report("Image is read-only");
        break;
    case -ENOTSUP:
        error_report("Image is already committed");
        break;
    default:
        error_report("Error while committing image");
        break;
    }

    bdrv_delete(bs);
    if (ret) {
        return 1;
    }
    return 0;
}

/*
 * Returns true iff the first sector pointed to by 'buf' contains at least
 * a non-NUL byte.
 *
 * 'pnum' is set to the number of sectors (including and immediately following
 * the first one) that are known to be in the same allocated/unallocated state.
 */
static int is_allocated_sectors(const uint8_t *buf, int n, int *pnum)
{
    bool is_zero;
    int i;

    if (n <= 0) {
        *pnum = 0;
        return 0;
    }
    is_zero = buffer_is_zero(buf, 512);
    for(i = 1; i < n; i++) {
        buf += 512;
        if (is_zero != buffer_is_zero(buf, 512)) {
            break;
        }
    }
    *pnum = i;
    return !is_zero;
}

/*
 * Like is_allocated_sectors, but if the buffer starts with a used sector,
 * up to 'min' consecutive sectors containing zeros are ignored. This avoids
 * breaking up write requests for only small sparse areas.
 */
static int is_allocated_sectors_min(const uint8_t *buf, int n, int *pnum,
    int min)
{
    int ret;
    int num_checked, num_used;

    if (n < min) {
        min = n;
    }

    ret = is_allocated_sectors(buf, n, pnum);
    if (!ret) {
        return ret;
    }

    num_used = *pnum;
    buf += BDRV_SECTOR_SIZE * *pnum;
    n -= *pnum;
    num_checked = num_used;

    while (n > 0) {
        ret = is_allocated_sectors(buf, n, pnum);

        buf += BDRV_SECTOR_SIZE * *pnum;
        n -= *pnum;
        num_checked += *pnum;
        if (ret) {
            num_used = num_checked;
        } else if (*pnum >= min) {
            break;
        }
    }

    *pnum = num_used;
    return 1;
}

/*
 * Compares two buffers sector by sector. Returns 0 if the first sector of both
 * buffers matches, non-zero otherwise.
 *
 * pnum is set to the number of sectors (including and immediately following
 * the first one) that are known to have the same comparison result
 */
static int compare_sectors(const uint8_t *buf1, const uint8_t *buf2, int n,
    int *pnum)
{
    int res, i;

    if (n <= 0) {
        *pnum = 0;
        return 0;
    }

    res = !!memcmp(buf1, buf2, 512);
    for(i = 1; i < n; i++) {
        buf1 += 512;
        buf2 += 512;

        if (!!memcmp(buf1, buf2, 512) != res) {
            break;
        }
    }

    *pnum = i;
    return res;
}

#define IO_BUF_SIZE (2 * 1024 * 1024)

static int64_t sectors_to_bytes(int64_t sectors)
{
    return sectors << BDRV_SECTOR_BITS;
}

static int64_t sectors_to_process(int64_t total, int64_t from)
{
    return MIN(total - from, IO_BUF_SIZE >> BDRV_SECTOR_BITS);
}

/*
 * Check if passed sectors are empty (not allocated or contain only 0 bytes)
 *
 * Returns 0 in case sectors are filled with 0, 1 if sectors contain non-zero
 * data and negative value on error.
 *
 * @param bs:  Driver used for accessing file
 * @param sect_num: Number of first sector to check
 * @param sect_count: Number of sectors to check
 * @param filename: Name of disk file we are checking (logging purpose)
 * @param buffer: Allocated buffer for storing read data
 */
static int check_empty_sectors(BlockDriverState *bs, int64_t sect_num,
                               int sect_count, const char *filename,
                               uint8_t *buffer)
{
    int pnum, ret = 0;
    ret = bdrv_read(bs, sect_num, buffer, sect_count);
    if (ret < 0) {
        error_report("Error while reading offset %" PRId64 " of %s: %s",
                     sectors_to_bytes(sect_num), filename, strerror(-ret));
        return ret;
    }
    ret = is_allocated_sectors(buffer, sect_count, &pnum);
    if (ret || pnum != sect_count) {
        printf("Content mismatch at offset %" PRId64 "!\n",
                sectors_to_bytes(ret ? sect_num : sect_num + pnum));
        return 1;
    }

    return 0;
}

/*
 * Compares two images. Exit codes:
 *
 * 0 - Images are identical
 * 1 - Images differ
 * >1 - Error occurred
 */
static int img_compare(int argc, char **argv)
{
    const char *fmt1 = NULL, *fmt2 = NULL, *cache, *filename1, *filename2;
    BlockDriverState *bs1, *bs2;
    int64_t total_sectors1, total_sectors2;
    uint8_t *buf1 = NULL, *buf2 = NULL;
    int pnum1, pnum2;
    int allocated1, allocated2;
    int ret = 0; /* return value - 0 Ident, 1 Different, >1 Error */
    bool progress = false, strict = false;
    int flags;
    int64_t total_sectors;
    int64_t sector_num = 0;
    int64_t nb_sectors;
    int c, pnum;
    uint64_t bs_sectors;
    uint64_t progress_base;

    cache = BDRV_DEFAULT_CACHE;
    for (;;) {
        c = getopt(argc, argv, "hf:F:T:ps");
        if (c == -1) {
            break;
        }
        switch (c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt1 = optarg;
            break;
        case 'F':
            fmt2 = optarg;
            break;
        case 'T':
            cache = optarg;
            break;
        case 'p':
            progress = true;
            break;
        case 's':
            strict = true;
            break;
        }
    }

    if (optind > argc - 2) {
        help();
    }
    filename1 = argv[optind++];
    filename2 = argv[optind++];

    /* Initialize before goto out */
    qemu_progress_init(progress, 2.0);

    flags = BDRV_O_FLAGS;
    ret = bdrv_parse_cache_flags(cache, &flags);
    if (ret < 0) {
        error_report("Invalid source cache option: %s", cache);
        ret = 2;
        goto out3;
    }

    bs1 = bdrv_new_open(filename1, fmt1, flags, true);
    if (!bs1) {
        error_report("Can't open file %s", filename1);
        ret = 2;
        goto out3;
    }

    bs2 = bdrv_new_open(filename2, fmt2, flags, true);
    if (!bs2) {
        error_report("Can't open file %s", filename2);
        ret = 2;
        goto out2;
    }

    buf1 = qemu_blockalign(bs1, IO_BUF_SIZE);
    buf2 = qemu_blockalign(bs2, IO_BUF_SIZE);
    bdrv_get_geometry(bs1, &bs_sectors);
    total_sectors1 = bs_sectors;
    bdrv_get_geometry(bs2, &bs_sectors);
    total_sectors2 = bs_sectors;
    total_sectors = MIN(total_sectors1, total_sectors2);
    progress_base = MAX(total_sectors1, total_sectors2);

    qemu_progress_print(0, 100);

    if (strict && total_sectors1 != total_sectors2) {
        ret = 1;
        printf("Strict mode: Image size mismatch!\n");
        goto out;
    }

    for (;;) {
        nb_sectors = sectors_to_process(total_sectors, sector_num);
        if (nb_sectors <= 0) {
            break;
        }
        allocated1 = bdrv_is_allocated_above(bs1, NULL, sector_num, nb_sectors,
                                             &pnum1);
        if (allocated1 < 0) {
            ret = 3;
            error_report("Sector allocation test failed for %s", filename1);
            goto out;
        }

        allocated2 = bdrv_is_allocated_above(bs2, NULL, sector_num, nb_sectors,
                                             &pnum2);
        if (allocated2 < 0) {
            ret = 3;
            error_report("Sector allocation test failed for %s", filename2);
            goto out;
        }
        nb_sectors = MIN(pnum1, pnum2);

        if (allocated1 == allocated2) {
            if (allocated1) {
                ret = bdrv_read(bs1, sector_num, buf1, nb_sectors);
                if (ret < 0) {
                    error_report("Error while reading offset %" PRId64 " of %s:"
                                 " %s", sectors_to_bytes(sector_num), filename1,
                                 strerror(-ret));
                    ret = 4;
                    goto out;
                }
                ret = bdrv_read(bs2, sector_num, buf2, nb_sectors);
                if (ret < 0) {
                    error_report("Error while reading offset %" PRId64
                                 " of %s: %s", sectors_to_bytes(sector_num),
                                 filename2, strerror(-ret));
                    ret = 4;
                    goto out;
                }
                ret = compare_sectors(buf1, buf2, nb_sectors, &pnum);
                if (ret || pnum != nb_sectors) {
                    printf("Content mismatch at offset %" PRId64 "!\n",
                            sectors_to_bytes(
                                ret ? sector_num : sector_num + pnum));
                    ret = 1;
                    goto out;
                }
            }
        } else {
            if (strict) {
                ret = 1;
                printf("Strict mode: Offset %" PRId64
                        " allocation mismatch!\n",
                        sectors_to_bytes(sector_num));
                goto out;
            }

            if (allocated1) {
                ret = check_empty_sectors(bs1, sector_num, nb_sectors,
                                          filename1, buf1);
            } else {
                ret = check_empty_sectors(bs2, sector_num, nb_sectors,
                                          filename2, buf1);
            }
            if (ret) {
                if (ret < 0) {
                    error_report("Error while reading offset %" PRId64 ": %s",
                                 sectors_to_bytes(sector_num), strerror(-ret));
                    ret = 4;
                }
                goto out;
            }
        }
        sector_num += nb_sectors;
        qemu_progress_print(((float) nb_sectors / progress_base)*100, 100);
    }

    if (total_sectors1 != total_sectors2) {
        BlockDriverState *bs_over;
        int64_t total_sectors_over;
        const char *filename_over;

        printf("Warning: Image size mismatch!\n");
        if (total_sectors1 > total_sectors2) {
            total_sectors_over = total_sectors1;
            bs_over = bs1;
            filename_over = filename1;
        } else {
            total_sectors_over = total_sectors2;
            bs_over = bs2;
            filename_over = filename2;
        }

        for (;;) {
            nb_sectors = sectors_to_process(total_sectors_over, sector_num);
            if (nb_sectors <= 0) {
                break;
            }
            ret = bdrv_is_allocated_above(bs_over, NULL, sector_num,
                                          nb_sectors, &pnum);
            if (ret < 0) {
                ret = 3;
                error_report("Sector allocation test failed for %s",
                             filename_over);
                goto out;

            }
            nb_sectors = pnum;
            if (ret) {
                ret = check_empty_sectors(bs_over, sector_num, nb_sectors,
                                          filename_over, buf1);
                if (ret) {
                    if (ret < 0) {
                        error_report("Error while reading offset %" PRId64
                                     " of %s: %s", sectors_to_bytes(sector_num),
                                     filename_over, strerror(-ret));
                        ret = 4;
                    }
                    goto out;
                }
            }
            sector_num += nb_sectors;
            qemu_progress_print(((float) nb_sectors / progress_base)*100, 100);
        }
    }

    printf("Images are identical.\n");
    ret = 0;

out:
    bdrv_delete(bs2);
    qemu_vfree(buf1);
    qemu_vfree(buf2);
out2:
    bdrv_delete(bs1);
out3:
    qemu_progress_end();
    return ret;
}

static int img_convert(int argc, char **argv)
{
    int c, n, n1, bs_n, bs_i, compress, cluster_size, cluster_sectors;
    int64_t ret = 0;
    int progress = 0, flags, src_flags;
    const char *fmt, *out_fmt, *cache, *src_cache, *out_baseimg, *out_filename;
    BlockDriver *drv, *proto_drv;
    BlockDriverState **bs = NULL, *out_bs = NULL;
    int64_t total_sectors, nb_sectors, sector_num, bs_offset,
            sector_num_next_status = 0;
    uint64_t bs_sectors;
    uint8_t * buf = NULL;
    const uint8_t *buf1;
    BlockDriverInfo bdi;
    QEMUOptionParameter *param = NULL, *create_options = NULL;
    QEMUOptionParameter *out_baseimg_param;
    char *options = NULL;
    int min_sparse = 8; /* Need at least 4k of zeros for sparse detection */
    Error *local_err = NULL;

    fmt = NULL;
    out_fmt = "raw";
    cache = "unsafe";
    src_cache = BDRV_DEFAULT_CACHE;
    out_baseimg = NULL;
    compress = 0;
    for(;;) {
        c = getopt(argc, argv, "hf:O:B:ce6o:S:pt:T:");
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt = optarg;
            break;
        case 'O':
            out_fmt = optarg;
            break;
        case 'B':
            out_baseimg = optarg;
            break;
        case 'c':
            compress = 1;
            break;
        case 'e':
            error_report("qemu-img: option -e is deprecated, please use \'-o "
                  "encryption\' instead!");
            ret = -1;
            goto fail_getopt;
        case '6':
            error_report("qemu-img: option -6 is deprecated, please use \'-o "
                  "compat6\' instead!");
            ret = -1;
            goto fail_getopt;
        case 'o':
            if (!is_valid_option_list(optarg)) {
                error_report("Invalid option list: %s", optarg);
                ret = -1;
                goto fail_getopt;
            }
            if (!options) {
                options = g_strdup(optarg);
            } else {
                char *old_options = options;
                options = g_strdup_printf("%s,%s", options, optarg);
                g_free(old_options);
            }
            break;
        case 'S':
        {
            int64_t sval;
            char *end;
            sval = strtosz_suffix(optarg, &end, STRTOSZ_DEFSUFFIX_B);
            if (sval < 0 || *end) {
                error_report("Invalid minimum zero buffer size for sparse output specified");
                ret = -1;
                goto fail_getopt;
            }

            min_sparse = sval / BDRV_SECTOR_SIZE;
            break;
        }
        case 'p':
            progress = 1;
            break;
        case 't':
            cache = optarg;
            break;
        case 'T':
            src_cache = optarg;
            break;
        }
    }

    /* Initialize before goto out */
    qemu_progress_init(progress, 1.0);

    bs_n = argc - optind - 1;
    out_filename = bs_n >= 1 ? argv[argc - 1] : NULL;

    if (options && has_help_option(options)) {
        ret = print_block_option_help(out_filename, out_fmt);
        goto out;
    }

    if (bs_n < 1) {
        help();
    }


    if (bs_n > 1 && out_baseimg) {
        error_report("-B makes no sense when concatenating multiple input "
                     "images");
        ret = -1;
        goto out;
    }

    src_flags = BDRV_O_FLAGS;
    ret = bdrv_parse_cache_flags(src_cache, &src_flags);
    if (ret < 0) {
        error_report("Invalid source cache option: %s", src_cache);
        goto out;
    }

    qemu_progress_print(0, 100);

    bs = qemu_mallocz(bs_n * sizeof(BlockDriverState *));

    total_sectors = 0;
    for (bs_i = 0; bs_i < bs_n; bs_i++) {
        bs[bs_i] = bdrv_new_open(argv[optind + bs_i], fmt, src_flags, true);
        if (!bs[bs_i]) {
            error_report("Could not open '%s'", argv[optind + bs_i]);
            ret = -1;
            goto out;
        }
        bdrv_get_geometry(bs[bs_i], &bs_sectors);
        total_sectors += bs_sectors;
    }

    /* Find driver and parse its options */
    drv = bdrv_find_format(out_fmt);
    if (!drv) {
        error_report("Unknown file format '%s'", out_fmt);
        ret = -1;
        goto out;
    }

    proto_drv = bdrv_find_protocol(out_filename, &local_err);
    if (!proto_drv) {
        qerror_report_err(local_err);
        error_free(local_err);
        ret = -1;
        goto out;
    }

    create_options = append_option_parameters(create_options,
                                              drv->create_options);
    create_options = append_option_parameters(create_options,
                                              proto_drv->create_options);

    if (options) {
        param = parse_option_parameters(options, create_options, param);
        if (param == NULL) {
            error_report("Invalid options for file format '%s'.", out_fmt);
            ret = -1;
            goto out;
        }
    } else {
        param = parse_option_parameters("", create_options, param);
    }

    set_option_parameter_int(param, BLOCK_OPT_SIZE, total_sectors * 512);
    ret = add_old_style_options(out_fmt, param, out_baseimg, NULL);
    if (ret < 0) {
        goto out;
    }

    /* Get backing file name if -o backing_file was used */
    out_baseimg_param = get_option_parameter(param, BLOCK_OPT_BACKING_FILE);
    if (out_baseimg_param) {
        out_baseimg = out_baseimg_param->value.s;
    }

    /* Check if compression is supported */
    if (compress) {
        QEMUOptionParameter *encryption =
            get_option_parameter(param, BLOCK_OPT_ENCRYPT);

        if (!drv->bdrv_write_compressed) {
            error_report("Compression not supported for this file format");
            ret = -1;
            goto out;
        }

        if (encryption && encryption->value.n) {
            error_report("Compression and encryption not supported at "
                         "the same time");
            ret = -1;
            goto out;
        }
    }

    /* Create the new image */
    ret = bdrv_create(drv, out_filename, param);
    if (ret < 0) {
        if (ret == -ENOTSUP) {
            error_report("Formatting not supported for file format '%s'",
                         out_fmt);
        } else if (ret == -EFBIG) {
            error_report("The image size is too large for file format '%s'",
                         out_fmt);
        } else {
            error_report("%s: error while converting %s: %s",
                         out_filename, out_fmt, strerror(-ret));
        }
        goto out;
    }

    flags = BDRV_O_RDWR;
    ret = bdrv_parse_cache_flags(cache, &flags);
    if (ret < 0) {
        error_report("Invalid cache option: %s", cache);
        goto out;
    }

    out_bs = bdrv_new_open(out_filename, out_fmt, flags, true);
    if (!out_bs) {
        ret = -1;
        goto out;
    }

    bs_i = 0;
    bs_offset = 0;
    bdrv_get_geometry(bs[0], &bs_sectors);
    buf = qemu_blockalign(out_bs, IO_BUF_SIZE);

    if (compress) {
        ret = bdrv_get_info(out_bs, &bdi);
        if (ret < 0) {
            error_report("could not get block driver info");
            goto out;
        }
        cluster_size = bdi.cluster_size;
        if (cluster_size <= 0 || cluster_size > IO_BUF_SIZE) {
            error_report("invalid cluster size");
            ret = -1;
            goto out;
        }
        cluster_sectors = cluster_size >> 9;
        sector_num = 0;

        nb_sectors = total_sectors;

        for(;;) {
            int64_t bs_num;
            int remainder;
            uint8_t *buf2;

            nb_sectors = total_sectors - sector_num;
            if (nb_sectors <= 0)
                break;
            if (nb_sectors >= cluster_sectors)
                n = cluster_sectors;
            else
                n = nb_sectors;

            bs_num = sector_num - bs_offset;
            assert (bs_num >= 0);
            remainder = n;
            buf2 = buf;
            while (remainder > 0) {
                int nlow;
                while (bs_num == bs_sectors) {
                    bs_i++;
                    assert (bs_i < bs_n);
                    bs_offset += bs_sectors;
                    bdrv_get_geometry(bs[bs_i], &bs_sectors);
                    bs_num = 0;
                    /* printf("changing part: sector_num=%" PRId64 ", "
                       "bs_i=%d, bs_offset=%" PRId64 ", bs_sectors=%" PRId64
                       "\n", sector_num, bs_i, bs_offset, bs_sectors); */
                }
                assert (bs_num < bs_sectors);

                nlow = (remainder > bs_sectors - bs_num) ? bs_sectors - bs_num : remainder;

                ret = bdrv_read(bs[bs_i], bs_num, buf2, nlow);
                if (ret < 0) {
                    error_report("error while reading sector %" PRId64 ": %s",
                                 bs_num, strerror(-ret));
                    goto out;
                }

                buf2 += nlow * 512;
                bs_num += nlow;

                remainder -= nlow;
            }
            assert (remainder == 0);

            if (n < cluster_sectors) {
                memset(buf + n * 512, 0, cluster_size - n * 512);
            }
            if (!buffer_is_zero(buf, cluster_size)) {
                ret = bdrv_write_compressed(out_bs, sector_num, buf,
                                            cluster_sectors);
                if (ret != 0) {
                    error_report("error while compressing sector %" PRId64
                                 ": %s", sector_num, strerror(-ret));
                    goto out;
                }
            }
            sector_num += n;
            qemu_progress_print(100.0 * sector_num / total_sectors, 0);
        }
        /* signal EOF to align */
        bdrv_write_compressed(out_bs, 0, NULL, 0);
    } else {
        int has_zero_init = bdrv_has_zero_init(out_bs);

        sector_num = 0; // total number of sectors converted so far
        nb_sectors = total_sectors - sector_num;

        for(;;) {
            nb_sectors = total_sectors - sector_num;
            if (nb_sectors <= 0) {
                ret = 0;
                break;
            }

            while (sector_num - bs_offset >= bs_sectors) {
                bs_i ++;
                assert (bs_i < bs_n);
                bs_offset += bs_sectors;
                bdrv_get_geometry(bs[bs_i], &bs_sectors);
                /* printf("changing part: sector_num=%" PRId64 ", bs_i=%d, "
                  "bs_offset=%" PRId64 ", bs_sectors=%" PRId64 "\n",
                   sector_num, bs_i, bs_offset, bs_sectors); */
            }

            if ((out_baseimg || has_zero_init) &&
                sector_num >= sector_num_next_status) {
                n = nb_sectors > INT_MAX ? INT_MAX : nb_sectors;
                ret = bdrv_get_block_status(bs[bs_i], sector_num - bs_offset,
                                            n, &n1);
                if (ret < 0) {
                    error_report("error while reading block status of sector %"
                                 PRId64 ": %s", sector_num - bs_offset,
                                 strerror(-ret));
                    goto out;
                }
                /* If the output image is zero initialized, we are not working
                 * on a shared base and the input is zero we can skip the next
                 * n1 sectors */
                if (has_zero_init && !out_baseimg && (ret & BDRV_BLOCK_ZERO)) {
                    sector_num += n1;
                    continue;
                }
                /* If the output image is being created as a copy on write
                 * image, assume that sectors which are unallocated in the
                 * input image are present in both the output's and input's
                 * base images (no need to copy them). */
                if (out_baseimg) {
                    if (!(ret & BDRV_BLOCK_DATA)) {
                        sector_num += n1;
                        continue;
                    }
                    /* The next 'n1' sectors are allocated in the input image.
                     * Copy only those as they may be followed by unallocated
                     * sectors. */
                    nb_sectors = n1;
                }
                /* avoid redundant callouts to get_block_status */
                sector_num_next_status = sector_num + n1;
            }

            n = MIN(nb_sectors, IO_BUF_SIZE / 512);
            if (sector_num_next_status) {
                assert(sector_num_next_status > sector_num);
                n = MIN(n, sector_num_next_status - sector_num);
            }
            n = MIN(n, bs_sectors - (sector_num - bs_offset));
            n1 = n;

            ret = bdrv_read(bs[bs_i], sector_num - bs_offset, buf, n);
            if (ret < 0) {
                error_report("error while reading sector %" PRId64 ": %s",
                             sector_num - bs_offset, strerror(-ret));
                goto out;
            }
            /* NOTE: at the same time we convert, we do not write zero
               sectors to have a chance to compress the image. Ideally, we
               should add a specific call to have the info to go faster */
            buf1 = buf;
            while (n > 0) {
                if (!has_zero_init ||
                    is_allocated_sectors_min(buf1, n, &n1, min_sparse)) {
                    ret = bdrv_write(out_bs, sector_num, buf1, n1);
                    if (ret < 0) {
                        error_report("error while writing sector %" PRId64
                                     ": %s", sector_num, strerror(-ret));
                        goto out;
                    }
                }
                sector_num += n1;
                n -= n1;
                buf1 += n1 * 512;
            }
            qemu_progress_print(100.0 * sector_num / total_sectors, 0);
        }
    }
out:
    if (!ret) {
        qemu_progress_print(100, 0);
    }
    qemu_progress_end();
    free_option_parameters(create_options);
    free_option_parameters(param);
    qemu_vfree(buf);
    if (out_bs) {
        bdrv_delete(out_bs);
    }
    if (bs) {
        for (bs_i = 0; bs_i < bs_n; bs_i++) {
            if (bs[bs_i]) {
                bdrv_delete(bs[bs_i]);
            }
        }
        qemu_free(bs);
    }
fail_getopt:
    g_free(options);

    if (ret) {
        return 1;
    }
    return 0;
}


static void dump_snapshots(BlockDriverState *bs)
{
    QEMUSnapshotInfo *sn_tab, *sn;
    int nb_sns, i;
    char buf[256];

    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns <= 0)
        return;
    printf("Snapshot list:\n");
    printf("%s\n", bdrv_snapshot_dump(buf, sizeof(buf), NULL));
    for(i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        printf("%s\n", bdrv_snapshot_dump(buf, sizeof(buf), sn));
    }
    qemu_free(sn_tab);
}

static void dump_json_image_info_list(ImageInfoList *list)
{
    Error *errp = NULL;
    QString *str;
    QmpOutputVisitor *ov = qmp_output_visitor_new();
    QObject *obj;
    visit_type_ImageInfoList(qmp_output_get_visitor(ov),
                             &list, NULL, &errp);
    obj = qmp_output_get_qobject(ov);
    str = qobject_to_json_pretty(obj);
    assert(str != NULL);
    printf("%s\n", qstring_get_str(str));
    qobject_decref(obj);
    qmp_output_visitor_cleanup(ov);
    QDECREF(str);
}

static void collect_snapshots(BlockDriverState *bs , ImageInfo *info)
{
    int i, sn_count;
    QEMUSnapshotInfo *sn_tab = NULL;
    SnapshotInfoList *info_list, *cur_item = NULL;
    sn_count = bdrv_snapshot_list(bs, &sn_tab);

    for (i = 0; i < sn_count; i++) {
        info->has_snapshots = true;
        info_list = g_new0(SnapshotInfoList, 1);

        info_list->value                = g_new0(SnapshotInfo, 1);
        info_list->value->id            = g_strdup(sn_tab[i].id_str);
        info_list->value->name          = g_strdup(sn_tab[i].name);
        info_list->value->vm_state_size = sn_tab[i].vm_state_size;
        info_list->value->date_sec      = sn_tab[i].date_sec;
        info_list->value->date_nsec     = sn_tab[i].date_nsec;
        info_list->value->vm_clock_sec  = sn_tab[i].vm_clock_nsec / 1000000000;
        info_list->value->vm_clock_nsec = sn_tab[i].vm_clock_nsec % 1000000000;

        /* XXX: waiting for the qapi to support qemu-queue.h types */
        if (!cur_item) {
            info->snapshots = cur_item = info_list;
        } else {
            cur_item->next = info_list;
            cur_item = info_list;
        }

    }

    g_free(sn_tab);
}

static void dump_json_image_info(ImageInfo *info)
{
    Error *errp = NULL;
    QString *str;
    QmpOutputVisitor *ov = qmp_output_visitor_new();
    QObject *obj;
    visit_type_ImageInfo(qmp_output_get_visitor(ov),
                         &info, NULL, &errp);
    obj = qmp_output_get_qobject(ov);
    str = qobject_to_json_pretty(obj);
    assert(str != NULL);
    printf("%s\n", qstring_get_str(str));
    qobject_decref(obj);
    qmp_output_visitor_cleanup(ov);
    QDECREF(str);
}

static void collect_image_info(BlockDriverState *bs,
                   ImageInfo *info,
                   const char *filename,
                   const char *fmt)
{
    uint64_t total_sectors;
    char backing_filename[1024];
    char backing_filename2[1024];
    BlockDriverInfo bdi;

    bdrv_get_geometry(bs, &total_sectors);

    info->filename        = g_strdup(filename);
    info->format          = g_strdup(bdrv_get_format_name(bs));
    info->virtual_size    = total_sectors * 512;
    info->actual_size     = bdrv_get_allocated_file_size(bs);
    info->has_actual_size = info->actual_size >= 0;
    if (bdrv_is_encrypted(bs)) {
        info->encrypted = true;
        info->has_encrypted = true;
    }
    if (bdrv_get_info(bs, &bdi) >= 0) {
        if (bdi.cluster_size != 0) {
            info->cluster_size = bdi.cluster_size;
            info->has_cluster_size = true;
        }
        info->dirty_flag = bdi.is_dirty;
        info->has_dirty_flag = true;
    }
    bdrv_get_backing_filename(bs, backing_filename, sizeof(backing_filename));
    if (backing_filename[0] != '\0') {
        info->backing_filename = g_strdup(backing_filename);
        info->has_backing_filename = true;
        bdrv_get_full_backing_filename(bs, filename, backing_filename2,
                                       sizeof(backing_filename2));

        if (strcmp(backing_filename, backing_filename2) != 0) {
            info->full_backing_filename =
                        g_strdup(backing_filename2);
            info->has_full_backing_filename = true;
        }

        if (bs->backing_format[0]) {
            info->backing_filename_format = g_strdup(bs->backing_format);
            info->has_backing_filename_format = true;
        }
    }
}

static void dump_human_image_info(ImageInfo *info)
{
    char size_buf[128], dsize_buf[128];
    if (!info->has_actual_size) {
        snprintf(dsize_buf, sizeof(dsize_buf), "unavailable");
    } else {
        get_human_readable_size(dsize_buf, sizeof(dsize_buf),
                                info->actual_size);
    }
    get_human_readable_size(size_buf, sizeof(size_buf), info->virtual_size);
    printf("image: %s\n"
           "file format: %s\n"
           "virtual size: %s (%" PRId64 " bytes)\n"
           "disk size: %s\n",
           info->filename, info->format, size_buf,
           info->virtual_size,
           dsize_buf);

    if (info->has_encrypted && info->encrypted) {
        printf("encrypted: yes\n");
    }

    if (info->has_cluster_size) {
        printf("cluster_size: %" PRId64 "\n", info->cluster_size);
    }

    if (info->has_dirty_flag && info->dirty_flag) {
        printf("cleanly shut down: no\n");
    }

    if (info->has_backing_filename) {
        printf("backing file: %s", info->backing_filename);
        if (info->has_full_backing_filename) {
            printf(" (actual path: %s)", info->full_backing_filename);
        }
        putchar('\n');
        if (info->has_backing_filename_format) {
            printf("backing file format: %s\n", info->backing_filename_format);
        }
    }

    if (info->has_snapshots) {
        SnapshotInfoList *elem;
        char buf[256];

        printf("Snapshot list:\n");
        printf("%s\n", bdrv_snapshot_dump(buf, sizeof(buf), NULL));

        /* Ideally bdrv_snapshot_dump() would operate on SnapshotInfoList but
         * we convert to the block layer's native QEMUSnapshotInfo for now.
         */
        for (elem = info->snapshots; elem; elem = elem->next) {
            QEMUSnapshotInfo sn = {
                .vm_state_size = elem->value->vm_state_size,
                .date_sec = elem->value->date_sec,
                .date_nsec = elem->value->date_nsec,
                .vm_clock_nsec = elem->value->vm_clock_sec * 1000000000ULL +
                                 elem->value->vm_clock_nsec,
            };

            pstrcpy(sn.id_str, sizeof(sn.id_str), elem->value->id);
            pstrcpy(sn.name, sizeof(sn.name), elem->value->name);
            printf("%s\n", bdrv_snapshot_dump(buf, sizeof(buf), &sn));
        }
    }
}

static void dump_human_image_info_list(ImageInfoList *list)
{
    ImageInfoList *elem;
    bool delim = false;

    for (elem = list; elem; elem = elem->next) {
        if (delim) {
            printf("\n");
        }
        delim = true;

        dump_human_image_info(elem->value);
    }
}

static gboolean str_equal_func(gconstpointer a, gconstpointer b)
{
    return strcmp(a, b) == 0;
}

/**
 * Open an image file chain and return an ImageInfoList
 *
 * @filename: topmost image filename
 * @fmt: topmost image format (may be NULL to autodetect)
 * @chain: true  - enumerate entire backing file chain
 *         false - only topmost image file
 *
 * Returns a list of ImageInfo objects or NULL if there was an error opening an
 * image file.  If there was an error a message will have been printed to
 * stderr.
 */
static ImageInfoList *collect_image_info_list(const char *filename,
                                              const char *fmt,
                                              bool chain)
{
    ImageInfoList *head = NULL;
    ImageInfoList **last = &head;
    GHashTable *filenames;

    filenames = g_hash_table_new_full(g_str_hash, str_equal_func, NULL, NULL);

    while (filename) {
        BlockDriverState *bs;
        ImageInfo *info;
        ImageInfoList *elem;

        if (g_hash_table_lookup_extended(filenames, filename, NULL, NULL)) {
            error_report("Backing file '%s' creates an infinite loop.",
                         filename);
            goto err;
        }
        g_hash_table_insert(filenames, (gpointer)filename, NULL);

        bs = bdrv_new_open(filename, fmt, BDRV_O_FLAGS | BDRV_O_NO_BACKING,
                           false);
        if (!bs) {
            goto err;
        }

        info = g_new0(ImageInfo, 1);
        collect_image_info(bs, info, filename, fmt);
        collect_snapshots(bs, info);

        elem = g_new0(ImageInfoList, 1);
        elem->value = info;
        *last = elem;
        last = &elem->next;

        bdrv_delete(bs);

        filename = fmt = NULL;
        if (chain) {
            if (info->has_full_backing_filename) {
                filename = info->full_backing_filename;
            } else if (info->has_backing_filename) {
                filename = info->backing_filename;
            }
            if (info->has_backing_filename_format) {
                fmt = info->backing_filename_format;
            }
        }
    }
    g_hash_table_destroy(filenames);
    return head;

err:
    qapi_free_ImageInfoList(head);
    g_hash_table_destroy(filenames);
    return NULL;
}

static int img_info(int argc, char **argv)
{
    int c;
    OutputFormat output_format = OFORMAT_HUMAN;
    bool chain = false;
    const char *filename, *fmt, *output;
    ImageInfoList *list;

    fmt = NULL;
    output = NULL;
    for(;;) {
        int option_index = 0;
        static const struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"format", required_argument, 0, 'f'},
            {"output", required_argument, 0, OPTION_OUTPUT},
            {"backing-chain", no_argument, 0, OPTION_BACKING_CHAIN},
            {0, 0, 0, 0}
        };
        c = getopt_long(argc, argv, "f:h",
                        long_options, &option_index);
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt = optarg;
            break;
        case OPTION_OUTPUT:
            output = optarg;
            break;
        case OPTION_BACKING_CHAIN:
            chain = true;
            break;
        }
    }
    if (optind >= argc) {
        help();
    }
    filename = argv[optind++];

    if (output && !strcmp(output, "json")) {
        output_format = OFORMAT_JSON;
    } else if (output && !strcmp(output, "human")) {
        output_format = OFORMAT_HUMAN;
    } else if (output) {
        error_report("--output must be used with human or json as argument.");
        return 1;
    }

    list = collect_image_info_list(filename, fmt, chain);
    if (!list) {
        return 1;
    }

    switch (output_format) {
    case OFORMAT_HUMAN:
        dump_human_image_info_list(list);
        break;
    case OFORMAT_JSON:
        if (chain) {
            dump_json_image_info_list(list);
        } else {
            dump_json_image_info(list->value);
        }
        break;
    }

    qapi_free_ImageInfoList(list);
    return 0;
}


typedef struct MapEntry {
    int flags;
    int depth;
    int64_t start;
    int64_t length;
    int64_t offset;
    BlockDriverState *bs;
} MapEntry;

static void dump_map_entry(OutputFormat output_format, MapEntry *e,
                           MapEntry *next)
{
    switch (output_format) {
    case OFORMAT_HUMAN:
        if ((e->flags & BDRV_BLOCK_DATA) &&
            !(e->flags & BDRV_BLOCK_OFFSET_VALID)) {
            error_report("File contains external, encrypted or compressed clusters.");
            exit(1);
        }
        if ((e->flags & (BDRV_BLOCK_DATA|BDRV_BLOCK_ZERO)) == BDRV_BLOCK_DATA) {
            printf("%#-16"PRIx64"%#-16"PRIx64"%#-16"PRIx64"%s\n",
                   e->start, e->length, e->offset, e->bs->filename);
        }
        /* This format ignores the distinction between 0, ZERO and ZERO|DATA.
         * Modify the flags here to allow more coalescing.
         */
        if (next &&
            (next->flags & (BDRV_BLOCK_DATA|BDRV_BLOCK_ZERO)) != BDRV_BLOCK_DATA) {
            next->flags &= ~BDRV_BLOCK_DATA;
            next->flags |= BDRV_BLOCK_ZERO;
        }
        break;
    case OFORMAT_JSON:
        printf("%s{ \"start\": %"PRId64", \"length\": %"PRId64", \"depth\": %d,"
               " \"zero\": %s, \"data\": %s",
               (e->start == 0 ? "[" : ",\n"),
               e->start, e->length, e->depth,
               (e->flags & BDRV_BLOCK_ZERO) ? "true" : "false",
               (e->flags & BDRV_BLOCK_DATA) ? "true" : "false");
        if (e->flags & BDRV_BLOCK_OFFSET_VALID) {
            printf(", \"offset\": %"PRId64"", e->offset);
        }
        putchar('}');

        if (!next) {
            printf("]\n");
        }
        break;
    }
}

static int get_block_status(BlockDriverState *bs, int64_t sector_num,
                            int nb_sectors, MapEntry *e)
{
    int64_t ret;
    int depth;

    /* As an optimization, we could cache the current range of unallocated
     * clusters in each file of the chain, and avoid querying the same
     * range repeatedly.
     */

    depth = 0;
    for (;;) {
        ret = bdrv_get_block_status(bs, sector_num, nb_sectors, &nb_sectors);
        if (ret < 0) {
            return ret;
        }
        assert(nb_sectors);
        if (ret & (BDRV_BLOCK_ZERO|BDRV_BLOCK_DATA)) {
            break;
        }
        bs = bs->backing_hd;
        if (bs == NULL) {
            ret = 0;
            break;
        }

        depth++;
    }

    e->start = sector_num * BDRV_SECTOR_SIZE;
    e->length = nb_sectors * BDRV_SECTOR_SIZE;
    e->flags = ret & ~BDRV_BLOCK_OFFSET_MASK;
    e->offset = ret & BDRV_BLOCK_OFFSET_MASK;
    e->depth = depth;
    e->bs = bs;
    return 0;
}

static int img_map(int argc, char **argv)
{
    int c;
    OutputFormat output_format = OFORMAT_HUMAN;
    BlockDriverState *bs;
    const char *filename, *fmt, *output;
    int64_t length;
    MapEntry curr = { .length = 0 }, next;
    int ret = 0;

    fmt = NULL;
    output = NULL;
    for (;;) {
        int option_index = 0;
        static const struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"format", required_argument, 0, 'f'},
            {"output", required_argument, 0, OPTION_OUTPUT},
            {0, 0, 0, 0}
        };
        c = getopt_long(argc, argv, "f:h",
                        long_options, &option_index);
        if (c == -1) {
            break;
        }
        switch (c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt = optarg;
            break;
        case OPTION_OUTPUT:
            output = optarg;
            break;
        }
    }
    if (optind >= argc) {
        help();
    }
    filename = argv[optind++];

    if (output && !strcmp(output, "json")) {
        output_format = OFORMAT_JSON;
    } else if (output && !strcmp(output, "human")) {
        output_format = OFORMAT_HUMAN;
    } else if (output) {
        error_report("--output must be used with human or json as argument.");
        return 1;
    }

    bs = bdrv_new_open(filename, fmt, BDRV_O_FLAGS, true);
    if (!bs) {
        return 1;
    }

    if (output_format == OFORMAT_HUMAN) {
        printf("%-16s%-16s%-16s%s\n", "Offset", "Length", "Mapped to", "File");
    }

    length = bdrv_getlength(bs);
    while (curr.start + curr.length < length) {
        int64_t nsectors_left;
        int64_t sector_num;
        int n;

        sector_num = (curr.start + curr.length) >> BDRV_SECTOR_BITS;

        /* Probe up to 1 GiB at a time.  */
        nsectors_left = DIV_ROUND_UP(length, BDRV_SECTOR_SIZE) - sector_num;
        n = MIN(1 << (30 - BDRV_SECTOR_BITS), nsectors_left);
        ret = get_block_status(bs, sector_num, n, &next);

        if (ret < 0) {
            error_report("Could not read file metadata: %s", strerror(-ret));
            goto out;
        }

        if (curr.length != 0 && curr.flags == next.flags &&
            curr.depth == next.depth &&
            ((curr.flags & BDRV_BLOCK_OFFSET_VALID) == 0 ||
             curr.offset + curr.length == next.offset)) {
            curr.length += next.length;
            continue;
        }

        if (curr.length > 0) {
            dump_map_entry(output_format, &curr, &next);
        }
        curr = next;
    }

    dump_map_entry(output_format, &curr, NULL);

out:
    bdrv_close(bs);
    bdrv_delete(bs);
    return ret < 0;
}

#define SNAPSHOT_LIST   1
#define SNAPSHOT_CREATE 2
#define SNAPSHOT_APPLY  3
#define SNAPSHOT_DELETE 4

static int img_snapshot(int argc, char **argv)
{
    BlockDriverState *bs;
    QEMUSnapshotInfo sn;
    char *filename, *snapshot_name = NULL;
    int c, ret = 0, bdrv_oflags;
    int action = 0;
    qemu_timeval tv;

    bdrv_oflags = BDRV_O_FLAGS | BDRV_O_RDWR;
    /* Parse commandline parameters */
    for(;;) {
        c = getopt(argc, argv, "la:c:d:h");
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            return 0;
        case 'l':
            if (action) {
                help();
                return 0;
            }
            action = SNAPSHOT_LIST;
            bdrv_oflags &= ~BDRV_O_RDWR; /* no need for RW */
            break;
        case 'a':
            if (action) {
                help();
                return 0;
            }
            action = SNAPSHOT_APPLY;
            snapshot_name = optarg;
            break;
        case 'c':
            if (action) {
                help();
                return 0;
            }
            action = SNAPSHOT_CREATE;
            snapshot_name = optarg;
            break;
        case 'd':
            if (action) {
                help();
                return 0;
            }
            action = SNAPSHOT_DELETE;
            snapshot_name = optarg;
            break;
        }
    }

    if (optind >= argc) {
        help();
    }
    filename = argv[optind++];

    /* Open the image */
    bs = bdrv_new_open(filename, NULL, bdrv_oflags, true);
    if (!bs) {
        return 1;
    }

    /* Perform the requested action */
    switch(action) {
    case SNAPSHOT_LIST:
        dump_snapshots(bs);
        break;

    case SNAPSHOT_CREATE:
        memset(&sn, 0, sizeof(sn));
        pstrcpy(sn.name, sizeof(sn.name), snapshot_name);

        qemu_gettimeofday(&tv);
        sn.date_sec = tv.tv_sec;
        sn.date_nsec = tv.tv_usec * 1000;

        ret = bdrv_snapshot_create(bs, &sn);
        if (ret) {
            error_report("Could not create snapshot '%s': %d (%s)",
                snapshot_name, ret, strerror(-ret));
        }
        break;

    case SNAPSHOT_APPLY:
        ret = bdrv_snapshot_goto(bs, snapshot_name);
        if (ret) {
            error_report("Could not apply snapshot '%s': %d (%s)",
                snapshot_name, ret, strerror(-ret));
        }
        break;

    case SNAPSHOT_DELETE:
        ret = bdrv_snapshot_delete(bs, snapshot_name);
        if (ret) {
            error_report("Could not delete snapshot '%s': %d (%s)",
                snapshot_name, ret, strerror(-ret));
        }
        break;
    }

    /* Cleanup */
    bdrv_delete(bs);
    if (ret) {
        return 1;
    }
    return 0;
}

static int img_rebase(int argc, char **argv)
{
    BlockDriverState *bs, *bs_old_backing = NULL, *bs_new_backing = NULL;
    BlockDriver *old_backing_drv, *new_backing_drv;
    char *filename;
    const char *fmt, *cache, *src_cache, *out_basefmt, *out_baseimg;
    int c, flags, src_flags, ret;
    int unsafe = 0;
    int progress = 0;

    /* Parse commandline parameters */
    fmt = NULL;
    cache = BDRV_DEFAULT_CACHE;
    src_cache = BDRV_DEFAULT_CACHE;
    out_baseimg = NULL;
    out_basefmt = NULL;
    for(;;) {
        c = getopt(argc, argv, "hf:F:b:upt:T:");
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            return 0;
        case 'f':
            fmt = optarg;
            break;
        case 'F':
            out_basefmt = optarg;
            break;
        case 'b':
            out_baseimg = optarg;
            break;
        case 'u':
            unsafe = 1;
            break;
        case 'p':
            progress = 1;
            break;
        case 't':
            cache = optarg;
            break;
        case 'T':
            src_cache = optarg;
            break;
        }
    }

    if ((optind >= argc) || (!unsafe && !out_baseimg)) {
        help();
    }
    filename = argv[optind++];

    qemu_progress_init(progress, 2.0);
    qemu_progress_print(0, 100);

    flags = BDRV_O_RDWR | (unsafe ? BDRV_O_NO_BACKING : 0);
    ret = bdrv_parse_cache_flags(cache, &flags);
    if (ret < 0) {
        error_report("Invalid cache option: %s\n", cache);
        return -1;
    }

    src_flags = BDRV_O_FLAGS;
    ret = bdrv_parse_cache_flags(src_cache, &src_flags);
    if (ret < 0) {
        error_report("Invalid source cache option: %s", src_cache);
        return -1;
    }

    /*
     * Open the images.
     *
     * Ignore the old backing file for unsafe rebase in case we want to correct
     * the reference to a renamed or moved backing file.
     */
    bs = bdrv_new_open(filename, fmt, flags, true);
    if (!bs) {
        return 1;
    }

    /* Find the right drivers for the backing files */
    old_backing_drv = NULL;
    new_backing_drv = NULL;

    if (!unsafe && bs->backing_format[0] != '\0') {
        old_backing_drv = bdrv_find_format(bs->backing_format);
        if (old_backing_drv == NULL) {
            error_report("Invalid format name: '%s'", bs->backing_format);
            ret = -1;
            goto out;
        }
    }

    if (out_basefmt != NULL) {
        new_backing_drv = bdrv_find_format(out_basefmt);
        if (new_backing_drv == NULL) {
            error_report("Invalid format name: '%s'", out_basefmt);
            ret = -1;
            goto out;
        }
    }

    /* For safe rebasing we need to compare old and new backing file */
    if (unsafe) {
        /* Make the compiler happy */
        bs_old_backing = NULL;
        bs_new_backing = NULL;
    } else {
        char backing_name[1024];

        bs_old_backing = bdrv_new("old_backing");
        bdrv_get_backing_filename(bs, backing_name, sizeof(backing_name));
        ret = bdrv_open(bs_old_backing, backing_name, src_flags,
                        old_backing_drv);
        if (ret) {
            error_report("Could not open old backing file '%s'", backing_name);
            goto out;
        }
        if (out_baseimg[0]) {
            bs_new_backing = bdrv_new("new_backing");
            ret = bdrv_open(bs_new_backing, out_baseimg, src_flags,
                        new_backing_drv);
            if (ret) {
                error_report("Could not open new backing file '%s'",
                             out_baseimg);
                goto out;
            }
        }
    }

    /*
     * Check each unallocated cluster in the COW file. If it is unallocated,
     * accesses go to the backing file. We must therefore compare this cluster
     * in the old and new backing file, and if they differ we need to copy it
     * from the old backing file into the COW file.
     *
     * If qemu-img crashes during this step, no harm is done. The content of
     * the image is the same as the original one at any time.
     */
    if (!unsafe) {
        uint64_t num_sectors;
        uint64_t old_backing_num_sectors;
        uint64_t new_backing_num_sectors = 0;
        uint64_t sector;
        int n;
        uint8_t * buf_old;
        uint8_t * buf_new;
        float local_progress = 0;

        buf_old = qemu_blockalign(bs, IO_BUF_SIZE);
        buf_new = qemu_blockalign(bs, IO_BUF_SIZE);

        bdrv_get_geometry(bs, &num_sectors);
        bdrv_get_geometry(bs_old_backing, &old_backing_num_sectors);
        if (bs_new_backing) {
            bdrv_get_geometry(bs_new_backing, &new_backing_num_sectors);
        }

        if (num_sectors != 0) {
            local_progress = (float)100 /
                (num_sectors / MIN(num_sectors, IO_BUF_SIZE / 512));
        }

        for (sector = 0; sector < num_sectors; sector += n) {

            /* How many sectors can we handle with the next read? */
            if (sector + (IO_BUF_SIZE / 512) <= num_sectors) {
                n = (IO_BUF_SIZE / 512);
            } else {
                n = num_sectors - sector;
            }

            /* If the cluster is allocated, we don't need to take action */
            ret = bdrv_is_allocated(bs, sector, n, &n);
            if (ret < 0) {
                error_report("error while reading image metadata: %s",
                             strerror(-ret));
                goto out;
            }
            if (ret) {
                continue;
            }

            /*
             * Read old and new backing file and take into consideration that
             * backing files may be smaller than the COW image.
             */
            if (sector >= old_backing_num_sectors) {
                memset(buf_old, 0, n * BDRV_SECTOR_SIZE);
            } else {
                if (sector + n > old_backing_num_sectors) {
                    n = old_backing_num_sectors - sector;
                }

                ret = bdrv_read(bs_old_backing, sector, buf_old, n);
                if (ret < 0) {
                    error_report("error while reading from old backing file");
                    goto out;
                }
            }

            if (sector >= new_backing_num_sectors || !bs_new_backing) {
                memset(buf_new, 0, n * BDRV_SECTOR_SIZE);
            } else {
                if (sector + n > new_backing_num_sectors) {
                    n = new_backing_num_sectors - sector;
                }

                ret = bdrv_read(bs_new_backing, sector, buf_new, n);
                if (ret < 0) {
                    error_report("error while reading from new backing file");
                    goto out;
                }
            }

            /* If they differ, we need to write to the COW file */
            uint64_t written = 0;

            while (written < n) {
                int pnum;

                if (compare_sectors(buf_old + written * 512,
                    buf_new + written * 512, n - written, &pnum))
                {
                    ret = bdrv_write(bs, sector + written,
                        buf_old + written * 512, pnum);
                    if (ret < 0) {
                        error_report("Error while writing to COW image: %s",
                            strerror(-ret));
                        goto out;
                    }
                }

                written += pnum;
            }
            qemu_progress_print(local_progress, 100);
        }

        qemu_vfree(buf_old);
        qemu_vfree(buf_new);
    }

    /*
     * Change the backing file. All clusters that are different from the old
     * backing file are overwritten in the COW file now, so the visible content
     * doesn't change when we switch the backing file.
     */
    if (out_baseimg && *out_baseimg) {
        ret = bdrv_change_backing_file(bs, out_baseimg, out_basefmt);
    } else {
        ret = bdrv_change_backing_file(bs, NULL, NULL);
    }

    if (ret == -ENOSPC) {
        error_report("Could not change the backing file to '%s': No "
                     "space left in the file header", out_baseimg);
    } else if (ret < 0) {
        error_report("Could not change the backing file to '%s': %s",
            out_baseimg, strerror(-ret));
    }

    qemu_progress_print(100, 0);
    /*
     * TODO At this point it is possible to check if any clusters that are
     * allocated in the COW file are the same in the backing file. If so, they
     * could be dropped from the COW file. Don't do this before switching the
     * backing file, in case of a crash this would lead to corruption.
     */
out:
    qemu_progress_end();
    /* Cleanup */
    if (!unsafe) {
        if (bs_old_backing != NULL) {
            bdrv_delete(bs_old_backing);
        }
        if (bs_new_backing != NULL) {
            bdrv_delete(bs_new_backing);
        }
    }

    bdrv_delete(bs);
    if (ret) {
        return 1;
    }
    return 0;
}

static int img_resize(int argc, char **argv)
{
    int c, ret, relative;
    const char *filename, *fmt, *size;
    int64_t n, total_size;
    BlockDriverState *bs = NULL;
    QEMUOptionParameter *param;
    QEMUOptionParameter resize_options[] = {
        {
            .name = BLOCK_OPT_SIZE,
            .type = OPT_SIZE,
            .help = "Virtual disk size"
        },
        { NULL }
    };

    fmt = NULL;
    for(;;) {
        c = getopt(argc, argv, "f:h");
        if (c == -1) {
            break;
        }
        switch(c) {
        case '?':
        case 'h':
            help();
            break;
        case 'f':
            fmt = optarg;
            break;
        }
    }
    if (optind + 1 >= argc) {
        help();
    }
    filename = argv[optind++];
    size = argv[optind++];

    /* Choose grow, shrink, or absolute resize mode */
    switch (size[0]) {
    case '+':
        relative = 1;
        size++;
        break;
    case '-':
        relative = -1;
        size++;
        break;
    default:
        relative = 0;
        break;
    }

    /* Parse size */
    param = parse_option_parameters("", resize_options, NULL);
    if (set_option_parameter(param, BLOCK_OPT_SIZE, size)) {
        /* Error message already printed when size parsing fails */
        ret = -1;
        goto out;
    }
    n = get_option_parameter(param, BLOCK_OPT_SIZE)->value.n;
    free_option_parameters(param);

    bs = bdrv_new_open(filename, fmt, BDRV_O_FLAGS | BDRV_O_RDWR, true);
    if (!bs) {
        ret = -1;
        goto out;
    }

    if (relative) {
        total_size = bdrv_getlength(bs) + n * relative;
    } else {
        total_size = n;
    }
    if (total_size <= 0) {
        error_report("New image size must be positive");
        ret = -1;
        goto out;
    }

    ret = bdrv_truncate(bs, total_size);
    switch (ret) {
    case 0:
        printf("Image resized.\n");
        break;
    case -ENOTSUP:
        error_report("This image format does not support resize");
        break;
    case -EACCES:
        error_report("Image is read-only");
        break;
    default:
        error_report("Error resizing image (%d)", -ret);
        break;
    }
out:
    if (bs) {
        bdrv_delete(bs);
    }
    if (ret) {
        return 1;
    }
    return 0;
}

static const img_cmd_t img_cmds[] = {
#define DEF(option, callback, arg_string)        \
    { option, callback },
#include "qemu-img-cmds.h"
#undef DEF
#undef GEN_DOCS
    { NULL, NULL, },
};

int main(int argc, char **argv)
{
    const img_cmd_t *cmd;
    const char *cmdname;

    bdrv_init();
    if (argc < 2)
        help();
    cmdname = argv[1];
    argc--; argv++;

    /* find the command */
    for(cmd = img_cmds; cmd->name != NULL; cmd++) {
        if (!strcmp(cmdname, cmd->name)) {
            return cmd->handler(argc, argv);
        }
    }

    /* not found */
    help();
    return 0;
}
