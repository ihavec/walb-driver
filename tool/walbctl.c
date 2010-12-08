/**
 * Control walb device.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "walb.h"
#include "walb_log_device.h"
#include "walb_log_record.h"
#include "random.h"
#include "walb_ioctl.h"

#include "util.h"
#include "logpack.h"
#include "walblog_format.h"


typedef struct config
{
        char *cmd_str; /* command string */
        char *ldev_name; /* log device name */
        char *ddev_name; /* data device name */

        /* size_t log_sector_size; /\* sector size of log device *\/ */
        /* size_t log_dev_size; /\* size of log device by the sector *\/ */

        int n_snapshots; /* maximum number of snapshots to keep */

        char *wdev_name; /* walb device */
        char *wldev_name;  /* walblog device */
        u64 lsid; /* lsid */

        u64 lsid0; /* from lsid */
        u64 lsid1; /* to lsid */
        
} config_t;

void show_help()
{
        printf("Usage: walbctl COMMAND OPTIONS\n"
               "\n"
               "COMMAND:\n"
               "  format_ldev LDEV DDEV (NSNAP) (SIZE)\n"
               "      Format log device.\n"
               "\n"
               "  (NIY)create_wdev LDEV DDEV NAME\n"
               "      Make walb/walblog device.\n"
               "\n"
               "  (NIY)create_snapshot WDEV NAME\n"
               "      Create snapshot.\n"
               "\n"
               "  (NIY)delete_snapshot WDEV NAME\n"
               "      Delete snapshot.\n"
               "\n"
               "  (NIY)num_snapshot WDEV (LRANGE | TRANGE | SRANGE)\n"
               "      Get number of snapshots.\n"
               "\n"
               "  (NIY)list_snapshot WDEV (LRANGE | TRANGE | SRANGE)\n"
               "      Get list of snapshots.\n"
               "\n"
               "  (NIY)checkpoint WDEV\n"
               "      Make checkpoint to reduce redo time after crash.\n"
               "\n"
               "  cat_wldev WLDEV (LRANGE) > WLOG\n"
               "      Extract wlog from walblog device.\n"
               "\n"
               "  show_wldev WLDEV (LRANGE)\n"
               "      Show wlog in walblog device.\n"
               "\n"
               "  show_wlog (LRANGE) < WLOG\n"
               "      Show wlog in stdin.\n"
               "\n"
               "  redo_wlog DDEV (LRANGE) < WLOG\n"
               "      Redo wlog to data device.\n"
               "\n"
               "  set_oldest_lsid WDEV LSID\n"
               "      Delete old logs in the device.\n"
               "\n"
               "  get_oldest_lsid WDEV\n"
               "      Get oldest_lsid in the device.\n"
               "\n"
               "OPTIONS:\n"
               "  N_SNAP: --n_snap [max number of snapshots]\n"
               "  SIZE:   --size [size of stuff]\n"
               "  LRANGE: --lsid0 [from lsid] --lsid1 [to lsid]\n"
               "  TRANGE: --time0 [from time] --time1 [to time]\n"
               "  SRANGE: --snap0 [from snapshot] --snap1 [to snapshot]\n"
               "  LSID:   --lsid [lsid]\n"
               "  DDEV:   --ddev [data device path]\n"
               "  LDEV:   --ldev [log device path]\n"
               "  WDEV:   --wdev [walb device path]\n"
               "  WLDEV:  --wldev [walblog device path]\n"
               "  NAME:   --name [name of stuff]\n"
               "  WLOG:   walb log data as stream\n"
               "\n"
               "NIY: Not Implemented Yet.\n"
                );
}

void init_config(config_t* cfg)
{
        ASSERT(cfg != NULL);

        cfg->n_snapshots = 10000;

        cfg->lsid0 = (u64)(-1);
        cfg->lsid1 = (u64)(-1);
}


enum {
        OPT_LDEV = 1,
        OPT_DDEV,
        OPT_N_SNAP,
        OPT_WDEV,
        OPT_WLDEV,
        OPT_LSID,
        OPT_LSID0,
        OPT_LSID1
};


