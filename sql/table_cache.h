/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2011 Monty Program Ab
   Copyright (C) 2013 Sergey Vojtovich and MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


struct TDC_element
{
  uchar m_key[NAME_LEN + 1 + NAME_LEN + 1];
  uint m_key_length;
  ulong version;
  bool flushed;
  TABLE_SHARE *share;

  typedef I_P_List <TABLE, TABLE_share> TABLE_list;
  typedef I_P_List <TABLE, I_P_List_adapter<TABLE, &TABLE::share_all_next,
                                            &TABLE::share_all_prev> >
          All_share_tables_list;
  /**
    Protects ref_count, m_flush_tickets, all_tables, free_tables, flushed,
    all_tables_refs.
  */
  mysql_mutex_t LOCK_table_share;
  mysql_cond_t COND_release;
  TDC_element *next, **prev;            /* Link to unused shares */
  uint ref_count;                       /* How many TABLE objects uses this */
  uint all_tables_refs;                 /* Number of refs to all_tables */
  /**
    List of tickets representing threads waiting for the share to be flushed.
  */
  Wait_for_flush_list m_flush_tickets;
  /*
    Doubly-linked (back-linked) lists of used and unused TABLE objects
    for this share.
  */
  All_share_tables_list all_tables;
  TABLE_list free_tables;
};


enum enum_tdc_remove_table_type
{
  TDC_RT_REMOVE_ALL,
  TDC_RT_REMOVE_NOT_OWN,
  TDC_RT_REMOVE_UNUSED,
  TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE
};

extern ulong tdc_size;
extern ulong tc_size;

extern void tdc_init(void);
extern void tdc_start_shutdown(void);
extern void tdc_deinit(void);
extern ulong tdc_records(void);
extern void tdc_purge(bool all);
extern TDC_element *tdc_lock_share(THD *thd, const char *db,
                                   const char *table_name);
extern void tdc_unlock_share(TDC_element *element);
extern TABLE_SHARE *tdc_acquire_share(THD *thd, TABLE_LIST *tl, uint flags,
                                      TABLE **out_table= 0);
extern void tdc_release_share(TABLE_SHARE *share);
extern bool tdc_remove_table(THD *thd, enum_tdc_remove_table_type remove_type,
                             const char *db, const char *table_name,
                             bool kill_delayed_threads);
extern int tdc_wait_for_old_version(THD *thd, const char *db,
                                    const char *table_name,
                                    ulong wait_timeout, uint deadlock_weight,
                                    ulong refresh_version= ULONG_MAX);
extern ulong tdc_refresh_version(void);
extern ulong tdc_increment_refresh_version(void);
extern int tdc_iterate(THD *thd, my_hash_walk_action action, void *argument,
                       bool no_dups= false);

extern uint tc_records(void);
extern void tc_purge(bool mark_flushed= false);
extern void tc_add_table(THD *thd, TABLE *table);
extern bool tc_release_table(TABLE *table);

/**
  Create a table cache key for non-temporary table.

  @param key         Buffer for key (must be at least NAME_LEN*2+2 bytes).
  @param db          Database name.
  @param table_name  Table name.

  @return Length of key.
*/

inline uint tdc_create_key(char *key, const char *db, const char *table_name)
{
  /*
    In theory caller should ensure that both db and table_name are
    not longer than NAME_LEN bytes. In practice we play safe to avoid
    buffer overruns.
  */
  return (uint) (strmake(strmake(key, db, NAME_LEN) + 1, table_name,
                         NAME_LEN) - key + 1);
}