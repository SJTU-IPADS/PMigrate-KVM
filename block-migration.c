/*
 * QEMU live block migration
 *
 * Copyright IBM, Corp. 2009
 *
 * Authors:
 *  Liran Schour   <lirans@il.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu-common.h"
#include "block_int.h"
#include "hw/hw.h"
#include "qemu-queue.h"
#include "qemu-timer.h"
#include "monitor.h"
#include "block-migration.h"
#include "migration.h"
#include "blockdev.h"
#include <assert.h>

//classicsong
#include "migr-vqueue.h"

#define BLOCK_SIZE (BDRV_SECTORS_PER_DIRTY_CHUNK << BDRV_SECTOR_BITS)

#define BLK_MIG_FLAG_DEVICE_BLOCK       0x01
#define BLK_MIG_FLAG_EOS                0x02
#define BLK_MIG_FLAG_PROGRESS           0x04

#define MAX_IS_ALLOCATED_SEARCH 65536

/*
 * classicsong
 * disk_vnum
 */
#define DISK_VNUM_OFFSET               3
#define DISK_VNUM_MASK                 (0x3f << DISK_VNUM_OFFSET)
#define DISK_NEGOTIATE                 0x3f


#define DEBUG_BLK_MIGRATION

#ifdef DEBUG_BLK_MIGRATION
#define DPRINTF(fmt, ...) \
    do { printf("blk_migration: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

typedef struct BlkMigDevState {
    BlockDriverState *bs;
    int bulk_completed;
    int shared_base;
    int64_t cur_sector;
    int64_t cur_dirty;
    int64_t completed_sectors;
    int64_t total_sectors;
    int64_t dirty;
    QSIMPLEQ_ENTRY(BlkMigDevState) entry;
    unsigned long *aio_bitmap;
} BlkMigDevState;

typedef struct BlkMigBlock {
    uint8_t *buf;
    BlkMigDevState *bmds;
    int64_t sector;
    int nr_sectors;
    struct iovec iov;
    QEMUIOVector qiov;
    BlockDriverAIOCB *aiocb;
    int ret;
    int64_t time;
    QSIMPLEQ_ENTRY(BlkMigBlock) entry;
    int done; //sheepx86: non-used variable
} BlkMigBlock;

typedef struct BlkMigState {
    int blk_enable;
    int shared_base;
    QSIMPLEQ_HEAD(bmds_list, BlkMigDevState) bmds_list;
    QSIMPLEQ_HEAD(blk_list, BlkMigBlock) blk_list;
    int submitted;
    int read_done;
    int transferred;
    int64_t total_sector_sum;
    int prev_progress;
    int bulk_completed;
    long double total_time;
    int reads;
} BlkMigState;

static BlkMigState block_mig_state;

uint64_t blk_read_remaining(void);

uint64_t 
blk_read_remaining(void) {
    uint64_t left = block_mig_state.submitted + block_mig_state.read_done;
    DPRINTF("Data remaing read %d, %d\n", block_mig_state.submitted, block_mig_state.read_done);
    return  left * BLOCK_SIZE;
}

static void blk_send(QEMUFile *f, BlkMigBlock * blk)
{
    int len;

    /* sector number and flags */
    qemu_put_be64(f, (blk->sector << BDRV_SECTOR_BITS)
                     | BLK_MIG_FLAG_DEVICE_BLOCK);

    /* device name */
    len = strlen(blk->bmds->bs->device_name);
    qemu_put_byte(f, len);
    qemu_put_buffer(f, (uint8_t *)blk->bmds->bs->device_name, len);

    qemu_put_buffer(f, blk->buf, BLOCK_SIZE);
}

unsigned long disk_save_block_slave(void *ptr, int iter_num, QEMUFile *f);
//classicsong
unsigned long
disk_save_block_slave(void *ptr, int iter_num, QEMUFile *f) {
    int len;
    BlkMigBlock *blk = (BlkMigBlock *)ptr;

    //DPRINTF("put disk data, %lx\n", blk->sector);
    /* sector number and flags 
     * and iter number (classicsong)
     */
    qemu_put_be64(f, (blk->sector << BDRV_SECTOR_BITS)
		  | BLK_MIG_FLAG_DEVICE_BLOCK | (iter_num << DISK_VNUM_OFFSET));

    /* device name */
    len = strlen(blk->bmds->bs->device_name);
    qemu_put_byte(f, len);
    qemu_put_buffer(f, (uint8_t *)blk->bmds->bs->device_name, len);
//    DPRINTF("device name %s\n", blk->bmds->bs->device_name);

    qemu_put_buffer(f, blk->buf, BLOCK_SIZE);

    qemu_free(blk->buf);
    qemu_free(blk);

    return BLOCK_SIZE;
}

int blk_mig_active(void)
{
    return !QSIMPLEQ_EMPTY(&block_mig_state.bmds_list);
}

uint64_t blk_mig_bytes_transferred(void)
{
    BlkMigDevState *bmds;
    uint64_t sum = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        sum += bmds->completed_sectors;
    }
    return sum << BDRV_SECTOR_BITS;
}

uint64_t blk_mig_bytes_remaining(void)
{
    return blk_mig_bytes_total() - blk_mig_bytes_transferred();
}

uint64_t blk_mig_bytes_total(void)
{
    BlkMigDevState *bmds;
    uint64_t sum = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        sum += bmds->total_sectors;
    }
    return sum << BDRV_SECTOR_BITS;
}

