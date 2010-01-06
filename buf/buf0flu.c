/*****************************************************************************

Copyright (c) 1995, 2009, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0flu.c
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "buf0flu.h"

#ifdef UNIV_NONINL
#include "buf0flu.ic"
#endif

#include "buf0buf.h"
#include "srv0srv.h"
#include "page0zip.h"
#ifndef UNIV_HOTBACKUP
#include "ut0byte.h"
#include "ut0lst.h"
#include "page0page.h"
#include "fil0fil.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "os0file.h"
#include "trx0sys.h"

/**********************************************************************
These statistics are generated for heuristics used in estimating the
rate at which we should flush the dirty blocks to avoid bursty IO
activity. Note that the rate of flushing not only depends on how many
dirty pages we have in the buffer pool but it is also a fucntion of
how much redo the workload is generating and at what rate. */
/* @{ */

/** Number of intervals for which we keep the history of these stats.
Each interval is 1 second, defined by the rate at which
srv_error_monitor_thread() calls buf_flush_stat_update(). */
#define BUF_FLUSH_STAT_N_INTERVAL 20

/** Sampled values buf_flush_stat_cur.
Not protected by any mutex.  Updated by buf_flush_stat_update(). */
static buf_flush_stat_t	buf_flush_stat_arr[BUF_FLUSH_STAT_N_INTERVAL];

/** Cursor to buf_flush_stat_arr[]. Updated in a round-robin fashion. */
static ulint		buf_flush_stat_arr_ind;

/** Values at start of the current interval. Reset by
buf_flush_stat_update(). */
static buf_flush_stat_t	buf_flush_stat_cur;

/** Running sum of past values of buf_flush_stat_cur.
Updated by buf_flush_stat_update(). Not protected by any mutex. */
static buf_flush_stat_t	buf_flush_stat_sum;

/** Number of pages flushed through non flush_list flushes. */
static ulint buf_lru_flush_page_count = 0;

/* @} */

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
static
ibool
buf_flush_validate_low(void);
/*========================*/
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/********************************************************************//**
Inserts a modified block into the flush list. */
UNIV_INTERN
void
buf_flush_insert_into_flush_list(
/*=============================*/
	buf_block_t*	block)	/*!< in/out: block which is modified */
{
	//ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&block->mutex));
	ut_ad(mutex_own(&flush_list_mutex));
	ut_ad((UT_LIST_GET_FIRST(buf_pool->flush_list) == NULL)
	      || (UT_LIST_GET_FIRST(buf_pool->flush_list)->oldest_modification
		  <= block->page.oldest_modification));

	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->page.in_LRU_list);
	ut_ad(block->page.in_page_hash);
	ut_ad(!block->page.in_zip_hash);
	ut_ad(!block->page.in_flush_list);
	ut_d(block->page.in_flush_list = TRUE);
	UT_LIST_ADD_FIRST(flush_list, buf_pool->flush_list, &block->page);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_low());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
}

/********************************************************************//**
Inserts a modified block into the flush list in the right sorted position.
This function is used by recovery, because there the modifications do not
necessarily come in the order of lsn's. */
UNIV_INTERN
void
buf_flush_insert_sorted_into_flush_list(
/*====================================*/
	buf_block_t*	block)	/*!< in/out: block which is modified */
{
	buf_page_t*	prev_b;
	buf_page_t*	b;

	//ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&block->mutex));
	ut_ad(mutex_own(&flush_list_mutex));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	ut_ad(block->page.in_LRU_list);
	ut_ad(block->page.in_page_hash);
	ut_ad(!block->page.in_zip_hash);
	ut_ad(!block->page.in_flush_list);
	ut_d(block->page.in_flush_list = TRUE);

	prev_b = NULL;
	b = UT_LIST_GET_FIRST(buf_pool->flush_list);

	if (srv_fast_recovery) {
	/* speed hack */
	if (b == NULL || b->oldest_modification < block->page.oldest_modification) {
		UT_LIST_ADD_FIRST(flush_list, buf_pool->flush_list, &block->page);
	} else {
		b = UT_LIST_GET_LAST(buf_pool->flush_list);
		if (b->oldest_modification < block->page.oldest_modification) {
			/* align oldest_modification not to sort */
			block->page.oldest_modification = b->oldest_modification;
		}
		UT_LIST_ADD_LAST(flush_list, buf_pool->flush_list, &block->page);
	}
	} else {
	/* normal */
	while (b && b->oldest_modification > block->page.oldest_modification) {
		ut_ad(b->in_flush_list);
		prev_b = b;
		b = UT_LIST_GET_NEXT(flush_list, b);
	}

	if (prev_b == NULL) {
		UT_LIST_ADD_FIRST(flush_list, buf_pool->flush_list, &block->page);
	} else {
		UT_LIST_INSERT_AFTER(flush_list, buf_pool->flush_list,
				     prev_b, &block->page);
	}
	}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_low());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
}

/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., the transition FILE_PAGE => NOT_USED allowed.
@return	TRUE if can replace immediately */
UNIV_INTERN
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage)	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */
{
	//ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	//ut_ad(bpage->in_LRU_list); /* optimistic use */

	if (UNIV_LIKELY(bpage->in_LRU_list && buf_page_in_file(bpage))) {

		return(bpage->oldest_modification == 0
		       && buf_page_get_io_fix(bpage) == BUF_IO_NONE
		       && bpage->buf_fix_count == 0);
	}

	/* permited not to own LRU_mutex..  */
/*
	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Error: buffer block state %lu"
		" in the LRU list!\n",
		(ulong) buf_page_get_state(bpage));
	ut_print_buf(stderr, bpage, sizeof(buf_page_t));
	putc('\n', stderr);
*/

	return(FALSE);
}

