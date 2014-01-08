/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2013 Joshua C. Klontz                                           *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <llvm/PassManager.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Target/TargetLibraryInfo.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Vectorize.h>
#include <lua.hpp>
#include <iostream>
#include <sstream>

#include "likely/likely_compiler.h"

using namespace llvm;
using namespace std;

#define LLVM_VALUE_IS_INT(VALUE) (llvm::isa<Constant>(VALUE))
#define LLVM_VALUE_TO_INT(VALUE) (llvm::cast<Constant>(VALUE)->getUniqueInteger().getZExtValue())

static likely_type likely_type_native = likely_type_null;
static IntegerType *NativeIntegerType = NULL;
static StructType *TheMatrixStruct = NULL;

namespace {

struct TypedValue
{
    Value *value;
    likely_type type;
    TypedValue() : value(NULL), type(likely_type_null) {}
    TypedValue(Value *value_, likely_type type_) : value(value_), type(type_) {}
    operator Value*() const { return value; }
    operator likely_type() const { return type; }
    bool isNull() const { return value == NULL; }
};

struct ExpressionBuilder : public IRBuilder<>
{
    BasicBlock *entry;
    Module *module;
    Function *function;
    MDNode *node;

    typedef map<string,TypedValue> Closure;
    vector<Closure> closures;

    ExpressionBuilder(Module *module, Function *function)
        : IRBuilder<>(getGlobalContext()), module(module), function(function)
    {
        entry = BasicBlock::Create(getGlobalContext(), "entry", function);
        SetInsertPoint(entry);

        // Create self-referencing loop node
        vector<Value*> metadata;
        MDNode *tmp = MDNode::getTemporary(getGlobalContext(), metadata);
        metadata.push_back(tmp);
        node = MDNode::get(getGlobalContext(), metadata);
        tmp->replaceAllUsesWith(node);
        MDNode::deleteTemporary(tmp);
    }

    static TypedValue constant(double value, likely_type type = likely_type_native)
    {
        const int depth = likely_depth(type);
        if (likely_floating(type)) {
            if (value == 0) value = -0; // IEEE/LLVM optimization quirk
            if      (depth == 64) return TypedValue(ConstantFP::get(Type::getDoubleTy(getGlobalContext()), value), type);
            else if (depth == 32) return TypedValue(ConstantFP::get(Type::getFloatTy(getGlobalContext()), value), type);
            else                  { likely_assert(false, "invalid floating point constant depth: %d", depth); return TypedValue(); }
        } else {
            return TypedValue(Constant::getIntegerValue(Type::getIntNTy(getGlobalContext(), depth), APInt(depth, uint64_t(value))), type);
        }
    }

    static TypedValue zero(likely_type type = likely_type_native) { return constant(0, type); }
    static TypedValue one (likely_type type = likely_type_native) { return constant(1, type); }
    static TypedValue intMax(likely_type type) { const int bits = likely_depth(type); return constant((1 << (bits - (likely_signed(type) ? 1 : 0)))-1, bits); }
    static TypedValue intMin(likely_type type) { const int bits = likely_depth(type); return constant(likely_signed(type) ? (1 << (bits - 1)) : 0, bits); }
    static TypedValue type(likely_type type) { return constant(type, int(sizeof(likely_type)*8)); }

    TypedValue data    (const TypedValue &matrix) { return TypedValue(CreatePointerCast(CreateLoad(CreateStructGEP(matrix, 0), "data"), ty(matrix, true)), matrix.type & likely_type_mask); }
    TypedValue channels(const TypedValue &matrix) { return likely_multi_channel(matrix) ? TypedValue(CreateLoad(CreateStructGEP(matrix, 2), "channels"), likely_type_native) : one(); }
    TypedValue columns (const TypedValue &matrix) { return likely_multi_column (matrix) ? TypedValue(CreateLoad(CreateStructGEP(matrix, 3), "columns" ), likely_type_native) : one(); }
    TypedValue rows    (const TypedValue &matrix) { return likely_multi_row    (matrix) ? TypedValue(CreateLoad(CreateStructGEP(matrix, 4), "rows"    ), likely_type_native) : one(); }
    TypedValue frames  (const TypedValue &matrix) { return likely_multi_frame  (matrix) ? TypedValue(CreateLoad(CreateStructGEP(matrix, 5), "frames"  ), likely_type_native) : one(); }

    void steps(const TypedValue &matrix, Value **columnStep, Value **rowStep, Value **frameStep)
    {
        *columnStep = channels(matrix);
        *rowStep    = CreateMul(columns(matrix), *columnStep, "y_step");
        *frameStep  = CreateMul(rows(matrix), *rowStep, "t_step");
    }

    void annotateParallel(Instruction *i) const
    {
        i->setMetadata("llvm.mem.parallel_loop_access", node);
    }

    TypedValue cast(const TypedValue &x, likely_type type)
    {
        if ((x.type & likely_type_mask) == (type & likely_type_mask))
            return x;
        Type *dstType = ty(type);
        return TypedValue(CreateCast(CastInst::getCastOpcode(x, likely_signed(x.type), dstType, likely_signed(type)), x, dstType), type);
    }

    void addVariable(const string &name, const TypedValue &value)
    {
        Closure &closure = closures.back();
        Closure::iterator it = closure.find(name);
        if (it != closure.end()) {
            // Update variable
            CreateStore(value, it->second);
        } else {
            // New variable
            AllocaInst *variable = CreateAlloca(ty(value), 0, name);
            CreateStore(value, variable);
            closure.insert(pair<string,TypedValue>(name, TypedValue(variable, value)));
        }
    }

    TypedValue getVariable(const string &name)
    {
        for (vector<Closure>::reverse_iterator rit = closures.rbegin(); rit != closures.rend(); rit++) {
            Closure::iterator it = rit->find(name);
            if (it != rit->end()) {
                LoadInst *variable = CreateLoad(it->second, name);
                return TypedValue(variable, it->second);
            }
        }
        return TypedValue();
    }

