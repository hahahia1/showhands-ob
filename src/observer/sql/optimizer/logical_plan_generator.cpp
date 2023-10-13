/* Copyright (c) 2023 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/08/16.
//

#include "sql/optimizer/logical_plan_generator.h"

#include "sql/operator/aggregation_logical_operator.h"
#include "sql/operator/calc_logical_operator.h"
#include "sql/operator/delete_logical_operator.h"
#include "sql/operator/explain_logical_operator.h"
#include "sql/operator/group_by_logical_operator.h"
#include "sql/operator/insert_logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/order_by_logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/project_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/update_logical_operator.h"
#include "sql/stmt/calc_stmt.h"
#include "sql/stmt/delete_stmt.h"
#include "sql/stmt/explain_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/stmt/insert_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "sql/stmt/update_stmt.h"

using namespace std;

RC LogicalPlanGenerator::create(Stmt *stmt,
                                unique_ptr<LogicalOperator> &logical_operator,
                                std::map<std::string, LogicalOperator *> *map) {
  RC rc = RC::SUCCESS;
  switch (stmt->type()) {
    case StmtType::CALC: {
      CalcStmt *calc_stmt = static_cast<CalcStmt *>(stmt);
      rc = create_plan(calc_stmt, logical_operator);
    } break;

    case StmtType::SELECT: {
      SelectStmt *select_stmt = static_cast<SelectStmt *>(stmt);
      rc = create_plan(select_stmt, logical_operator, map);
    } break;

    case StmtType::INSERT: {
      InsertStmt *insert_stmt = static_cast<InsertStmt *>(stmt);
      rc = create_plan(insert_stmt, logical_operator);
    } break;

    case StmtType::DELETE: {
      DeleteStmt *delete_stmt = static_cast<DeleteStmt *>(stmt);
      rc = create_plan(delete_stmt, logical_operator);
    } break;

    case StmtType::UPDATE: {
      UpdateStmt *update_stmt = static_cast<UpdateStmt *>(stmt);
      rc = create_plan(update_stmt, logical_operator);
    } break;

    case StmtType::EXPLAIN: {
      ExplainStmt *explain_stmt = static_cast<ExplainStmt *>(stmt);
      rc = create_plan(explain_stmt, logical_operator);
    } break;
    default: {
      rc = RC::UNIMPLENMENT;
    }
  }
  return rc;
}

RC LogicalPlanGenerator::create_plan(
    CalcStmt *calc_stmt, std::unique_ptr<LogicalOperator> &logical_operator) {
  logical_operator.reset(
      new CalcLogicalOperator(std::move(calc_stmt->expressions())));
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(
    SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator,
    std::map<std::string, LogicalOperator *> *map) {
  // 构造取数据部分算子树
  unique_ptr<LogicalOperator> table_oper(nullptr);
  const std::vector<Table *> &tables = select_stmt->tables();
  const std::vector<Field> &all_fields = select_stmt->query_fields();
  const std::vector<Field> &aggr_fields = select_stmt->aggr_query_fields();
  const std::vector<Field> &order_by_fields = select_stmt->order_by_fields();
  const std::vector<Field> &group_by_fields = select_stmt->group_by_fields();
  if (map == nullptr) {
    map = new std::map<std::string, LogicalOperator *>;
  }
  for (Table *table : tables) {
    std::vector<Field> fields;
    for (const Field &field : all_fields) {
      if (0 == strcmp(field.table_name(), table->name())) {
        fields.push_back(field);
      }
    }
    if (map->count(table->name()) > 0) {
      if (table_oper == nullptr) {
        // 不应该有这个分支 因为默认外部子查询的链接是在join树的右边
      } else {
        JoinLogicalOperator *join_oper = new JoinLogicalOperator;
        join_oper->add_child(std::move(table_oper));
        join_oper->set_is_right_sub_link(true);
        join_oper->set_right_link(map->at(table->name()));
        table_oper = unique_ptr<LogicalOperator>(join_oper);
      }
    } else {
      unique_ptr<LogicalOperator> table_get_oper(
          new TableGetLogicalOperator(table, fields, true /*readonly*/));
      map->insert({table->name(), table_get_oper.get()});
      if (table_oper == nullptr) {
        table_oper = std::move(table_get_oper);
      } else {
        JoinLogicalOperator *join_oper = new JoinLogicalOperator;
        join_oper->add_child(std::move(table_oper));
        join_oper->add_child(std::move(table_get_oper));
        table_oper = unique_ptr<LogicalOperator>(join_oper);
      }
    }
  }

  // 构造谓词判断部分
  unique_ptr<LogicalOperator> predicate_oper;
  RC rc = create_plan(select_stmt->filter_stmt(), predicate_oper, map);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create predicate logical plan. rc=%s", strrc(rc));
    return rc;
  }
  // 构造投影算子
  unique_ptr<LogicalOperator> project_oper(
      new ProjectLogicalOperator(all_fields));

  if (predicate_oper) {
    if (table_oper) {
      predicate_oper->add_child(std::move(table_oper));
    }
    project_oper->add_child(std::move(predicate_oper));
  } else {
    if (table_oper) {
      project_oper->add_child(std::move(table_oper));
    }
  }

  unique_ptr<LogicalOperator> root_ptr = std::move(project_oper);
  if (!group_by_fields.empty()) {
    // 存在group by
    unique_ptr<LogicalOperator> group_by_oper(
        new GroupByLogicalOperator(group_by_fields));
    group_by_oper->add_child(std::move(root_ptr));
    root_ptr = std::move(group_by_oper);
  }

  if (!order_by_fields.empty()) {
    // 存在order by
    unique_ptr<LogicalOperator> order_by_oper(new OrderByLogicalOperator(
        order_by_fields, select_stmt->order_by_directions()));
    order_by_oper->add_child(std::move(root_ptr));
    root_ptr = std::move(order_by_oper);
  }

  if (!aggr_fields.empty()) {
    // 存在聚合函数
    unique_ptr<LogicalOperator> aggr_oper(new AggregationLogicalOperator(
        aggr_fields, select_stmt->aggr_field_to_query_field_map()));

    // 收集having子句谓词
    std::vector<unique_ptr<Expression>> cmp_exprs;
    const std::vector<FilterUnit *> &filter_units =
        select_stmt->having_filter_stmt()->filter_units();
    for (const FilterUnit *filter_unit : filter_units) {
      FilterObj &filter_obj_left = (FilterObj &)filter_unit->left();
      FilterObj &filter_obj_right = (FilterObj &)filter_unit->right();

      unique_ptr<Expression> left = filter_obj_left.to_expression(nullptr);
      unique_ptr<Expression> right = filter_obj_right.to_expression(nullptr);
      /*unique_ptr<Expression> left(
          filter_obj_left.is_attr
              ? static_cast<Expression *>(new FieldExpr(filter_obj_left.field))
              : static_cast<Expression *>(
                    new ValueExpr(filter_obj_left.value)));*/

      /*unique_ptr<Expression> right(
          filter_obj_right.is_attr
              ? static_cast<Expression *>(new FieldExpr(filter_obj_right.field))
              : static_cast<Expression *>(
                    new ValueExpr(filter_obj_right.value)));*/

      unique_ptr<ComparisonExpr> cmp_expr(new ComparisonExpr(
          filter_unit->comp(), std::move(left), std::move(right)));
      aggr_oper->add_expression(std::move(cmp_expr));
    }

    aggr_oper->add_child(std::move(root_ptr));
    root_ptr = std::move(aggr_oper);
  }

  logical_operator.swap(root_ptr);

  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(
    FilterStmt *filter_stmt, unique_ptr<LogicalOperator> &logical_operator,
    std::map<std::string, LogicalOperator *> *map) {
  std::vector<unique_ptr<Expression>> cmp_exprs;
  const std::vector<FilterUnit *> &filter_units = filter_stmt->filter_units();
  for (const FilterUnit *filter_unit : filter_units) {
    FilterObj &filter_obj_left = (FilterObj &)filter_unit->left();
    FilterObj &filter_obj_right = (FilterObj &)filter_unit->right();

    unique_ptr<Expression> left = filter_obj_left.to_expression(map);
    unique_ptr<Expression> right = filter_obj_right.to_expression(map);
    /*
    const FilterObj &filter_obj_left = filter_unit->left();
    const FilterObj &filter_obj_right = filter_unit->right();
    // 这里加入Select之后需要重写
    unique_ptr<Expression> left(nullptr);
    unique_ptr<Expression> right(nullptr);
    if (filter_obj_left.is_selects) {
      // TODO
      left = std::move(unique_ptr<Expression>(static_cast<Expression *>(
          new SelectExpr(filter_obj_left.stmt, map))));

    } else {
      left = std::move(unique_ptr<Expression>(
          filter_obj_left.is_attr
              ? static_cast<Expression *>(new FieldExpr(filter_obj_left.field))
              : static_cast<Expression *>(
                    new ValueExpr(filter_obj_left.value))));
    }
    if (filter_obj_right.is_selects) {
      right = std::move(unique_ptr<Expression>(static_cast<Expression *>(
          new SelectExpr(filter_obj_right.stmt, map))));
    } else {
      right = std::move(unique_ptr<Expression>(
          filter_obj_right.is_attr
              ? static_cast<Expression *>(new FieldExpr(filter_obj_right.field))
              : static_cast<Expression *>(
                    new ValueExpr(filter_obj_right.value))));
    }
    */

    ComparisonExpr *cmp_expr = new ComparisonExpr(
        filter_unit->comp(), std::move(left), std::move(right));
    cmp_exprs.emplace_back(cmp_expr);
  }

  unique_ptr<PredicateLogicalOperator> predicate_oper;
  if (!cmp_exprs.empty()) {
    unique_ptr<ConjunctionExpr> conjunction_expr(
        new ConjunctionExpr(ConjunctionExpr::Type::AND, cmp_exprs));
    predicate_oper = unique_ptr<PredicateLogicalOperator>(
        new PredicateLogicalOperator(std::move(conjunction_expr)));
  }

  logical_operator = std::move(predicate_oper);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(
    InsertStmt *insert_stmt, unique_ptr<LogicalOperator> &logical_operator) {
  Table *table = insert_stmt->table();
  // vector<Value> values(insert_stmt->values(),
  //                      insert_stmt->values() + insert_stmt->value_amount());

  // InsertLogicalOperator *insert_operator =
  //     new InsertLogicalOperator(table, values);
  InsertLogicalOperator *insert_operator =
      new InsertLogicalOperator(table, insert_stmt->insert_values());
  logical_operator.reset(insert_operator);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(
    UpdateStmt *update_stmt, unique_ptr<LogicalOperator> &logical_operator) {
  Table *table = update_stmt->table();
  FilterStmt *filter_stmt = update_stmt->filter_stmt();
  std::vector<std::string> attribute_names = update_stmt->attribute_names();
  std::vector<Value> values = update_stmt->values();
  std::map<std::string, SelectStmt *> col_name_to_selects =
      update_stmt->col_name_to_selects();

  std::vector<Field> fields;
  for (int i = table->table_meta().sys_field_num();
       i < table->table_meta().field_num(); i++) {
    const FieldMeta *field_meta = table->table_meta().field(i);
    fields.push_back(Field(table, field_meta));
  }
  unique_ptr<LogicalOperator> table_get_oper(
      new TableGetLogicalOperator(table, fields, false /*readonly*/));

  unique_ptr<LogicalOperator> predicate_oper;
  RC rc = create_plan(filter_stmt, predicate_oper);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  std::vector<std::string> set_selects_attr_name;
  for (auto iter : col_name_to_selects) {
    set_selects_attr_name.push_back(iter.first);
  }

  unique_ptr<UpdateLogicalOperator> update_oper(new UpdateLogicalOperator(
      table, attribute_names, values, set_selects_attr_name));

  if (!col_name_to_selects.empty()) {
    // exsits selects in 'SET col EQ value'

    std::map<std::string, SelectStmt *>::iterator iter =
        col_name_to_selects.begin();

    for (; iter != col_name_to_selects.end(); iter++) {
      std::string col_name = iter->first;
      unique_ptr<LogicalOperator> tmp_logical_oper;
      rc = this->create_plan((iter->second), tmp_logical_oper);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      // set_selects_attr_name.emplace_back(col_name);
      update_oper.get()->add_set_selects_oper(std::move(tmp_logical_oper));
    }
  }

  if (predicate_oper) {
    predicate_oper->add_child(std::move(table_get_oper));
    update_oper->add_child(std::move(predicate_oper));
  } else {
    update_oper->add_child(std::move(table_get_oper));
  }

  logical_operator = std::move(update_oper);

  return rc;
}

RC LogicalPlanGenerator::create_plan(
    DeleteStmt *delete_stmt, unique_ptr<LogicalOperator> &logical_operator) {
  Table *table = delete_stmt->table();
  FilterStmt *filter_stmt = delete_stmt->filter_stmt();
  std::vector<Field> fields;
  for (int i = table->table_meta().sys_field_num();
       i < table->table_meta().field_num(); i++) {
    const FieldMeta *field_meta = table->table_meta().field(i);
    fields.push_back(Field(table, field_meta));
  }
  unique_ptr<LogicalOperator> table_get_oper(
      new TableGetLogicalOperator(table, fields, false /*readonly*/));

  unique_ptr<LogicalOperator> predicate_oper;
  RC rc = create_plan(filter_stmt, predicate_oper);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  unique_ptr<LogicalOperator> delete_oper(new DeleteLogicalOperator(table));

  if (predicate_oper) {
    predicate_oper->add_child(std::move(table_get_oper));
    delete_oper->add_child(std::move(predicate_oper));
  } else {
    delete_oper->add_child(std::move(table_get_oper));
  }

  logical_operator = std::move(delete_oper);
  return rc;
}

RC LogicalPlanGenerator::create_plan(
    ExplainStmt *explain_stmt, unique_ptr<LogicalOperator> &logical_operator) {
  Stmt *child_stmt = explain_stmt->child();
  unique_ptr<LogicalOperator> child_oper;
  RC rc = create(child_stmt, child_oper);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create explain's child operator. rc=%s", strrc(rc));
    return rc;
  }

  logical_operator = unique_ptr<LogicalOperator>(new ExplainLogicalOperator);
  logical_operator->add_child(std::move(child_oper));
  return rc;
}