static inline void add_avg_read_time(int64_t time)
{
    block_mig_state.reads++;
    block_mig_state.total_time += time;
}

static inline long double compute_read_bwidth(void)
{
    assert(block_mig_state.total_time != 0);
    return  (block_mig_state.reads * BLOCK_SIZE)/ block_mig_state.total_time;
}

static int bmds_aio_inflight(BlkMigDevState *bmds, int64_t sector)
{
    int64_t chunk = sector / (int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK;

    if ((sector << BDRV_SECTOR_BITS) < bdrv_getlength(bmds->bs)) {
        return !!(bmds->aio_bitmap[chunk / (sizeof(unsigned long) * 8)] &
            (1UL << (chunk % (sizeof(unsigned long) * 8))));
    } else {
        return 0;
    }
}

static void bmds_set_aio_inflight(BlkMigDevState *bmds, int64_t sector_num,
                             int nb_sectors, int set)
{
    int64_t start, end;
    unsigned long val, idx, bit;

    start = sector_num / BDRV_SECTORS_PER_DIRTY_CHUNK;
    end = (sector_num + nb_sectors - 1) / BDRV_SECTORS_PER_DIRTY_CHUNK;

    for (; start <= end; start++) {
        idx = start / (sizeof(unsigned long) * 8);
        bit = start % (sizeof(unsigned long) * 8);
        val = bmds->aio_bitmap[idx];
        if (set) {
            val |= 1UL << bit;
        } else {
            val &= ~(1UL << bit);
        }
        bmds->aio_bitmap[idx] = val;
    }
}

static void alloc_aio_bitmap(BlkMigDevState *bmds)
{
    BlockDriverState *bs = bmds->bs;
    int64_t bitmap_size;

    bitmap_size = (bdrv_getlength(bs) >> BDRV_SECTOR_BITS) +
            BDRV_SECTORS_PER_DIRTY_CHUNK * 8 - 1;
    bitmap_size /= BDRV_SECTORS_PER_DIRTY_CHUNK * 8;

    bmds->aio_bitmap = qemu_mallocz(bitmap_size);
}

static void blk_mig_read_cb(void *opaque, int ret)
{
    BlkMigBlock *blk = opaque;

    blk->ret = ret;

    blk->time = qemu_get_clock_ns(rt_clock) - blk->time;

    add_avg_read_time(blk->time);

    QSIMPLEQ_INSERT_TAIL(&block_mig_state.blk_list, blk, entry);
    bmds_set_aio_inflight(blk->bmds, blk->sector, blk->nr_sectors, 0);

    block_mig_state.submitted--;
    block_mig_state.read_done++;
    DPRINTF("callback, id %lx, blk %p, ret %d\n", pthread_self(), blk, ret);


    if (block_mig_state.submitted < 0)
        fprintf(stderr, "submitted %d < 0\n", block_mig_state.submitted);
    assert(block_mig_state.submitted >= 0);
}

static int mig_save_device_bulk(Monitor *mon, QEMUFile *f,
                                BlkMigDevState *bmds)
{
    int64_t total_sectors = bmds->total_sectors;
    int64_t cur_sector = bmds->cur_sector;
    BlockDriverState *bs = bmds->bs;
    BlkMigBlock *blk;
    int nr_sectors;

    if (bmds->shared_base) {
        while (cur_sector < total_sectors &&
               !bdrv_is_allocated(bs, cur_sector, MAX_IS_ALLOCATED_SEARCH,
                                  &nr_sectors)) {
            cur_sector += nr_sectors;
        }
    }

    if (cur_sector >= total_sectors) {
        bmds->cur_sector = bmds->completed_sectors = total_sectors;
        return 1;
    }

    bmds->completed_sectors = cur_sector;

    cur_sector &= ~((int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK - 1);

    /* we are going to transfer a full block even if it is not allocated */
    nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;

    if (total_sectors - cur_sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
        nr_sectors = total_sectors - cur_sector;
    }

    blk = qemu_malloc(sizeof(BlkMigBlock));
    blk->buf = qemu_malloc(BLOCK_SIZE);
    blk->bmds = bmds;
    blk->sector = cur_sector;
    blk->nr_sectors = nr_sectors;

    blk->iov.iov_base = blk->buf;
    blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
    qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

    blk->time = qemu_get_clock_ns(rt_clock);
    //classicsong

    blk->aiocb = bdrv_aio_readv(bs, cur_sector, &blk->qiov,
                                nr_sectors, blk_mig_read_cb, blk);
    if (!blk->aiocb) {
        goto error;
    }
    block_mig_state.submitted++;

    bdrv_reset_dirty(bs, cur_sector, nr_sectors);
    bmds->cur_sector = cur_sector + nr_sectors;

    return (bmds->cur_sector >= total_sectors);

error:
    monitor_printf(mon, "Error reading sector %" PRId64 "\n", cur_sector);
    qemu_file_set_error(f);
    qemu_free(blk->buf);
    qemu_free(blk);
    return 0;
}

static void set_dirty_tracking(int enable)
{
    BlkMigDevState *bmds;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        bdrv_set_dirty_tracking(bmds->bs, enable);
    }
}

void set_dirty_tracking_master(int enable);

void set_dirty_tracking_master(int enable) {
    set_dirty_tracking(enable);
}
/*
 * classicsong
 * add a para QEMUFile for init_blk_migration_it
 */
struct blk_migration_it_stru{
    Monitor * mon;
    QEMUFile *f;
};

static void init_blk_migration_it(void *opaque, BlockDriverState *bs)
{
    struct blk_migration_it_stru *tmp = (struct blk_migration_it_stru *)opaque;
    Monitor *mon = tmp->mon;
    QEMUFile *f = tmp->f;
    BlkMigDevState *bmds;
    int64_t sectors;

    //classicsong
    int len;

    if (!bdrv_is_read_only(bs)) {
        sectors = bdrv_getlength(bs) >> BDRV_SECTOR_BITS;
        if (sectors <= 0) {
            return;
        }

        bmds = qemu_mallocz(sizeof(BlkMigDevState));
        bmds->bs = bs;
        bmds->bulk_completed = 0;
        bmds->total_sectors = sectors;
        bmds->completed_sectors = 0;
        bmds->shared_base = block_mig_state.shared_base;
        alloc_aio_bitmap(bmds);
        drive_get_ref(drive_get_by_blockdev(bs));
        bdrv_set_in_use(bs, 1);

        block_mig_state.total_sector_sum += sectors;

        /* classicsong
         * we will negotiate the bs size and name to the target machine
         */
        qemu_put_be64(f, DISK_NEGOTIATE << DISK_VNUM_OFFSET);
        len = strlen(bs->device_name);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)bs->device_name, len);
        qemu_put_be64(f, sectors);
        DPRINTF("NEGOTIATE disk bs %s, size %ld\n", bs->device_name, sectors);
	
        if (bmds->shared_base) {
            monitor_printf(mon, "Start migration for %s with shared base "
                                "image\n",
                           bs->device_name);
        } else {
            monitor_printf(mon, "Start full migration for %s\n",
                           bs->device_name);
        }

        QSIMPLEQ_INSERT_TAIL(&block_mig_state.bmds_list, bmds, entry);
    }
}