int parse_opt(int argc, char* const argv[], config_t *cfg)
{
        int c;

        while (1) {
                int option_index = 0;
                static struct option long_options[] = {
                        {"ldev", 1, 0, OPT_LDEV}, /* log device */
                        {"ddev", 1, 0, OPT_DDEV}, /* data device */
                        {"n_snap", 1, 0, OPT_N_SNAP}, /* num of snapshots */
                        {"wdev", 1, 0, OPT_WDEV}, /* walb device */
                        {"wldev", 1, 0, OPT_WLDEV}, /* walb log device */
                        {"lsid", 1, 0, OPT_LSID}, /* lsid */
                        {"lsid0", 1, 0, OPT_LSID0},
                        {"lsid1", 1, 0, OPT_LSID1},
                        {0, 0, 0, 0}
                };

                c = getopt_long(argc, argv, "", long_options, &option_index);
                if (c == -1) {
                        break;
                }
                switch (c) {
                case OPT_LDEV:
                        cfg->ldev_name = strdup(optarg);
                        LOG("ldev: %s\n", optarg);
                        break;
                case OPT_DDEV:
                        cfg->ddev_name = strdup(optarg);
                        LOG("ddev: %s\n", optarg);
                        break;
                case OPT_N_SNAP:
                        cfg->n_snapshots = atoi(optarg);
                        break;
                case OPT_WDEV:
                        cfg->wdev_name = strdup(optarg);
                        break;
                case OPT_WLDEV:
                        cfg->wldev_name = strdup(optarg);
                        break;
                case OPT_LSID:
                        cfg->lsid = atoll(optarg);
                        break;
                case OPT_LSID0:
                        cfg->lsid0 = atoll(optarg);
                        break;
                case OPT_LSID1:
                        cfg->lsid1 = atoll(optarg);
                        break;
                default:
                        LOG("unknown option.\n");
                }
        }

        if (optind < argc) {
                LOG("command: ");
                while (optind < argc) {
                        cfg->cmd_str = strdup(argv[optind]);
                        LOG("%s ", argv[optind]);
                        optind ++;
                }
                LOG("\n");
        } else {
                show_help();
                return -1;
        }

        return 0;
}


/**
 * Initialize log device.
 *
 * @fd block device file descripter.
 * @logical_bs logical block size.
 * @physical_bs physical block size.
 * @ddev_lb device size [logical block].
 * @ldev_lb log device size [logical block]
 * @n_snapshots number of snapshots to keep.
 *
 * @return true in success, or false.
 */
bool init_walb_metadata(int fd, int logical_bs, int physical_bs,
                        u64 ddev_lb, u64 ldev_lb, int n_snapshots)
{
        ASSERT(fd >= 0);
        ASSERT(logical_bs > 0);
        ASSERT(physical_bs > 0);
        ASSERT(ddev_lb < (u64)(-1));
        ASSERT(ldev_lb < (u64)(-1));

        walb_super_sector_t super_sect;
        walb_snapshot_sector_t *snap_sectp;

        ASSERT(sizeof(super_sect) <= (size_t)physical_bs);
        ASSERT(sizeof(*snap_sectp) <= (size_t)physical_bs);

        /* Calculate number of snapshot sectors. */
        int n_sectors;
        int t = max_n_snapshots_in_sector(physical_bs);
        n_sectors = (n_snapshots + t - 1) / t;

        LOG("metadata_size: %d\n", n_sectors);

        /* Prepare super sector */
        memset(&super_sect, 0, sizeof(super_sect));

        super_sect.logical_bs = logical_bs;
        super_sect.physical_bs = physical_bs;
        super_sect.snapshot_metadata_size = n_sectors;
        generate_uuid(super_sect.uuid);
        
        super_sect.ring_buffer_size =
                ldev_lb / (physical_bs / logical_bs)
                - get_ring_buffer_offset(physical_bs, n_snapshots);

        super_sect.oldest_lsid = 0;
        super_sect.written_lsid = 0;
        super_sect.device_size = ddev_lb;

        /* Write super sector */
        if (! write_super_sector(fd, &super_sect)) {
                LOG("write super sector failed.\n");
                goto error0;
        }

        /* Prepare super sectors
           Bitmap data will be all 0. */
        snap_sectp = (walb_snapshot_sector_t *)alloc_sector_zero(physical_bs);
        if (snap_sectp == NULL) {
                goto error0;
        }
        
        /* Write metadata sectors */
        int i = 0;
        for (i = 0; i < n_sectors; i ++) {
                if (! write_snapshot_sector(fd, &super_sect, snap_sectp, i)) {
                        goto error1;
                }
        }

#if 1        
        /* Read super sector and print for debug. */
        memset(&super_sect, 0, sizeof(super_sect));
        if (! read_super_sector(fd, &super_sect, physical_bs, n_snapshots)) {
                goto error1;
        }
        /* print_super_sector(&super_sect); */

        /* Read first snapshot sector and print for debug. */
        memset(snap_sectp, 0, physical_bs);
        if (! read_snapshot_sector(fd, &super_sect, snap_sectp, 0)) {
                goto error1;
        }
        /* print_snapshot_sector(snap_sectp, physical_bs); */
        
#endif
        
        return true;

error1:
        free(snap_sectp);
error0:
        return false;
}


