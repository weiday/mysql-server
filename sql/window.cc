/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "sql/window.h"

#include <sys/types.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

#include "m_ctype.h"
#include "my_base.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "my_table_map.h"
#include "mysql/udf_registration_types.h"
#include "mysqld_error.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"                         // ER_THD
#include "sql/enum_query_type.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"      // Item_sum
#include "sql/item_timefunc.h"                  // Item_date_add_interval
#include "sql/key_spec.h"
#include "sql/mem_root_array.h"
#include "sql/parse_tree_nodes.h" // PT_*
#include "sql/sql_array.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_error.h"
#include "sql/sql_exception_handler.h" // handle_std_exception
#include "sql/sql_lex.h"       // SELECT_LEX
#include "sql/sql_list.h"
#include "sql/sql_optimizer.h" // JOIN
#include "sql/sql_resolver.h"  // find_order_in_list
#include "sql/sql_show.h"
#include "sql/sql_time.h"
#include "sql/sql_tmp_table.h" // free_tmp_table
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/window_lex.h"
#include "sql_string.h"
#include "template_utils.h"

/**
  Shallow clone the list of ORDER objects using mem_root and return
  the cloned list.
*/
static ORDER *clone(THD *thd, ORDER *order)
{
  ORDER *clone= nullptr;
  ORDER **prev_next= &clone;
  for (; order != nullptr; order= order->next)
  {
    ORDER *o= new (thd->mem_root) PT_order_expr(nullptr, ORDER_ASC);
    std::memcpy(o, order, sizeof(*order));
    *prev_next= o;
    prev_next= &o->next;
  }

  *prev_next= nullptr; // final object should have a null next pointer
  return clone;
}


/**
  Append order expressions at the end of *first_next ordering list
  representing the partitioning columns.
*/
static void append_to_back(ORDER **first_next, ORDER *column)
{
  ORDER **prev_next= first_next;
  /*
    find last next pointer in list and make that point to column
    effectively appending it.
  */
  for (; *prev_next != nullptr; prev_next= &(*prev_next)->next) {}
  *prev_next= column;
}


ORDER *Window::first_partition_by() const
{
  return m_partition_by != NULL ? m_partition_by->value.first : NULL;
}


ORDER *Window::first_order_by() const
{
  return m_order_by != NULL ? m_order_by->value.first : NULL;
}


bool Window::check_window_functions(THD *thd, SELECT_LEX *select)
{
  List_iterator<Item_sum> li(m_functions);
  Item *wf;
  // DBUG_ASSERT(m_row_optimizable);

  m_row_optimizable= (m_frame != nullptr && m_frame->m_unit == WFU_ROWS);
  m_range_optimizable= (m_frame != nullptr && m_frame->m_unit == WFU_RANGE);

  m_static_aggregates=
    ((m_frame == nullptr && m_order_by == nullptr) ||
     ((m_row_optimizable || m_range_optimizable) &&
      (m_frame->m_from->m_border_type == WBT_UNBOUNDED_PRECEDING &&
       m_frame->m_to->m_border_type == WBT_UNBOUNDED_FOLLOWING)));

  m_opt_nth_row.m_offsets.clear();
  m_opt_lead_lag.m_offsets.clear();
  m_opt_nth_row.m_offsets.init(thd->mem_root);
  m_opt_lead_lag.m_offsets.init(thd->mem_root);

  while((wf= li++))
  {
    Evaluation_requirements reqs;

    Item_sum *wfs= down_cast<Item_sum*>(wf);
    if (wfs->check_wf_semantics(thd, select, &reqs))
      return true;

    m_needs_frame_buffering|= reqs.needs_buffer;
    m_needs_peerset|= reqs.needs_peerset;
    m_opt_first_row|= reqs.opt_first_row;
    m_opt_last_row|= reqs.opt_last_row;
    m_row_optimizable&= reqs.row_optimizable;
    m_range_optimizable&= reqs.range_optimizable;

    if (reqs.opt_nth_row.m_rowno > 0)
      m_opt_nth_row.m_offsets.push_back(reqs.opt_nth_row);

    /*
      INT_MIN64 can't be specified due 2's complement range.
      Offset is always given as a positive value; lead converted to negative
      but can't get to INT_MIN64. So, if we see this value, this window
      function isn't LEAD or LAG. 
    */
    if (reqs.opt_ll_row.m_rowno != INT_MIN64)
      m_opt_lead_lag.m_offsets.push_back(reqs.opt_ll_row);

    if (thd->lex->is_explain() && m_frame != nullptr && !wfs->framing())
    {
      /*
        SQL2014 <window clause> SR6b: functions which do not respect frames
        shouldn't have any frame specification in their window; we are more
        relaxed, as some users may find it handy to have one single
        window definition for framing and non-framing functions; but in case
        it's a user's mistake, we send a Note in EXPLAIN.
      */
      push_warning_printf(thd, Sql_condition::SL_NOTE,
                          ER_WINDOW_FUNCTION_IGNORES_FRAME,
                          ER_THD(thd, ER_WINDOW_FUNCTION_IGNORES_FRAME),
                          wfs->func_name(), printable_name());
    }
  }

  /*
    We do not allow FROM_LAST yet, so sorting guarantees sequential traveral
    of the frame buffer under evaluation of several NTH_VALUE functions invoked
    on a window, which is important for the optimized wf eval strategy
  */
  std::sort(m_opt_nth_row.m_offsets.begin(),
            m_opt_nth_row.m_offsets.end());
  std::sort(m_opt_lead_lag.m_offsets.begin(),
            m_opt_lead_lag.m_offsets.end());
  m_is_last_row_in_frame= !m_needs_frame_buffering;
  return false;
}


static Item_cache *make_result_item(Item *value)
{
  Item *order_expr= *down_cast<Item_ref *>(value)->ref;
  Item_cache *result= nullptr;

  switch (order_expr->result_type())
  {
    case INT_RESULT:
      result= new Item_cache_int(value->data_type());
      break;
    case REAL_RESULT:
      result= new Item_cache_real();
      break;
    case DECIMAL_RESULT:
      result= new Item_cache_decimal();
      break;
    case STRING_RESULT:
      if (value->is_temporal())
        result= new Item_cache_datetime(value->data_type());
      else if (value->data_type() == MYSQL_TYPE_JSON)
        result= new Item_cache_json();
      else
        result= new Item_cache_str(value);
      break;
    default:
      DBUG_ASSERT(false);
  }

  result->setup(value);
  return result;
}