    static Type *ty(likely_type type, bool pointer = false)
    {
        const int bits = likely_depth(type);
        const bool floating = likely_floating(type);
        if (floating) {
            if      (bits == 16) return pointer ? Type::getHalfPtrTy(getGlobalContext())   : Type::getHalfTy(getGlobalContext());
            else if (bits == 32) return pointer ? Type::getFloatPtrTy(getGlobalContext())  : Type::getFloatTy(getGlobalContext());
            else if (bits == 64) return pointer ? Type::getDoublePtrTy(getGlobalContext()) : Type::getDoubleTy(getGlobalContext());
        } else {
            if      (bits == 1)  return pointer ? Type::getInt1PtrTy(getGlobalContext())  : (Type*)Type::getInt1Ty(getGlobalContext());
            else if (bits == 8)  return pointer ? Type::getInt8PtrTy(getGlobalContext())  : (Type*)Type::getInt8Ty(getGlobalContext());
            else if (bits == 16) return pointer ? Type::getInt16PtrTy(getGlobalContext()) : (Type*)Type::getInt16Ty(getGlobalContext());
            else if (bits == 32) return pointer ? Type::getInt32PtrTy(getGlobalContext()) : (Type*)Type::getInt32Ty(getGlobalContext());
            else if (bits == 64) return pointer ? Type::getInt64PtrTy(getGlobalContext()) : (Type*)Type::getInt64Ty(getGlobalContext());
        }
        likely_assert(false, "ty invalid matrix bits: %d and floating: %d", bits, floating);
        return NULL;
    }
};

struct KernelInfo
{
    vector<TypedValue> srcs;
    TypedValue i, c, x, y, t;
    likely_type dims;

    KernelInfo(const vector<TypedValue> &srcs) : srcs(srcs) {}

    void init(const vector<TypedValue> &srcs, ExpressionBuilder &builder, const TypedValue &dst, const TypedValue &i)
    {
        this->srcs = srcs;
        this->i = i;
        this->dims = dst.type & likely_type_multi_dimension;

        Value *columnStep, *rowStep, *frameStep;
        builder.steps(dst, &columnStep, &rowStep, &frameStep);
        Value *frameRemainder = builder.CreateURem(i, frameStep, "t_rem");
        t = TypedValue(builder.CreateUDiv(i, frameStep, "t"), likely_type_native);

        Value *rowRemainder = builder.CreateURem(frameRemainder, rowStep, "y_rem");
        y = TypedValue(builder.CreateUDiv(frameRemainder, rowStep, "y"), likely_type_native);

        Value *columnRemainder = builder.CreateURem(rowRemainder, columnStep, "c");
        x = TypedValue(builder.CreateUDiv(rowRemainder, columnStep, "x"), likely_type_native);

        c = TypedValue(columnRemainder, likely_type_native);
    }
};

struct Operation
{
    static map<string, const Operation*> operations;

    static TypedValue expression(ExpressionBuilder &builder, const KernelInfo &info, likely_ir ir)
    {
        string operator_;
        if (lua_istable(ir, -1)) {
            lua_rawgeti(ir, -1, 1);
            operator_ = lua_tostring(ir, -1);
            lua_pop(ir, 1);
        } else {
            operator_ = lua_tostring(ir, -1);
        }

        map<string,const Operation*>::iterator it = operations.find(operator_);
        if (it != operations.end())
            return it->second->call(builder, info, ir);

        const TypedValue variable = builder.getVariable(operator_);
        if (!variable.isNull())
            return variable;

        bool ok;
        TypedValue c = constant(operator_, &ok);
        if (!ok)
            c = ExpressionBuilder::constant(likely_type_from_string(operator_.c_str()), likely_type_u32);
        likely_assert(c.type != likely_type_null, "unrecognized literal: %s", operator_.c_str());
        return c;
    }

protected:
    static TypedValue constant(const string &str, bool *ok)
    {
        char *p;
        const double value = strtod(str.c_str(), &p);
        const likely_type type = likely_type_from_value(value);
        *ok = (*p == 0);
        return ExpressionBuilder::constant(value, type);
    }

    static likely_type validFloatType(likely_type type)
    {
        likely_set_floating(&type, true);
        likely_set_signed(&type, true);
        likely_set_depth(&type, likely_depth(type) > 32 ? 64 : 32);
        return type;
    }

private:
    virtual TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, likely_ir ir) const
    {
        vector<TypedValue> operands;
        if (lua_istable(ir, -1)) {
            int index = 2;
            bool done = false;
            while (!done) {
                lua_rawgeti(ir, -1, index++);
                if (!lua_isnil(ir, -1)) operands.push_back(expression(builder, info, ir));
                else                    done = true;
                lua_pop(ir, 1);
            }
        }
        return call(builder, info, operands);
    }

    virtual TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, const vector<TypedValue> &args) const = 0;
};
map<string, const Operation*> Operation::operations;

template <class T>
struct RegisterOperation
{
    RegisterOperation(const string &symbol)
    {
        Operation::operations.insert(pair<string, const Operation*>(symbol, new T()));
    }
};
#define LIKELY_REGISTER_OPERATION(OP, SYM) static struct RegisterOperation<OP##Operation> Register##OP##Operation(SYM);
#define LIKELY_REGISTER(OP) LIKELY_REGISTER_OPERATION(OP, #OP)

class letOperation : public Operation
{
    TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, likely_ir ir) const
    {
        builder.closures.push_back(ExpressionBuilder::Closure());

        lua_rawgeti(ir, -1, 2);
        addToClosure(builder, info, ir);
        lua_pop(ir, 1);

        lua_rawgeti(ir, -1, 3);
        TypedValue result = expression(builder, info, ir);
        lua_pop(ir, 1);

        builder.closures.pop_back();
        return result;
    }

    TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, const vector<TypedValue> &args) const
    {
        (void) builder;
        (void) info;
        (void) args;
        likely_assert(false, "'let' logic error");
        return TypedValue();
    }

    void addToClosure(ExpressionBuilder &builder, const KernelInfo &info, likely_ir ir) const
    {
        const int len = luaL_len(ir, -1);
        if (len > 0) {
            for (int i=1; i<=len; i++) {
                lua_rawgeti(ir, -1, i);
                addToClosure(builder, info, ir);
                lua_pop(ir, 1);
            }
        } else {
            lua_pushnil(ir);
            while (lua_next(ir, -2)) {
                lua_pushvalue(ir, -2);
                lua_insert(ir, -2);
                builder.addVariable(lua_tostring(ir, -2), expression(builder, info, ir));
                lua_pop(ir, 2);
            }
        }
    }
};
LIKELY_REGISTER(let)

class NullaryOperation : public Operation
{
    TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, const vector<TypedValue> &args) const
    {
        assert(args.empty());
        (void) args;
        return callNullary(builder, info);
    }
    virtual TypedValue callNullary(ExpressionBuilder &builder, const KernelInfo &info) const = 0;
};

#define LIKELY_REGISTER_INDEX(OP)                                                    \
class OP##Operation : public NullaryOperation                                        \
{                                                                                    \
    TypedValue callNullary(ExpressionBuilder &builder, const KernelInfo &info) const \
    {                                                                                \
        (void) builder;                                                              \
        return info.OP;                                                              \
    }                                                                                \
};                                                                                   \
LIKELY_REGISTER(OP)                                                                  \

LIKELY_REGISTER_INDEX(i)
LIKELY_REGISTER_INDEX(c)
LIKELY_REGISTER_INDEX(x)
LIKELY_REGISTER_INDEX(y)
LIKELY_REGISTER_INDEX(t)