/********************************************************************//**
Returns TRUE if the block is modified and ready for flushing.
@return	TRUE if can flush immediately */
UNIV_INLINE
ibool
buf_flush_ready_for_flush(
/*======================*/
	buf_page_t*	bpage,	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) */
	enum buf_flush	flush_type)/*!< in: BUF_FLUSH_LRU or BUF_FLUSH_LIST */
{
	//ut_a(buf_page_in_file(bpage));
	//ut_ad(buf_pool_mutex_own()); /*optimistic...*/
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(flush_type == BUF_FLUSH_LRU || BUF_FLUSH_LIST);

	if (buf_page_in_file(bpage) && bpage->oldest_modification != 0
	    && buf_page_get_io_fix(bpage) == BUF_IO_NONE) {
		ut_ad(bpage->in_flush_list);

		if (flush_type != BUF_FLUSH_LRU) {

			return(TRUE);

		} else if (bpage->buf_fix_count == 0) {

			/* If we are flushing the LRU list, to avoid deadlocks
			we require the block not to be bufferfixed, and hence
			not latched. */

			return(TRUE);
		}
	}

	return(FALSE);
}

/********************************************************************//**
Remove a block from the flush list of modified blocks. */
UNIV_INTERN
void
buf_flush_remove(
/*=============*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	//ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	mutex_enter(&flush_list_mutex);

	ut_ad(bpage->in_flush_list);
	ut_d(bpage->in_flush_list = FALSE);

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_PAGE:
		/* clean compressed pages should not be on the flush list */
	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		mutex_exit(&flush_list_mutex);
		ut_error;
		return;
	case BUF_BLOCK_ZIP_DIRTY:
		buf_page_set_state(bpage, BUF_BLOCK_ZIP_PAGE);
		UT_LIST_REMOVE(flush_list, buf_pool->flush_list, bpage);
		buf_LRU_insert_zip_clean(bpage);
		break;
	case BUF_BLOCK_FILE_PAGE:
		UT_LIST_REMOVE(flush_list, buf_pool->flush_list, bpage);
		break;
	}

	bpage->oldest_modification = 0;

	ut_d(UT_LIST_VALIDATE(flush_list, buf_page_t, buf_pool->flush_list,
			      ut_ad(ut_list_node_313->in_flush_list)));
	mutex_exit(&flush_list_mutex);
}

/********************************************************************//**
Updates the flush system data structures when a write is completed. */
UNIV_INTERN
void
buf_flush_write_complete(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	enum buf_flush	flush_type;

	ut_ad(bpage);

	buf_flush_remove(bpage);

	flush_type = buf_page_get_flush_type(bpage);
	buf_pool->n_flush[flush_type]--;

	if (flush_type == BUF_FLUSH_LRU) {
		/* Put the block to the end of the LRU list to wait to be
		moved to the free list */

		buf_LRU_make_block_old(bpage);

		buf_pool->LRU_flush_ended++;
	}

	/* fprintf(stderr, "n pending flush %lu\n",
	buf_pool->n_flush[flush_type]); */

	if ((buf_pool->n_flush[flush_type] == 0)
	    && (buf_pool->init_flush[flush_type] == FALSE)) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}
}