static void init_blk_migration(Monitor *mon, QEMUFile *f)
{
    struct blk_migration_it_stru tmp;
    block_mig_state.submitted = 0;
    block_mig_state.read_done = 0;
    block_mig_state.transferred = 0;
    block_mig_state.total_sector_sum = 0;
    block_mig_state.prev_progress = -1;
    block_mig_state.bulk_completed = 0;
    block_mig_state.total_time = 0;
    block_mig_state.reads = 0;

    tmp.mon = mon;
    tmp.f = f;

    bdrv_iterate(init_blk_migration_it, &tmp);
}

unsigned long total_disk_read = 0UL;
unsigned long total_disk_put_task = 0UL;

static unsigned long blk_mig_save_bulked_block_sync(Monitor *mon, QEMUFile *f, 
                                                    struct migration_task_queue *task_q)
{
    int64_t completed_sector_sum = 0;
    int64_t total_sectors;
    int64_t sector;
    int nr_sectors;
    BlkMigDevState *bmds;
    BlkMigBlock *blk;
    int progress;
    unsigned long data_sent = 0;
    struct task_body *body;
    struct timespec sleep = {0, 100000000}; //sleep 100ms
    unsigned long time_delta;

    monitor_printf(mon, "disk bulk, transfer all disk data\n");

