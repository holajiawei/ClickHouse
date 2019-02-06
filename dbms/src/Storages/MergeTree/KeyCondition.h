#pragma once

#include <sstream>
#include <optional>

#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/Set.h>
#include <Core/SortDescription.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTFunction.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/MergeTree/FieldRange.h>


namespace DB
{

class IFunction;
using FunctionBasePtr = std::shared_ptr<IFunctionBase>;


/// Class that extends arbitrary objects with infinities, like +-inf for floats
class FieldWithInfinity
{
public:
    enum Type
    {
        MINUS_INFINITY = -1,
        NORMAL = 0,
        PLUS_INFINITY = 1
    };

    explicit FieldWithInfinity(const Field & field_);
    FieldWithInfinity(Field && field_);

    static FieldWithInfinity getMinusInfinity();
    static FieldWithInfinity getPlusinfinity();

    bool operator<(const FieldWithInfinity & other) const;
    bool operator==(const FieldWithInfinity & other) const;

private:
    Field field;
    Type type;

    FieldWithInfinity(const Type type_);
};


/** Condition on the index.
  *
  * Consists of the conditions for the key belonging to all possible ranges or sets,
  *  as well as logical operators AND/OR/NOT above these conditions.
  *
  * Constructs a reverse polish notation from these conditions
  *  and can calculate (interpret) its satisfiability over key ranges.
  */
class KeyCondition
{
public:
    /// Does not take into account the SAMPLE section. all_columns - the set of all columns of the table.
    KeyCondition(
        const SelectQueryInfo & query_info,
        const Context & context,
        const Names & key_column_names,
        const ExpressionActionsPtr & key_expr);

    /// Whether the condition is feasible in the key range.
    /// left_key and right_key must contain all fields in the sort_descr in the appropriate order.
    /// data_types - the types of the key columns.
    bool mayBeTrueInRange(size_t used_key_size, const Field * left_key, const Field * right_key, const DataTypes & data_types) const;

    /// Whether the condition is feasible in the direct product of single column ranges specified by `parallelogram`.
    bool mayBeTrueInParallelogram(const std::vector<Range> & parallelogram, const DataTypes & data_types) const;

    /// Is the condition valid in a semi-infinite (not limited to the right) key range.
    /// left_key must contain all the fields in the sort_descr in the appropriate order.
    bool mayBeTrueAfter(size_t used_key_size, const Field * left_key, const DataTypes & data_types) const;

    /// Checks that the index can not be used.
    bool alwaysUnknownOrTrue() const;

    /// Get the maximum number of the key element used in the condition.
    size_t getMaxKeyColumn() const;

    /// Impose an additional condition: the value in the column `column` must be in the range `range`.
    /// Returns whether there is such a column in the key.
    bool addCondition(const String & column, const Range & range);

    String toString() const;


    /** A chain of possibly monotone functions.
      * If the key column is wrapped in functions that can be monotonous in some value ranges
      * (for example: -toFloat64(toDayOfWeek(date))), then here the functions will be located: toDayOfWeek, toFloat64, negate.
      */
    using MonotonicFunctionsChain = std::vector<FunctionBasePtr>;


    static Block getBlockWithConstants(
        const ASTPtr & query, const SyntaxAnalyzerResultPtr & syntax_analyzer_result, const Context & context);

    static std::optional<Range> applyMonotonicFunctionsChainToRange(
        Range key_range,
        MonotonicFunctionsChain & functions,
        DataTypePtr current_type);

private:
    /// The expression is stored as Reverse Polish Notation.
    struct RPNElement
    {
        enum Function
        {
            /// Atoms of a Boolean expression.
            FUNCTION_IN_RANGE,
            FUNCTION_NOT_IN_RANGE,
            FUNCTION_IN_SET,
            FUNCTION_NOT_IN_SET,
            FUNCTION_UNKNOWN, /// Can take any value.
            /// Operators of the logical expression.
            FUNCTION_NOT,
            FUNCTION_AND,
            FUNCTION_OR,
            /// Constants
            ALWAYS_FALSE,
            ALWAYS_TRUE,
        };

        RPNElement() {}
        RPNElement(Function function_) : function(function_) {}
        RPNElement(Function function_, size_t key_column_) : function(function_), key_column(key_column_) {}
        RPNElement(Function function_, size_t key_column_, const Range & range_)
            : function(function_), range(range_), key_column(key_column_) {}

        String toString() const;

        Function function = FUNCTION_UNKNOWN;

        /// For FUNCTION_IN_RANGE and FUNCTION_NOT_IN_RANGE.
        Range range;
        size_t key_column = 0;
        std::vector<size_t> function_argument_stack;
        /// For FUNCTION_IN_SET, FUNCTION_NOT_IN_SET
        using MergeTreeSetIndexPtr = std::shared_ptr<MergeTreeSetIndex>;
        MergeTreeSetIndexPtr set_index;

        mutable MonotonicFunctionsChain monotonic_functions_chain;    /// The function execution does not violate the constancy.
    };

    using RPN = std::vector<RPNElement>;
    using ColumnIndices = std::map<String, size_t>;

    using AtomMap = std::unordered_map<std::string, bool(*)(RPNElement & out, const Field & value)>;

public:
    static const AtomMap atom_map;

private:
    bool mayBeTrueInRange(
        size_t used_key_size,
        const Field * left_key,
        const Field * right_key,
        const DataTypes & data_types,
        bool right_bounded) const;

    void traverseAST(const ASTPtr & node, const Context & context, Block & block_with_constants);
    bool atomFromAST(const ASTPtr & node, const Context & context, Block & block_with_constants, RPNElement & out);
    bool operatorFromAST(const ASTFunction * func, RPNElement & out);

    /** Is node the key column
      *  or expression in which column of key is wrapped by chain of functions,
      *  that can be monotonic on certain ranges?
      * If these conditions are true, then returns number of column in key, type of resulting expression
      *  and fills chain of possibly-monotonic functions.
      */
    bool isKeyPossiblyWrappedByMonotonicFunctions(
        const ASTPtr & node,
        const Context & context,
        size_t & out_key_column_num,
        DataTypePtr & out_key_res_column_type,
        MonotonicFunctionsChain & out_functions_chain);

    bool isKeyPossiblyWrappedByMonotonicFunctionsImpl(
        const ASTPtr & node,
        size_t & out_key_column_num,
        DataTypePtr & out_key_column_type,
        std::vector<const ASTFunction *> & out_functions_chain);

    bool canConstantBeWrappedByMonotonicFunctions(
        const ASTPtr & node,
        size_t & out_key_column_num,
        DataTypePtr & out_key_column_type,
        Field & out_value,
        DataTypePtr & out_type);

    /// If it's possible to make an RPNElement
    /// that will filter values (possibly tuples) by the content of 'prepared_set',
    /// do it and return true.
    bool tryPrepareSetIndex(
        const ASTs & args,
        const Context & context,
        RPNElement & out,
        size_t & out_key_column_num);

    RPN rpn;

    ColumnIndices key_columns;
    ExpressionActionsPtr key_expr;
    PreparedSets prepared_sets;
};

}