/**
 * Execute log device format.
 *
 * @return true in success, or false.
 */
bool do_format_ldev(const config_t *cfg)
{
        ASSERT(cfg->cmd_str);
        ASSERT(strcmp(cfg->cmd_str, "format_ldev") == 0);

        /*
         * Check devices.
         */
        if (check_bdev(cfg->ldev_name) < 0) {
                LOG("format_ldev: check log device failed %s.\n",
                    cfg->ldev_name);
        }
        if (check_bdev(cfg->ddev_name) < 0) {
                LOG("format_ldev: check data device failed %s.\n",
                    cfg->ddev_name);
        }

        /*
         * Block size.
         */
        int ldev_logical_bs = get_bdev_logical_block_size(cfg->ldev_name);
        int ddev_logical_bs = get_bdev_logical_block_size(cfg->ddev_name);
        int ldev_physical_bs = get_bdev_physical_block_size(cfg->ldev_name);
        int ddev_physical_bs = get_bdev_physical_block_size(cfg->ddev_name);
        if (ldev_logical_bs != ddev_logical_bs ||
            ldev_physical_bs != ddev_physical_bs) {
                LOG("logical or physical block size is different.\n");
                goto error0;
        }
        int logical_bs = ldev_logical_bs;
        int physical_bs = ldev_physical_bs;

        /*
         * Device size.
         */
        u64 ldev_size = get_bdev_size(cfg->ldev_name);
        u64 ddev_size = get_bdev_size(cfg->ddev_name);

        /*
         * Debug print.
         */
        LOG("logical_bs: %d\n"
            "physical_bs: %d\n"
            "ddev_size: %zu\n"
            "ldev_size: %zu\n",
            logical_bs, physical_bs, ddev_size, ldev_size);
        
        if (logical_bs <= 0 || physical_bs <= 0 ||
            ldev_size == (u64)(-1) || ldev_size == (u64)(-1) ) {
                LOG("getting block device parameters failed.\n");
                goto error0;
        }
        if (ldev_size % logical_bs != 0 ||
            ddev_size % logical_bs != 0) {
                LOG("device size is not multiple of logical_bs\n");
                goto error0;
        }
        
        int fd;
        fd = open(cfg->ldev_name, O_RDWR);
        if (fd < 0) {
                perror("open failed");
                goto error0;
        }

        if (! init_walb_metadata(fd, logical_bs, physical_bs,
                                 ddev_size / logical_bs,
                                 ldev_size / logical_bs,
                                 cfg->n_snapshots)) {

                LOG("initialize walb log device failed.\n");
                goto error1;
        }
        
        close(fd);
        return true;

error1:
        close(fd);
error0:
        return false;
}


/**
 * Cat logpack in specified range.
 *
 * @return true in success, or false.
 */