    body = (struct task_body *)malloc(sizeof(struct task_body));
    body->type = TASK_TYPE_DISK;
    body->len = 0;
    body->iter_num = task_q->iter_num;

    DPRINTF("Start disk sync ops, first iteration\n");

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        total_sectors = bmds->total_sectors;

        if (bmds->bulk_completed == 0) {
            if (bmds->shared_base) {
                fprintf(stderr, "has not consider shared based case\n");
                return 0;
            }

            //DPRINTF("handle bmds %p, sector [%lx:%lx]\n", bmds, bmds->cur_sector, bmds->total_sectors);
            for (sector = bmds->cur_sector; sector < bmds->total_sectors;) {
                if (total_sectors - sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
                    nr_sectors = total_sectors - sector;
                } else {
                    nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
                }

                blk = qemu_malloc(sizeof(BlkMigBlock));
                blk->buf = qemu_malloc(BLOCK_SIZE);
                blk->bmds = bmds;
                blk->sector = sector;
                blk->nr_sectors = nr_sectors;
                blk->done = 0;
    

                time_delta = qemu_get_clock_ns(rt_clock);
                if (bdrv_read(bmds->bs, sector, blk->buf,
                              nr_sectors) < 0) {
                    fprintf(stderr, "Error reading block device");
                    return -1;
                }
                total_disk_read += (qemu_get_clock_ns(rt_clock) - time_delta);

                /*
                 * classicsong create task
                 */
                body->blocks[body->len++].ptr = blk;
                
                bdrv_reset_dirty(bmds->bs, sector, nr_sectors);

                if (body->len == DEFAULT_DISK_BATCH_LEN) {
                    time_delta = qemu_get_clock_ns(rt_clock);
                    while (task_q->task_pending > MAX_TASK_PENDING) {
                        nanosleep(&sleep, NULL);
                    }

                    if (queue_push_task(task_q, body) < 0)
                        fprintf(stderr, "Enqueue task error\n");

                    body = (struct task_body *)malloc(sizeof(struct task_body));
                    body->type = TASK_TYPE_DISK;
                    body->len = 0;
                    body->iter_num = task_q->iter_num;
                    total_disk_put_task += (qemu_get_clock_ns(rt_clock) - time_delta);
                }

                sector += BDRV_SECTORS_PER_DIRTY_CHUNK;
                bmds->cur_dirty = sector;
            }

            if (body->len != 0) {
                DPRINTF("additional disk task %d\n", body->len);
                if (queue_push_task(task_q, body) < 0)
                    fprintf(stderr, "Enqueue task error\n");
            } else {
                free(body);
            }
        }

        bmds->bulk_completed = 1;
    
        completed_sector_sum += bmds->completed_sectors;
        
        /*
         * report code
         */
        if (block_mig_state.total_sector_sum != 0) {
            progress = completed_sector_sum * 100 /
                block_mig_state.total_sector_sum;
        } else {
            progress = 100;
        }

        /*
        if (progress != block_mig_state.prev_progress) {
            block_mig_state.prev_progress = progress;
            qemu_put_be64(f, (progress << BDRV_SECTOR_BITS)
                          | BLK_MIG_FLAG_PROGRESS);
            monitor_printf(mon, "Completed %d %%\r", progress);
            monitor_flush(mon);
        }
        */
    }

    block_mig_state.bulk_completed = 1;

    return data_sent;
}

static int blk_mig_save_bulked_block(Monitor *mon, QEMUFile *f)
{
    int64_t completed_sector_sum = 0;
    BlkMigDevState *bmds;
    int progress;
    int ret = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        if (bmds->bulk_completed == 0) {
            if (mig_save_device_bulk(mon, f, bmds) == 1) {
                /* completed bulk section for this device */
                bmds->bulk_completed = 1;
            }
            completed_sector_sum += bmds->completed_sectors;
            ret = 1;
            break;
        } else {
            completed_sector_sum += bmds->completed_sectors;
        }
    }

    if (block_mig_state.total_sector_sum != 0) {
        progress = completed_sector_sum * 100 /
                   block_mig_state.total_sector_sum;
    } else {
        progress = 100;
    }
    if (progress != block_mig_state.prev_progress) {
        block_mig_state.prev_progress = progress;
        qemu_put_be64(f, (progress << BDRV_SECTOR_BITS)
                         | BLK_MIG_FLAG_PROGRESS);
        monitor_printf(mon, "Completed %d %%\r", progress);
        monitor_flush(mon);
    }

    return ret;
}

static void blk_mig_reset_dirty_cursor(void)
{
    BlkMigDevState *bmds;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        bmds->cur_dirty = 0;
    }
}

void blk_mig_reset_dirty_cursor_master(void);