/********************************************************************//**
Flush a batch of writes to the datafiles that have already been
written by the OS. */
static
void
buf_flush_sync_datafiles(void)
/*==========================*/
{
	/* Wake possible simulated aio thread to actually post the
	writes to the operating system */
	os_aio_simulated_wake_handler_threads();

	/* Wait that all async writes to tablespaces have been posted to
	the OS */
	os_aio_wait_until_no_pending_writes();

	/* Now we flush the data to disk (for example, with fsync) */
	fil_flush_file_spaces(FIL_TABLESPACE);

	return;
}

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk,
and also wakes up the aio thread if simulated aio is used. It is very
important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
static
void
buf_flush_buffered_writes(void)
/*===========================*/
{
	byte*		write_buf;
	ulint		len;
	ulint		len2;
	ulint		i;

	if (!srv_use_doublewrite_buf || trx_doublewrite == NULL) {
		/* Sync the writes to the disk. */
		buf_flush_sync_datafiles();
		return;
	}

	mutex_enter(&(trx_doublewrite->mutex));

	/* Write first to doublewrite buffer blocks. We use synchronous
	aio and thus know that file write has been completed when the
	control returns. */

	if (trx_doublewrite->first_free == 0) {

		mutex_exit(&(trx_doublewrite->mutex));

		return;
	}

	for (i = 0; i < trx_doublewrite->first_free; i++) {

		const buf_block_t*	block;

		block = (buf_block_t*) trx_doublewrite->buf_block_arr[i];

		if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
		    || block->page.zip.data) {
			/* No simple validate for compressed pages exists. */
			continue;
		}

		if (UNIV_UNLIKELY
		    (memcmp(block->frame + (FIL_PAGE_LSN + 4),
			    block->frame + (UNIV_PAGE_SIZE
					    - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
			    4))) {
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: ERROR: The page to be written"
				" seems corrupt!\n"
				"InnoDB: The lsn fields do not match!"
				" Noticed in the buffer pool\n"
				"InnoDB: before posting to the"
				" doublewrite buffer.\n");
		}

		if (!block->check_index_page_at_flush) {
		} else if (page_is_comp(block->frame)) {
			if (UNIV_UNLIKELY
			    (!page_simple_validate_new(block->frame))) {
corrupted_page:
				buf_page_print(block->frame, 0);

				ut_print_timestamp(stderr);
				fprintf(stderr,
					"  InnoDB: Apparent corruption of an"
					" index page n:o %lu in space %lu\n"
					"InnoDB: to be written to data file."
					" We intentionally crash server\n"
					"InnoDB: to prevent corrupt data"
					" from ending up in data\n"
					"InnoDB: files.\n",
					(ulong) buf_block_get_page_no(block),
					(ulong) buf_block_get_space(block));

				ut_error;
			}
		} else if (UNIV_UNLIKELY
			   (!page_simple_validate_old(block->frame))) {

			goto corrupted_page;
		}
	}

	/* increment the doublewrite flushed pages counter */
	srv_dblwr_pages_written+= trx_doublewrite->first_free;
	srv_dblwr_writes++;

	len = ut_min(TRX_SYS_DOUBLEWRITE_BLOCK_SIZE,
		     trx_doublewrite->first_free) * UNIV_PAGE_SIZE;

	write_buf = trx_doublewrite->write_buf;
	i = 0;

	fil_io(OS_FILE_WRITE, TRUE, TRX_SYS_SPACE, 0,
	       trx_doublewrite->block1, 0, len,
	       (void*) write_buf, NULL);

	for (len2 = 0; len2 + UNIV_PAGE_SIZE <= len;
	     len2 += UNIV_PAGE_SIZE, i++) {
		const buf_block_t* block = (buf_block_t*)
			trx_doublewrite->buf_block_arr[i];

		if (UNIV_LIKELY(!block->page.zip.data)
		    && UNIV_LIKELY(buf_block_get_state(block)
				   == BUF_BLOCK_FILE_PAGE)
		    && UNIV_UNLIKELY
		    (memcmp(write_buf + len2 + (FIL_PAGE_LSN + 4),
			    write_buf + len2
			    + (UNIV_PAGE_SIZE
			       - FIL_PAGE_END_LSN_OLD_CHKSUM + 4), 4))) {
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: ERROR: The page to be written"
				" seems corrupt!\n"
				"InnoDB: The lsn fields do not match!"
				" Noticed in the doublewrite block1.\n");
		}
	}

	if (trx_doublewrite->first_free <= TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		goto flush;
	}

	len = (trx_doublewrite->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
		* UNIV_PAGE_SIZE;

	write_buf = trx_doublewrite->write_buf
		+ TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;
	ut_ad(i == TRX_SYS_DOUBLEWRITE_BLOCK_SIZE);

	fil_io(OS_FILE_WRITE, TRUE, TRX_SYS_SPACE, 0,
	       trx_doublewrite->block2, 0, len,
	       (void*) write_buf, NULL);

	for (len2 = 0; len2 + UNIV_PAGE_SIZE <= len;
	     len2 += UNIV_PAGE_SIZE, i++) {
		const buf_block_t* block = (buf_block_t*)
			trx_doublewrite->buf_block_arr[i];

		if (UNIV_LIKELY(!block->page.zip.data)
		    && UNIV_LIKELY(buf_block_get_state(block)
				   == BUF_BLOCK_FILE_PAGE)
		    && UNIV_UNLIKELY
		    (memcmp(write_buf + len2 + (FIL_PAGE_LSN + 4),
			    write_buf + len2
			    + (UNIV_PAGE_SIZE
			       - FIL_PAGE_END_LSN_OLD_CHKSUM + 4), 4))) {
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: ERROR: The page to be"
				" written seems corrupt!\n"
				"InnoDB: The lsn fields do not match!"
				" Noticed in"
				" the doublewrite block2.\n");
		}
	}

flush:
	/* Now flush the doublewrite buffer data to disk */

	fil_flush(TRX_SYS_SPACE);

	/* We know that the writes have been flushed to disk now
	and in recovery we will find them in the doublewrite buffer
	blocks. Next do the writes to the intended positions. */

	for (i = 0; i < trx_doublewrite->first_free; i++) {
		const buf_block_t* block = (buf_block_t*)
			trx_doublewrite->buf_block_arr[i];

		ut_a(buf_page_in_file(&block->page));
		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER,
			       FALSE, buf_page_get_space(&block->page),
			       buf_page_get_zip_size(&block->page),
			       buf_page_get_page_no(&block->page), 0,
			       buf_page_get_zip_size(&block->page),
			       (void*)block->page.zip.data,
			       (void*)block);

			/* Increment the counter of I/O operations used
			for selecting LRU policy. */
			buf_LRU_stat_inc_io();

			continue;
		}

		ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

		if (UNIV_UNLIKELY(memcmp(block->frame + (FIL_PAGE_LSN + 4),
					 block->frame
					 + (UNIV_PAGE_SIZE
					    - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
					 4))) {
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: ERROR: The page to be written"
				" seems corrupt!\n"
				"InnoDB: The lsn fields do not match!"
				" Noticed in the buffer pool\n"
				"InnoDB: after posting and flushing"
				" the doublewrite buffer.\n"
				"InnoDB: Page buf fix count %lu,"
				" io fix %lu, state %lu\n",
				(ulong)block->page.buf_fix_count,
				(ulong)buf_block_get_io_fix(block),
				(ulong)buf_block_get_state(block));
		}

		fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER,
		       FALSE, buf_block_get_space(block), 0,
		       buf_block_get_page_no(block), 0, UNIV_PAGE_SIZE,
		       (void*)block->frame, (void*)block);

		/* Increment the counter of I/O operations used
		for selecting LRU policy. */
		buf_LRU_stat_inc_io();
	}

	/* Sync the writes to the disk. */
	buf_flush_sync_datafiles();

	/* We can now reuse the doublewrite memory buffer: */
	trx_doublewrite->first_free = 0;

	mutex_exit(&(trx_doublewrite->mutex));
}

