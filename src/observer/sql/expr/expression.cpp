/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/07/05.
//

#include "sql/expr/expression.h"

#include "sql/expr/tuple.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/physical_operator.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/stmt/stmt.h"

using namespace std;

RC FieldExpr::get_value(const Tuple &tuple, Value &value) const {
  if (field_.get_aggr_func_type() != AggrFuncType::NONE) {
  }
  return tuple.find_cell(TupleCellSpec(table_name(), field_name()), value);
}

RC ValueExpr::get_value(const Tuple &tuple, Value &value) const {
  value = value_;
  return RC::SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
CastExpr::CastExpr(unique_ptr<Expression> child, AttrType cast_type)
    : child_(std::move(child)), cast_type_(cast_type) {}

CastExpr::~CastExpr() {}

RC CastExpr::cast(const Value &value, Value &cast_value) const {
  RC rc = RC::SUCCESS;
  if (this->value_type() == value.attr_type()) {
    cast_value = value;
    return rc;
  }

  switch (cast_type_) {
    case BOOLEANS: {
      bool val = value.get_boolean();
      cast_value.set_boolean(val);
    } break;
    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported convert from type %d to %d", child_->value_type(),
               cast_type_);
    }
  }
  return rc;
}

RC CastExpr::get_value(const Tuple &tuple, Value &cell) const {
  RC rc = child_->get_value(tuple, cell);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(cell, cell);
}

RC CastExpr::try_get_value(Value &value) const {
  RC rc = child_->try_get_value(value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, value);
}

////////////////////////////////////////////////////////////////////////////////

ComparisonExpr::ComparisonExpr(ExprOp comp, unique_ptr<Expression> left,
                               unique_ptr<Expression> right)
    : comp_(comp), left_(std::move(left)), right_(std::move(right)) {}

ComparisonExpr::~ComparisonExpr() {}

RC ComparisonExpr::compare_value(const Value &left, const Value &right,
                                 bool &result) const {
  RC rc = RC::SUCCESS;
  // int cmp_result = left.compare(right);
  int cmp_result;
  rc = left.compare(right, cmp_result);
  if (rc != RC::SUCCESS) {
    LOG_WARN("Compare value error, left type:%s, right type:%s.",
             attr_type_to_string(left.attr_type()),
             attr_type_to_string(right.attr_type()));
    // return rc;
  }
  result = false;
  switch (comp_) {
    case EQUAL_TO: {
      result = (0 == cmp_result);
    } break;
    case LESS_EQUAL: {
      result = (cmp_result <= 0);
    } break;
    case NOT_EQUAL: {
      result = (cmp_result != 0);
    } break;
    case LESS_THAN: {
      result = (cmp_result < 0);
    } break;
    case GREAT_EQUAL: {
      result = (cmp_result >= 0);
    } break;
    case GREAT_THAN: {
      result = (cmp_result > 0);
    } break;
    case ExprOp::IS_EQUAL: {
      // 只要两边同时是null即返回通过
      //  result = (0 == cmp_result);
      if ((left.attr_type() == AttrType::NULL_ATTR) &&
          (right.attr_type() == AttrType::NULL_ATTR)) {
        rc = RC::SUCCESS;
        result = true;
      } else {
        rc = RC::SUCCESS;
        result = false;
      }
    } break;
    case ExprOp::IS_NOT_EQUAL: {
      // 只要不是两边同时是null即返回通过
      //  result = (0 == cmp_result);
      if (!((left.attr_type() == AttrType::NULL_ATTR) &&
            (right.attr_type() == AttrType::NULL_ATTR))) {
        rc = RC::SUCCESS;
        result = true;
      } else {
        rc = RC::SUCCESS;
        result = false;
      }
    } break;
    case ExprOp::LIKE: {
      result = (0 == cmp_result);
    } break;
    case ExprOp::NOT_LIKE: {
      result = (0 != cmp_result);
    } break;
    case ExprOp::IN_COMP: {
      result = (0 == cmp_result);
    } break;
    case ExprOp::NOT_IN_COMP: {
      result = (0 == cmp_result);
    } break;
    default: {
      LOG_WARN("unsupported comparison. %d", comp_);
      rc = RC::INTERNAL;
    } break;
  }

  return rc;
}