bool Window::setup_range_expressions(THD *thd)
{
  const PT_order_list *o= order();

  if (o == nullptr && m_frame->m_unit == WFU_RANGE)
  {
    /*
      Without ORDER BY, all rows are peers, so in a RANGE frame CURRENT ROW
      extends to infinity, which we rewrite accordingly.
      We do not touch other border types (e.g. N PRECEDING) as they must be
      checked in more detail later.
    */
    {
      if (m_frame->m_from->m_border_type == WBT_CURRENT_ROW)
        m_frame->m_from->m_border_type= WBT_UNBOUNDED_PRECEDING;
      if (m_frame->m_to->m_border_type == WBT_CURRENT_ROW)
        m_frame->m_to->m_border_type= WBT_UNBOUNDED_FOLLOWING;
    }
  }

  PT_border *ba[]= { m_frame->m_from, m_frame->m_to };
  auto constexpr siz= sizeof(ba) / sizeof(PT_border *);

  for (auto border : Bounds_checked_array<PT_border *>(ba, siz))
  {
    Item_func *cmp= nullptr, **cmp_ptr= nullptr /* to silence warning */ ;
    enum_window_border_type border_type= border->m_border_type;
    switch (border_type)
    {
    case WBT_UNBOUNDED_PRECEDING:
    case WBT_UNBOUNDED_FOLLOWING:
      /* no computation required */
      break;
    case WBT_VALUE_PRECEDING:
    case WBT_VALUE_FOLLOWING:
      {
        /*
          Frame uses RANGE <value>, require ORDER BY with one column
          cf. SQL 2014 7.15 <window clause>, SR 13.a.ii
        */
        if (!o || o->value.elements != 1)
        {
          my_error(ER_WINDOW_RANGE_FRAME_ORDER_TYPE, MYF(0),
                   printable_name());
          return true;
        }

        /* check the ORDER BY type */
        Item *order_expr= *(o->value.first->item);
        switch (order_expr->result_type())
        {
        case INT_RESULT:
        case REAL_RESULT:
        case DECIMAL_RESULT:
          goto ok;
        case STRING_RESULT:
          if (order_expr->is_temporal())
            goto ok;
        default: ;
        }
        my_error(ER_WINDOW_RANGE_FRAME_ORDER_TYPE, MYF(0),
                 printable_name());
        return true;
    ok: ;
      }
      // fall through
    case WBT_CURRENT_ROW:
      {
        bool asc= o->value.first->direction == ORDER_ASC;
        /*
          Below, "value" is the value of ORDER BY expr at current row.
          "nr" is the value of the ORDER BY expr at another row in partition
          which we want to determine whether resided in the specified RANGE.

          We poke in the actual value of expr of the current row (cached) into value
          in Cached_item_xxx:cmp.
        */
        Item *nr= m_order_by_items[0]->get_item();
        /*
          WBT_CURRENT_ROW:
          if FROM:
          asc ? nr < value : nr > value ;
          if TO: < becomes > and vice-versa.
          WBT_VALUE_PRECEDING:
          if FROM:
          asc ? nr < value - border->val_int() :
          nr > value + border->val_int())
          if TO: < becomes > and vice-versa.
          WBT_VALUE_FOLLOWING:
          If FROM:
          asc ? nr < value + border->val_int() :
          nr > value - border->val_int()
          if TO: < becomes > and vice-versa.
        */
        Item_cache *value_i= make_result_item(nr);
        if (value_i == nullptr)
          return true;
        Item *cmp_arg;
        if (border_type == WBT_VALUE_PRECEDING ||
            border_type == WBT_VALUE_FOLLOWING)
        {
          cmp_arg= border->build_addop(value_i,
                                       border_type == WBT_VALUE_PRECEDING,
                                       asc, this);
          if (cmp_arg == nullptr)
            return true;
        }
        else
          cmp_arg= value_i;
        if ((border == m_frame->m_from) ? asc : !asc)
          cmp= new Item_func_lt(nr, cmp_arg);
        else
          cmp= new Item_func_gt(nr, cmp_arg);
        if (cmp == nullptr)
          return true;
        cmp_ptr= &m_comparators[border_type][border == m_frame->m_to];
        *cmp_ptr= cmp;

        break;
      }
    }

    if (cmp != nullptr &&
        cmp->fix_fields(thd, (Item **)cmp_ptr))
      return true;
  }

  return false;
}


ORDER *Window::sorting_order(THD *thd)
{
  if (m_sorting_order == nullptr)
  {
    ORDER *part= partition() ? partition()->value.first : nullptr;
    ORDER *ord= order() ? order()->value.first : nullptr;

    /*
      1. Copy both lists
      2. Append the ORDER BY list to the partition list.

      This ensures that all columns are present in the resulting sort ordering
      and that all ORDER BY expressions are at the end.
      The resulting sort can the be used to detect partition change and also
      satify the window ordering.
    */
    if (ord == nullptr)
      m_sorting_order= part;
    else if (part == nullptr)
      m_sorting_order= ord;
    else
    {
      ORDER *sorting= clone(thd, part);
      ORDER *ob= clone(thd, ord);
      append_to_back(&sorting, ob);
      m_sorting_order= sorting;
    }
  }
  return m_sorting_order;
}