void blk_mig_reset_dirty_cursor_master(void)
{
    blk_mig_reset_dirty_cursor();
}

static int mig_save_device_dirty(Monitor *mon, QEMUFile *f,
                                 BlkMigDevState *bmds, int is_async)
{
    BlkMigBlock *blk;
    int64_t total_sectors = bmds->total_sectors;
    int64_t sector;
    int nr_sectors;

    for (sector = bmds->cur_dirty; sector < bmds->total_sectors;) {
        if (bmds_aio_inflight(bmds, sector)) {
            qemu_aio_flush();
        }
        if (bdrv_get_dirty(bmds->bs, sector)) {

            if (total_sectors - sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
                nr_sectors = total_sectors - sector;
            } else {
                nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
            }
            blk = qemu_malloc(sizeof(BlkMigBlock));
            blk->buf = qemu_malloc(BLOCK_SIZE);
            blk->bmds = bmds;
            blk->sector = sector;
            blk->nr_sectors = nr_sectors;

            blk->done = 0;
            if (is_async) {
                blk->iov.iov_base = blk->buf;
                blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
                qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

                blk->time = qemu_get_clock_ns(rt_clock);

                blk->aiocb = bdrv_aio_readv(bmds->bs, sector, &blk->qiov,
                                            nr_sectors, blk_mig_read_cb, blk);
                if (!blk->aiocb) {
                    goto error;
                }
                block_mig_state.submitted++;
                bmds_set_aio_inflight(bmds, sector, nr_sectors, 1);
            } else {
                if (bdrv_read(bmds->bs, sector, blk->buf,
                              nr_sectors) < 0) {
                    goto error;
                }
                blk_send(f, blk);

                qemu_free(blk->buf);
                qemu_free(blk);
            }

            bdrv_reset_dirty(bmds->bs, sector, nr_sectors);
            break;
        }
        sector += BDRV_SECTORS_PER_DIRTY_CHUNK;
        bmds->cur_dirty = sector;
    }

    return (bmds->cur_dirty >= bmds->total_sectors);

error:
    monitor_printf(mon, "Error reading sector %" PRId64 "\n", sector);
    qemu_file_set_error(f);
    qemu_free(blk->buf);
    qemu_free(blk);
    return 0;
}

static int blk_mig_save_dirty_block(Monitor *mon, QEMUFile *f, int is_async)
{
    BlkMigDevState *bmds;
    int ret = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        if (mig_save_device_dirty(mon, f, bmds, is_async) == 0) {
            ret = 1;
            break;
        }
    }

    return ret;
}

static void flush_blks(QEMUFile* f)
{
    BlkMigBlock *blk;

    DPRINTF("%s Enter submitted %d read_done %d transferred %d\n",
            __FUNCTION__, block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);

    while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
        if (qemu_file_rate_limit(f)) {
            break;
        }
        if (blk->ret < 0) {
            qemu_file_set_error(f);
            break;
        }
        blk_send(f, blk);

        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);
        qemu_free(blk->buf);
        qemu_free(blk);

        block_mig_state.read_done--;
        block_mig_state.transferred++;
        assert(block_mig_state.read_done >= 0);
    }

    DPRINTF("%s Exit submitted %d read_done %d transferred %d\n", __FUNCTION__,
            block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);
}

static int64_t get_remaining_dirty(void)
{
    BlkMigDevState *bmds;
    int64_t dirty = 0;

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        dirty += bdrv_get_dirty_count(bmds->bs);
    }

    return dirty * BLOCK_SIZE;
}

int64_t get_remaining_dirty_master(void);

int64_t get_remaining_dirty_master(void) 
{
    return get_remaining_dirty();
}

static int is_stage2_completed(void)
{
    int64_t remaining_dirty;
    long double bwidth;

    if (block_mig_state.bulk_completed == 1) {

        remaining_dirty = get_remaining_dirty();
        if (remaining_dirty == 0) {
            return 1;
        }

        bwidth = compute_read_bwidth();

        if ((remaining_dirty / bwidth) <=
            migrate_max_downtime()) {
            /* finish stage2 because we think that we can finish remaing work
               below max_downtime */

            return 1;
        }
    }

    return 0;
}

static void blk_mig_cleanup(Monitor *mon)
{
    BlkMigDevState *bmds;
    BlkMigBlock *blk;

    set_dirty_tracking(0);

    while ((bmds = QSIMPLEQ_FIRST(&block_mig_state.bmds_list)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.bmds_list, entry);
        bdrv_set_in_use(bmds->bs, 0);
        drive_put_ref(drive_get_by_blockdev(bmds->bs));
        qemu_free(bmds->aio_bitmap);
        qemu_free(bmds);
    }

    while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);
        qemu_free(blk->buf);
        qemu_free(blk);
    }

    monitor_printf(mon, "\n");
}

