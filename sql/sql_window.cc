#include "sql_select.h"
#include "item_windowfunc.h"
#include "filesort.h"
#include "sql_base.h"
#include "sql_window.h"

//TODO: why pass List<Window_spec> by value??
int
setup_windows(THD *thd, Item **ref_pointer_array, TABLE_LIST *tables,
	      List<Item> &fields, List<Item> &all_fields, 
              List<Window_spec> win_specs)
{
  int res= 0;
  Window_spec *win_spec;
  DBUG_ENTER("setup_windows");
  List_iterator<Window_spec> it(win_specs);
  while ((win_spec= it++))
  {
    bool hidden_group_fields;
    res= setup_group(thd, ref_pointer_array, tables, fields, all_fields,
                     win_spec->partition_list.first, &hidden_group_fields);
    res= res || setup_order(thd, ref_pointer_array, tables, fields, all_fields,
                            win_spec->order_list.first);
  }
  DBUG_RETURN(res);
}


/*
  @brief
    This function is called by JOIN::exec to compute window function values
  
  @detail
    JOIN::exec calls this after it has filled the temporary table with query
    output. The temporary table has fields to store window function values.

  @return
    false OK
    true  Error
*/

bool JOIN::process_window_functions(List<Item> *curr_fields_list)
{
  /*
   TODO Get this code to set can_compute_window_function during preparation,
   not during execution.

   The reason for this is the following:
   Our single scan optimization for window functions without tmp table,
   is valid, if and only if, we only need to perform one sorting operation,
   via filesort. The cases where we need to perform one sorting operation only:

   * A select with only one window function.
   * A select with multiple window functions, but they must have their
     partition and order by clauses compatible. This means that one ordering
     is acceptable for both window functions.

       For example:
       partition by a, b, c; order by d, e    results in sorting by a b c d e.
       partition by a; order by d             results in sorting by a d.

       This kind of sorting is compatible. The less specific partition does
       not care for the order of b and c columns so it is valid if we sort
       by those in case of equality over a.

       partition by a, b; order by d, e      results in sorting by a b d e
       partition by a; order by e            results in sorting by a e

      This sorting is incompatible due to the order by clause. The partition by
      clause is compatible, (partition by a) is a prefix for (partition by a, b)
      However, order by e is not a prefix for order by d, e, thus it is not
      compatible.

    The rule for having compatible sorting is thus:
      Each partition order must contain the other window functions partitions
      prefixes, or be a prefix itself. This must hold true for all partitions.
      Analog for the order by clause.  
  */

  List<Item_window_func> window_functions;
  SQL_I_List<ORDER> largest_partition;
  SQL_I_List<ORDER> largest_order_by;
  List_iterator_fast<Item> it(*curr_fields_list);
  Item *item;

#if 0
  bool can_compute_window_live = !need_tmp;
  /*
    psergey-winfunc: temporarily disabled the below because there is no 
    way to test it. Enable it back when we can.
  */

  // Construct the window_functions item list and check if they can be
  // computed using only one sorting.
  //
  // TODO: Perhaps group functions into compatible sorting bins
  // to minimize the number of sorting passes required to compute all of them.
  while ((item= it++))
  {
    if (item->type() == Item::WINDOW_FUNC_ITEM)
    {
      Item_window_func *item_win = (Item_window_func *) item;
      window_functions.push_back(item_win);
      if (!can_compute_window_live)
        continue;  // No point checking  since we have to perform multiple sorts.
      Window_spec *spec = item_win->window_spec;
      // Having an empty partition list on one window function and a
      // not empty list on a separate window function causes the sorting
      // to be incompatible.
      //
      // Example:
      // over (partition by a, order by x) && over (order by x).
      //
      // The first function requires an ordering by a first and then by x,
      // while the seond function requires an ordering by x first.
      // The same restriction is not required for the order by clause.
      if (largest_partition.elements && !spec->partition_list.elements)
      {
        can_compute_window_live= FALSE;
        continue;
      }
      can_compute_window_live= test_if_order_compatible(largest_partition,
                                                        spec->partition_list);
      if (!can_compute_window_live)
        continue;

      can_compute_window_live= test_if_order_compatible(largest_order_by,
                                                        spec->order_list);
      if (!can_compute_window_live)
        continue;

      if (largest_partition.elements < spec->partition_list.elements)
        largest_partition = spec->partition_list;
      if (largest_order_by.elements < spec->order_list.elements)
        largest_order_by = spec->order_list;
    }
  }
  if (can_compute_window_live && window_functions.elements && table_count == 1)
  {
    ha_rows examined_rows = 0;
    ha_rows found_rows = 0;
    ha_rows filesort_retval;
    SORT_FIELD *s_order= (SORT_FIELD *) my_malloc(sizeof(SORT_FIELD) *
        (largest_partition.elements + largest_order_by.elements) + 1,
        MYF(MY_WME | MY_ZEROFILL | MY_THREAD_SPECIFIC));

    size_t pos= 0;
    for (ORDER* curr = largest_partition.first; curr; curr=curr->next, pos++)
      s_order[pos].item = *curr->item;

    for (ORDER* curr = largest_order_by.first; curr; curr=curr->next, pos++)
      s_order[pos].item = *curr->item;

    table[0]->sort.io_cache=(IO_CACHE*) my_malloc(sizeof(IO_CACHE),
                                               MYF(MY_WME | MY_ZEROFILL|
                                                   MY_THREAD_SPECIFIC));


    filesort_retval= filesort(thd, table[0], s_order,
                              (largest_partition.elements + largest_order_by.elements),
                              this->select, HA_POS_ERROR, FALSE,
                              &examined_rows, &found_rows,
                              this->explain->ops_tracker.report_sorting(thd));
    table[0]->sort.found_records= filesort_retval;

    join_tab->read_first_record = join_init_read_record;
    join_tab->records= found_rows;

    my_free(s_order);
  }
  else
#endif
  {
    while ((item= it++))
    {
      if (item->type() == Item::WINDOW_FUNC_ITEM)
      {
        Item_window_func *item_win = (Item_window_func *) item;
        Window_spec *spec = item_win->window_spec;
        // spec->partition_list
        // spec->order_list
        ha_rows examined_rows = 0;
        ha_rows found_rows = 0;
        ha_rows filesort_retval;
        /*
          psergey: Igor suggests to use create_sort_index() here, but I think
          it doesn't make sense: create_sort_index() assumes that it operates
          on a base table in the join. 
          It calls test_if_skip_sort_order, checks for quick_select and what
          not. 
          It also assumes that ordering comes either from ORDER BY or GROUP BY.
          todo: check this again.
        */
        uint total_size= spec->partition_list.elements + 
                         spec->order_list.elements;
        SORT_FIELD *s_order= 
          (SORT_FIELD *) my_malloc(sizeof(SORT_FIELD) * (total_size+1), 
                                   MYF(MY_WME | MY_ZEROFILL | MY_THREAD_SPECIFIC));
        size_t pos= 0;
        for (ORDER* curr = spec->partition_list.first; curr; curr=curr->next, pos++)
          s_order[pos].item = *curr->item;

        for (ORDER* curr = spec->order_list.first; curr; curr=curr->next, pos++)
          s_order[pos].item = *curr->item;
        
        /* This is free'd by free_io_cache call below. */
        table[0]->sort.io_cache=(IO_CACHE*) my_malloc(sizeof(IO_CACHE),
                                                   MYF(MY_WME | MY_ZEROFILL|
                                                       MY_THREAD_SPECIFIC));

        Filesort_tracker dummy_tracker(false);
        filesort_retval= filesort(thd, table[0], s_order,
                                  total_size,
                                  this->select, HA_POS_ERROR, FALSE,
                                  &examined_rows, &found_rows,
                                  &dummy_tracker);
        table[0]->sort.found_records= filesort_retval;

        join_tab->read_first_record = join_init_read_record;
        join_tab->records= found_rows;

        my_free(s_order);
        
        /*
          Go through the sorted array and compute the window function
        */
        READ_RECORD info;
        if (init_read_record(&info, thd, table[0], select, 0, 1, FALSE))
          return true;

        item_win->setup_partition_border_check(thd);

        int err;
        TABLE *tbl= *table;
        while (!(err=info.read_record(&info)))
        {
          store_record(tbl,record[1]);
          
          /* 
            This will cause window function to compute its value for the
            current row : 
          */
          item_win->advance_window();

          /* Put the new value into temptable's field */
          item_win->save_in_field(item_win->result_field, true);
          err= tbl->file->ha_update_row(tbl->record[1], tbl->record[0]);
          if (err && err != HA_ERR_RECORD_IS_THE_SAME)
            return true;
        }
        item_win->set_read_value_from_result_field();
        end_read_record(&info);
        filesort_free_buffers(table[0], true);
        free_io_cache(table[0]);
      }
    }
  }
  return false;
}