ComparisonExpr *ComparisonExpr::clone() {
  std::unique_ptr<Expression> new_left;
  std::unique_ptr<Expression> new_right;

  if (left_type() == ExprType::FIELD) {
    Field *tmp_field = left_field();
    new_left = std::move(unique_ptr<Expression>(
        static_cast<Expression *>(new FieldExpr(*tmp_field))));
  } else {
    Value tmp_value = static_cast<ValueExpr *>(left().get())->get_value();
    new_left = std::move(unique_ptr<Expression>(
        static_cast<Expression *>(new ValueExpr(tmp_value))));
  }

  if (right_type() == ExprType::FIELD) {
    Field *tmp_field = right_field();
    new_right = std::move(unique_ptr<Expression>(
        static_cast<Expression *>(new FieldExpr(*tmp_field))));
  } else {
    Value tmp_value = static_cast<ValueExpr *>(right().get())->get_value();
    new_right = std::move(unique_ptr<Expression>(
        static_cast<Expression *>(new ValueExpr(tmp_value))));
  }

  return new ComparisonExpr(this->comp(), std::move(new_left),
                            std::move(new_right));
}

RC ComparisonExpr::try_get_value(Value &cell) const {
  if (left_->type() == ExprType::VALUE && right_->type() == ExprType::VALUE) {
    ValueExpr *left_value_expr = static_cast<ValueExpr *>(left_.get());
    ValueExpr *right_value_expr = static_cast<ValueExpr *>(right_.get());
    const Value &left_cell = left_value_expr->get_value();
    const Value &right_cell = right_value_expr->get_value();

    bool value = false;
    RC rc = compare_value(left_cell, right_cell, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
    } else {
      cell.set_boolean(value);
    }
    return rc;
  }

  return RC::INVALID_ARGUMENT;
}

RC ComparisonExpr::get_value(const Tuple &tuple, Value &value) const {
  Value left_value;
  Value right_value;
  if (this->comp() == ExprOp::IN_COMP) {
    // todo 这里要判断一个集合的逻辑 不断取左右的value形成左右value的集合
    //  然后比较两边的集合
    // 当前仅考虑左边是单值 右边是集合

    RC rc = left_->get_value(tuple, left_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }

    if (right_->type() == ExprType::LIST) {
      std::vector<Value> *tmp_value_set = new std::vector<Value>;
      rc = static_cast<ListExpression *>(right_.get())
               ->get_value_list(tuple, tmp_value_set);
      if (rc != RC::SUCCESS) {
        LOG_WARN("Error when get value set from ExprList: %s.",
                 right_->name().c_str());
        return rc;
      }
      bool result = false;
      for (auto iter : *tmp_value_set) {
        rc = compare_value(left_value, iter, result);
        if (rc != RC::SUCCESS) {
          LOG_WARN("Error when compare value.");
          return rc;
        }
        if (result) {
          value.set_boolean(true);
          break;
        }
      }
      delete tmp_value_set;
      return RC::SUCCESS;
    } else {
      bool result = false;
      Value tmp_value(0);
      rc = static_cast<SelectExpr *>(right_.get())->open();
      while (1) {
        rc = right_->get_value(tuple, tmp_value);
        if (rc != RC::SUCCESS) {
          if (rc == RC::RECORD_EOF) {
            break;
          } else {
            return rc;
          }
        }
        compare_value(left_value, tmp_value, result);
        if (result) {
          value.set_boolean(true);
          return RC::SUCCESS;
        }
      }
      rc = static_cast<SelectExpr *>(right_.get())->close();
      value.set_boolean(false);
      return RC::SUCCESS;
    }

  } else if (this->comp() == ExprOp::NOT_IN_COMP) {
    //  这里判断NOT IN的逻辑

    RC rc = left_->get_value(tuple, left_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }
    bool result = false;
    Value tmp_value(0);
    // std::vector<Value> *tmp_value_set = new std::vector<Value>;
    // rc = static_cast<SelectExpr *>(right_.get())->get_value_list(tuple,
    // tmp_value_set);
    rc = static_cast<SelectExpr *>(right_.get())->open();
    while (1) {
      rc = right_->get_value(tuple, tmp_value);
      if (rc != RC::SUCCESS) {
        if (rc == RC::RECORD_EOF) {
          break;
        } else {
          return rc;
        }
      }
      compare_value(left_value, tmp_value, result);
      if (result) {
        value.set_boolean(false);
        return RC::SUCCESS;
      }
    }
    rc = static_cast<SelectExpr *>(right_.get())->close();
    value.set_boolean(true);
    return RC::SUCCESS;
  } else {
    RC rc = RC::SUCCESS;
    if (left_->type() == ExprType::SELECTION) {
      rc = static_cast<SelectExpr *>(left_.get())->open();
      if (rc != RC::SUCCESS) {
        LOG_WARN("ComparisonExpr left is select, but open failed.");
        return rc;
      }
      Value tmp_value(0);
      std::vector<Value> value_set;
      while (1) {
        rc = left_->get_value(tuple, tmp_value);
        if (rc != RC::SUCCESS) {
          if (rc == RC::RECORD_EOF) {
            break;
          } else {
            return rc;
          }
        }
        value_set.emplace_back(Value(tmp_value));
      }
      if (value_set.size() != 1) {
        LOG_WARN("compare to select can only have 1 rvalue.");
        return RC::SELECT_EXPR_INVALID_ARGUMENT;
      }
      left_value.set_value(value_set[0]);
      // rc = left_->get_value(tuple, left_value);
      // if (rc != RC::SUCCESS) {
      //   LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      //   return rc;
      // }
      rc = static_cast<SelectExpr *>(left_.get())->close();
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to close select expression. rc=%s", strrc(rc));
        return rc;
      }

    } else {
      rc = left_->get_value(tuple, left_value);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
        return rc;
      }
    }

    if (right_->type() == ExprType::SELECTION) {
      rc = static_cast<SelectExpr *>(right_.get())->open();
      if (rc != RC::SUCCESS) {
        LOG_WARN("ComparisonExpr right is select, but open failed.");
        return rc;
      }
      Value tmp_value(0);
      std::vector<Value> value_set;
      while (1) {
        rc = right_->get_value(tuple, tmp_value);
        if (rc != RC::SUCCESS) {
          if (rc == RC::RECORD_EOF) {
            break;
          } else {
            return rc;
          }
        }
        value_set.emplace_back(Value(tmp_value));
      }
      if (value_set.size() != 1) {
        LOG_WARN("compare to select can only have 1 rvalue.");
        return RC::SELECT_EXPR_INVALID_ARGUMENT;
      }
      right_value.set_value(value_set[0]);
      // rc = right_->get_value(tuple, right_value);
      // rc = static_cast<SelectExpr *>(right_.get())->close();
      // if (rc != RC::SUCCESS) {
      //   LOG_WARN("failed to get value of right expression. rc=%s",
      //   strrc(rc)); return rc;
      // }
      rc = static_cast<SelectExpr *>(right_.get())->close();
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to close select expression. rc=%s", strrc(rc));
        return rc;
      }
    } else {
      rc = right_->get_value(tuple, right_value);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
        return rc;
      }
    }

    bool bool_value = false;
    rc = compare_value(left_value, right_value, bool_value);
    if (rc == RC::SUCCESS) {
      value.set_boolean(bool_value);
    }
    return rc;
  }
}