bool Window::resolve_reference(THD *thd, Item_sum *wf, PT_window **m_window)
{
  Prepared_stmt_arena_holder stmt_arena_holder(thd);
  
  if (!(*m_window)->is_reference())
  {
    (*m_window)->m_functions.push_back(wf);
    return false;
  }

  SELECT_LEX *curr= thd->lex->current_select();

  List_iterator<Window> wi(curr->m_windows);
  Window *w;
  while ((w= wi++))
  {
    if (w->name() == nullptr)
      continue;

    if (my_strcasecmp(system_charset_info,
                      (*m_window)->name()->str_value.ptr(),
                      w->name()->str_value.ptr()) == 0)
    {
      (*m_window)->~PT_window(); // destroy the reference, no further need

      /* Replace with pointer to the definition */
      (*m_window)= static_cast<PT_window *>(w);
      (*m_window)->m_functions.base_list::push_back(wf);
      return false;
    }
  }

  my_error(ER_WINDOW_NO_SUCH_WINDOW, MYF(0), (*m_window)->printable_name());
  return true;
}


void Window::check_partition_boundary()
{
  DBUG_ENTER("check_partition_boundary");
  bool anything_changed= false;

  if (m_part_row_number == 0) // first row in first partition
  {
    anything_changed= true;
  }

  List_iterator<Cached_item> li(m_partition_items);
  Cached_item *item;


  /**
    If we have partitioning and any one of the partitioning columns have
    changed since last row, we have a new partition.
  */
  while ((item= li++))
  {
    anything_changed|= item->cmp();
  }

  m_partition_border= anything_changed;

  if (m_partition_border)
  {
    m_part_row_number= 1;
    m_first_rowno_in_range_frame= 1;
  } else
  {
    m_part_row_number++;
  }

  DBUG_VOID_RETURN;
}


void Window::reset_order_by_peer_set()
{
  DBUG_ENTER("reset_order_by_peer_set");

  List_iterator<Cached_item> li(m_order_by_items);
  Cached_item *item;

  while ((item= li++))
  {
    /*
      A side-effect of doing this comparison, is to update the cache, so that
      when we compare the new value to itself later, it is in its peer set.
    */
    (void)item->cmp();
  }

  DBUG_VOID_RETURN;
}


bool Window::in_new_order_by_peer_set()
{
  DBUG_ENTER("in_new_order_by_peer_set");
  bool anything_changed= false;

  List_iterator<Cached_item> li(m_order_by_items);
  Cached_item *item;

  while ((item= li++))
  {
    anything_changed|= item->cmp();
  }

  DBUG_RETURN(anything_changed);
}


bool Window::before_or_after_frame(bool before)
{
  PT_border *border;
  enum_window_border_type infinity; // the extreme bound of the border
  if (before)
  {
    border= frame()->m_from;
    infinity= WBT_UNBOUNDED_PRECEDING;
  }
  else
  {
    border= frame()->m_to;
    infinity= WBT_UNBOUNDED_FOLLOWING;
  }

  enum enum_window_border_type border_type= border->m_border_type;

  if (border_type == infinity)
    return false; // all rows included

  List_iterator<Cached_item> li(m_order_by_items);
  Cached_item *cur_row = li++;
  Item *candidate= cur_row->get_item();
  /*
    'cur_row' represents the value of the current row's windowing ORDER BY
    expression, and 'candidate' represents the same expression in the
    candidate row. Our caller is calculating the WF's value for 'cur_row';
    to this aim, here we want to know if 'candidate' is part of the frame of
    'cur_row'.

    First, as the candidate row has just been copied back from the frame
    buffer, we must update the item's null_value
  */
  (void)candidate->update_null_value();

  const bool asc= order()->value.first->direction == ORDER_ASC;
  const bool nulls_at_infinity= // true if NULLs stick to 'infinity'
    before ? asc : !asc;

  DBUG_ASSERT(border_type == WBT_CURRENT_ROW ||
              border_type == WBT_VALUE_PRECEDING ||
              border_type == WBT_VALUE_FOLLOWING);

  if (cur_row->null_value)                           // Current row is NULL
  {
    if (candidate->null_value)
      return false; // peer, so can't be before or after
    else
      return !nulls_at_infinity;
  }

  // Current row is not NULL:
  if (candidate->null_value)
    return nulls_at_infinity;

  // both value non-NULL, so compare

  Item_func *comparator= m_comparators[border_type][!before];
  /*
    'comparator' is set to compare 'cur_row' with 'candidate' but it has an
    old value of 'cur_row', update it.

    'comparator' is one of
      candidate {<, >} cur_row
      candidate {<, >} cur_row {-,+} constant

    The second form is used when the the RANGE frame boundary is
    WBT_VALUE_PRECEDING/WBT_VALUE_FOLLOWING, "constant" above being the value
    specified in the query, cf. setup in Window::setup_range_expressions.
  */
  Item *to_update;
  if (border_type == WBT_CURRENT_ROW)
  {
    to_update= comparator->arguments()[1];
  }
  else
  {
    Item_func *addop=
      down_cast<Item_func *>(comparator->arguments()[1]);
    to_update= addop->arguments()[0];
  }
  cur_row->copy_to_Item_cache(down_cast<Item_cache*>(to_update));
  return comparator->val_int();
}


bool Window::check_unique_name(List<Window> &windows)
{
  List_iterator<Window> w_it(windows);
  Window *w;

  if (m_name == nullptr)
    return false;

  while ((w= w_it++))
  {
    if (w->name() == nullptr)
      continue;

    if (w != this && m_name->eq(w->name(), false))
    {
      my_error(ER_WINDOW_DUPLICATE_NAME, MYF(0), printable_name());
      return true;
    }
  }

  return false;
}


bool Window::setup_ordering_cached_items(THD *thd,
                                         SELECT_LEX *select,
                                         const PT_order_list *o,
                                         bool partition_order)
{
  if (o == nullptr)
    return false;

  for (ORDER *order= o->value.first; order; order= order->next)
  {
    if (partition_order)
    {
      Item_ref *ir= new Item_ref(&select->context,
                                 order->item,
                                 (char *)"<no matter>",
                                 (char *)"<window partition by>");
      if (ir == nullptr)
        return true;

      Cached_item *ci= new_Cached_item(thd, ir);
      if (ci == nullptr)
        return true;

      m_partition_items.push_back(ci);
    }
    else
    {
      Item_ref *ir= new Item_ref(&select->context,
                                 order->item,
                                 (char *)"<no matter>",
                                 (char *)"<window order by>");
      if (ir == nullptr)
        return true;

      Cached_item *ci= new_Cached_item(thd, ir);
      if (ci == nullptr)
        return true;

      m_order_by_items.push_back(ci);
    }
  }
  return false;
}