class UnaryOperation : public Operation
{
    TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, const vector<TypedValue> &args) const
    {
        assert(args.size() == 1);
        return callUnary(builder, info, args[0]);
    }
    virtual TypedValue callUnary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &arg) const = 0;
};

class argOperation : public UnaryOperation
{
    TypedValue callUnary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &arg) const
    {
        int index = LLVM_VALUE_TO_INT(arg.value);
        const TypedValue &matrix = info.srcs[index];
        Value *i;
        if ((matrix.type & likely_type_multi_dimension) == info.dims) {
            // This matrix has the same dimensionality as the output
            i = info.i;
        } else {
            Value *columnStep, *rowStep, *frameStep;
            builder.steps(matrix, &columnStep, &rowStep, &frameStep);
            i = ExpressionBuilder::zero();
            if (likely_multi_channel(matrix)) i = info.c;
            if (likely_multi_column(matrix))  i = builder.CreateAdd(builder.CreateMul(info.x, columnStep), i);
            if (likely_multi_row(matrix))     i = builder.CreateAdd(builder.CreateMul(info.y, rowStep), i);
            if (likely_multi_frame(matrix))   i = builder.CreateAdd(builder.CreateMul(info.t, frameStep), i);
        }

        LoadInst *load = builder.CreateLoad(builder.CreateGEP(builder.data(matrix), i));
        builder.annotateParallel(load);
        return TypedValue(load, matrix.type);
    }
};
LIKELY_REGISTER(arg)

class typeOperation : public UnaryOperation
{
    TypedValue callUnary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &arg) const
    {
        (void) builder;
        (void) info;
        return ExpressionBuilder::type(arg);
    }
};
LIKELY_REGISTER(type)

class UnaryMathOperation : public UnaryOperation
{
    TypedValue callUnary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &x) const
    {
        (void) info;
        TypedValue xc = builder.cast(x, validFloatType(x.type));
        vector<Type*> args;
        args.push_back(xc.value->getType());
        return TypedValue(builder.CreateCall(Intrinsic::getDeclaration(builder.module, id(), args), xc), xc.type);
    }
    virtual Intrinsic::ID id() const = 0;
};

#define LIKELY_REGISTER_UNARY_MATH(OP)                 \
class OP##Operation : public UnaryMathOperation        \
{                                                      \
    Intrinsic::ID id() const { return Intrinsic::OP; } \
};                                                     \
LIKELY_REGISTER(OP)                                    \

LIKELY_REGISTER_UNARY_MATH(sqrt)
LIKELY_REGISTER_UNARY_MATH(sin)
LIKELY_REGISTER_UNARY_MATH(cos)
LIKELY_REGISTER_UNARY_MATH(exp)
LIKELY_REGISTER_UNARY_MATH(exp2)
LIKELY_REGISTER_UNARY_MATH(log)
LIKELY_REGISTER_UNARY_MATH(log10)
LIKELY_REGISTER_UNARY_MATH(log2)
LIKELY_REGISTER_UNARY_MATH(fabs)
LIKELY_REGISTER_UNARY_MATH(floor)
LIKELY_REGISTER_UNARY_MATH(ceil)
LIKELY_REGISTER_UNARY_MATH(trunc)
LIKELY_REGISTER_UNARY_MATH(rint)
LIKELY_REGISTER_UNARY_MATH(nearbyint)
LIKELY_REGISTER_UNARY_MATH(round)

class BinaryOperation : public Operation
{
    TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, const vector<TypedValue> &args) const
    {
        assert(args.size() == 2);
        return callBinary(builder, info, args[0], args[1]);
    }
    virtual TypedValue callBinary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &arg1, const TypedValue &arg2) const = 0;
};

class castOperation : public BinaryOperation
{
    TypedValue callBinary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &x, const TypedValue &type) const
    {
        (void) info;
        return builder.cast(x, (likely_type)LLVM_VALUE_TO_INT(type.value));
    }
};
LIKELY_REGISTER(cast)

class ArithmeticOperation : public BinaryOperation
{
    TypedValue callBinary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &arg1, const TypedValue &arg2) const
    {
        (void) info;
        likely_type type = likely_type_from_types(arg1, arg2);
        return callArithmetic(builder, builder.cast(arg1, type), builder.cast(arg2, type), type);
    }
    virtual TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &lhs, const TypedValue &rhs, likely_type type) const = 0;
};

class addOperation : public ArithmeticOperation
{
    TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &lhs, const TypedValue &rhs, likely_type type) const
    {
        if (likely_floating(type)) {
            return TypedValue(builder.CreateFAdd(lhs, rhs), type);
        } else {
            if (likely_saturation(type)) {
                CallInst *result = builder.CreateCall2(Intrinsic::getDeclaration(builder.module, likely_signed(type) ? Intrinsic::sadd_with_overflow : Intrinsic::uadd_with_overflow, lhs.value->getType()), lhs, rhs);
                Value *overflowResult = likely_signed(type) ? builder.CreateSelect(builder.CreateICmpSGE(lhs, ExpressionBuilder::zero(type)), builder.intMax(type), builder.intMin(type)) : ExpressionBuilder::intMax(type);
                return TypedValue(builder.CreateSelect(builder.CreateExtractValue(result, 1), overflowResult, builder.CreateExtractValue(result, 0)), type);
            } else {
                return TypedValue(builder.CreateAdd(lhs, rhs), type);
            }
        }
    }
};
LIKELY_REGISTER_OPERATION(add, "+")

class subtractOperation : public ArithmeticOperation
{
    TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &lhs, const TypedValue &rhs, likely_type type) const
    {
        if (likely_floating(type)) {
            return TypedValue(builder.CreateFSub(lhs, rhs), type);
        } else {
            if (likely_saturation(type)) {
                CallInst *result = builder.CreateCall2(Intrinsic::getDeclaration(builder.module, likely_signed(type) ? Intrinsic::ssub_with_overflow : Intrinsic::usub_with_overflow, lhs.value->getType()), lhs, rhs);
                Value *overflowResult = likely_signed(type) ? builder.CreateSelect(builder.CreateICmpSGE(lhs, ExpressionBuilder::zero(type)), builder.intMax(type), builder.intMin(type)) : ExpressionBuilder::intMin(type);
                return TypedValue(builder.CreateSelect(builder.CreateExtractValue(result, 1), overflowResult, builder.CreateExtractValue(result, 0)), type);
            } else {
                return TypedValue(builder.CreateSub(lhs, rhs), type);
            }
        }
    }
};
LIKELY_REGISTER_OPERATION(subtract, "-")