/********************************************************************//**
Posts a buffer page for writing. If the doublewrite memory buffer is
full, calls buf_flush_buffered_writes and waits for for free space to
appear. */
static
void
buf_flush_post_to_doublewrite_buf(
/*==============================*/
	buf_page_t*	bpage)	/*!< in: buffer block to write */
{
	ulint	zip_size;
try_again:
	mutex_enter(&(trx_doublewrite->mutex));

	ut_a(buf_page_in_file(bpage));

	if (trx_doublewrite->first_free
	    >= 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		mutex_exit(&(trx_doublewrite->mutex));

		buf_flush_buffered_writes();

		goto try_again;
	}

	zip_size = buf_page_get_zip_size(bpage);

	if (UNIV_UNLIKELY(zip_size)) {
		/* Copy the compressed page and clear the rest. */
		memcpy(trx_doublewrite->write_buf
		       + UNIV_PAGE_SIZE * trx_doublewrite->first_free,
		       bpage->zip.data, zip_size);
		memset(trx_doublewrite->write_buf
		       + UNIV_PAGE_SIZE * trx_doublewrite->first_free
		       + zip_size, 0, UNIV_PAGE_SIZE - zip_size);
	} else {
		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);

		memcpy(trx_doublewrite->write_buf
		       + UNIV_PAGE_SIZE * trx_doublewrite->first_free,
		       ((buf_block_t*) bpage)->frame, UNIV_PAGE_SIZE);
	}

	trx_doublewrite->buf_block_arr[trx_doublewrite->first_free] = bpage;

	trx_doublewrite->first_free++;

	if (trx_doublewrite->first_free
	    >= 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		mutex_exit(&(trx_doublewrite->mutex));

		buf_flush_buffered_writes();

		return;
	}

	mutex_exit(&(trx_doublewrite->mutex));
}
#endif /* !UNIV_HOTBACKUP */

/********************************************************************//**
Initializes a page for writing to the tablespace. */
UNIV_INTERN
void
buf_flush_init_for_writing(
/*=======================*/
	byte*		page,		/*!< in/out: page */
	void*		page_zip_,	/*!< in/out: compressed page, or NULL */
	ib_uint64_t	newest_lsn)	/*!< in: newest modification lsn
					to the page */
{
	ut_ad(page);

	if (page_zip_) {
		page_zip_des_t*	page_zip = page_zip_;
		ulint		zip_size = page_zip_get_size(page_zip);
		ut_ad(zip_size);
		ut_ad(ut_is_2pow(zip_size));
		ut_ad(zip_size <= UNIV_PAGE_SIZE);

		switch (UNIV_EXPECT(fil_page_get_type(page), FIL_PAGE_INDEX)) {
		case FIL_PAGE_TYPE_ALLOCATED:
		case FIL_PAGE_INODE:
		case FIL_PAGE_IBUF_BITMAP:
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			/* These are essentially uncompressed pages. */
			memcpy(page_zip->data, page, zip_size);
			/* fall through */
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
		case FIL_PAGE_INDEX:
			mach_write_ull(page_zip->data
				       + FIL_PAGE_LSN, newest_lsn);
			memset(page_zip->data + FIL_PAGE_FILE_FLUSH_LSN, 0, 8);
			mach_write_to_4(page_zip->data
					+ FIL_PAGE_SPACE_OR_CHKSUM,
					srv_use_checksums
					? page_zip_calc_checksum(
						page_zip->data, zip_size)
					: BUF_NO_CHECKSUM_MAGIC);
			return;
		}

		ut_print_timestamp(stderr);
		fputs("  InnoDB: ERROR: The compressed page to be written"
		      " seems corrupt:", stderr);
		ut_print_buf(stderr, page, zip_size);
		fputs("\nInnoDB: Possibly older version of the page:", stderr);
		ut_print_buf(stderr, page_zip->data, zip_size);
		putc('\n', stderr);
		ut_error;
	}

	/* Write the newest modification lsn to the page header and trailer */
	mach_write_ull(page + FIL_PAGE_LSN, newest_lsn);

	mach_write_ull(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
		       newest_lsn);

	/* Store the new formula checksum */

	mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
			srv_use_checksums
			? buf_calc_page_new_checksum(page)
			: BUF_NO_CHECKSUM_MAGIC);

	/* We overwrite the first 4 bytes of the end lsn field to store
	the old formula checksum. Since it depends also on the field
	FIL_PAGE_SPACE_OR_CHKSUM, it has to be calculated after storing the
	new formula checksum. */

	mach_write_to_4(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
			srv_use_checksums
			? buf_calc_page_old_checksum(page)
			: BUF_NO_CHECKSUM_MAGIC);
}

#ifndef UNIV_HOTBACKUP
/********************************************************************//**
Does an asynchronous write of a buffer page. NOTE: in simulated aio and
also when the doublewrite buffer is used, we must call
buf_flush_buffered_writes after we have posted a batch of writes! */
static
void
buf_flush_write_block_low(
/*======================*/
	buf_page_t*	bpage)	/*!< in: buffer block to write */
{
	ulint	zip_size	= buf_page_get_zip_size(bpage);
	page_t*	frame		= NULL;
#ifdef UNIV_LOG_DEBUG
	static ibool univ_log_debug_warned;
#endif /* UNIV_LOG_DEBUG */

	ut_ad(buf_page_in_file(bpage));

	/* We are not holding buf_pool_mutex or block_mutex here.
	Nevertheless, it is safe to access bpage, because it is
	io_fixed and oldest_modification != 0.  Thus, it cannot be
	relocated in the buffer pool or removed from flush_list or
	LRU_list. */
	//ut_ad(!buf_pool_mutex_own());
	ut_ad(!mutex_own(&LRU_list_mutex));
	ut_ad(!mutex_own(&flush_list_mutex));
	ut_ad(!mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_WRITE);
	ut_ad(bpage->oldest_modification != 0);

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(bpage->space, bpage->offset) == 0);
#endif
	ut_ad(bpage->newest_modification != 0);

#ifdef UNIV_LOG_DEBUG
	if (!univ_log_debug_warned) {
		univ_log_debug_warned = TRUE;
		fputs("Warning: cannot force log to disk if"
		      " UNIV_LOG_DEBUG is defined!\n"
		      "Crash recovery will not work!\n",
		      stderr);
	}