bool do_cat_wldev(const config_t *cfg)
{
        ASSERT(cfg->cmd_str);
        ASSERT(strcmp(cfg->cmd_str, "cat_wldev") == 0);

        /*
         * Check device.
         */
        if (check_bdev(cfg->wldev_name) < 0) {
                LOG("cat_wldev: check log device failed %s.\n",
                    cfg->wldev_name);
        }
        int logical_bs = get_bdev_logical_block_size(cfg->wldev_name);
        int physical_bs = get_bdev_physical_block_size(cfg->wldev_name);

        int fd = open(cfg->wldev_name, O_RDONLY);
        if (fd < 0) {
                perror("open failed");
                goto error0;
        }
        
        /* Allocate memory and read super block */
        walb_super_sector_t *super_sectp = 
                (walb_super_sector_t *)alloc_sector(physical_bs);
        if (super_sectp == NULL) { goto error1; }

        u64 off0 = get_super_sector0_offset(physical_bs);
        if (! read_sector(fd, (u8 *)super_sectp, physical_bs, off0)) {
                LOG("read super sector0 failed.\n");
                goto error1;
        }
        
        walb_logpack_header_t *logpack =
                (walb_logpack_header_t *)alloc_sector(physical_bs);
        if (logpack == NULL) { goto error2; }

        /* print_super_sector(super_sectp); */
        u64 oldest_lsid = super_sectp->oldest_lsid;
        LOG("oldest_lsid: %"PRIu64"\n", oldest_lsid);

        /* Range check */
        u64 lsid, begin_lsid, end_lsid;
        if (cfg->lsid0 == (u64)(-1)) {
                begin_lsid = oldest_lsid;
        } else {
                begin_lsid = cfg->lsid0;
        }
        if (cfg->lsid0 < oldest_lsid) {
                LOG("given lsid0 %"PRIu64" < oldest_lsid %"PRIu64"\n",
                    cfg->lsid0, oldest_lsid);
                goto error3;
        }
        end_lsid = cfg->lsid1;
        if (begin_lsid > end_lsid) {
                LOG("lsid0 < lsid1 property is required.\n");
                goto error3;
        }

        size_t bufsize = 1024 * 1024; /* 1MB */
        u8 *buf = alloc_sectors(physical_bs, bufsize / physical_bs);
        if (buf == NULL) {
                goto error3;
        }

        /* Prepare and write walblog_header. */
        walblog_header_t* wh = (walblog_header_t *)buf;
        ASSERT(WALBLOG_HEADER_SIZE <= bufsize);
        memset(wh, 0, WALBLOG_HEADER_SIZE);
        wh->header_size = WALBLOG_HEADER_SIZE;
        wh->sector_type = SECTOR_TYPE_WALBLOG_HEADER;
        wh->checksum = 0;
        wh->version = WALB_VERSION;
        wh->logical_bs = logical_bs;
        wh->physical_bs = physical_bs;
        copy_uuid(wh->uuid, super_sectp->uuid);
        wh->begin_lsid = begin_lsid;
        wh->end_lsid = end_lsid;
        /* Checksum */
        u32 wh_sum = checksum((const u8 *)wh, WALBLOG_HEADER_SIZE);
        wh->checksum = wh_sum;
        /* Write */
        write_data(1, buf, WALBLOG_HEADER_SIZE);
        LOG("lsid %"PRIu64" to %"PRIu64"\n", begin_lsid, end_lsid);

        /* Write each logpack to stdout. */
        lsid = begin_lsid;
        while (lsid < end_lsid) {

                /* Logpack header */
                if (! read_logpack_header(fd, super_sectp, lsid, logpack)) {
                        break;
                }
                LOG("logpack %"PRIu64"\n", logpack->logpack_lsid);
                write_logpack_header(1, super_sectp, logpack);
                
                /* Realloc buffer if buffer size is not enough. */
                if (bufsize / physical_bs < logpack->total_io_size) {
                        if (! realloc_sectors(&buf, physical_bs, logpack->total_io_size)) {
                                LOG("realloc_sectors failed.\n");
                                goto error3;
                        }
                        bufsize = (u32)logpack->total_io_size * physical_bs;
                        LOG("realloc_sectors called. %zu bytes\n", bufsize);
                }

                /* Logpack data. */
                if (! read_logpack_data(fd, super_sectp, logpack, buf, bufsize)) {
                        LOG("read logpack data failed.\n");
                        goto error4;
                }
                write_data(1, buf, logpack->total_io_size * physical_bs);
                
                lsid += logpack->total_io_size + 1;
        }

        free(buf);
        free(logpack);
        free(super_sectp);
        close(fd);
        return true;

error4:
        free(buf);
error3:
        free(logpack);
error2:
        free(super_sectp);
error1:
        close(fd);
error0:
        return false;
}