bool Window::resolve_window_ordering(THD *thd,
                                   SELECT_LEX *select,
                                   Ref_item_array ref_item_array,
                                   TABLE_LIST *tables,
                                   List<Item> &fields,
                                   List<Item> &all_fields,
                                   ORDER *o,
                                   bool partition_order)
{
  DBUG_ENTER("resolve_window_ordering");
  DBUG_ASSERT(o);

  const char* sav_where= thd->where;
  thd->where= partition_order ? "window partition by" : "window order by";

  for (ORDER *order= o; order; order= order->next)
  {
    Item *oi= *order->item;

    /* Order by position is not allowed for windows: legacy SQL 1992 only */
    if (oi->type() == Item::INT_ITEM && oi->basic_const_item())
    {
      my_error(ER_WINDOW_ILLEGAL_ORDER_BY, MYF(0), printable_name());
      DBUG_RETURN(true);
    }

    if (find_order_in_list(thd, ref_item_array, tables, order,
                           fields, all_fields, false))
      DBUG_RETURN(true);
    oi= *order->item;

    if (order->used_alias)
    {
      /*
        Order by using alias is not allowed for windows, cf. SQL 2011, section
        7.11 <window clause>, SR 4. Throw the same error code as when alias is
        argument of a window function, or any function.
      */
      my_error(ER_BAD_FIELD_ERROR, MYF(0), oi->item_name.ptr(), thd->where);
      DBUG_RETURN(true);
    }

    if (!oi->fixed && oi->fix_fields(thd, order->item))
      DBUG_RETURN(true);
    oi= *order->item;

    /*
      Check SQL 2014 section 7.15 <window clause> SR 7 : A window cannot
      contain a windowing function without an intervening query expression.
    */
    if (oi->has_wf())
    {
      my_error(ER_WINDOW_NESTED_WINDOW_FUNC_USE_IN_WINDOW_SPEC, MYF(0),
               printable_name());
      DBUG_RETURN(true);
    }

    /*
      Call split_sum_func if an aggregate function is part of order by
      expression.
    */
    if (oi->has_aggregation() && oi->type() != Item::SUM_FUNC_ITEM)
    {
      oi->split_sum_func(thd, ref_item_array, all_fields);
      if (thd->is_error())
        DBUG_RETURN(true);
    }
  }

  thd->where= sav_where;
  DBUG_RETURN(false);
}


bool Window::equal_sort(THD *thd, Window *w1, Window *w2)
{

  ORDER *o1= w1->sorting_order(thd);
  ORDER *o2= w2->sorting_order(thd);

  if (o1 == nullptr || o2 == nullptr)
    return false;

  while (o1 != nullptr && o2 != nullptr)
  {
    if (o1->direction != o2->direction ||
        !(*o1->item)->eq(*o2->item, false))
      return false;

    o1= o1->next;
    o2= o2->next;
  }
  return o1 == nullptr && o2 == nullptr; // equal so far, now also same length
}


void Window::reorder_and_eliminate_sorts(THD *thd, List<Window> &windows,
                                         SELECT_LEX &select)
{
  for (uint i= 0; i < windows.elements - 1; i++)
  {
    for (uint j= i + 1; j < windows.elements; j++)
    {
      if (equal_sort(thd, windows[i], windows[j]))
      {
        windows[j]->m_sort_redundant= true;
        if (j > i + 1)
        {
          // move up to right after window[i], so we can share sort
          windows.swap_elts(i + 1, j);
        } // else already in right place
        break;
      }
    }
  }
}


bool Window::check_constant_bound(THD *thd, PT_border *border)
{
  const enum_window_border_type b_t= border->m_border_type;

  if (b_t == WBT_VALUE_PRECEDING || b_t == WBT_VALUE_FOLLOWING)
  {
    char const *save_where= thd->where;
    thd->where= "window frame bound";
    Item **border_ptr= border->border_ptr();

    /*
      For RANGE frames, resolving is already done in setup_range_expressions,
      so we need a test
    */
    DBUG_ASSERT(((*border_ptr)->fixed && m_frame->m_unit == WFU_RANGE) ||
                ((!(*border_ptr)->fixed || (*border_ptr)->basic_const_item())
                 && m_frame->m_unit == WFU_ROWS));

    if (!(*border_ptr)->fixed &&
        (*border_ptr)->fix_fields(thd, border_ptr))
      return true;

    if (!(*border_ptr)->const_for_execution() || // allow dyn. arg
        (*border_ptr)->has_subquery())
    {
      my_error(ER_WINDOW_RANGE_BOUND_NOT_CONSTANT, MYF(0),
               printable_name());
      return true;
    }
    thd->where= save_where;
  }

  return false;
}