class multiplyOperation : public ArithmeticOperation
{
    TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &lhs, const TypedValue &rhs, likely_type type) const
    {
        if (likely_floating(type)) {
            return TypedValue(builder.CreateFMul(lhs, rhs), type);
        } else {
            if (likely_saturation(type)) {
                CallInst *result = builder.CreateCall2(Intrinsic::getDeclaration(builder.module, likely_signed(type) ? Intrinsic::smul_with_overflow : Intrinsic::umul_with_overflow, lhs.value->getType()), lhs, rhs);
                Value *zero = ExpressionBuilder::zero(type);
                Value *overflowResult = likely_signed(type) ? builder.CreateSelect(builder.CreateXor(builder.CreateICmpSGE(lhs, zero), builder.CreateICmpSGE(rhs, zero)), builder.intMin(type), builder.intMax(type)) : ExpressionBuilder::intMax(type);
                return TypedValue(builder.CreateSelect(builder.CreateExtractValue(result, 1), overflowResult, builder.CreateExtractValue(result, 0)), type);
            } else {
                return TypedValue(builder.CreateMul(lhs, rhs), type);
            }
        }
    }
};
LIKELY_REGISTER_OPERATION(multiply, "*")

class divideOperation : public ArithmeticOperation
{
    TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &n, const TypedValue &d, likely_type type) const
    {
        if (likely_floating(type)) {
            return TypedValue(builder.CreateFDiv(n, d), type);
        } else {
            if (likely_signed(type)) {
                if (likely_saturation(type)) {
                    Value *safe_i = builder.CreateAdd(n, builder.CreateZExt(builder.CreateICmpNE(builder.CreateOr(builder.CreateAdd(d, ExpressionBuilder::one(type)), builder.CreateAdd(n, ExpressionBuilder::intMin(type))), ExpressionBuilder::zero(type)), n.value->getType()));
                    return TypedValue(builder.CreateSDiv(safe_i, d), type);
                } else {
                    return TypedValue(builder.CreateSDiv(n, d), type);
                }
            } else {
                return TypedValue(builder.CreateUDiv(n, d), type);
            }
        }
    }
};
LIKELY_REGISTER_OPERATION(divide, "/")

#define LIKELY_REGISTER_COMPARISON(OP, SYM)                                                                                     \
class OP##Operation : public ArithmeticOperation                                                                                \
{                                                                                                                               \
    TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &lhs, const TypedValue &rhs, likely_type type) const \
    {                                                                                                                           \
        return TypedValue(likely_floating(type) ? builder.CreateFCmpO##OP(lhs, rhs)                                             \
                                                : (likely_signed(type) ? builder.CreateICmpS##OP(lhs, rhs)                      \
                                                                       : builder.CreateICmpU##OP(lhs, rhs)), type);             \
    }                                                                                                                           \
};                                                                                                                              \
LIKELY_REGISTER_OPERATION(OP, SYM)                                                                                              \

LIKELY_REGISTER_COMPARISON(LT, "<")
LIKELY_REGISTER_COMPARISON(LE, "<=")
LIKELY_REGISTER_COMPARISON(GT, ">")
LIKELY_REGISTER_COMPARISON(GE, ">=")

#define LIKELY_REGISTER_EQUALITY(OP, SYM)                                                                                       \
class OP##Operation : public ArithmeticOperation                                                                                \
{                                                                                                                               \
    TypedValue callArithmetic(ExpressionBuilder &builder, const TypedValue &lhs, const TypedValue &rhs, likely_type type) const \
    {                                                                                                                           \
        return TypedValue(likely_floating(type) ? builder.CreateFCmpO##OP(lhs, rhs)                                             \
                                                : builder.CreateICmp##OP(lhs, rhs), type);                                      \
    }                                                                                                                           \
};                                                                                                                              \
LIKELY_REGISTER_OPERATION(OP, SYM)                                                                                              \

LIKELY_REGISTER_EQUALITY(EQ, "==")
LIKELY_REGISTER_EQUALITY(NE, "!=")

class BinaryMathOperation : public BinaryOperation
{
    TypedValue callBinary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &x, const TypedValue &n) const
    {
        (void) info;
        const likely_type type = nIsInteger() ? x.type : likely_type_from_types(x, n);
        TypedValue xc = builder.cast(x, validFloatType(type));
        TypedValue nc = builder.cast(n, nIsInteger() ? likely_type_i32 : xc.type);
        vector<Type*> args;
        args.push_back(xc.value->getType());
        return TypedValue(builder.CreateCall2(Intrinsic::getDeclaration(builder.module, id(), args), xc, nc), xc.type);
    }
    virtual Intrinsic::ID id() const = 0;
    virtual bool nIsInteger() const { return false; }
};

class powiOperation : public BinaryMathOperation
{
    Intrinsic::ID id() const { return Intrinsic::powi; }
    bool nIsInteger() const { return true; }
};
LIKELY_REGISTER(powi)

#define LIKELY_REGISTER_BINARY_MATH(OP)                \
class OP##Operation : public BinaryMathOperation       \
{                                                      \
    Intrinsic::ID id() const { return Intrinsic::OP; } \
};                                                     \
LIKELY_REGISTER(OP)                                    \

LIKELY_REGISTER_BINARY_MATH(pow)
LIKELY_REGISTER_BINARY_MATH(copysign)

class TernaryOperation : public Operation
{
    TypedValue call(ExpressionBuilder &builder, const KernelInfo &info, const vector<TypedValue> &args) const
    {
        assert(args.size() == 3);
        return callTernary(builder, info, args[0], args[1], args[2]);
    }
    virtual TypedValue callTernary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &arg1, const TypedValue &arg2, const TypedValue &arg3) const = 0;
};

class fmaOperation : public TernaryOperation
{
    TypedValue callTernary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &a, const TypedValue &b, const TypedValue &c) const
    {
        (void) info;
        const likely_type type = likely_type_from_types(likely_type_from_types(a, b), c);
        TypedValue ac = builder.cast(a, validFloatType(type));
        TypedValue bc = builder.cast(b, ac.type);
        TypedValue cc = builder.cast(c, ac.type);
        vector<Type*> args;
        args.push_back(ac.value->getType());
        return TypedValue(builder.CreateCall3(Intrinsic::getDeclaration(builder.module, Intrinsic::fma, args), ac, bc, cc), ac.type);
    }
};
LIKELY_REGISTER(fma)

class selectOperation : public TernaryOperation
{
    TypedValue callTernary(ExpressionBuilder &builder, const KernelInfo &info, const TypedValue &c, const TypedValue &t, const TypedValue &f) const
    {
         (void) info;
         const likely_type type = likely_type_from_types(t, f);
         return TypedValue(builder.CreateSelect(c,  builder.cast(t, type), builder.cast(f, type)), type);
    }
};
LIKELY_REGISTER(select)