#else
	/* Force the log to the disk before writing the modified block */
	log_write_up_to(bpage->newest_modification, LOG_WAIT_ALL_GROUPS, TRUE);
#endif
	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_PAGE: /* The page should be dirty. */
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	case BUF_BLOCK_ZIP_DIRTY:
		frame = bpage->zip.data;
		if (UNIV_LIKELY(srv_use_checksums)) {
			ut_a(mach_read_from_4(frame + FIL_PAGE_SPACE_OR_CHKSUM)
			     == page_zip_calc_checksum(frame, zip_size));
		}
		mach_write_ull(frame + FIL_PAGE_LSN,
			       bpage->newest_modification);
		memset(frame + FIL_PAGE_FILE_FLUSH_LSN, 0, 8);
		break;
	case BUF_BLOCK_FILE_PAGE:
		frame = bpage->zip.data;
		if (!frame) {
			frame = ((buf_block_t*) bpage)->frame;
		}

		buf_flush_init_for_writing(((buf_block_t*) bpage)->frame,
					   bpage->zip.data
					   ? &bpage->zip : NULL,
					   bpage->newest_modification);
		break;
	}

	if (!srv_use_doublewrite_buf || !trx_doublewrite) {
		fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER,
		       FALSE, buf_page_get_space(bpage), zip_size,
		       buf_page_get_page_no(bpage), 0,
		       zip_size ? zip_size : UNIV_PAGE_SIZE,
		       frame, bpage);
	} else {
		buf_flush_post_to_doublewrite_buf(bpage);
	}
}

/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: in simulated aio we must call
os_aio_simulated_wake_handler_threads after we have posted a batch of
writes! NOTE: buf_pool_mutex and buf_page_get_mutex(bpage) must be
held upon entering this function, and they will be released by this
function. */
static
void
buf_flush_page(
/*===========*/
	buf_page_t*	bpage,		/*!< in: buffer control block */
	enum buf_flush	flush_type)	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	mutex_t*	block_mutex;
	ibool		is_uncompressed;

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
	//ut_ad(buf_pool_mutex_own());
#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own(&page_hash_latch, RW_LOCK_EX)
	      || rw_lock_own(&page_hash_latch, RW_LOCK_SHARED));
#endif
	ut_ad(buf_page_in_file(bpage));

	block_mutex = buf_page_get_mutex(bpage);
	ut_ad(mutex_own(block_mutex));

	mutex_enter(&buf_pool_mutex);
	rw_lock_s_unlock(&page_hash_latch);

	ut_ad(buf_flush_ready_for_flush(bpage, flush_type));

	buf_page_set_io_fix(bpage, BUF_IO_WRITE);

	buf_page_set_flush_type(bpage, flush_type);

	if (buf_pool->n_flush[flush_type] == 0) {

		os_event_reset(buf_pool->no_flush[flush_type]);
	}

	buf_pool->n_flush[flush_type]++;

	is_uncompressed = (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
	ut_ad(is_uncompressed == (block_mutex != &buf_pool_zip_mutex));

	switch (flush_type) {
		ibool	is_s_latched;
	case BUF_FLUSH_LIST:
		/* If the simulated aio thread is not running, we must
		not wait for any latch, as we may end up in a deadlock:
		if buf_fix_count == 0, then we know we need not wait */

		is_s_latched = (bpage->buf_fix_count == 0);
		if (is_s_latched && is_uncompressed) {
			rw_lock_s_lock_gen(&((buf_block_t*) bpage)->lock,
					   BUF_IO_WRITE);
		}

		mutex_exit(block_mutex);
		//buf_pool_mutex_exit();
		mutex_exit(&buf_pool_mutex);

		/* Even though bpage is not protected by any mutex at
		this point, it is safe to access bpage, because it is
		io_fixed and oldest_modification != 0.  Thus, it
		cannot be relocated in the buffer pool or removed from
		flush_list or LRU_list. */

		if (!is_s_latched) {
			buf_flush_buffered_writes();

			if (is_uncompressed) {
				rw_lock_s_lock_gen(&((buf_block_t*) bpage)
						   ->lock, BUF_IO_WRITE);
			}
		}

		break;

	case BUF_FLUSH_LRU:
		/* VERY IMPORTANT:
		Because any thread may call the LRU flush, even when owning
		locks on pages, to avoid deadlocks, we must make sure that the
		s-lock is acquired on the page without waiting: this is
		accomplished because buf_flush_ready_for_flush() must hold,
		and that requires the page not to be bufferfixed. */

		if (is_uncompressed) {
			rw_lock_s_lock_gen(&((buf_block_t*) bpage)->lock,
					   BUF_IO_WRITE);
		}

		/* Note that the s-latch is acquired before releasing the
		buf_pool mutex: this ensures that the latch is acquired
		immediately. */

		mutex_exit(block_mutex);
		//buf_pool_mutex_exit();
		mutex_exit(&buf_pool_mutex);
		break;

	default:
		ut_error;
	}

	/* Even though bpage is not protected by any mutex at this
	point, it is safe to access bpage, because it is io_fixed and
	oldest_modification != 0.  Thus, it cannot be relocated in the
	buffer pool or removed from flush_list or LRU_list. */

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr,
			"Flushing %u space %u page %u\n",
			flush_type, bpage->space, bpage->offset);
	}
#endif /* UNIV_DEBUG */
	buf_flush_write_block_low(bpage);
}

