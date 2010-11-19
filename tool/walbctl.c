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

#include "util.h"


typedef struct config
{
        char* cmd_str; /* command string */
        char *ldev_name; /* log device name */

        size_t log_sector_size; /* sector size of log device */
        size_t log_dev_size; /* size of log device by the sector */

        int n_snapshots; /* maximum number of snapshots to keep */
        
} config_t;

config_t cfg_;

void show_help()
{
        printf("log format: walbctl mklog --ldev [block device path]\n");
}

void init_config(config_t* cfg)
{
        ASSERT(cfg != NULL);

        cfg->n_snapshots = 10000;
}

int parse_opt(int argc, char* const argv[])
{
        int c;

        while (1) {
                int option_index = 0;
                static struct option long_options[] = {
                        {"ldev", 1, 0, 1}, /* log device */
                        {"n_snap", 1, 0, 2}, /* num of snapshots */
                        {0, 0, 0, 0}
                };

                c = getopt_long(argc, argv, "", long_options, &option_index);
                if (c == -1) {
                        break;
                }
                switch (c) {
                case 1:
                        cfg_.ldev_name = strdup(optarg);
                        printf("ldev: %s\n", optarg);
                        break;
                case 2:
                        cfg_.n_snapshots = atoi(optarg);
                        break;
                default:
                        printf("default\n");
                }
        }

        if (optind < argc) {
                printf("command: ");
                while (optind < argc) {
                        cfg_.cmd_str = strdup(argv[optind]);
                        printf("%s ", argv[optind]);
                        optind ++;
                }
                printf("\n");
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
 * @dev_size device size by the sector.
 * @n_snapshots number of snapshots to keep.
 *
 * @return true in success, or false.
 */
bool init_walb_metadata(int fd, int logical_bs, int physical_bs, u64 dev_size, int n_snapshots)
{
        ASSERT(fd >= 0);
        ASSERT(logical_bs > 0);
        ASSERT(physical_bs > 0);
        ASSERT(dev_size < (u64)(-1));

        walb_super_sector_t super_sect;
        walb_snapshot_sector_t *snap_sectp;

        ASSERT(sizeof(super_sect) <= (size_t)physical_bs);
        ASSERT(sizeof(*snap_sectp) <= (size_t)physical_bs);

        /* Calculate number of snapshot sectors. */
        int n_sectors;
        int t = max_n_snapshots_in_sector(physical_bs);
        n_sectors = (n_snapshots + t - 1) / t;

        printf("metadata_size: %d\n", n_sectors);

        /* Prepare super sector */
        memset(&super_sect, 0, sizeof(super_sect));

        super_sect.logical_bs = logical_bs;
        super_sect.physical_bs = physical_bs;
        super_sect.snapshot_metadata_size = n_sectors;
        generate_uuid(super_sect.uuid);
        
        super_sect.start_offset = get_ring_buffer_offset(physical_bs, n_snapshots);

        super_sect.oldest_lsid = 0;
        super_sect.written_lsid = 0;
        super_sect.device_size = dev_size;

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
        print_super_sector(&super_sect);

        /* Read first snapshot sector and print for debug. */
        memset(snap_sectp, 0, physical_bs);
        if (! read_snapshot_sector(fd, &super_sect, snap_sectp, 0)) {
                goto error1;
        }
        print_snapshot_sector(snap_sectp, physical_bs);
        
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
 * @return 0 in success, or -1.
 */
int format_log_dev()
{
        ASSERT(cfg_.cmd_str);
        ASSERT(strcmp(cfg_.cmd_str, "mklog") == 0);
        
        if (check_log_dev(cfg_.ldev_name) < 0) {
                printf("format_log_dev: check failed.");
        }
        int logical_bs = get_bdev_logical_block_size(cfg_.ldev_name);
        int physical_bs = get_bdev_physical_block_size(cfg_.ldev_name);
        u64 size = get_bdev_size(cfg_.ldev_name);

        printf("logical_bs: %d\n"
               "physical_bs: %d\n"
               "size: %zu\n", logical_bs, physical_bs, size);


        if (physical_bs < 0 || size == (u64)(-1)) {
                printf("getting block device parameters failed.\n");
                goto error0;
        }
        
        int fd;
        fd = open(cfg_.ldev_name, O_RDWR);
        if (fd < 0) {
                perror("open failed");
                goto error0;
        }

        if (! init_walb_metadata(fd, logical_bs, physical_bs, size, cfg_.n_snapshots)) {

                printf("initialize walb log device failed.\n");
                goto error1;
        }
        
        close(fd);
        return 0;

error1:
        close(fd);
error0:
        return -1;
}

void dispatch()
{
        ASSERT(cfg_.cmd_str != NULL);
        if (strcmp(cfg_.cmd_str, "mklog") == 0) {

                format_log_dev();
        }
}

int main(int argc, char* const argv[])
{
        init_random();
        
        init_config(&cfg_);
        if (parse_opt(argc, argv) != 0) {
                return 0;
        }
        dispatch();
        
        return 0;
}