struct JITResources
{
    string name;
    Module *module;
    ExecutionEngine *executionEngine;
    TargetMachine *targetMachine;

    JITResources(bool native, const string &symbol_name = string())
        : name(symbol_name)
    {
        if (TheMatrixStruct == NULL) {
            assert(sizeof(likely_size) == sizeof(void*));
            InitializeNativeTarget();
            InitializeNativeTargetAsmPrinter();
            InitializeNativeTargetAsmParser();
            initializeScalarOpts(*PassRegistry::getPassRegistry());

            likely_set_depth(&likely_type_native, sizeof(likely_size)*8);
            NativeIntegerType = Type::getIntNTy(getGlobalContext(), likely_depth(likely_type_native));
            TheMatrixStruct = StructType::create("likely_matrix",
                                                 Type::getInt8PtrTy(getGlobalContext()), // data
                                                 PointerType::getUnqual(StructType::create(getGlobalContext(), "likely_matrix_private")), // d_ptr
                                                 NativeIntegerType,                      // channels
                                                 NativeIntegerType,                      // columns
                                                 NativeIntegerType,                      // rows
                                                 NativeIntegerType,                      // frames
                                                 Type::getInt32Ty(getGlobalContext()),   // type
                                                 NULL);
        }

        const bool JIT = symbol_name.empty();

        if (JIT) {
            static int index = 0;
            stringstream stream; stream << "likely_" << index++;
            name = stream.str();
        }

        module = new Module(name, getGlobalContext());
        likely_assert(module != NULL, "failed to create module");

        if (native) {
            string targetTriple = sys::getProcessTriple();
#ifdef _WIN32
            if (JIT)
                targetTriple = targetTriple + "-elf";
#endif
            module->setTargetTriple(targetTriple);
        }

        string error;
        EngineBuilder engineBuilder(module);
        engineBuilder.setMCPU(sys::getHostCPUName())
                     .setEngineKind(EngineKind::JIT)
                     .setOptLevel(CodeGenOpt::Aggressive)
                     .setErrorStr(&error)
                     .setUseMCJIT(true);

        if (JIT) {
            executionEngine = engineBuilder.create();
            likely_assert(executionEngine != NULL, "failed to create execution engine with error: %s", error.c_str());
        } else {
            executionEngine = NULL;
        }

        engineBuilder.setCodeModel(CodeModel::Default);
        targetMachine = engineBuilder.selectTarget();
        likely_assert(targetMachine != NULL, "failed to create target machine with error: %s", error.c_str());
    }

    ~JITResources()
    {
        delete targetMachine;
        delete executionEngine; // owns module
    }
};

struct FunctionBuilder : private JITResources
{
    likely_type *type;
    void *f;