/**
 * Redo wlog.
 *
 * wlog is read from stdin.
 * --ddev (required)
 * --lsid0 (optional, default is the first lsid in the wlog.)
 * --lsid1 (optional, default is the last lsid in the wlog.)
 * redo logs of lsid0 <= lsid < lsid1.
 */
bool do_redo_wlog(const config_t *cfg)
{
        ASSERT(cfg->cmd_str);
        ASSERT(strcmp(cfg->cmd_str, "redo_wlog") == 0);

        walblog_header_t* wh = (walblog_header_t *)malloc(WALBLOG_HEADER_SIZE);
        if (wh == NULL) { goto error0; }

        /* Check data device. */
        if (check_bdev(cfg->ddev_name) < 0) {
                LOG("redo_wlog: check data device failed %s.\n",
                    cfg->ddev_name);
        }
        
        /* Read wlog header. */
        read_data(0, (u8 *)wh, WALBLOG_HEADER_SIZE);
        check_wlog_header(wh);
        print_wlog_header(wh); /* debug */

        /* Set block size */
        int lbs = wh->logical_bs;
        int pbs = wh->physical_bs;
        if (pbs % lbs != 0) {
                LOG("physical_bs %% logical_bs must be 0.\n");
                goto error1;
        }
        int n_lb_in_pb = pbs / lbs;

        int ddev_lbs = get_bdev_logical_block_size(cfg->ddev_name);
        int ddev_pbs = get_bdev_physical_block_size(cfg->ddev_name);
        if (ddev_lbs != lbs || ddev_pbs != pbs) {
                LOG("block size check is not valid\n"
                    "(wlog lbs %d, ddev lbs %d, wlog pbs %d, ddev pbs %d.\n",
                    lbs, ddev_lbs, pbs, ddev_pbs);
                goto error1;
        }
        
        
        
        /* now editing */
        

        /* Deside begin_lsid and end_lsid. */
        



        
        

        
        free(wh);
        return true;


error1:
        free(wh);
error0:
        return false;
}

/**
 * Show wlog from stdin.
 *
 */