/***********************************************************//**
Flushes to disk all flushable pages within the flush area.
@return	number of pages flushed */
static
ulint
buf_flush_try_neighbors(
/*====================*/
	ulint		space,		/*!< in: space id */
	ulint		offset,		/*!< in: page offset */
	enum buf_flush	flush_type,	/*!< in: BUF_FLUSH_LRU or
					BUF_FLUSH_LIST */
	ulint		flush_neighbors)
{
	buf_page_t*	bpage;
	ulint		low, high;
	ulint		count		= 0;
	ulint		i;

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN || !flush_neighbors) {
		/* If there is little space, it is better not to flush any
		block except from the end of the LRU list */

		low = offset;
		high = offset + 1;
	} else {
		/* When flushed, dirty blocks are searched in neighborhoods of
		this size, and flushed along with the original page. */

		ulint	buf_flush_area	= ut_min(BUF_READ_AHEAD_AREA,
						 buf_pool->curr_size / 16);

		low = (offset / buf_flush_area) * buf_flush_area;
		high = (offset / buf_flush_area + 1) * buf_flush_area;
	}

	/* fprintf(stderr, "Flush area: low %lu high %lu\n", low, high); */

	if (high > fil_space_get_size(space)) {
		high = fil_space_get_size(space);
	}

	//buf_pool_mutex_enter();
	rw_lock_s_lock(&page_hash_latch);

	for (i = low; i < high; i++) {

		bpage = buf_page_hash_get(space, i);

		if (!bpage) {

			continue;
		}

		ut_a(buf_page_in_file(bpage));

		/* We avoid flushing 'non-old' blocks in an LRU flush,
		because the flushed blocks are soon freed */

		if (flush_type != BUF_FLUSH_LRU
		    || i == offset
		    || buf_page_is_old(bpage)) {
			mutex_t* block_mutex = buf_page_get_mutex_enter(bpage);

			if (block_mutex && buf_flush_ready_for_flush(bpage, flush_type)
			    && (i == offset || !bpage->buf_fix_count)) {
				/* We only try to flush those
				neighbors != offset where the buf fix count is
				zero, as we then know that we probably can
				latch the page without a semaphore wait.
				Semaphore waits are expensive because we must
				flush the doublewrite buffer before we start
				waiting. */

				buf_flush_page(bpage, flush_type);
				ut_ad(!mutex_own(block_mutex));
				count++;

				//buf_pool_mutex_enter();
				rw_lock_s_lock(&page_hash_latch);
			} else if (block_mutex) {
				mutex_exit(block_mutex);
			}
		}
	}

	//buf_pool_mutex_exit();
	rw_lock_s_unlock(&page_hash_latch);

	return(count);
}

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list or flush_list.
NOTE 1: in the case of an LRU flush the calling thread may own latches to
pages: to avoid deadlocks, this function must be written so that it cannot
end up waiting for these latches! NOTE 2: in the case of a flush list flush,
the calling thread is not allowed to own any latches on pages!
@return number of blocks for which the write request was queued;
ULINT_UNDEFINED if there was a flush of the same type already running */
UNIV_INTERN
ulint
buf_flush_batch(
/*============*/
	enum buf_flush	flush_type,	/*!< in: BUF_FLUSH_LRU or
					BUF_FLUSH_LIST; if BUF_FLUSH_LIST,
					then the caller must not own any
					latches on pages */
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	ib_uint64_t	lsn_limit)	/*!< in the case BUF_FLUSH_LIST all
					blocks whose oldest_modification is
					smaller than this should be flushed
					(if their number does not exceed
					min_n), otherwise ignored */
{
	buf_page_t*	bpage;
	buf_page_t*	prev_bpage	= NULL;
	ulint		page_count	= 0;
	ulint		old_page_count;
	ulint		space;
	ulint		offset;
	ulint		remaining	= 0;

	ut_ad((flush_type == BUF_FLUSH_LRU)
	      || (flush_type == BUF_FLUSH_LIST));
#ifdef UNIV_SYNC_DEBUG
	ut_ad((flush_type != BUF_FLUSH_LIST)
	      || sync_thread_levels_empty_gen(TRUE));
#endif /* UNIV_SYNC_DEBUG */
	//buf_pool_mutex_enter();
	mutex_enter(&buf_pool_mutex);

	if ((buf_pool->n_flush[flush_type] > 0)
	    || (buf_pool->init_flush[flush_type] == TRUE)) {

		/* There is already a flush batch of the same type running */

		//buf_pool_mutex_exit();
		mutex_exit(&buf_pool_mutex);

		return(ULINT_UNDEFINED);
	}

	buf_pool->init_flush[flush_type] = TRUE;

	mutex_exit(&buf_pool_mutex);

	if (flush_type == BUF_FLUSH_LRU) {
		mutex_enter(&LRU_list_mutex);
	}

	for (;;) {
flush_next:
		/* If we have flushed enough, leave the loop */
		if (page_count >= min_n) {

			break;
		}

		/* Start from the end of the list looking for a suitable
		block to be flushed. */

		if (flush_type == BUF_FLUSH_LRU) {
			bpage = UT_LIST_GET_LAST(buf_pool->LRU);
		} else {
			ut_ad(flush_type == BUF_FLUSH_LIST);

			mutex_enter(&flush_list_mutex);
			remaining = UT_LIST_GET_LEN(buf_pool->flush_list);
			bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
			if (bpage) {
				prev_bpage = UT_LIST_GET_PREV(flush_list, bpage);
			}
			mutex_exit(&flush_list_mutex);
			if (!bpage
			    || bpage->oldest_modification >= lsn_limit) {
				/* We have flushed enough */

				break;
			}
			ut_ad(bpage->in_flush_list);
		}

		/* Note that after finding a single flushable page, we try to
		flush also all its neighbors, and after that start from the
		END of the LRU list or flush list again: the list may change
		during the flushing and we cannot safely preserve within this
		function a pointer to a block in the list! */

		do {
			mutex_t*block_mutex = buf_page_get_mutex_enter(bpage);
			ibool	ready;

			//ut_a(buf_page_in_file(bpage));

			if (block_mutex) {
				ready = buf_flush_ready_for_flush(bpage, flush_type);
				mutex_exit(block_mutex);
			} else {
				ready = FALSE;
			}

			if (ready) {
				space = buf_page_get_space(bpage);
				offset = buf_page_get_page_no(bpage);

				//buf_pool_mutex_exit();
				if (flush_type == BUF_FLUSH_LRU) {
					mutex_exit(&LRU_list_mutex);
				}

				old_page_count = page_count;

				/* Try to flush also all the neighbors */
				page_count += buf_flush_try_neighbors(
					space, offset, flush_type, srv_flush_neighbor_pages);
				/* fprintf(stderr,
				"Flush type %lu, page no %lu, neighb %lu\n",
				flush_type, offset,
				page_count - old_page_count); */

				//buf_pool_mutex_enter();
				if (flush_type == BUF_FLUSH_LRU) {
					mutex_enter(&LRU_list_mutex);
				}
				goto flush_next;

			} else if (flush_type == BUF_FLUSH_LRU) {
				bpage = UT_LIST_GET_PREV(LRU, bpage);
			} else {
				ut_ad(flush_type == BUF_FLUSH_LIST);

				mutex_enter(&flush_list_mutex);
				bpage = UT_LIST_GET_PREV(flush_list, bpage);
				//ut_ad(!bpage || bpage->in_flush_list); /* optimistic */
				if (bpage != prev_bpage) {
					/* the search may warp.. retrying */
					bpage = NULL;
				}
				if (bpage) {
					prev_bpage = UT_LIST_GET_PREV(flush_list, bpage);
				}
				mutex_exit(&flush_list_mutex);
				remaining--;
			}
		} while (bpage != NULL);

		if (remaining)
			goto flush_next;

		/* If we could not find anything to flush, leave the loop */

		break;
	}

	if (flush_type == BUF_FLUSH_LRU) {
		mutex_exit(&LRU_list_mutex);
	}

	mutex_enter(&buf_pool_mutex);

	buf_pool->init_flush[flush_type] = FALSE;

	if (buf_pool->n_flush[flush_type] == 0) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}

	//buf_pool_mutex_exit();
	mutex_exit(&buf_pool_mutex);

	buf_flush_buffered_writes();