    FunctionBuilder(likely_ir ir, const vector<likely_type> &types, bool native, const string &symbol_name = string())
        : JITResources(native, symbol_name)
    {
        type = new likely_type[types.size()];
        memcpy(type, types.data(), sizeof(likely_type) * types.size());

        Function *function = getFunction(name, module, (likely_arity)types.size(), PointerType::getUnqual(TheMatrixStruct));
        vector<TypedValue> srcs = getArgs(function, types);
        ExpressionBuilder builder(module, function);
        KernelInfo info(srcs);

        TypedValue dstChannels = getDimensions(builder, info, ir, "channels", srcs.size() > 0 ? srcs[0] : TypedValue());
        TypedValue dstColumns  = getDimensions(builder, info, ir, "columns" , srcs.size() > 0 ? srcs[0] : TypedValue());
        TypedValue dstRows     = getDimensions(builder, info, ir, "rows"    , srcs.size() > 0 ? srcs[0] : TypedValue());
        TypedValue dstFrames   = getDimensions(builder, info, ir, "frames"  , srcs.size() > 0 ? srcs[0] : TypedValue());

        Function *thunk;
        likely_type dstType;
        {
            thunk = getFunction(name+"_thunk", module, (likely_arity)types.size(), Type::getVoidTy(getGlobalContext()), PointerType::getUnqual(TheMatrixStruct), NativeIntegerType, NativeIntegerType);
            vector<TypedValue> srcs = getArgs(thunk, types);
            TypedValue stop = srcs.back(); srcs.pop_back();
            stop.value->setName("stop");
            stop.type = likely_type_native;
            TypedValue start = srcs.back(); srcs.pop_back();
            start.value->setName("start");
            start.type = likely_type_native;
            TypedValue dst = srcs.back(); srcs.pop_back();
            dst.value->setName("dst");

            likely_set_multi_channel(&dst.type, likely_multi_channel(dstChannels.type));
            likely_set_multi_column (&dst.type, likely_multi_column (dstColumns.type));
            likely_set_multi_row    (&dst.type, likely_multi_row    (dstRows.type));
            likely_set_multi_frame  (&dst.type, likely_multi_frame  (dstFrames.type));

            ExpressionBuilder builder(module, thunk);
            KernelInfo info(srcs);

            // The kernel assumes there is at least one iteration
            BasicBlock *body = BasicBlock::Create(getGlobalContext(), "kernel_body", thunk);
            builder.CreateBr(body);
            builder.SetInsertPoint(body);
            PHINode *i = builder.CreatePHI(NativeIntegerType, 2, "i");
            i->addIncoming(start, builder.entry);

            info.init(srcs, builder, dst, TypedValue(i, likely_type_native));
            TypedValue result = Operation::expression(builder, info, ir);
            dstType = dst.type = result.type;
            StoreInst *store = builder.CreateStore(result, builder.CreateGEP(builder.data(dst), i));
            builder.annotateParallel(store);

            Value *increment = builder.CreateAdd(i, builder.one(), "kernel_increment");
            BasicBlock *loopLatch = BasicBlock::Create(getGlobalContext(), "kernel_latch", thunk);
            builder.CreateBr(loopLatch);
            builder.SetInsertPoint(loopLatch);
            BasicBlock *loopExit = BasicBlock::Create(getGlobalContext(), "kernel_exit", thunk);
            BranchInst *latch = builder.CreateCondBr(builder.CreateICmpEQ(increment, stop, "kernel_test"), loopExit, body);
            latch->setMetadata("llvm.loop", builder.node);
            i->addIncoming(increment, loopLatch);
            builder.SetInsertPoint(loopExit);

            builder.CreateRetVoid();

            FunctionPassManager functionPassManager(module);
            functionPassManager.add(createVerifierPass(PrintMessageAction));
            if (native) {
                targetMachine->addAnalysisPasses(functionPassManager);
                functionPassManager.add(new TargetLibraryInfo(Triple(module->getTargetTriple())));
                functionPassManager.add(new DataLayout(module));
            }
            functionPassManager.add(createBasicAliasAnalysisPass());
            functionPassManager.add(createLICMPass());
            functionPassManager.add(createLoopVectorizePass());
            functionPassManager.add(createInstructionCombiningPass());
            functionPassManager.add(createEarlyCSEPass());
            functionPassManager.add(createCFGSimplificationPass());
            functionPassManager.doInitialization();
//            DebugFlag = true;
            functionPassManager.run(*thunk);
        }

        static FunctionType* LikelyNewSignature = NULL;
        if (LikelyNewSignature == NULL) {
            Type *newReturn = PointerType::getUnqual(TheMatrixStruct);
            vector<Type*> newParameters;
            newParameters.push_back(Type::getInt32Ty(getGlobalContext())); // type
            newParameters.push_back(NativeIntegerType); // channels
            newParameters.push_back(NativeIntegerType); // columns
            newParameters.push_back(NativeIntegerType); // rows
            newParameters.push_back(NativeIntegerType); // frames
            newParameters.push_back(Type::getInt8PtrTy(getGlobalContext())); // data
            newParameters.push_back(Type::getInt8Ty(getGlobalContext())); // copy
            LikelyNewSignature = FunctionType::get(newReturn, newParameters, false);
        }
        Function *likelyNew = Function::Create(LikelyNewSignature, GlobalValue::ExternalLinkage, "likely_new", module);
        likelyNew->setCallingConv(CallingConv::C);
        likelyNew->setDoesNotAlias(0);
        likelyNew->setDoesNotAlias(6);
        likelyNew->setDoesNotCapture(6);

        std::vector<Value*> likelyNewArgs;
        likelyNewArgs.push_back(ExpressionBuilder::type(dstType));
        likelyNewArgs.push_back(dstChannels);
        likelyNewArgs.push_back(dstColumns);
        likelyNewArgs.push_back(dstRows);
        likelyNewArgs.push_back(dstFrames);
        likelyNewArgs.push_back(ConstantPointerNull::get(Type::getInt8PtrTy(getGlobalContext())));
        likelyNewArgs.push_back(builder.constant(0, 8));
        Value *dst = builder.CreateCall(likelyNew, likelyNewArgs);

        // An impossible case used to ensure that `likely_new` isn't stripped when optimizing executable size
        if (likelyNew == NULL)
            likely_new(likely_type_null, 0, 0, 0, 0, NULL, 0);

        Value *kernelSize = builder.CreateMul(builder.CreateMul(builder.CreateMul(dstChannels, dstColumns), dstRows), dstFrames);

        if (!types.empty() && likely_parallel(types[0])) {
            static FunctionType *likelyForkType = NULL;
            if (likelyForkType == NULL) {
                vector<Type*> likelyForkParameters;
                likelyForkParameters.push_back(thunk->getType());
                likelyForkParameters.push_back(Type::getInt8Ty(getGlobalContext()));
                likelyForkParameters.push_back(NativeIntegerType);
                likelyForkParameters.push_back(PointerType::getUnqual(TheMatrixStruct));
                Type *likelyForkReturn = Type::getVoidTy(getGlobalContext());
                likelyForkType = FunctionType::get(likelyForkReturn, likelyForkParameters, true);
            }
            Function *likelyFork = Function::Create(likelyForkType, GlobalValue::ExternalLinkage, "likely_fork", module);
            likelyFork->setCallingConv(CallingConv::C);
            likelyFork->setDoesNotCapture(4);
            likelyFork->setDoesNotAlias(4);

            vector<Value*> likelyForkArgs;
            likelyForkArgs.push_back(module->getFunction(thunk->getName()));
            likelyForkArgs.push_back(ExpressionBuilder::constant((double)types.size(), 8));
            likelyForkArgs.push_back(kernelSize);
            likelyForkArgs.insert(likelyForkArgs.end(), srcs.begin(), srcs.end());
            likelyForkArgs.push_back(dst);
            builder.CreateCall(likelyFork, likelyForkArgs);
        } else {
            vector<Value*> thunkArgs;
            for (const TypedValue &src : srcs)
                thunkArgs.push_back(src.value);
            thunkArgs.push_back(dst);
            thunkArgs.push_back(ExpressionBuilder::zero());
            thunkArgs.push_back(kernelSize);
            builder.CreateCall(thunk, thunkArgs);
        }

        builder.CreateRet(dst);

        FunctionPassManager functionPassManager(module);
        functionPassManager.add(createVerifierPass(PrintMessageAction));
        functionPassManager.add(createInstructionCombiningPass());
        functionPassManager.run(*function);

//        module->dump();

        if (executionEngine) {
            executionEngine->finalizeObject();
            f = executionEngine->getPointerToFunction(function);
        } else {
            f = NULL;
        }
    }

    void write(const string &fileName) const
    {
        const string extension = fileName.substr(fileName.find_last_of(".") + 1);

        string errorInfo;
        tool_output_file output(fileName.c_str(), errorInfo);
        if (extension == "ll") {
            module->print(output.os(), NULL);
        } else if (extension == "bc") {
            WriteBitcodeToFile(module, output.os());
        } else {
            PassManager pm;
            formatted_raw_ostream fos(output.os());
            targetMachine->addPassesToEmitFile(pm, fos, extension == "s" ? TargetMachine::CGFT_AssemblyFile : TargetMachine::CGFT_ObjectFile);
            pm.run(*module);
        }

        likely_assert(errorInfo.empty(), "failed to write to: %s with error: %s", fileName.c_str(), errorInfo.c_str());
        output.keep();
    }

    ~FunctionBuilder()
    {
        delete[] type;
    }

private:
    static Function *getFunction(const string &name, Module *m, likely_arity arity, Type *ret, Type *dst = NULL, Type *start = NULL, Type *stop = NULL)
    {
        PointerType *matrixPointer = PointerType::getUnqual(TheMatrixStruct);
        Function *function;
        switch (arity) {
          case 0: function = cast<Function>(m->getOrInsertFunction(name, ret, dst, start, stop, NULL)); break;
          case 1: function = cast<Function>(m->getOrInsertFunction(name, ret, matrixPointer, dst, start, stop, NULL)); break;
          case 2: function = cast<Function>(m->getOrInsertFunction(name, ret, matrixPointer, matrixPointer, dst, start, stop, NULL)); break;
          case 3: function = cast<Function>(m->getOrInsertFunction(name, ret, matrixPointer, matrixPointer, matrixPointer, dst, start, stop, NULL)); break;
          default: { function = NULL; likely_assert(false, "FunctionBuilder::getFunction invalid arity: %d", arity); }
        }
        function->addFnAttr(Attribute::NoUnwind);
        function->setCallingConv(CallingConv::C);
        if (ret->isPointerTy())
            function->setDoesNotAlias(0);
        size_t num_mats = arity;
        if (dst) num_mats++;
        for (size_t i=0; i<num_mats; i++) {
            function->setDoesNotAlias((unsigned int)i+1);
            function->setDoesNotCapture((unsigned int)i+1);
        }
        return function;
    }