bool do_show_wlog(const config_t *cfg)
{
        ASSERT(cfg->cmd_str);
        ASSERT(strcmp(cfg->cmd_str, "show_wlog") == 0);

        walblog_header_t* wh = (walblog_header_t *)malloc(WALBLOG_HEADER_SIZE);
        if (wh == NULL) { goto error0; }
        
        /* Read and print wlog header. */
        read_data(0, (u8 *)wh, WALBLOG_HEADER_SIZE);
        print_wlog_header(wh);

        /* Check wlog header. */
        check_wlog_header(wh);
        
        /* Set block size. */
        int logical_bs = wh->logical_bs;
        int physical_bs = wh->physical_bs;
        if (physical_bs % logical_bs != 0) {
                LOG("physical_bs %% logical_bs must be 0.\n");
                goto error1;
        }
        int n_lb_in_pb = physical_bs / logical_bs;

        /* Buffer for logpack header. */
        struct walb_logpack_header *logpack;
        logpack = (struct walb_logpack_header *)alloc_sector(physical_bs);
        if (logpack == NULL) { goto error1; }

        /* Buffer for logpack data. */
        size_t bufsize = 1024 * 1024; /* 1MB */
        u8 *buf = alloc_sectors(physical_bs, bufsize / physical_bs);
        if (buf == NULL) { goto error2; }
        
        
        /* Read, print and check each logpack */
        while (read_data(0, (u8 *)logpack, physical_bs)) {

                /* Print logpack header. */
                print_logpack_header(logpack);

                /* Check buffer size */
                u32 total_io_size = logpack->total_io_size;
                if (total_io_size * physical_bs > bufsize) {
                        if (! realloc_sectors(&buf, physical_bs, total_io_size)) {
                                LOG("realloc_sectors failed.\n");
                                goto error3;
                        }
                        bufsize = total_io_size * physical_bs;
                }

                /* Read logpack data */
                if (! read_data(0, buf, total_io_size * physical_bs)) {
                        LOG("read logpack data failed.\n");
                        goto error3;
                }

                /* Confirm checksum. */
                int i;
                for (i = 0; i < logpack->n_records; i ++) {

                        if (logpack->record[i].is_padding == 0) {

                                int off_pb = logpack->record[i].lsid_local - 1;

                                int size_lb = logpack->record[i].io_size;                                                     int size_pb;
                                if (size_lb % n_lb_in_pb == 0) {
                                        size_pb = size_lb / n_lb_in_pb;
                                } else {
                                        size_pb = size_lb / n_lb_in_pb + 1;
                                }
                                
                                if (checksum(buf + (off_pb * physical_bs), size_pb * physical_bs) == logpack->record[i].checksum) {
                                        printf("record %d: checksum valid\n", i);
                                } else {
                                        printf("record %d: checksum invalid\n", i);
                                }
                                
                                
                        } else {
                                printf("record %d: padding\n", i);
                        }

                }
        }

        free(buf);
        return true;

error3:
        free(buf);
error2:
        free(logpack);
error1:
        free(wh);
error0:
        return false;
}

/**
 * Show logpack header inside walblog device.
 *
 * @return true in success, or false.
 */
bool do_show_wldev(const config_t *cfg)
{
        ASSERT(strcmp(cfg->cmd_str, "show_wldev") == 0);

        /*
         * Check device.
         */
        if (check_bdev(cfg->wldev_name) < 0) {
                LOG("check log device failed %s.\n",
                    cfg->wldev_name);
        }
        int physical_bs = get_bdev_physical_block_size(cfg->wldev_name);

        int fd = open(cfg->wldev_name, O_RDONLY);
        if (fd < 0) {
                perror("open failed");
                goto error0;
        }
        
        /* Allocate memory and read super block */
        walb_super_sector_t *super_sectp = 
                (walb_super_sector_t *)alloc_sector(physical_bs);
        if (super_sectp == NULL) { goto error1; }

        u64 off0 = get_super_sector0_offset(physical_bs);
        if (! read_sector(fd, (u8 *)super_sectp, physical_bs, off0)) {
                LOG("read super sector0 failed.\n");
                goto error1;
        }
        
        walb_logpack_header_t *logpack =
                (walb_logpack_header_t *)alloc_sector(physical_bs);
        if (logpack == NULL) { goto error2; }

        print_super_sector(super_sectp);
        u64 oldest_lsid = super_sectp->oldest_lsid;
        LOG("oldest_lsid: %"PRIu64"\n", oldest_lsid);

        /* Range check */
        u64 lsid, begin_lsid, end_lsid;
        if (cfg->lsid0 == (u64)(-1)) {
                begin_lsid = oldest_lsid;
        } else {
                begin_lsid = cfg->lsid0;
        }
        if (cfg->lsid0 < oldest_lsid) {
                LOG("given lsid0 %"PRIu64" < oldest_lsid %"PRIu64"\n",
                    cfg->lsid0, oldest_lsid);
                goto error3;
        }
        end_lsid = cfg->lsid1;
        if (begin_lsid > end_lsid) {
                LOG("lsid0 < lsid1 property is required.\n");
                goto error3;
        }
        
        /* Print each logpack header. */
        lsid = begin_lsid;
        while (lsid < end_lsid) {
                if (! read_logpack_header(fd, super_sectp, lsid, logpack)) {
                        break;
                }
                print_logpack_header(logpack);
                lsid += logpack->total_io_size + 1;
        }

        free(logpack);
        free(super_sectp);
        close(fd);
        return true;

error3:
        free(logpack);
error2:
        free(super_sectp);
error1:
        close(fd);
error0:
        return false;
}