////////////////////////////////////////////////////////////////////////////////
ConjunctionExpr::ConjunctionExpr(Type type,
                                 vector<unique_ptr<Expression>> &children)
    : conjunction_type_(type), children_(std::move(children)) {}

RC ConjunctionExpr::get_value(const Tuple &tuple, Value &value) const {
  RC rc = RC::SUCCESS;
  if (children_.empty()) {
    value.set_boolean(true);
    return rc;
  }

  Value tmp_value;
  for (const unique_ptr<Expression> &expr : children_) {
    rc = expr->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value by child expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = tmp_value.get_boolean();
    if ((conjunction_type_ == Type::AND && !bool_value) ||
        (conjunction_type_ == Type::OR && bool_value)) {
      value.set_boolean(bool_value);
      return rc;
    }
  }

  bool default_value = (conjunction_type_ == Type::AND);
  value.set_boolean(default_value);
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, Expression *left,
                               Expression *right)
    : arithmetic_type_(type), left_(left), right_(right) {}
ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type,
                               unique_ptr<Expression> left,
                               unique_ptr<Expression> right)
    : arithmetic_type_(type),
      left_(std::move(left)),
      right_(std::move(right)) {}

AttrType ArithmeticExpr::value_type() const {
  if (!right_) {
    return left_->value_type();
  }

  if (left_->value_type() == AttrType::NULL_ATTR ||
      right_->value_type() == AttrType::NULL_ATTR) {
    return AttrType::NULL_ATTR;
  }

  if (left_->value_type() == AttrType::INTS &&
      right_->value_type() == AttrType::INTS && arithmetic_type_ != Type::DIV) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value,
                              Value &value) const {
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();
  if (target_type == AttrType::NULL_ATTR) {
    value.set_null(nullptr, 4);
    return rc;
  }

  switch (arithmetic_type_) {
    case Type::ADD: {
      if (target_type == AttrType::INTS) {
        value.set_int(left_value.get_int() + right_value.get_int());
      } else {
        value.set_float(left_value.get_float() + right_value.get_float());
      }
    } break;

    case Type::SUB: {
      if (target_type == AttrType::INTS) {
        value.set_int(left_value.get_int() - right_value.get_int());
      } else {
        value.set_float(left_value.get_float() - right_value.get_float());
      }
    } break;

    case Type::MUL: {
      if (target_type == AttrType::INTS) {
        value.set_int(left_value.get_int() * right_value.get_int());
      } else {
        value.set_float(left_value.get_float() * right_value.get_float());
      }
    } break;

    case Type::DIV: {
      if (target_type == AttrType::INTS) {
        if (right_value.get_int() == 0) {
          // NOTE:
          // 设置为整数最大值是不正确的。通常的做法是设置为NULL，但是当前的miniob没有NULL概念，所以这里设置为整数最大值。
          // value.set_int(numeric_limits<int>::max());
          value.set_null(nullptr, 4);
        } else {
          value.set_int(left_value.get_int() / right_value.get_int());
        }
      } else {
        if (right_value.get_float() > -EPSILON &&
            right_value.get_float() < EPSILON) {
          // NOTE:
          // 设置为浮点数最大值是不正确的。通常的做法是设置为NULL，但是当前的miniob没有NULL概念，所以这里设置为浮点数最大值。
          // value.set_float(numeric_limits<float>::max());
          value.set_null(nullptr, 4);
        } else {
          value.set_float(left_value.get_float() / right_value.get_float());
        }
      }
    } break;

    case Type::NEGATIVE: {
      if (target_type == AttrType::INTS) {
        value.set_int(-left_value.get_int());
      } else {
        value.set_float(-left_value.get_float());
      }
    } break;

    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported arithmetic type. %d", arithmetic_type_);
    } break;
  }
  return rc;
}