bool Window::check_border_sanity(THD *thd, Window *w,
                                 const PT_frame *f, bool prepare)
{
  if (f == nullptr)
    return false;

  const PT_frame &fr= *f;

  PT_border *ba[]= { fr.m_from, fr.m_to };
  auto constexpr siz= sizeof(ba) / sizeof(PT_border *);

  for (auto border : Bounds_checked_array<PT_border *>(ba, siz))
  {
    enum_window_border_type border_t= border->m_border_type;
    switch (fr.m_unit)
    {
    case WFU_ROWS:
    case WFU_RANGE:

      // A check specific of the frame's start
      if (border == fr.m_from)
      {
        if (border_t == WBT_UNBOUNDED_FOLLOWING)
        {
          /*
            SQL 2014 section 7.15 <window clause>, SR 8.a
          */
          my_error(ER_WINDOW_FRAME_START_ILLEGAL, MYF(0), w->printable_name());
          return true;
        }
      }
      // A check specific of the frame's end
      else
      {
        if (border_t == WBT_UNBOUNDED_PRECEDING)
        {
          /*
            SQL 2014 section 7.15 <window clause>, SR 8.b
          */
          my_error(ER_WINDOW_FRAME_END_ILLEGAL, MYF(0), w->printable_name());
          return true;
        }
        enum_window_border_type from_t= fr.m_from->m_border_type;
        if ((from_t == WBT_CURRENT_ROW &&
             border_t == WBT_VALUE_PRECEDING) ||
            (border_t == WBT_CURRENT_ROW &&
             (from_t == WBT_VALUE_FOLLOWING)) ||
            (from_t == WBT_VALUE_FOLLOWING &&
             border_t == WBT_VALUE_PRECEDING))
        {
          /*
            SQL 2014 section 7.15 <window clause>, SR 8.c and 8.d
          */
          my_error(ER_WINDOW_FRAME_ILLEGAL, MYF(0), w->printable_name());
          return true;
        }
      }

      // Common code for start and end
      if (border_t == WBT_VALUE_PRECEDING || border_t == WBT_VALUE_FOLLOWING)
      {
        // INTERVAL only allowed with RANGE
        if (fr.m_unit == WFU_ROWS && border->m_date_time)
        {
          my_error(ER_WINDOW_ROWS_INTERVAL_USE, MYF(0), w->printable_name());
          return true;
        }

        if (w->check_constant_bound(thd, border))
          return true;

        Item *o_item= nullptr;

        if (prepare && border->m_value->type() == Item::PARAM_ITEM)
        {
          // postpone check till execute time
        }
        // Only integer values can be specified as args for ROW frames
        else if (fr.m_unit == WFU_ROWS &&
                 ((border_t == WBT_VALUE_PRECEDING ||
                   border_t == WBT_VALUE_FOLLOWING) &&
                  border->m_value->type() != Item::INT_ITEM))
        {
          my_error(ER_WINDOW_FRAME_ILLEGAL, MYF(0), w->printable_name());
          return true;
        }
        else if (fr.m_unit == WFU_RANGE &&
                 (o_item= w->m_order_by_items[0]->get_item())->result_type()
                 == STRING_RESULT &&
                 o_item->is_temporal())
        {
          /*
            SQL 2014 section 7.15 <window clause>, GR 5.b.i.1.B.I.1: if value
            is NULL or negative, we should give an error.
          */
          Interval interval;
          char buffer[STRING_BUFFER_USUAL_SIZE];
          String value(buffer, sizeof(buffer), thd->collation());
          get_interval_value(border->m_value, border->m_int_type, &value,
                             &interval);

          if (border->m_value->null_value ||
              interval.neg)
          {
            my_error(ER_WINDOW_FRAME_ILLEGAL, MYF(0), w->printable_name());
            return true;
          }
        }
        else if (border->m_value->val_int() < 0)
        {
          // GR 5.b.i.1.B.I.1
          my_error(ER_WINDOW_FRAME_ILLEGAL, MYF(0), w->printable_name());
          return true;
        }
      }
      break;
    case WFU_GROUPS:
      DBUG_ASSERT(false); // not yet implemented
      break;
    }
  }

  return false;
}


/**
  Simplified adjacency list: a window can maximum reference (depends on)
  one other window due to syntax restrictions. If there is no dependency,
  m_list[wno] == UNUSED. If w1 depends on w2, m_list[w1] == w2.
*/
class AdjacencyList
{
public:
  static constexpr uint UNUSED= std::numeric_limits<uint>::max();
  uint * const m_list;
  const uint m_card;
  AdjacencyList(uint elements) : m_list(new uint[elements]), m_card(elements)
  {
    for (auto &i : Bounds_checked_array<uint>(m_list, elements) )
    {
      i= UNUSED;
    }
  }
  ~AdjacencyList() { delete[] m_list; }

  /**
    Add a dependency.
    @param wno        the window that references another in its definition
    @param depends_on the window referenced
  */
  void add(uint wno, uint depends_on)
  {
    DBUG_ASSERT(wno <= m_card && depends_on <= m_card);
    DBUG_ASSERT(m_list[wno] == UNUSED);
    m_list[wno]= depends_on;
  }

  /**
    If the window depends on another window, return 1, else 0.
    
    @param wno the window
    @returns the out degree
  */
  uint out_degree(uint wno)
  {
    DBUG_ASSERT(wno <= m_card);
    return m_list[wno] == UNUSED ? 0 : 1;
  }

  /**
    Return the number of windows that depend on this one.

    @param wno the window
    @returns the in degree
  */
  uint in_degree(uint wno)
  {
    DBUG_ASSERT(wno <= m_card);
    uint degree= 0; // a priori

    for (auto i : Bounds_checked_array<uint>(m_list, m_card) )
    {
      degree+= i==wno ? 1 : 0;
    }
    return degree;
  }

  /**
    Return true of there is a circularity in the graph
  */
  bool check_circularity()
  {
    if (m_card == 1)
      return m_list[0] != UNUSED; // could have been resolved to itself

    std::unordered_set<uint> completed;

    for (uint i= 0; i < m_card; i++)
    {
      if (completed.count(i) != 0)
        continue; // Already checked.

      std::unordered_set<uint> visited;
      visited.insert(i);
      completed.insert(i);

      for (uint dep= m_list[i]; dep != UNUSED; dep= m_list[dep])
      {
        DBUG_ASSERT(dep <= m_card);
        if (visited.count(dep) != 0)
          return true; // found circularity
        visited.insert(dep);
        completed.insert(dep);
      }
    }
    return false;
  }
};


/**
  For now, we allow only the use of the new temp table memory engine,
  and not the old HEAP engine. This makes the old logic to handle overflow
  irrelevant, but we keep it now in case we decide to allow using HEAP again.
  FIXME.
*/
static bool check_acceptable_storage_engines(THD *thd)
{
  if (thd->variables.internal_tmp_mem_storage_engine != TMP_TABLE_TEMPTABLE)
  {
    my_error(ER_WINDOW_SE_NOT_ACCEPTABLE, MYF(0));
    return true;
  }

  return false;
}