void blk_mig_cleanup_master(Monitor *mon);

void blk_mig_cleanup_master(Monitor *mon)
{
    blk_mig_cleanup(mon);
}

static unsigned long 
flush_blks_master(struct migration_task_queue *task_q, QEMUFile *f, int last) {
    //put the task into task queue
    BlkMigBlock *blk;
    struct task_body *body;

    DPRINTF("%s Enter submitted %d read_done %d transferred %d\n",
            __FUNCTION__, block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);

    /*
     * If it is the end of the iteration and no data in the blk_list 
     * return
     */
    if (last == 1) {
        if (block_mig_state.read_done == 0)
            return 0;
    }
    /*
     * This is no the end of the iteration
     * If there is not enough blocks to at least half fullfil a single task
     * Skip
     */
    else if (block_mig_state.read_done < DEFAULT_DISK_BATCH_MIN_LEN)
        return 0;

    body = (struct task_body *)malloc(sizeof(struct task_body));
    body->type = TASK_TYPE_DISK;
    body->len = 0;
    body->iter_num = task_q->iter_num;

    while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
        if (blk->ret < 0) {
            qemu_file_set_error(f);
            break;
        }

        body->blocks[body->len++].ptr = blk;

        QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);

        block_mig_state.read_done--;
        block_mig_state.transferred++;
        assert(block_mig_state.read_done >= 0);

        if (body->len == DEFAULT_DISK_BATCH_LEN)
            break;
    }

    if (queue_push_task(task_q, body) < 0)
        fprintf(stderr, "Enqueue task error\n");

    DPRINTF("%s Exit submitted %d read_done %d transferred %d\n", __FUNCTION__,
            block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);

    DPRINTF("Task enqueue len %d\n", body->len);

    return body->len * BLOCK_SIZE;
}

//classicsong
static int 
mig_save_device_dirty_sync(Monitor *mon, QEMUFile *f,
                           BlkMigDevState *bmds, struct migration_task_queue *task_q) {
    BlkMigBlock *blk;
    int64_t total_sectors = bmds->total_sectors;
    int64_t sector;
    int nr_sectors;
    unsigned long data_sent = 0;
    struct task_body *body;
    struct timespec sleep = {0, 100000000}; //sleep 100ms

    monitor_printf(mon, "last iteration for disk");
    //data_sent += flush_blks_master(task_q, f, 1);

    body = (struct task_body *)malloc(sizeof(struct task_body));
    body->type = TASK_TYPE_DISK;
    body->len = 0;
    body->iter_num = task_q->iter_num;

    DPRINTF("Start disk sync ops, last iteration\n");
    /*
     * the for loop will handle all dirty pages in this sector
     */
    for (sector = bmds->cur_dirty; sector < bmds->total_sectors;) {
        if (bmds_aio_inflight(bmds, sector)) {
            fprintf(stderr, "there is aio inflight\n");
            qemu_aio_flush();
        }
        if (bdrv_get_dirty(bmds->bs, sector)) {

            if (total_sectors - sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
                nr_sectors = total_sectors - sector;
            } else {
                nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
            }
            blk = qemu_malloc(sizeof(BlkMigBlock));
            blk->buf = qemu_malloc(BLOCK_SIZE);
            blk->bmds = bmds;
            blk->sector = sector;
            blk->nr_sectors = nr_sectors;
            blk->done = 0;
    
            if (bdrv_read(bmds->bs, sector, blk->buf,
                          nr_sectors) < 0) {
                fprintf(stderr, "Error reading block device");
                return -1;
            }
     
            /*
             * classicsong create task
             */
            body->blocks[body->len++].ptr = blk;

            bdrv_reset_dirty(bmds->bs, sector, nr_sectors);
            data_sent += BLOCK_SIZE;

            if (body->len == DEFAULT_DISK_BATCH_LEN) {
                while (task_q->task_pending > MAX_TASK_PENDING) {
                    nanosleep(&sleep, NULL);
                }

                if (queue_push_task(task_q, body) < 0)
                    fprintf(stderr, "Enqueue task error\n");

                body = (struct task_body *)malloc(sizeof(struct task_body));
                body->type = TASK_TYPE_DISK;
                body->len = 0;
                body->iter_num = task_q->iter_num;
            }
        }

        sector += BDRV_SECTORS_PER_DIRTY_CHUNK;
        bmds->cur_dirty = sector;
    }

    if (body->len != 0) {
        if (queue_push_task(task_q, body) < 0)
            fprintf(stderr, "Enqueue task error\n");
    } else
        free(body);
            
    return data_sent;
}