RC ArithmeticExpr::get_value(const Tuple &tuple, Value &value) const {
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  return calc_value(left_value, right_value, value);
}

RC ArithmeticExpr::try_get_value(Value &value) const {
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->try_get_value(left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->try_get_value(right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

RC SelectExpr::get_value(const Tuple &tuple, Value &value) const {
  RC rc = RC::SUCCESS;
  if (RC::SUCCESS == (rc = this->pyhsical_root_ptr_->next())) {
    Tuple *tuple = this->pyhsical_root_ptr_->current_tuple();
    if (tuple->cell_num() > 1) {
      LOG_WARN("select expression has more than one col.");
      return RC::SELECT_EXPR_INVALID_ARGUMENT;
    }
    rc = tuple->cell_at(0, value);
    return rc;
  }
  return rc;
}

RC SelectExpr::get_value_list(const Tuple &tuple,
                              std::vector<Value> *&value_set) {
  RC rc = RC::SUCCESS;
  rc = this->open();
  if (rc != RC::SUCCESS) {
    LOG_WARN("error when open SelectExpr.");
    return rc;
  }

  while (1) {
    Value *tmp_value = new Value(0);
    rc = this->get_value(tuple, *tmp_value);
    if (rc != RC::SUCCESS) {
      if (rc == RC::RECORD_EOF) {
        break;
      } else {
        LOG_WARN("Error when get value from child in %s.",
                 this->name().c_str());
        return rc;
      }
    }
    value_set->push_back(*tmp_value);
  }
  rc = this->close();
  if (rc != RC::SUCCESS) {
    LOG_WARN("error when close SelectExpr.");
    return rc;
  }
  return rc;
}

SelectExpr::SelectExpr(Stmt *stmt,
                       std::map<std::string, LogicalOperator *> *map) {
  std::unique_ptr<LogicalOperator> tmp_ptr(nullptr);
  logical_plan_generator_->create(stmt, tmp_ptr, map);
  logical_root_ptr_ = tmp_ptr.release();
}

RC SelectExpr::gen_physical() {
  std::unique_ptr<PhysicalOperator> tmp_physical_oper(nullptr);
  physical_plan_generator_->create(*this->logical_root_ptr_, tmp_physical_oper);
  this->pyhsical_root_ptr_ = tmp_physical_oper.release();
  return RC::SUCCESS;
}
RC SelectExpr::open() { return this->pyhsical_root_ptr_->open(nullptr); }

RC SelectExpr::close() { return this->pyhsical_root_ptr_->close(); }

///////////////////////////////////////////////////////////////

// ListExpression

RC ListExpression::get_value(const Tuple &tuple, Value &value) const {
  // 理论来说一个ListExpression是不应该有get_value单值的
  // 我们这里返回列表第一个值
  if (this->expr_list.empty()) {
    value.set_null(nullptr, 4);
  }
  RC rc = expr_list.at(0)->get_value(tuple, value);
  return rc;
}

RC ListExpression::get_value_list(const Tuple &tuple,
                                  std::vector<Value> *&value_set) {
  // 返回列表中的Expression对应的值的vector
  if (this->expr_list.empty()) {
    return RC::EXPRESSION_LIST_NULL;
  }
  RC rc = RC::SUCCESS;
  for (int i = 0; i < expr_list.size(); i++) {
    Value tmp_value;
    rc = expr_list[i]->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Error when get value of %s.", expr_list[i]->name().c_str());
      return rc;
    }
    LOG_INFO("Get value of %s.", expr_list[i]->name().c_str());
    value_set->push_back(tmp_value);
  }
  return rc;
}