bool Window::setup_windows(THD* thd,
                           SELECT_LEX *select,
                           Ref_item_array ref_item_array,
                           TABLE_LIST *tables,
                           List<Item> &fields,
                           List<Item> &all_fields,
                           List<Window> &windows)
{
  bool some_window_needs_frame_buffer= false;

  if (check_acceptable_storage_engines(thd))
    return true;

  if (!select->first_execution)
  {
    /*
      Execute: Re-prepare Items needed for windows. Can we move all this to
      prepare time only by using correct allocation for all the items?
      What about if they refer to SELECT list items, like some of them do?
      (In which case the solution might depend on what's generally done for
      SELECT lists.)
      Execute: Check frame borders if they contain ? parameters
    */
    List_iterator<Window> w_it(windows);
    Window *w;
    while ((w= w_it++))
    {
      /*
        We can encounter aggregate functions in the ORDER BY and PARTITION clauses
        of window function, so make sure we allow it:
      */
      nesting_map save_allow_sum_func= thd->lex->allow_sum_func;
      thd->lex->allow_sum_func|= (nesting_map)1 << select->nest_level;

      if (w->m_partition_by != nullptr &&
          w->resolve_window_ordering(thd, select, ref_item_array,
                                     tables, fields, all_fields,
                                     w->m_partition_by->value.first, true))
        return true;

      if (w->m_order_by != nullptr &&
          w->resolve_window_ordering(thd, select, ref_item_array,
                                     tables, fields, all_fields,
                                     w->m_order_by->value.first, false))
        return true;

      thd->lex->allow_sum_func= save_allow_sum_func;
    }

    w_it.rewind();
    while ((w= w_it++))
    {
      const PT_frame *f= w->frame();
      const PT_order_list *o= w->order();
      if (w->setup_ordering_cached_items(thd, select, o, false))
        return true;

      if (w->setup_ordering_cached_items(thd, select, w->partition(), true))
        return true;

      /*
        Need to redo these to set up for example cached item for RANK
      */
      if (w->check_window_functions(thd, select))
        return true;

      /*
        So we can determine is a row's value falls within range of current row's
      */
      if (f != nullptr && f->m_unit == WFU_RANGE &&
          w->setup_range_expressions(thd))
        return true;

      /*
        We need to check again in case ? parameters are used for window borders.
      */
      if (check_border_sanity(thd, w, f, false))
        return true;

      some_window_needs_frame_buffer|= w->needs_buffering();
    }

    return false;
  }

  Prepared_stmt_arena_holder ps_arena_holder(thd);

  /*
    We can encounter aggregate functions in the ORDER BY and PARTITION clauses
    of window function, so make sure we allow it:
  */
  nesting_map save_allow_sum_func= thd->lex->allow_sum_func;
  thd->lex->allow_sum_func|= (nesting_map)1 << select->nest_level;

  List_iterator<Window> w_it(windows);
  Window *w;
  while ((w= w_it++))
  {
    w->m_select= select;

    if (w->m_partition_by != nullptr &&
        w->resolve_window_ordering(thd, select, ref_item_array,
                                 tables, fields, all_fields,
                                 w->m_partition_by->value.first, true))
      return true;

    if (w->m_order_by != nullptr &&
        w->resolve_window_ordering(thd, select, ref_item_array,
                                 tables, fields, all_fields,
                                 w->m_order_by->value.first, false))
      return true;
  }

  thd->lex->allow_sum_func= save_allow_sum_func;

  /* Our adjacency list uses std::unordered_set which may throw, so "try" */
  try
  {
    /*
      If window N depends on (references) window M for its definition,
      we add the relation n->m to the adjacency list, cf.
      w1->set_ancestor(w2) vs. adj.add(i, j) below. 
    */
    AdjacencyList adj(windows.elements);

    /* Resolve inter-window references */
    List_iterator<Window> wi1(windows);
    Window *w1= wi1++;
    for (uint i= 0; i < windows.elements; i++, (w1= wi1++))
    {
      if (w1->m_inherit_from != nullptr)
      {
        bool resolved= false;
        List_iterator<Window> wi2(windows);
        Window *w2= wi2++;
        for (uint j= 0; j < windows.elements; j++, (w2= wi2++))
        {
          if (w2->m_name == nullptr)
            continue;

          if (my_strcasecmp(system_charset_info,
                            w1->m_inherit_from->str_value.ptr(),
                            w2->m_name->str_value.ptr()) == 0)
          {
            w1->set_ancestor(w2);
            resolved= true;
            adj.add(i, j);
            break;
          }
        }

        if (!resolved)
        {
          my_error(ER_WINDOW_NO_SUCH_WINDOW, MYF(0), w1->m_inherit_from->str_value.ptr());
          return true;
        }
      }
    }

    if (adj.check_circularity())
    {
      my_error(ER_WINDOW_CIRCULARITY_IN_WINDOW_GRAPH, MYF(0));
      return true;
    }

    /* We now know all references are resolved and they form a DAG */
    for (uint i= 0; i < windows.elements; i++)
    {
      if (adj.out_degree(i) != 0)
      {
        /* Only the root can specify partition. SR 10.c) */
        const Window * const non_root= windows[i];

        if (non_root->m_partition_by != nullptr)
        {
          my_error(ER_WINDOW_NO_CHILD_PARTITIONING, MYF(0));
          return true;
        }
      }

      if (adj.in_degree(i) == 0)
      {

        /* All windows that nobody depend on (leaves in DAG tree). */
        const Window * const leaf= windows[i];
        const Window *seen_orderer= nullptr;

        /* SR 10.d) No redefines of ORDER BY along inheritance path */
        for (const Window *w= leaf; w != nullptr; w= w->m_ancestor)
        {
          if (w->m_order_by != nullptr)
          {
            if (seen_orderer != nullptr)
            {
              my_error(ER_WINDOW_NO_REDEFINE_ORDER_BY, MYF(0),
                       seen_orderer->printable_name(),
                       w->printable_name());
              return true;
            }
            else
            {
              seen_orderer= w;
            }
          }
        }
      }
      else
      {
        /*
          This window has at least one dependant SQL 2014 section
          7.15 <window clause> SR 10.e
        */
        const Window * const ancestor= windows[i];
        if (ancestor->m_frame != nullptr)
        {
          my_error(ER_WINDOW_NO_INHERIT_FRAME, MYF(0), ancestor->printable_name());
          return true;
        }
      }
    }
  }
  catch (...)
  {
    /* purecov: begin inspected */
    handle_std_exception("setup_windows");
    return true;
    /* purecov: end */
  }

  w_it.rewind();
  w_it.init(windows);
  while ((w= w_it++))
  {
    const PT_frame *f= w->frame();
    const PT_order_list *o= w->order();

    if (w->check_unique_name(windows))
      return true;

    if (w->setup_ordering_cached_items(thd, select, o, false))
      return true;

    if (w->setup_ordering_cached_items(thd, select, w->partition(), true))
      return true;

    if (w->check_window_functions(thd, select))
      return true;

    some_window_needs_frame_buffer|= w->needs_buffering();
    
    /*
      initialize the physical sorting order by merging the partition clause
      and the ordering clause of the window specification.
    */
    (void)w->sorting_order(thd);

    /* For now, we do not support EXCLUDE */
    if (f != nullptr && f->m_exclusion != nullptr)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "EXCLUDE");
      return true;
    }

    /* For now, we do not support GROUPS */
    if (f != nullptr && f->m_unit == WFU_GROUPS)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "GROUPS");
      return true;
    }