    static vector<TypedValue> getArgs(Function *function, const vector<likely_type> &types)
    {
        vector<TypedValue> result;
        Function::arg_iterator args = function->arg_begin();
        likely_arity n = 0;
        while (args != function->arg_end()) {
            Value *src = args++;
            stringstream name; name << "arg_" << int(n);
            src->setName(name.str());
            result.push_back(TypedValue(src, n < types.size() ? types[n] : likely_type_null));
            n++;
        }
        return result;
    }

    static TypedValue getDimensions(ExpressionBuilder &builder, const KernelInfo &info, likely_ir ir, const char *axis, const TypedValue &arg0)
    {
        lua_getfield(ir, -1, axis);
        Value *result;
        if (lua_isnil(ir, -1)) {
            if (arg0.isNull()) {
                result = ExpressionBuilder::constant(1);
            } else {
                if      (!strcmp(axis, "channels")) result = builder.channels(arg0);
                else if (!strcmp(axis, "columns"))  result = builder.columns (arg0);
                else if (!strcmp(axis, "rows"))     result = builder.rows    (arg0);
                else                                result = builder.frames  (arg0);
            }
        } else {
            result = builder.cast(Operation::expression(builder, info, ir), likely_type_native);
        }

        likely_type type = likely_type_native;
        const bool isMulti = (!LLVM_VALUE_IS_INT(result)) || (LLVM_VALUE_TO_INT(result) > 1);
        if      (!strcmp(axis, "channels")) likely_set_multi_channel(&type, isMulti);
        else if (!strcmp(axis, "columns"))  likely_set_multi_column (&type, isMulti);
        else if (!strcmp(axis, "rows"))     likely_set_multi_row    (&type, isMulti);
        else                                likely_set_multi_frame  (&type, isMulti);

        lua_pop(ir, 1);
        return TypedValue(result, type);
    }
};

struct VTable : public JITResources
{
    static PointerType *vtableType;
    likely_ir ir;
    likely_arity n;
    vector<FunctionBuilder*> functions;
    Function *likelyDispatch;

    VTable(likely_ir ir_)
        : JITResources(true), ir(ir_)
    {
        n = computeArityRecursive(ir);

        if (vtableType == NULL)
            vtableType = PointerType::getUnqual(StructType::create(getGlobalContext(), "VTable"));

        static FunctionType *LikelyDispatchSignature = NULL;
        if (LikelyDispatchSignature == NULL) {
            Type *dispatchReturn = PointerType::getUnqual(TheMatrixStruct);
            vector<Type*> dispatchParameters;
            dispatchParameters.push_back(vtableType);
            dispatchParameters.push_back(PointerType::getUnqual(PointerType::getUnqual(TheMatrixStruct)));
            LikelyDispatchSignature = FunctionType::get(dispatchReturn, dispatchParameters, false);
        }
        likelyDispatch = Function::Create(LikelyDispatchSignature, GlobalValue::ExternalLinkage, "likely_dispatch", module);
        likelyDispatch->setCallingConv(CallingConv::C);
        likelyDispatch->setDoesNotAlias(0);
        likelyDispatch->setDoesNotAlias(1);
        likelyDispatch->setDoesNotAlias(2);
        likelyDispatch->setDoesNotCapture(1);
        likelyDispatch->setDoesNotCapture(2);
    }

    ~VTable()
    {
        lua_close(ir);
        for (FunctionBuilder *function : functions)
            delete function;
    }

    likely_function compile() const
    {
        static FunctionType* functionType = NULL;
        if (functionType == NULL) {
            Type *functionReturn = PointerType::getUnqual(TheMatrixStruct);
            vector<Type*> functionParameters;
            functionParameters.push_back(PointerType::getUnqual(TheMatrixStruct));
            functionType = FunctionType::get(functionReturn, functionParameters, true);
        }
        Function *function = getFunction(functionType);
        BasicBlock *entry = BasicBlock::Create(getGlobalContext(), "entry", function);
        IRBuilder<> builder(entry);

        Value *array;
        if (n > 0) {
            array = builder.CreateAlloca(PointerType::getUnqual(TheMatrixStruct), Constant::getIntegerValue(Type::getInt32Ty(getGlobalContext()), APInt(32, (uint64_t)n)));
            builder.CreateStore(function->arg_begin(), builder.CreateGEP(array, Constant::getIntegerValue(NativeIntegerType, APInt(8*sizeof(void*), 0))));
            if (n > 1) {
                Value *vaList = builder.CreateAlloca(IntegerType::getInt8PtrTy(getGlobalContext()));
                Value *vaListRef = builder.CreateBitCast(vaList, Type::getInt8PtrTy(getGlobalContext()));
                builder.CreateCall(Intrinsic::getDeclaration(module, Intrinsic::vastart), vaListRef);
                for (likely_arity i=1; i<n; i++)
                    builder.CreateStore(builder.CreateVAArg(vaList, PointerType::getUnqual(TheMatrixStruct)), builder.CreateGEP(array, Constant::getIntegerValue(NativeIntegerType, APInt(8*sizeof(void*), i))));
                builder.CreateCall(Intrinsic::getDeclaration(module, Intrinsic::vaend), vaListRef);
            }
        } else {
            array = ConstantPointerNull::get(PointerType::getUnqual(PointerType::getUnqual(TheMatrixStruct)));
        }
        builder.CreateRet(builder.CreateCall2(likelyDispatch, thisVTable(), array));

        return reinterpret_cast<likely_function>(finalize(function));
    }

    likely_function_n compileN() const
    {
        static FunctionType* functionType = NULL;
        if (functionType == NULL) {
            Type *functionReturn = PointerType::getUnqual(TheMatrixStruct);
            vector<Type*> functionParameters;
            functionParameters.push_back(PointerType::getUnqual(PointerType::getUnqual(TheMatrixStruct)));
            functionType = FunctionType::get(functionReturn, functionParameters, true);
        }
        Function *function = getFunction(functionType);
        BasicBlock *entry = BasicBlock::Create(getGlobalContext(), "entry", function);
        IRBuilder<> builder(entry);
        builder.CreateRet(builder.CreateCall2(likelyDispatch, thisVTable(), function->arg_begin()));
        return reinterpret_cast<likely_function_n>(finalize(function));
    }

private:
    static likely_arity computeArityRecursive(lua_State *L)
    {
        if (!lua_istable(L, -1))
            return 0;

        lua_rawgeti(L, -1, 1);
        if (lua_isstring(L, -1) && !strcmp(lua_tostring(L, -1), "arg")) {
            lua_pushinteger(L, 2);
            lua_gettable(L, -3);
            likely_arity n = (likely_arity) lua_tointeger(L, -1) + 1;
            lua_pop(L, 2);
            return n;
        } else {
            lua_pop(L, 1);
        }

        likely_arity n = 0;
        const int len = luaL_len(L, -1);
        for (int i=1; i<=len; i++) {
            lua_pushinteger(L, i);
            lua_gettable(L, -2);
            n = max(n, computeArityRecursive(L));
            lua_pop(L, 1);
        }
        return n;
    }