#ifdef UNIV_DEBUG
	if (buf_debug_prints && page_count > 0) {
		ut_a(flush_type == BUF_FLUSH_LRU
		     || flush_type == BUF_FLUSH_LIST);
		fprintf(stderr, flush_type == BUF_FLUSH_LRU
			? "Flushed %lu pages in LRU flush\n"
			: "Flushed %lu pages in flush list flush\n",
			(ulong) page_count);
	}
#endif /* UNIV_DEBUG */

	srv_buf_pool_flushed += page_count;

	/* We keep track of all flushes happening as part of LRU
	flush. When estimating the desired rate at which flush_list
	should be flushed we factor in this value. */
	if (flush_type == BUF_FLUSH_LRU) {
		buf_lru_flush_page_count += page_count;
	}

	return(page_count);
}

/******************************************************************//**
Waits until a flush batch of the given type ends */
UNIV_INTERN
void
buf_flush_wait_batch_end(
/*=====================*/
	enum buf_flush	type)	/*!< in: BUF_FLUSH_LRU or BUF_FLUSH_LIST */
{
	ut_ad((type == BUF_FLUSH_LRU) || (type == BUF_FLUSH_LIST));

	os_event_wait(buf_pool->no_flush[type]);
}

/******************************************************************//**
Gives a recommendation of how many blocks should be flushed to establish
a big enough margin of replaceable blocks near the end of the LRU list
and in the free list.
@return number of blocks which should be flushed from the end of the
LRU list */
static
ulint
buf_flush_LRU_recommendation(void)
/*==============================*/
{
	buf_page_t*	bpage;
	ulint		n_replaceable;
	ulint		distance	= 0;
	ibool		have_LRU_mutex = FALSE;

	if(UT_LIST_GET_LEN(buf_pool->unzip_LRU))
		have_LRU_mutex = TRUE;

	//buf_pool_mutex_enter();
	if (have_LRU_mutex)
		mutex_enter(&LRU_list_mutex);

	n_replaceable = UT_LIST_GET_LEN(buf_pool->free);

	bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	while ((bpage != NULL)
	       && (n_replaceable < BUF_FLUSH_FREE_BLOCK_MARGIN
		   + BUF_FLUSH_EXTRA_MARGIN)
	       && (distance < BUF_LRU_FREE_SEARCH_LEN)) {

		mutex_t* block_mutex;
		if (!bpage->in_LRU_list) {
			/* reatart. but it is very optimistic */
			bpage = UT_LIST_GET_LAST(buf_pool->LRU);
			continue;
		}
		block_mutex = buf_page_get_mutex_enter(bpage);

		if (block_mutex && buf_flush_ready_for_replace(bpage)) {
			n_replaceable++;
		}

		if (block_mutex) {
			mutex_exit(block_mutex);
		}

		distance++;

		bpage = UT_LIST_GET_PREV(LRU, bpage);
	}

	//buf_pool_mutex_exit();
	if (have_LRU_mutex)
		mutex_exit(&LRU_list_mutex);

	if (n_replaceable >= BUF_FLUSH_FREE_BLOCK_MARGIN) {

		return(0);
	}

	return(BUF_FLUSH_FREE_BLOCK_MARGIN + BUF_FLUSH_EXTRA_MARGIN
	       - n_replaceable);
}