#if 0 // GROUPS not yet supported, see error above.
    /*
      If frame uses GROUPS <value>, require ORDER BY
      cf. SQL 2011 SR 11.c.i)
    */
    if (f != nullptr && f->m_unit == WFU_GROUPS)
    {
      if (o == nullptr || o->value.elements != 1)
      {
        my_error(ER_WINDOW_GROUPS_REQUIRES_ORDER_BY_COL, MYF(0),
                 w->printable_name());
        return true;
      }
      /* check the ORDER BY type */
      Item *order_expr= *(o->value.first->item);

      /* must be numeric */
      switch (order_expr->result_type())
      {
        case INT_RESULT:
        case REAL_RESULT:
        case DECIMAL_RESULT:
          goto ok2;
        default: ;
      }
      my_error(ER_WINDOW_GROUPS_REQUIRES_ORDER_BY_COL, MYF(0),
               w->printable_name());
      return true;
    ok2: ;
    }
#endif

    /*
      So we can determine is a row's value falls within range of current row's
    */
    if (f != nullptr && f->m_unit == WFU_RANGE &&
        w->setup_range_expressions(thd))
      return true;

    if (check_border_sanity(thd, w, f, true))
      return true;
  }

  reorder_and_eliminate_sorts(thd, windows, *select);

  /* Do this last, after any re-ordering */
  windows[windows.elements - 1]->m_last= true;
  return false;
}


bool Window::needs_sorting()
{
  return (m_partition_by != nullptr ||
          m_order_by != nullptr);
}


bool Window::has_dynamic_frame_upper_bound() const
{
  return m_frame == nullptr && m_order_by != nullptr;
}


bool Window::make_special_rows_cache(THD *thd, TABLE *out_tbl)
{
  DBUG_ASSERT(m_special_rows_cache_max_length == 0);
  // Each row may come either from frame buffer or out-table
  size_t l=
    std::max((needs_buffering() ? m_frame_buffer->s->reclength : 0),
             out_tbl->s->reclength);
  m_special_rows_cache_max_length= l;
  return
    !(m_special_rows_cache=
      (uchar*)thd->alloc((FBC_FIRST_KEY - FBC_LAST_KEY + 1) * l));
}


void Window::cleanup(THD *thd)
{
  if (m_needs_frame_buffering && m_frame_buffer != nullptr)
  {
    (void)m_frame_buffer->file->ha_index_or_rnd_end();
    free_tmp_table(thd, m_frame_buffer);
#ifdef WF_DEBUG
    m_frame_buffer_cache.clear();
#endif // WF_DEBUG
  }

  List<Cached_item> *lis[]= { &m_order_by_items, &m_partition_items };
  auto constexpr siz= sizeof(lis) / sizeof(List<Cached_item> *);

  for (auto it : Bounds_checked_array<List<Cached_item> *>(lis, siz))
  {
    List_iterator<Cached_item> li(*it);
    Cached_item *ci;
    while ((ci= li++))
    {
      if (ci != nullptr)
        ci->~Cached_item();
    }
  }

  m_frame_buffer_positions.clear();
  m_special_rows_cache_max_length= 0;

  m_frame_buffer_param= nullptr;
  m_outtable_param= nullptr;
  m_frame_buffer= nullptr;
}


void Window::reset_lead_lag()
{
  List_iterator<Item_sum> li(m_functions);
  Item_sum *f;
  while ((f= li++))
  {
    if (f->sum_func() == Item_sum::LEAD_LAG_FUNC)
    {
      down_cast<Item_lead_lag*>(f)->set_has_value(false);
      down_cast<Item_lead_lag*>(f)->set_use_default(false);
    }
  }
}