static unsigned long
disk_save_master(Monitor *mon, struct migration_task_queue *task_q, QEMUFile *f) {
    unsigned long data_sent = 0;
    BlkMigDevState *bmds;

    DPRINTF("enter disk_save_master\n");

    /*
     * first iteration transfer all blocks
     */
    if (block_mig_state.bulk_completed == 0)
        while (block_mig_state.bulk_completed == 0) {
            /* first finish the bulk phase */
            data_sent += blk_mig_save_bulked_block_sync(mon, f, task_q);
        }
    else
        /*
         * following iterations transfer dirty blocks
         */
        QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
            data_sent += mig_save_device_dirty_sync(mon, f, bmds, task_q);
        }

    /*
    do {
        DPRINTF("mig_save 2\n");                
//       sent_last = mig_save_device_iter_sync(mon, f, 0, task_q);
        sent_last = flush_blks_master(task_q, f, 1);
        data_sent += sent_last;
    } while (sent_last != 0);
    */

    return data_sent;
}

static unsigned long
disk_save_last_master(Monitor *mon, struct migration_task_queue *task_q, QEMUFile *f) {
    BlkMigDevState *bmds;
    unsigned long data_sent = 0;

    /* we know for sure that save bulk is completed and
       all async read completed */
    assert(block_mig_state.submitted == 0);

    QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
        data_sent += mig_save_device_dirty_sync(mon, f, bmds, task_q);
    }    

    if (qemu_file_has_error(f)) {
        return 0;
    }

    monitor_printf(mon, "Block migration completed\n");

    return data_sent;
}

unsigned long block_save_iter(int stage, Monitor *mon, 
                              struct migration_task_queue *task_queue, QEMUFile *f);
//from migration-master.c
void create_host_disk_master(void *opaque);

unsigned long 
block_save_iter(int stage, Monitor *mon, 
                struct migration_task_queue *task_queue, QEMUFile *f) {
    unsigned long bytes_transferred = 0;

    if (stage == 2)
        bytes_transferred = disk_save_master(mon, task_queue, f);

    if (stage == 3)
        bytes_transferred = disk_save_last_master(mon, task_queue, f);

    return bytes_transferred;
}

//modified by classicsong
static int block_save_live(Monitor *mon, QEMUFile *f, int stage, void *opaque)
{
    FdMigrationState *s = (FdMigrationState *)opaque;

    DPRINTF("Enter save live stage %d submitted %d transferred %d\n",
            stage, block_mig_state.submitted, block_mig_state.transferred);

    if (stage < 0) {
        blk_mig_cleanup(mon);
        return 0;
    }

    if (block_mig_state.blk_enable != 1) {
        /* no need to migrate storage */
        qemu_put_be64(f, BLK_MIG_FLAG_EOS);
        return 1;
    }

    if (stage == 1) {
        DPRINTF("Init block migration\n");
        init_blk_migration(mon, f);

        s->disk_task_queue->section_id = s->section_id;
        //start dirty track
        //set_dirty_tracking(1);
    }

    flush_blks(f);

    if (qemu_file_has_error(f)) {
        blk_mig_cleanup(mon);
        return 0;
    }

    monitor_printf(mon, "Block migration start\n");
    
    qemu_put_be64(f, BLK_MIG_FLAG_EOS);

    DPRINTF("Finish disk negotiation start disk master\n");

    create_host_disk_master(opaque);

    return 0;
    //return ((stage == 2) && is_stage2_completed());
}

unsigned long total_disk_write = 0UL;
extern struct migration_task_queue *reduce_q;

int disk_write(void *bs_p, int64_t addr, void *buf_p, int nr_sectors);
int 
disk_write(void *bs_p, int64_t addr, void *buf_p, int nr_sectors) {
    BlockDriverState *bs = bs_p;
    uint8_t *buf = buf_p;
    int ret;
    unsigned long time_delta;

    time_delta = qemu_get_clock_ns(rt_clock);
    ret = bdrv_write(bs, addr, buf, nr_sectors);
    total_disk_write += (qemu_get_clock_ns(rt_clock) - time_delta);

    qemu_free(buf);

    return ret;
};