    Function *getFunction(FunctionType *functionType) const
    {
        Function *function = cast<Function>(module->getOrInsertFunction(name, functionType));
        function->addFnAttr(Attribute::NoUnwind);
        function->setCallingConv(CallingConv::C);
        function->setDoesNotAlias(0);
        function->setDoesNotAlias(1);
        function->setDoesNotCapture(1);
        return function;
    }

    Constant *thisVTable() const
    {
        return ConstantExpr::getIntToPtr(ConstantInt::get(IntegerType::get(getGlobalContext(), 8*sizeof(this)), uintptr_t(this)), vtableType);
    }

    void *finalize(Function *function) const
    {
        FunctionPassManager functionPassManager(module);
        functionPassManager.add(createVerifierPass(PrintMessageAction));
        functionPassManager.run(*function);
        executionEngine->finalizeObject();
        return executionEngine->getPointerToFunction(function);
    }
};
PointerType *VTable::vtableType = NULL;

} // namespace (anonymous)

extern "C" LIKELY_EXPORT likely_mat likely_dispatch(struct VTable *vtable, likely_mat *m)
{
    void *function = NULL;
    for (size_t i=0; i<vtable->functions.size(); i++) {
        const FunctionBuilder *functionBuilder = vtable->functions[i];
        for (likely_arity j=0; j<vtable->n; j++)
            if (m[j]->type != functionBuilder->type[j])
                goto Next;
        function = functionBuilder->f;
        break;
    Next:
        continue;
    }

    if (function == NULL) {
        vector<likely_type> types;
        for (int i=0; i<vtable->n; i++)
            types.push_back(m[i]->type);
        FunctionBuilder *functionBuilder = new FunctionBuilder(vtable->ir, types, true);
        vtable->functions.push_back(functionBuilder);
        function = vtable->functions.back()->f;

        // An impossible case used to ensure that `likely_dispatch` isn't stripped when optimizing executable size
        if (function == NULL)
            likely_dispatch(NULL, NULL);
    }

    typedef likely_mat (*f0)(void);
    typedef likely_mat (*f1)(likely_const_mat);
    typedef likely_mat (*f2)(likely_const_mat, likely_const_mat);
    typedef likely_mat (*f3)(likely_const_mat, likely_const_mat, likely_const_mat);

    likely_mat dst;
    switch (vtable->n) {
      case 0: dst = reinterpret_cast<f0>(function)(); break;
      case 1: dst = reinterpret_cast<f1>(function)(m[0]); break;
      case 2: dst = reinterpret_cast<f2>(function)(m[0], m[1]); break;
      case 3: dst = reinterpret_cast<f3>(function)(m[0], m[1], m[2]); break;
      default: dst = NULL; likely_assert(false, "likely_dispatch invalid arity: %d", vtable->n);
    }

    return dst;
}

likely_ir likely_ir_from_string(const char *str)
{
    likely_ir L = luaL_newstate();
    luaL_dostring(L, (string("return ") + str).c_str());
    const int args = lua_gettop(L);
    likely_assert(args == 1, "'likely_ir_from_string' expected one result, got: %d", args);
    likely_assert(lua_istable(L, 1), "'likely_ir_from_string' expected a table result");
    return L;
}

static void toStream(lua_State *L, int index, stringstream &stream, int levels = 1)
{
    lua_pushvalue(L, index);
    const int type = lua_type(L, -1);
    if (type == LUA_TBOOLEAN) {
        stream << (lua_toboolean(L, -1) ? "true" : "false");
    } else if (type == LUA_TNUMBER) {
        stream << lua_tonumber(L, -1);
    } else if (type == LUA_TSTRING) {
        stream << "\"" << lua_tostring(L, -1) << "\"";
    } else if (type == LUA_TTABLE) {
        if (levels == 0) {
            stream << "table";
        } else {
            map<int,string> integers;
            map<string,string> strings;
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                lua_pushvalue(L, -2);
                int isnum;
                int key = lua_tointegerx(L, -1, &isnum);
                lua_pop(L, 1);
                stringstream value;
                toStream(L, -1, value, levels - 1);
                if (isnum) {
                    integers.insert(pair<int,string>(key, value.str()));
                } else {
                    lua_pushvalue(L, -2);
                    strings.insert(pair<string,string>(lua_tostring(L, -1), value.str()));
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }

            stream << "{";
            int expectedIndex = 1;
            for (map<int,string>::iterator iter = integers.begin(); iter != integers.end(); iter++) {
                if (iter != integers.begin())
                    stream << ", ";
                if (iter->first == expectedIndex) {
                    stream << iter->second;
                    expectedIndex++;
                } else {
                    stream << iter->first << "=" << iter->second;
                }
            }

            for (map<string,string>::iterator iter = strings.begin(); iter != strings.end(); iter++) {
                if (iter != strings.begin() || !integers.empty())
                    stream << ", ";
                stream << iter->first << "=" << iter->second;
            }
            stream << "}";
        }
    } else {
        stream << lua_typename(L, type);
    }
    lua_pop(L, 1);
}

const char *likely_ir_to_string(likely_ir ir)
{
    static string result;
    stringstream stream;
    toStream(ir, -1, stream, std::numeric_limits<int>::max());
    result = stream.str();
    return result.c_str();
}

likely_function likely_compile(likely_ir ir)
{
    return (new VTable(ir))->compile();
}

likely_function_n likely_compile_n(likely_ir ir)
{
    return (new VTable(ir))->compileN();
}

void likely_compile_to_file(likely_ir ir, const char *symbol_name, likely_type *types, likely_arity n, const char *file_name, bool native)
{
    FunctionBuilder(ir, vector<likely_type>(types, types+n), native, symbol_name).write(file_name);
}

void likely_stack_dump(lua_State *L, int levels)
{
    if (levels == 0)
        return;

    stringstream stream;
    const int top = lua_gettop(L);
    for (int i=1; i<=top; i++) {
        stream << i << ": ";
        toStream(L, i, stream, levels);
        stream << "\n";
    }
    fprintf(stderr, "Lua stack dump:\n%s", stream.str().c_str());
    abort();
}