void Window::reset_execution_state(Reset_level level)
{
  switch (level)
  {
    case RL_FULL:
      // Prepare a clean sheet for any new query resolution:
      m_partition_items.empty();
      m_order_by_items.empty();
      m_sorting_order= nullptr;
      /*
        order by elements in window functions need to be reset
        before the next execution. This is in line with resetting of
        global order by in "reinit_stmt_before_use".
        find_order_in_list() changes the item in order by. Hence the
        need to reset it to original pointer(item_ptr).
      */
      {
        const PT_order_list *li[]= { m_partition_by, m_order_by };
        auto constexpr siz= sizeof(li) / sizeof(PT_order_list*);
        for (auto it : Bounds_checked_array<const PT_order_list *>(li, siz))
        {
          if (it != nullptr)
          {
            for (ORDER *o= it->value.first; o != NULL; o= o->next)
              o->item= &o->item_ptr;
          }
        }
      }
      // fall-through
  case RL_ROUND:
      if (m_frame_buffer != nullptr && m_frame_buffer->is_created())
      {
        (void)m_frame_buffer->file->ha_index_or_rnd_end();
        m_frame_buffer->file->extra(HA_EXTRA_RESET_STATE);
        m_frame_buffer->file->ha_delete_all_rows();
      }
      m_frame_buffer_total_rows= 0;
      m_frame_buffer_partition_offset= 0;
      m_part_row_number= 0;
      // fall-through
    case RL_PARTITION:
      /*
        Forget positions in the frame buffer: they won't be valid in a new
        partition.
      */
      if (!m_frame_buffer_positions.empty())
      {
        for (auto &it : m_frame_buffer_positions)
        {
          it.m_rowno= -1;
        }
      } // else not allocated, empty result set

      m_tmp_pos.m_rowno= -1;
      /*
        w.frame_buffer()->file->ha_reset();
        We could truncate the file here if it is not too expensive..? FIXME
      */
      break;
  }


  /*
    These state variables are always set per row processed, so no need to
    reset here
  */
  // m_rowno_being_visited= 0;
  // m_last_rowno_in_peerset= 0;
  // m_partition_border= true;
  // m_frame_cardinality= 0;
  // m_dont_aggregate= false;
  // m_inverse_aggregation= false;
  // m_rowno_in_frame= 0;
  // m_rowno_in_partition= 0;
  // m_do_copy_null= false;
  // m_is_last_row_in_frame= false;

  /*
    These need resetting for all levels
  */
  m_last_row_output= 0;
  m_last_rowno_in_cache= 0;
  m_aggregates_primed= false;
  m_first_rowno_in_range_frame= 1;
  m_last_rowno_in_range_frame= 0;
}


bool Window::is_last_row_in_frame ()
{
  return (m_is_last_row_in_frame ||
          m_select->table_list.elements == 0);
}

//int64 Window::frame_cardinality()
//{
//  if (!m_needs_frame_buffering)
//    return m_rowno_in_frame;
//  else
//  {
//    if (m_frame == nullptr)
//      return m_last_rowno_in_cache;
//    else
//      return m_frame_cardinality;
//  }
//}


bool Window::has_two_pass_wf()
{
  List_iterator<Item_sum> it(m_functions);
  Item_result_field *f;
  while ((f= it++))
  {
    if (f->type() == Item::SUM_FUNC_ITEM &&
        !down_cast<Item_sum *>(f)->framing() &&
        down_cast<Item_sum *>(f)->two_pass())
      return true;
  }

  /*
    For RANGE frame, we first buffer all the rows in the partition due to the
    need to find last peer before first can be processed. This can be optimized,
    FIXME.
  */
  return m_frame != nullptr && m_frame->m_unit == WFU_RANGE;
}


void Window::print_border(String *str,
                          PT_border *border, enum_query_type qt) const
{
  const PT_border &b= *border;
  switch (b.m_border_type)
  {
    case WBT_CURRENT_ROW:
      str->append("CURRENT ROW");
      break;
    case WBT_VALUE_FOLLOWING:
    case WBT_VALUE_PRECEDING:

      if (b.m_date_time)
      {
        str->append("INTERVAL ");
        b.m_value->print(str, qt);
        str->append(' ');
        str->append(interval_names[b.m_int_type]);
        str->append(' ');
      }
      else
        b.m_value->print(str, qt);

      str->append(b.m_border_type == WBT_VALUE_PRECEDING ?
                  " PRECEDING":
                  " FOLLOWING");
      break;
    case WBT_UNBOUNDED_FOLLOWING:
      str->append("UNBOUNDED FOLLOWING");
      break;
    case WBT_UNBOUNDED_PRECEDING:
      str->append("UNBOUNDED PRECEDING");
      break;
  }

}


void Window::print_frame(String *str, enum_query_type qt) const
{
  const PT_frame &f= *m_frame;
  str->append(f.m_unit == WFU_ROWS ? "ROWS " :
              (f.m_unit == WFU_RANGE ? "RANGE " : "GROUPS "));

  str->append("BETWEEN ");
  print_border(str, f.m_from, qt);
  str->append(" AND ");
  print_border(str, f.m_to, qt);

  // if (f.m_exclusion != nullptr)
  // {
  //   str->append(" EXCLUDE ");
  //   const char *what;
  //   switch (f.m_exclusion->exclusion())
  //   {
  //     case WFX_CURRENT_ROW:
  //       what= "CURRENT ROW";
  //       break;
  //     case WFX_GROUP:
  //       what= "GROUP";
  //       break;
  //     case WFX_TIES:
  //       what= "TIES";
  //       break;
  //     case WFX_NO_OTHERS:
  //       what= "NO OTHERS";
  //       break;
  //   }
  //   str->append(what);
  // }
}


void Window::print(THD *thd, SELECT_LEX *lex, String *str, enum_query_type qt,
                   bool expand_definition) const
{
  if (m_name != nullptr && !expand_definition)
  {
    append_identifier(thd, str, m_name->item_name.ptr(),
                      m_name->item_name.length());
  }
  else
  {
    str->append('(');

    if (m_ancestor)
    {
      append_identifier(thd, str,
                        m_ancestor->m_name->item_name.ptr(),
                        strlen(m_ancestor->m_name->item_name.ptr()));
      str->append(' ');
    }


    if (m_partition_by != nullptr)
    {
      str->append("PARTITION BY ");
      lex->print_order(str, m_partition_by->value.first, qt);
      str->append(' ');
    }

    if (m_order_by != nullptr)
    {
      str->append("ORDER BY ");
      lex->print_order(str, m_order_by->value.first, qt);
      str->append(' ');
    }

    if (m_frame != nullptr)
    {
      print_frame(str, qt);
    }

    str->append(") ");
  }
}

void Window::reset_all_wf_state()
{
  List_iterator<Item_sum> ls(m_functions);
  Item_sum *sum;
  while ((sum= ls++))
  {
    bool framing[2]= {false, true};
    for (auto f : Bounds_checked_array<bool>(framing, 2))
    {
      (void)sum->walk(&Item::reset_wf_state,
                      Item::enum_walk(Item::WALK_POSTFIX),
                      (uchar*)&f);
    }
  }
}


bool Window::has_windowing_steps() const
{
  return m_select->join && m_select->join->m_windowing_steps;
}
