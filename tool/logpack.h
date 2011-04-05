/**
 * Logpack functions for walbctl.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#ifndef _WALB_LOGPACK_H
#define _WALB_LOGPACK_H

#include "walb.h"
#include "walb_log_record.h"
#include "walb_log_device.h"


bool read_logpack_header_from_wldev(int fd,
                                    const walb_super_sector_t* super_sectp,
                                    u64 lsid, walb_logpack_header_t* lhead);

bool read_logpack_data_from_wldev(int fd,
                                  const walb_super_sector_t* super_sectp,
                                  const walb_logpack_header_t* lhead,
                                  u8* buf, size_t bufsize);

void print_logpack_header(const walb_logpack_header_t* lhead);
bool check_logpack_header(const walb_logpack_header_t* lhead, int physical_bs);


bool read_logpack_header(int fd, int physical_bs,
                         walb_logpack_header_t* lhead);
bool read_logpack_data(int fd,
                       int logical_bs, int physical_bs,
                       const walb_logpack_header_t* lhead,
                       u8* buf, size_t bufsize);
bool write_logpack_header(int fd, int physical_bs,
                          const walb_logpack_header_t* lhead);

bool redo_logpack(int fd,
                  int logical_bs, int physical_bs,
                  const walb_logpack_header_t* lhead,
                  const u8* buf);


/*******************************************************************************
 * New logpack interface.
 *******************************************************************************/

#include "walb_sector.h"

/**
 * Logpack
 */
typedef struct logpack {

    struct sector_data *head_sect;
    struct sector_data_array* data_sects;

    int logical_bs;
    int physical_bs;

} logpack_t;

/**
 * Assertion for logpack.
 */
#define ASSERT_LOGPACK(logpack, is_checksum) ASSERT(is_valid_logpack(logpack, is_checksum))

logpack_t* alloc_logpack(int logical_bs, int physical_bs, int n_sectors);
void free_logpack(logpack_t* logpack);
bool realloc_logpack(logpack_t* logpack, int n_sectors);

bool is_valid_logpack(logpack_t* logpack, bool is_checksum);

static inline walb_logpack_header_t* logpack_get_header(logpack_t* logpack);
static inline u64 logpack_get_lsid(logpack_t* logpack);

/*
 * Not yet implemented.
 */
walb_logpack_header_t* create_random_logpack(int logical_bs, int physical_bs, const u8* buf);
bool logpack_add_io_request(logpack_t* logpack, const u8* data, int size);




/**
 * Get pointer to logpack header image.
 */
static inline walb_logpack_header_t* logpack_get_header(logpack_t* logpack)
{
    ASSERT(logpack != NULL);
    ASSERT(logpack->head_sect != NULL);
    ASSERT(logpack->head_sect->data != NULL);

    return (walb_logpack_header_t *)logpack->head_sect->data;
}

/**
 * Get lsid of logpack.
 */
static inline u64 logpack_get_lsid(logpack_t* logpack)
{
    ASSERT_LOGPACK(logpack, false);
    return logpack_get_header(logpack)->logpack_lsid;
}



#endif /* _WALB_LOGPACK_H */