/**
 * Set oldest_lsid.
 */
bool do_set_oldest_lsid(const config_t *cfg)
{
        ASSERT(strcmp(cfg->cmd_str, "set_oldest_lsid") == 0);
        
        if (check_bdev(cfg->wdev_name) < 0) {
                LOG("set_oldest_lsid: check walb device failed %s.\n",
                    cfg->wdev_name);
        }
        int fd = open(cfg->wdev_name, O_RDWR);
        if (fd < 0) {
                perror("open failed");
                goto error0;
        }

        u64 lsid = cfg->lsid;
        int ret = ioctl(fd, WALB_IOCTL_SET_OLDESTLSID, &lsid);
        if (ret < 0) {
                LOG("set_oldest_lsid: ioctl failed.\n");
                goto error1;
        }
        printf("oldest_lsid is set to %"PRIu64" successfully.\n", lsid);
        close(fd);
        return true;
        
error1:
        close(fd);
error0:
        return false;
}

/**
 * Get oldest_lsid.
 */
bool do_get_oldest_lsid(const config_t *cfg)
{
        ASSERT(strcmp(cfg->cmd_str, "get_oldest_lsid") == 0);
        
        if (check_bdev(cfg->wdev_name) < 0) {
                LOG("get_oldest_lsid: check walb device failed %s.\n",
                    cfg->wdev_name);
        }
        int fd = open(cfg->wdev_name, O_RDONLY);
        if (fd < 0) {
                perror("open failed");
                goto error0;
        }

        u64 lsid;
        int ret = ioctl(fd, WALB_IOCTL_GET_OLDESTLSID, &lsid);
        if (ret < 0) {
                LOG("get_oldest_lsid: ioctl failed.\n");
                goto error1;
        }
        printf("oldest_lsid is %"PRIu64"\n", lsid);
        close(fd);
        return true;
        
error1:
        close(fd);
error0:
        return false;
}        


/**
 * For command string to function.
 */
typedef bool (*command_fn)(const config_t *);

struct map_str_to_fn {
        const char *str;
        command_fn fn;
};


/**
 * Dispatch command.
 */
bool dispatch(const config_t *cfg)
{
        bool ret = false;
        ASSERT(cfg->cmd_str != NULL);

        struct map_str_to_fn map[] = {
                { "format_ldev", do_format_ldev },
                /* { "create_wdev", do_create_wdev }, */
                /* { "create_snapshot", do_create_snapshot }, */
                /* { "delete_snapshot", do_delete_snapshot }, */
                /* { "num_snapshot", do_num_snapshot }, */
                /* { "list_snapshot", do_list_snapshot }, */
                /* { "checkpoint", do_checkpoint }, */
                { "cat_wldev", do_cat_wldev },
                { "show_wlog", do_show_wlog },
                { "show_wldev", do_show_wldev },
                { "redo_wlog", do_redo_wlog },
                { "set_oldest_lsid", do_set_oldest_lsid },
                { "get_oldest_lsid", do_get_oldest_lsid },
        };
        int array_size = sizeof(map)/sizeof(map[0]);

        int i;
        for (i = 0; i < array_size; i ++) {
                if (strcmp(cfg->cmd_str, map[i].str) == 0) {
                        ret = (*map[i].fn)(cfg);
                        break;
                }
        }
        
        return ret;
}

int main(int argc, char* argv[])
{
        config_t cfgt;

        init_random();
        init_config(&cfgt);
        
        if (parse_opt(argc, argv, &cfgt) != 0) {
                return 1;
        }
        
        if (! dispatch(&cfgt)) {
                LOG("operation failed.\n");
        }
        
        return 0;
}