static int block_load(QEMUFile *f, void *opaque, int version_id)
{
    static int banner_printed;
    int len, flags;
    char device_name[256];
    int64_t addr;
    BlockDriverState *bs = NULL;
    uint8_t *buf;
    int64_t total_sectors = 0;
    int nr_sectors;
    int iter_num;

    //DPRINTF("Entering block_load\n");
    /*
     * at the initialization, the block_load will receive a BLK_MIG_FLAG_EOS
     */
    do {
        addr = qemu_get_be64(f);

        flags = addr & ~BDRV_SECTOR_MASK;
        addr >>= BDRV_SECTOR_BITS;

        /*
         * get iter_num
         */
        iter_num = (flags & DISK_VNUM_MASK) >> DISK_VNUM_OFFSET;
	
//        DPRINTF("handling iter %d, flags %x:%lx\n", iter_num, flags, addr);
        /*
         * only BLK_MIG_FLAG_DEVICE_BLOCK to transfer data
         */
        if (flags & BLK_MIG_FLAG_DEVICE_BLOCK) {
            //uint32_t disk_vnum = iter_num;
            //uint32_t curr_vnum;
            //volatile uint32_t *vnum_p;
            struct disk_task *task;
            struct timespec sleep = {0, 10000000}; //sleep 10ms

            /* get device name */
            len = qemu_get_byte(f);
            qemu_get_buffer(f, (uint8_t *)device_name, len);
            device_name[len] = '\0';
//            DPRINTF("[DEV]%s\n", device_name);
            bs = bdrv_find(device_name);
            if (!bs) {
                fprintf(stderr, "Error unknown block device %s\n",
                        device_name);
                return -EINVAL;
            }

            total_sectors = bdrv_getlength(bs) >> BDRV_SECTOR_BITS;
            if (total_sectors <= 0) {
                error_report("Error getting length of block device %s\n",
                             device_name);
                return -EINVAL;
            }

            /*
             * get version number pointer first
             * then get block data
             * the block data is sent at host end so we must receive it
             */
            //vnum_p = &(bs->version_queue[addr]);
	
            if (total_sectors - addr < BDRV_SECTORS_PER_DIRTY_CHUNK) {
                nr_sectors = total_sectors - addr;
            } else {
                nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
            }

            buf = qemu_malloc(BLOCK_SIZE);

            qemu_get_buffer(f, buf, BLOCK_SIZE);

            /*
        re_check_nor:
            curr_vnum = *vnum_p;
            /*
             * some one is holding the block
             *
            while (curr_vnum % 2 == 1) {
                curr_vnum = *vnum_p;
            }

            if (curr_vnum > disk_vnum * 2) {
                qemu_free(buf);
                continue;
            }

            /*
             * now we will hold the block
             *
            if (hold_block(vnum_p, curr_vnum, disk_vnum)) {
                /* fail holding the page *
                goto re_check_nor;
            }
            */
            task = (struct disk_task *)malloc(sizeof(struct disk_task));
            task->bs = bs;
            task->addr = addr;
            task->buf = buf;
            task->nr_sectors = nr_sectors;
            while (reduce_q->task_pending > MAX_TASK_PENDING) 
                nanosleep(&sleep, NULL);

            queue_push_task(reduce_q, task);
            /*
             * now we release the block
             */
            //release_block(vnum_p, disk_vnum);
        } else if (flags & BLK_MIG_FLAG_PROGRESS) {
            if (!banner_printed) {
                printf("Receiving block device images\n");
                banner_printed = 1;
            }
            printf("Completed %d %%%c", (int)addr,
                   (addr == 100) ? '\n' : '\r');
            fflush(stdout);
        } else if (iter_num == DISK_NEGOTIATE) {
            /*
             * get device name first
             * and then the sector size
             */
            len = qemu_get_byte(f);
            qemu_get_buffer(f, (uint8_t *)device_name, len);
            device_name[len] = '\0';

            bs = bdrv_find(device_name);
            if (!bs) {
                fprintf(stderr, "Error unknown block device %s\n",
                        device_name);
                return -EINVAL;
            }

            total_sectors = qemu_get_be64(f);
            DPRINTF("NEGOTIATE disk bs %s, size %ld\n", device_name, total_sectors);

            bs->version_queue = (uint32_t *)calloc(total_sectors, sizeof(uint32_t));
        } else if (!(flags & BLK_MIG_FLAG_EOS)) {
            fprintf(stderr, "Unknown flags\n");
            return -EINVAL;
        }

        if (qemu_file_has_error(f)) {
            return -EIO;
        }
    } while (!(flags & BLK_MIG_FLAG_EOS));

    return 0;
}

static void block_set_params(int blk_enable, int shared_base, void *opaque)
{
    block_mig_state.blk_enable = blk_enable;
    block_mig_state.shared_base = shared_base;

    /* shared base means that blk_enable = 1 */
    block_mig_state.blk_enable |= shared_base;
}

void blk_mig_init(void)
{
    QSIMPLEQ_INIT(&block_mig_state.bmds_list);
    QSIMPLEQ_INIT(&block_mig_state.blk_list);

    register_savevm_live(NULL, "block", 0, 1, block_set_params,
                         block_save_live, NULL, block_load, &block_mig_state);
}