/*********************************************************************//**
Flushes pages from the end of the LRU list if there is too small a margin
of replaceable pages there or in the free list. VERY IMPORTANT: this function
is called also by threads which have locks on pages. To avoid deadlocks, we
flush only pages such that the s-lock required for flushing can be acquired
immediately, without waiting. */
UNIV_INTERN
void
buf_flush_free_margin(
/*=======================*/
	ibool	wait)
{
	ulint	n_to_flush;
	ulint	n_flushed;

	n_to_flush = buf_flush_LRU_recommendation();

	if (n_to_flush > 0) {
		n_flushed = buf_flush_batch(BUF_FLUSH_LRU, n_to_flush, 0);
		if (wait && n_flushed == ULINT_UNDEFINED) {
			/* There was an LRU type flush batch already running;
			let us wait for it to end */

			buf_flush_wait_batch_end(BUF_FLUSH_LRU);
		}
	}
}

/*********************************************************************
Update the historical stats that we are collecting for flush rate
heuristics at the end of each interval.
Flush rate heuristic depends on (a) rate of redo log generation and
(b) the rate at which LRU flush is happening. */
UNIV_INTERN
void
buf_flush_stat_update(void)
/*=======================*/
{
	buf_flush_stat_t*	item;
	ib_uint64_t		lsn_diff;
	ib_uint64_t		lsn;
	ulint			n_flushed;

	lsn = log_get_lsn();
	if (buf_flush_stat_cur.redo == 0) {
		/* First time around. Just update the current LSN
		and return. */
		buf_flush_stat_cur.redo = lsn;
		return;
	}

	item = &buf_flush_stat_arr[buf_flush_stat_arr_ind];

	/* values for this interval */
	lsn_diff = lsn - buf_flush_stat_cur.redo;
	n_flushed = buf_lru_flush_page_count
		    - buf_flush_stat_cur.n_flushed;

	/* add the current value and subtract the obsolete entry. */
	buf_flush_stat_sum.redo += lsn_diff - item->redo;
	buf_flush_stat_sum.n_flushed += n_flushed - item->n_flushed;

	/* put current entry in the array. */
	item->redo = lsn_diff;
	item->n_flushed = n_flushed;

	/* update the index */
	buf_flush_stat_arr_ind++;
	buf_flush_stat_arr_ind %= BUF_FLUSH_STAT_N_INTERVAL;

	/* reset the current entry. */
	buf_flush_stat_cur.redo = lsn;
	buf_flush_stat_cur.n_flushed = buf_lru_flush_page_count;
}

/*********************************************************************
Determines the fraction of dirty pages that need to be flushed based
on the speed at which we generate redo log. Note that if redo log
is generated at a significant rate without corresponding increase
in the number of dirty pages (for example, an in-memory workload)
it can cause IO bursts of flushing. This function implements heuristics
to avoid this burstiness.
@return	number of dirty pages to be flushed / second */
UNIV_INTERN
ulint
buf_flush_get_desired_flush_rate(void)
/*==================================*/
{
	ulint			redo_avg;
	ulint			lru_flush_avg;
	ulint			n_dirty;
	ulint			n_flush_req;
	lint			rate;
	ib_uint64_t		lsn = log_get_lsn();
	ulint			log_capacity = log_get_capacity();

	/* log_capacity should never be zero after the initialization
	of log subsystem. */
	ut_ad(log_capacity != 0);

	/* Get total number of dirty pages. It is OK to access
	flush_list without holding any mtex as we are using this
	only for heuristics. */
	n_dirty = UT_LIST_GET_LEN(buf_pool->flush_list);

	/* An overflow can happen if we generate more than 2^32 bytes
	of redo in this interval i.e.: 4G of redo in 1 second. We can
	safely consider this as infinity because if we ever come close
	to 4G we'll start a synchronous flush of dirty pages. */
	/* redo_avg below is average at which redo is generated in
	past BUF_FLUSH_STAT_N_INTERVAL + redo generated in the current
	interval. */
	redo_avg = (ulint) (buf_flush_stat_sum.redo
			    / BUF_FLUSH_STAT_N_INTERVAL
			    + (lsn - buf_flush_stat_cur.redo));

	/* An overflow can happen possibly if we flush more than 2^32
	pages in BUF_FLUSH_STAT_N_INTERVAL. This is a very very
	unlikely scenario. Even when this happens it means that our
	flush rate will be off the mark. It won't affect correctness
	of any subsystem. */
	/* lru_flush_avg below is rate at which pages are flushed as
	part of LRU flush in past BUF_FLUSH_STAT_N_INTERVAL + the
	number of pages flushed in the current interval. */
	lru_flush_avg = buf_flush_stat_sum.n_flushed
			/ BUF_FLUSH_STAT_N_INTERVAL
			+ (buf_lru_flush_page_count
			   - buf_flush_stat_cur.n_flushed);

	n_flush_req = (n_dirty * redo_avg) / log_capacity;

	/* The number of pages that we want to flush from the flush
	list is the difference between the required rate and the
	number of pages that we are historically flushing from the
	LRU list */
	rate = n_flush_req - lru_flush_avg;
	return(rate > 0 ? (ulint) rate : 0);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
static
ibool
buf_flush_validate_low(void)
/*========================*/
{
	buf_page_t*	bpage;

	UT_LIST_VALIDATE(flush_list, buf_page_t, buf_pool->flush_list,
			 ut_ad(ut_list_node_313->in_flush_list));

	bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);

	while (bpage != NULL) {
		const ib_uint64_t om = bpage->oldest_modification;
		ut_ad(bpage->in_flush_list);
		//ut_a(buf_page_in_file(bpage)); /* optimistic */
		ut_a(om > 0);

		bpage = UT_LIST_GET_NEXT(flush_list, bpage);

		ut_a(!bpage || om >= bpage->oldest_modification);
	}

	return(TRUE);
}

/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
UNIV_INTERN
ibool
buf_flush_validate(void)
/*====================*/
{
	ibool	ret;

	//buf_pool_mutex_enter();
	mutex_enter(&flush_list_mutex);

	ret = buf_flush_validate_low();

	//buf_pool_mutex_exit();
	mutex_exit(&flush_list_mutex);

	return(ret);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#endif /* !UNIV_HOTBACKUP */
