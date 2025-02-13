/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asmjs/WasmText.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"

#include "jsnum.h"
#include "jsprf.h"
#include "jsstr.h"

#include "asmjs/WasmBinary.h"
#include "ds/LifoAlloc.h"
#include "js/CharacterEncoding.h"
#include "js/HashTable.h"

using namespace js;
using namespace js::wasm;
using mozilla::CheckedInt;
using mozilla::Maybe;

static const unsigned AST_LIFO_DEFAULT_CHUNK_SIZE = 4096;

/*****************************************************************************/
// wasm AST

class WasmAstExpr;

template <class T>
using WasmAstVector = mozilla::Vector<T, 0, LifoAllocPolicy<Fallible>>;

template <class K, class V, class HP>
using WasmAstHashMap = HashMap<K, V, HP, LifoAllocPolicy<Fallible>>;

typedef WasmAstVector<ValType> WasmAstValTypeVector;
typedef WasmAstVector<WasmAstExpr*> WasmAstExprVector;

struct WasmAstBase
{
    void* operator new(size_t numBytes, LifoAlloc& astLifo) throw() {
        return astLifo.alloc(numBytes);
    }
};

class WasmAstSig : public WasmAstBase
{
    WasmAstValTypeVector args_;
    ExprType ret_;

  public:
    WasmAstSig(WasmAstValTypeVector&& args, ExprType ret)
      : args_(Move(args)),
        ret_(ret)
    {}
    WasmAstSig(WasmAstSig&& rhs)
      : args_(Move(rhs.args_)),
        ret_(rhs.ret_)
    {}
    const WasmAstValTypeVector& args() const {
        return args_;
    }
    ExprType ret() const {
        return ret_;
    }

    typedef const WasmAstSig& Lookup;
    static HashNumber hash(Lookup sig) {
        return AddContainerToHash(sig.args(), HashNumber(sig.ret()));
    }
    static bool match(const WasmAstSig* lhs, Lookup rhs) {
        return lhs->ret() == rhs.ret() && EqualContainers(lhs->args(), rhs.args());
    }
};

class WasmAstNode : public WasmAstBase
{};

enum class WasmAstExprKind
{
    BinaryOperator,
    Block,
    Call,
    ComparisonOperator,
    Const,
    ConversionOperator,
    GetLocal,
    IfElse,
    Nop,
    SetLocal,
    UnaryOperator,
};

class WasmAstExpr : public WasmAstNode
{
    const WasmAstExprKind kind_;

  protected:
    explicit WasmAstExpr(WasmAstExprKind kind)
      : kind_(kind)
    {}

  public:
    WasmAstExprKind kind() const { return kind_; }

    template <class T>
    T& as() {
        MOZ_ASSERT(kind() == T::Kind);
        return static_cast<T&>(*this);
    }
};

struct WasmAstNop : WasmAstExpr
{
    WasmAstNop()
      : WasmAstExpr(WasmAstExprKind::Nop)
    {}
};

class WasmAstConst : public WasmAstExpr
{
    const Val val_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::Const;
    explicit WasmAstConst(Val val)
      : WasmAstExpr(Kind),
        val_(val)
    {}
    Val val() const { return val_; }
};

class WasmAstGetLocal : public WasmAstExpr
{
    uint32_t localIndex_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::GetLocal;
    explicit WasmAstGetLocal(uint32_t localIndex)
      : WasmAstExpr(Kind),
        localIndex_(localIndex)
    {}
    uint32_t localIndex() const {
        return localIndex_;
    }
};

class WasmAstSetLocal : public WasmAstExpr
{
    uint32_t localIndex_;
    WasmAstExpr& value_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::SetLocal;
    WasmAstSetLocal(uint32_t localIndex, WasmAstExpr& value)
      : WasmAstExpr(Kind),
        localIndex_(localIndex),
        value_(value)
    {}
    uint32_t localIndex() const {
        return localIndex_;
    }
    WasmAstExpr& value() const {
        return value_;
    }
};

class WasmAstBlock : public WasmAstExpr
{
    WasmAstExprVector exprs_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::Block;
    explicit WasmAstBlock(WasmAstExprVector&& exprs)
      : WasmAstExpr(Kind),
        exprs_(Move(exprs))
    {}

    const WasmAstExprVector& exprs() const { return exprs_; }
};

class WasmAstCall : public WasmAstExpr
{
    Expr expr_;
    uint32_t index_;
    WasmAstExprVector args_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::Call;
    WasmAstCall(Expr expr, uint32_t index, WasmAstExprVector&& args)
      : WasmAstExpr(Kind), expr_(expr), index_(index), args_(Move(args))
    {}

    Expr expr() const { return expr_; }
    uint32_t index() const { return index_; }
    const WasmAstExprVector& args() const { return args_; }
};

class WasmAstIfElse : public WasmAstExpr
{
    Expr expr_;
    WasmAstExpr* cond_;
    WasmAstExpr* ifBody_;
    WasmAstExpr* elseBody_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::IfElse;
    explicit WasmAstIfElse(Expr expr, WasmAstExpr* cond, WasmAstExpr* ifBody,
                           WasmAstExpr* elseBody = nullptr)
      : WasmAstExpr(Kind),
        expr_(expr),
        cond_(cond),
        ifBody_(ifBody),
        elseBody_(elseBody)
    {}

    bool hasElse() const { return expr_ == Expr::IfElse; }
    Expr expr() const { return expr_; }
    WasmAstExpr& cond() const { return *cond_; }
    WasmAstExpr& ifBody() const { return *ifBody_; }
    WasmAstExpr& elseBody() const { return *elseBody_; }
};

class WasmAstFunc : public WasmAstNode
{
    const uint32_t sigIndex_;
    WasmAstValTypeVector varTypes_;
    WasmAstExpr* const maybeBody_;

  public:
    WasmAstFunc(uint32_t sigIndex, WasmAstValTypeVector&& varTypes, WasmAstExpr* maybeBody)
      : sigIndex_(sigIndex),
        varTypes_(Move(varTypes)),
        maybeBody_(maybeBody)
    {}
    uint32_t sigIndex() const { return sigIndex_; }
    const WasmAstValTypeVector& varTypes() const { return varTypes_; }
    WasmAstExpr* maybeBody() const { return maybeBody_; }
};

class WasmAstImport : public WasmAstNode
{
    TwoByteChars module_;
    TwoByteChars func_;
    uint32_t sigIndex_;

  public:
    WasmAstImport(TwoByteChars module, TwoByteChars func, uint32_t sigIndex)
      : module_(module), func_(func), sigIndex_(sigIndex)
    {}
    TwoByteChars module() const { return module_; }
    TwoByteChars func() const { return func_; }
    uint32_t sigIndex() const { return sigIndex_; }
};

enum class WasmAstExportKind { Func, Memory };

class WasmAstExport : public WasmAstNode
{
    TwoByteChars name_;
    WasmAstExportKind kind_;
    union {
        uint32_t funcIndex_;
    } u;

  public:
    WasmAstExport(TwoByteChars name, uint32_t funcIndex)
      : name_(name), kind_(WasmAstExportKind::Func)
    {
        u.funcIndex_ = funcIndex;
    }
    explicit WasmAstExport(TwoByteChars name)
      : name_(name), kind_(WasmAstExportKind::Memory)
    {}
    TwoByteChars name() const { return name_; }
    WasmAstExportKind kind() const { return kind_; }
    size_t funcIndex() const { MOZ_ASSERT(kind_ == WasmAstExportKind::Func); return u.funcIndex_; }
};

class WasmAstSegment : public WasmAstNode
{
    uint32_t offset_;
    TwoByteChars text_;

  public:
    WasmAstSegment(uint32_t offset, TwoByteChars text)
      : offset_(offset), text_(text)
    {}
    uint32_t offset() const { return offset_; }
    TwoByteChars text() const { return text_; }
};

typedef WasmAstVector<WasmAstSegment*> WasmAstSegmentVector;

class WasmAstMemory : public WasmAstNode
{
    uint32_t initialSize_;
    WasmAstSegmentVector segments_;

  public:
    explicit WasmAstMemory(uint32_t initialSize, WasmAstSegmentVector&& segments)
      : initialSize_(initialSize),
        segments_(Move(segments))
    {}
    uint32_t initialSize() const { return initialSize_; }
    const WasmAstSegmentVector& segments() const { return segments_; }
};

class WasmAstModule : public WasmAstNode
{
    typedef WasmAstVector<WasmAstFunc*> FuncVector;
    typedef WasmAstVector<WasmAstImport*> ImportVector;
    typedef WasmAstVector<WasmAstExport*> ExportVector;
    typedef WasmAstVector<WasmAstSig*> SigVector;
    typedef WasmAstHashMap<WasmAstSig*, uint32_t, WasmAstSig> SigMap;

    LifoAlloc& lifo_;
    WasmAstMemory* memory_;
    SigVector sigs_;
    SigMap sigMap_;
    ImportVector imports_;
    ExportVector exports_;
    FuncVector funcs_;

  public:
    explicit WasmAstModule(LifoAlloc& lifo)
      : lifo_(lifo),
        memory_(nullptr),
        sigs_(lifo),
        sigMap_(lifo),
        imports_(lifo),
        exports_(lifo),
        funcs_(lifo)
    {}
    bool init() {
        return sigMap_.init();
    }
    bool setMemory(WasmAstMemory* memory) {
        if (memory_)
            return false;
        memory_ = memory;
        return true;
    }
    WasmAstMemory* maybeMemory() const {
        return memory_;
    }
    bool declare(WasmAstSig&& sig, uint32_t* sigIndex) {
        SigMap::AddPtr p = sigMap_.lookupForAdd(sig);
        if (p) {
            *sigIndex = p->value();
            return true;
        }
        *sigIndex = sigs_.length();
        return sigs_.append(new (lifo_) WasmAstSig(Move(sig))) &&
               sigMap_.add(p, sigs_.back(), *sigIndex);
    }
    const SigVector& sigs() const {
        return sigs_;
    }
    bool append(WasmAstFunc* func) {
        return funcs_.append(func);
    }
    const FuncVector& funcs() const {
        return funcs_;
    }
    const ImportVector& imports() const {
        return imports_;
    }
    bool append(WasmAstImport* imp) {
        return imports_.append(imp);
    }
    bool append(WasmAstExport* exp) {
        return exports_.append(exp);
    }
    const ExportVector& exports() const {
        return exports_;
    }
};

class WasmAstUnaryOperator final : public WasmAstExpr
{
    Expr expr_;
    WasmAstExpr* op_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::UnaryOperator;
    explicit WasmAstUnaryOperator(Expr expr, WasmAstExpr* op)
      : WasmAstExpr(Kind),
        expr_(expr), op_(op)
    {}

    Expr expr() const { return expr_; }
    WasmAstExpr* op() const { return op_; }
};

class WasmAstBinaryOperator final : public WasmAstExpr
{
    Expr expr_;
    WasmAstExpr* lhs_;
    WasmAstExpr* rhs_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::BinaryOperator;
    explicit WasmAstBinaryOperator(Expr expr, WasmAstExpr* lhs, WasmAstExpr* rhs)
      : WasmAstExpr(Kind),
        expr_(expr), lhs_(lhs), rhs_(rhs)
    {}

    Expr expr() const { return expr_; }
    WasmAstExpr* lhs() const { return lhs_; }
    WasmAstExpr* rhs() const { return rhs_; }
};

class WasmAstComparisonOperator final : public WasmAstExpr
{
    Expr expr_;
    WasmAstExpr* lhs_;
    WasmAstExpr* rhs_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::ComparisonOperator;
    explicit WasmAstComparisonOperator(Expr expr, WasmAstExpr* lhs, WasmAstExpr* rhs)
      : WasmAstExpr(Kind),
        expr_(expr), lhs_(lhs), rhs_(rhs)
    {}

    Expr expr() const { return expr_; }
    WasmAstExpr* lhs() const { return lhs_; }
    WasmAstExpr* rhs() const { return rhs_; }
};

class WasmAstConversionOperator final : public WasmAstExpr
{
    Expr expr_;
    WasmAstExpr* op_;

  public:
    static const WasmAstExprKind Kind = WasmAstExprKind::ConversionOperator;
    explicit WasmAstConversionOperator(Expr expr, WasmAstExpr* op)
      : WasmAstExpr(Kind),
        expr_(expr), op_(op)
    {}

    Expr expr() const { return expr_; }
    WasmAstExpr* op() const { return op_; }
};

/*****************************************************************************/
// wasm text token stream

class WasmToken
{
  public:
    enum Kind
    {
        BinaryOpcode,
        Block,
        Call,
        CallImport,
        CloseParen,
        ComparisonOpcode,
        Const,
        ConversionOpcode,
        EndOfFile,
        Error,
        Export,
        Func,
        GetLocal,
        If,
        IfElse,
        Import,
        Integer,
        Memory,
        Local,
        Module,
        Name,
        Nop,
        OpenParen,
        Param,
        Result,
        Segment,
        SetLocal,
        Text,
        UnaryOpcode,
        ValueType
    };
  private:
    Kind kind_;
    const char16_t* begin_;
    const char16_t* end_;
    union {
        uint32_t integer_;
        ValType valueType_;
        Expr expr_;
    } u;
  public:
    explicit WasmToken() = default;
    WasmToken(Kind kind, const char16_t* begin, const char16_t* end)
      : kind_(kind),
        begin_(begin),
        end_(end)
    {
        MOZ_ASSERT(kind_ != Error);
        MOZ_ASSERT((kind == EndOfFile) == (begin == end));
    }
    explicit WasmToken(uint32_t integer, const char16_t* begin, const char16_t* end)
      : kind_(Integer),
        begin_(begin),
        end_(end)
    {
        MOZ_ASSERT(begin != end);
        u.integer_ = integer;
    }
    explicit WasmToken(Kind kind, ValType valueType, const char16_t* begin, const char16_t* end)
      : kind_(kind),
        begin_(begin),
        end_(end)
    {
        MOZ_ASSERT(begin != end);
        MOZ_ASSERT(kind_ == ValueType || kind_ == Const);
        u.valueType_ = valueType;
    }
    explicit WasmToken(Kind kind, Expr expr, const char16_t* begin, const char16_t* end)
      : kind_(kind),
        begin_(begin),
        end_(end)
    {
        MOZ_ASSERT(begin != end);
        MOZ_ASSERT(kind_ == UnaryOpcode || kind_ == BinaryOpcode || kind_ == ComparisonOpcode ||
                   kind_ == ConversionOpcode);
        u.expr_ = expr;
    }
    explicit WasmToken(const char16_t* begin)
      : kind_(Error),
        begin_(begin),
        end_(begin)
    {}
    Kind kind() const {
        return kind_;
    }
    const char16_t* begin() const {
        return begin_;
    }
    const char16_t* end() const {
        return end_;
    }
    TwoByteChars text() const {
        MOZ_ASSERT(kind_ == Text);
        MOZ_ASSERT(begin_[0] == '"');
        MOZ_ASSERT(end_[-1] == '"');
        MOZ_ASSERT(end_ - begin_ >= 2);
        return TwoByteChars(begin_ + 1, end_ - begin_ - 2);
    }
    uint32_t integer() const {
        MOZ_ASSERT(kind_ == Integer);
        return u.integer_;
    }
    ValType valueType() const {
        MOZ_ASSERT(kind_ == ValueType || kind_ == Const);
        return u.valueType_;
    }
    Expr expr() const {
        MOZ_ASSERT(kind_ == UnaryOpcode || kind_ == BinaryOpcode || kind_ == ComparisonOpcode ||
                   kind_ == ConversionOpcode);
        return u.expr_;
    }
};

static bool
IsWasmNewLine(char16_t c)
{
    return c == '\n';
}

static bool
IsWasmSpace(char16_t c)
{
    switch (c) {
      case ' ':
      case '\n':
      case '\r':
      case '\t':
      case '\v':
      case '\f':
        return true;
      default:
        return false;
    }
}

static bool
IsWasmDigit(char16_t c)
{
    return c >= '0' && c <= '9';
}

static bool
IsWasmLetter(char16_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool
IsNameAfterDollar(char16_t c)
{
    return c == '_' || IsWasmDigit(c) || IsWasmLetter(c);
}

static bool
IsHexDigit(char c, uint8_t* value)
{
    if (c >= '0' && c <= '9') {
        *value = c - '0';
        return true;
    }

    if (c >= 'a' && c <= 'f') {
        *value = 10 + (c - 'a');
        return true;
    }

    if (c >= 'A' && c <= 'F') {
        *value = 10 + (c - 'A');
        return true;
    }

    return false;
}

static bool
ConsumeTextByte(const char16_t** curp, const char16_t* end, uint8_t *byte = nullptr)
{
    const char16_t*& cur = *curp;
    MOZ_ASSERT(cur != end);

    if (*cur != '\\') {
        if (byte)
            *byte = *cur;
        cur++;
        return true;
    }

    if (++cur == end)
        return false;

    uint8_t u8;
    switch (*cur) {
      case 'n': u8 = '\n'; break;
      case 't': u8 = '\t'; break;
      case '\\': u8 = '\\'; break;
      case '\"': u8 = '\"'; break;
      case '\'': u8 = '\''; break;
      default: {
        uint8_t lowNibble;
        if (!IsHexDigit(*cur, &lowNibble))
            return false;

        if (++cur == end)
            return false;

        uint8_t highNibble;
        if (!IsHexDigit(*cur, &highNibble))
            return false;

        u8 = lowNibble | (highNibble << 4);
        break;
      }
    }

    if (byte)
        *byte = u8;
    cur++;
    return true;
}

class WasmTokenStream
{
    static const uint32_t LookaheadSize = 2;

    const char16_t* cur_;
    const char16_t* const end_;
    const char16_t* lineStart_;
    unsigned line_;
    uint32_t lookaheadIndex_;
    uint32_t lookaheadDepth_;
    WasmToken lookahead_[LookaheadSize];

    bool consume(const char16_t* match) {
        const char16_t* p = cur_;
        for (; *match; p++, match++) {
            if (p == end_ || *p != *match)
                return false;
        }
        cur_ = p;
        return true;
    }
    WasmToken fail(const char16_t* begin) const {
        return WasmToken(begin);
    }
    WasmToken next() {
        while (cur_ != end_ && IsWasmSpace(*cur_)) {
            if (IsWasmNewLine(*cur_++)) {
                lineStart_ = cur_;
                line_++;
            }
        }

        if (cur_ == end_)
            return WasmToken(WasmToken::EndOfFile, cur_, cur_);

        const char16_t* begin = cur_;
        switch (*begin) {
          case '"':
            cur_++;
            while (true) {
                if (cur_ == end_)
                    return fail(begin);
                if (*cur_ == '"')
                    break;
                if (!ConsumeTextByte(&cur_, end_))
                    return fail(begin);
            }
            cur_++;
            return WasmToken(WasmToken::Text, begin, cur_);

          case '$':
            cur_++;
            while (cur_ != end_ && IsNameAfterDollar(*cur_))
                cur_++;
            return WasmToken(WasmToken::Name, begin, cur_);

          case '(':
            cur_++;
            return WasmToken(WasmToken::OpenParen, begin, cur_);

          case ')':
            cur_++;
            return WasmToken(WasmToken::CloseParen, begin, cur_);

          case '0': case '1': case '2': case '3': case '4':
          case '5': case '6': case '7': case '8': case '9': {
            CheckedInt<uint32_t> u32 = 0;
            while (cur_ != end_ && IsWasmDigit(*cur_)) {
                u32 *= 10;
                u32 += *cur_ - '0';
                if (!u32.isValid())
                    return fail(begin);
                cur_++;
            }
            return WasmToken(u32.value(), begin, cur_);
          }

          case 'b':
            if (consume(MOZ_UTF16("block")))
                return WasmToken(WasmToken::Block, begin, cur_);
            break;

          case 'c':
            if (consume(MOZ_UTF16("call"))) {
                if (consume(MOZ_UTF16("_import")))
                    return WasmToken(WasmToken::CallImport, begin, cur_);
                return WasmToken(WasmToken::Call, begin, cur_);
            }
            break;

          case 'e':
            if (consume(MOZ_UTF16("export")))
                return WasmToken(WasmToken::Export, begin, cur_);
            break;

          case 'f':
            if (consume(MOZ_UTF16("func")))
                return WasmToken(WasmToken::Func, begin, cur_);

            if (consume(MOZ_UTF16("f32"))) {
                if (!consume(MOZ_UTF16(".")))
                    return WasmToken(WasmToken::ValueType, ValType::F32, begin, cur_);

                switch (*cur_) {
                  case 'a':
                    if (consume(MOZ_UTF16("abs")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Abs, begin, cur_);
                    if (consume(MOZ_UTF16("add")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32Add, begin, cur_);
                    break;
                  case 'c':
                    if (consume(MOZ_UTF16("ceil")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Ceil, begin, cur_);
                    if (consume(MOZ_UTF16("const")))
                        return WasmToken(WasmToken::Const, ValType::F32, begin, cur_);
                    if (consume(MOZ_UTF16("convert_s/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F32ConvertSI32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("convert_u/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F32ConvertUI32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("copysign")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32CopySign, begin, cur_);
                    break;
                  case 'd':
                    if (consume(MOZ_UTF16("demote/f64")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F32DemoteF64,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("div")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32Div, begin, cur_);
                    break;
                  case 'e':
                    if (consume(MOZ_UTF16("eq")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F32Eq, begin, cur_);
                    break;
                  case 'f':
                    if (consume(MOZ_UTF16("floor")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Floor, begin, cur_);
                    break;
                  case 'g':
                    if (consume(MOZ_UTF16("ge")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F32Ge, begin, cur_);
                    if (consume(MOZ_UTF16("gt")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F32Gt, begin, cur_);
                    break;
                  case 'l':
                    if (consume(MOZ_UTF16("le")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F32Le, begin, cur_);
                    if (consume(MOZ_UTF16("lt")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F32Lt, begin, cur_);
                    break;
                  case 'm':
                    if (consume(MOZ_UTF16("max")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32Max, begin, cur_);
                    if (consume(MOZ_UTF16("min")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32Min, begin, cur_);
                    if (consume(MOZ_UTF16("mul")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32Mul, begin, cur_);
                    break;
                  case 'n':
                    if (consume(MOZ_UTF16("nearest")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Nearest, begin, cur_);
                    if (consume(MOZ_UTF16("neg")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Neg, begin, cur_);
                    if (consume(MOZ_UTF16("ne")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F32Ne, begin, cur_);
                    break;
                  case 'r':
                    if (consume(MOZ_UTF16("reinterpret/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F32ReinterpretI32,
                                         begin, cur_);
                    break;
                  case 's':
                    if (consume(MOZ_UTF16("sqrt")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Sqrt, begin, cur_);
                    if (consume(MOZ_UTF16("sub")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F32Sub, begin, cur_);
                    break;
                  case 't':
                    if (consume(MOZ_UTF16("trunc")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F32Trunc, begin, cur_);
                    break;
                }
                break;
            }
            if (consume(MOZ_UTF16("f64"))) {
                if (!consume(MOZ_UTF16(".")))
                    return WasmToken(WasmToken::ValueType, ValType::F64, begin, cur_);

                switch (*cur_) {
                  case 'a':
                    if (consume(MOZ_UTF16("abs")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Abs, begin, cur_);
                    if (consume(MOZ_UTF16("add")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64Add, begin, cur_);
                    break;
                  case 'c':
                    if (consume(MOZ_UTF16("ceil")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Ceil, begin, cur_);
                    if (consume(MOZ_UTF16("const")))
                        return WasmToken(WasmToken::Const, ValType::F64, begin, cur_);
                    if (consume(MOZ_UTF16("convert_s/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F64ConvertSI32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("convert_u/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F64ConvertUI32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("copysign")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64CopySign, begin, cur_);
                    break;
                  case 'd':
                    if (consume(MOZ_UTF16("div")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64Div, begin, cur_);
                    break;
                  case 'e':
                    if (consume(MOZ_UTF16("eq")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F64Eq, begin, cur_);
                    break;
                  case 'f':
                    if (consume(MOZ_UTF16("floor")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Floor, begin, cur_);
                    break;
                  case 'g':
                    if (consume(MOZ_UTF16("ge")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F64Ge, begin, cur_);
                    if (consume(MOZ_UTF16("gt")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F64Gt, begin, cur_);
                    break;
                  case 'l':
                    if (consume(MOZ_UTF16("le")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F64Le, begin, cur_);
                    if (consume(MOZ_UTF16("lt")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F64Lt, begin, cur_);
                    break;
                  case 'm':
                    if (consume(MOZ_UTF16("max")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64Max, begin, cur_);
                    if (consume(MOZ_UTF16("min")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64Min, begin, cur_);
                    if (consume(MOZ_UTF16("mul")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64Mul, begin, cur_);
                    break;
                  case 'n':
                    if (consume(MOZ_UTF16("nearest")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Nearest, begin, cur_);
                    if (consume(MOZ_UTF16("neg")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Neg, begin, cur_);
                    if (consume(MOZ_UTF16("ne")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::F64Ne, begin, cur_);
                    break;
                  case 'p':
                    if (consume(MOZ_UTF16("promote/f32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::F64PromoteF32,
                                         begin, cur_);
                    break;
                  case 's':
                    if (consume(MOZ_UTF16("sqrt")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Sqrt, begin, cur_);
                    if (consume(MOZ_UTF16("sub")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::F64Sub, begin, cur_);
                    break;
                  case 't':
                    if (consume(MOZ_UTF16("trunc")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::F64Trunc, begin, cur_);
                    break;
                }
                break;
            }
            break;

          case 'g':
            if (consume(MOZ_UTF16("get_local")))
                return WasmToken(WasmToken::GetLocal, begin, cur_);
            break;

          case 'i':
            if (consume(MOZ_UTF16("i32"))) {
                if (!consume(MOZ_UTF16(".")))
                    return WasmToken(WasmToken::ValueType, ValType::I32, begin, cur_);

                switch (*cur_) {
                  case 'a':
                    if (consume(MOZ_UTF16("add")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32Add, begin, cur_);
                    if (consume(MOZ_UTF16("and")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32And, begin, cur_);
                    break;
                  case 'c':
                    if (consume(MOZ_UTF16("const")))
                        return WasmToken(WasmToken::Const, ValType::I32, begin, cur_);
                    if (consume(MOZ_UTF16("clz")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I32Clz, begin, cur_);
                    if (consume(MOZ_UTF16("ctz")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I32Ctz, begin, cur_);
                    break;
                  case 'd':
                    if (consume(MOZ_UTF16("div_s")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32DivS, begin, cur_);
                    if (consume(MOZ_UTF16("div_u")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32DivU, begin, cur_);
                    break;
                  case 'e':
                    if (consume(MOZ_UTF16("eq")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32Eq, begin, cur_);
                    break;
                  case 'g':
                    if (consume(MOZ_UTF16("ge_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32GeS, begin, cur_);
                    if (consume(MOZ_UTF16("ge_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32GeU, begin, cur_);
                    if (consume(MOZ_UTF16("gt_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32GtS, begin, cur_);
                    if (consume(MOZ_UTF16("gt_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32GtU, begin, cur_);
                    break;
                  case 'l':
                    if (consume(MOZ_UTF16("le_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32LeS, begin, cur_);
                    if (consume(MOZ_UTF16("le_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32LeU, begin, cur_);
                    if (consume(MOZ_UTF16("lt_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32LtS, begin, cur_);
                    if (consume(MOZ_UTF16("lt_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32LtU, begin, cur_);
                    break;
                  case 'm':
                    if (consume(MOZ_UTF16("mul")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32Mul, begin, cur_);
                    break;
                  case 'n':
                    if (consume(MOZ_UTF16("ne")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I32Ne, begin, cur_);
                    break;
                  case 'o':
                    if (consume(MOZ_UTF16("or")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32Or, begin, cur_);
                    break;
                  case 'p':
                    if (consume(MOZ_UTF16("popcnt")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I32Popcnt, begin, cur_);
                    break;
                  case 'r':
                    if (consume(MOZ_UTF16("reinterpret/f32")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I32ReinterpretF32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("rem_s")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32RemS, begin, cur_);
                    if (consume(MOZ_UTF16("rem_u")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32RemU, begin, cur_);
                    break;
                  case 's':
                    if (consume(MOZ_UTF16("sub")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32Sub, begin, cur_);
                    if (consume(MOZ_UTF16("shl")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32Shl, begin, cur_);
                    if (consume(MOZ_UTF16("shr_s")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32ShrS, begin, cur_);
                    if (consume(MOZ_UTF16("shr_u")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32ShrU, begin, cur_);
                    break;
                  case 't':
                    if (consume(MOZ_UTF16("trunc_s/f32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I32TruncSF32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("trunc_s/f64")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I32TruncSF64,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("trunc_u/f32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I32TruncUF32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("trunc_u/f64")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I32TruncUF64,
                                         begin, cur_);
                    break;
                  case 'w':
                    if (consume(MOZ_UTF16("wrap/i64")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I32WrapI64,
                                         begin, cur_);
                    break;
                  case 'x':
                    if (consume(MOZ_UTF16("xor")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I32Xor, begin, cur_);
                    break;
                }
                break;
            }
            if (consume(MOZ_UTF16("i64"))) {
                if (!consume(MOZ_UTF16(".")))
                    return WasmToken(WasmToken::ValueType, ValType::I64, begin, cur_);

                switch (*cur_) {
                  case 'a':
                    if (consume(MOZ_UTF16("add")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64Add, begin, cur_);
                    if (consume(MOZ_UTF16("and")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64And, begin, cur_);
                    break;
                  case 'c':
                    if (consume(MOZ_UTF16("const")))
                        return WasmToken(WasmToken::Const, ValType::I64, begin, cur_);
                    if (consume(MOZ_UTF16("clz")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I64Clz, begin, cur_);
                    if (consume(MOZ_UTF16("ctz")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I64Ctz, begin, cur_);
                    break;
                  case 'd':
                    if (consume(MOZ_UTF16("div_s")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64DivS, begin, cur_);
                    if (consume(MOZ_UTF16("div_u")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64DivU, begin, cur_);
                    break;
                  case 'e':
                    if (consume(MOZ_UTF16("eq")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64Eq, begin, cur_);
                    if (consume(MOZ_UTF16("extend_s/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I64ExtendSI32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("extend_u/i32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I64ExtendUI32,
                                         begin, cur_);
                    break;
                  case 'g':
                    if (consume(MOZ_UTF16("ge_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64GeS, begin, cur_);
                    if (consume(MOZ_UTF16("ge_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64GeU, begin, cur_);
                    if (consume(MOZ_UTF16("gt_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64GtS, begin, cur_);
                    if (consume(MOZ_UTF16("gt_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64GtU, begin, cur_);
                    break;
                  case 'l':
                    if (consume(MOZ_UTF16("le_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64LeS, begin, cur_);
                    if (consume(MOZ_UTF16("le_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64LeU, begin, cur_);
                    if (consume(MOZ_UTF16("lt_s")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64LtS, begin, cur_);
                    if (consume(MOZ_UTF16("lt_u")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64LtU, begin, cur_);
                    break;
                  case 'm':
                    if (consume(MOZ_UTF16("mul")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64Mul, begin, cur_);
                    break;
                  case 'n':
                    if (consume(MOZ_UTF16("ne")))
                        return WasmToken(WasmToken::ComparisonOpcode, Expr::I64Ne, begin, cur_);
                    break;
                  case 'o':
                    if (consume(MOZ_UTF16("or")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64Or, begin, cur_);
                    break;
                  case 'p':
                    if (consume(MOZ_UTF16("popcnt")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I64Popcnt, begin, cur_);
                    break;
                  case 'r':
                    if (consume(MOZ_UTF16("reinterpret/f64")))
                        return WasmToken(WasmToken::UnaryOpcode, Expr::I64ReinterpretF64,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("rem_s")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64RemS, begin, cur_);
                    if (consume(MOZ_UTF16("rem_u")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64RemU, begin, cur_);
                    break;
                  case 's':
                    if (consume(MOZ_UTF16("sub")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64Sub, begin, cur_);
                    if (consume(MOZ_UTF16("shl")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64Shl, begin, cur_);
                    if (consume(MOZ_UTF16("shr_s")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64ShrS, begin, cur_);
                    if (consume(MOZ_UTF16("shr_u")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64ShrU, begin, cur_);
                    break;
                  case 't':
                    if (consume(MOZ_UTF16("trunc_s/f32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I64TruncSF32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("trunc_s/f64")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I64TruncSF64,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("trunc_u/f32")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I64TruncUF32,
                                         begin, cur_);
                    if (consume(MOZ_UTF16("trunc_u/f64")))
                        return WasmToken(WasmToken::ConversionOpcode, Expr::I64TruncUF64,
                                         begin, cur_);
                    break;
                  case 'x':
                    if (consume(MOZ_UTF16("xor")))
                        return WasmToken(WasmToken::BinaryOpcode, Expr::I64Xor, begin, cur_);
                    break;
                }
                break;
            }
            if (consume(MOZ_UTF16("import")))
                return WasmToken(WasmToken::Import, begin, cur_);
            if (consume(MOZ_UTF16("if"))) {
                if (consume(MOZ_UTF16("_else")))
                    return WasmToken(WasmToken::IfElse, begin, cur_);
                return WasmToken(WasmToken::If, begin, cur_);
            }
            break;

          case 'l':
            if (consume(MOZ_UTF16("local")))
                return WasmToken(WasmToken::Local, begin, cur_);
            break;

          case 'm':
            if (consume(MOZ_UTF16("module")))
                return WasmToken(WasmToken::Module, begin, cur_);
            if (consume(MOZ_UTF16("memory")))
                return WasmToken(WasmToken::Memory, begin, cur_);
            break;

          case 'n':
            if (consume(MOZ_UTF16("nop")))
                return WasmToken(WasmToken::Nop, begin, cur_);
            break;

          case 'p':
            if (consume(MOZ_UTF16("param")))
                return WasmToken(WasmToken::Param, begin, cur_);
            break;

          case 'r':
            if (consume(MOZ_UTF16("result")))
                return WasmToken(WasmToken::Result, begin, cur_);
            break;

          case 's':
            if (consume(MOZ_UTF16("set_local")))
                return WasmToken(WasmToken::SetLocal, begin, cur_);
            if (consume(MOZ_UTF16("segment")))
                return WasmToken(WasmToken::Segment, begin, cur_);
            break;

          default:
            break;
        }

        return fail(begin);
    }

  public:
    WasmTokenStream(const char16_t* text, UniqueChars* error)
      : cur_(text),
        end_(text + js_strlen(text)),
        lineStart_(text),
        line_(0),
        lookaheadIndex_(0),
        lookaheadDepth_(0)
    {}
    void generateError(WasmToken token, UniqueChars* error) {
        unsigned column = token.begin() - lineStart_ + 1;
        error->reset(JS_smprintf("parsing wasm text at %u:%u", line_, column));
    }

    WasmToken peek() {
        if (!lookaheadDepth_) {
            lookahead_[lookaheadIndex_] = next();
            lookaheadDepth_ = 1;
        }
        return lookahead_[lookaheadIndex_];
    }
    WasmToken get() {
        static_assert(LookaheadSize == 2, "can just flip");
        if (lookaheadDepth_) {
            lookaheadDepth_--;
            WasmToken ret = lookahead_[lookaheadIndex_];
            lookaheadIndex_ ^= 1;
            return ret;
        }
        return next();
    }
    void unget(WasmToken token) {
        static_assert(LookaheadSize == 2, "can just flip");
        lookaheadDepth_++;
        lookaheadIndex_ ^= 1;
        lookahead_[lookaheadIndex_] = token;
    }

    // Helpers:
    bool getIf(WasmToken::Kind kind) {
        if (peek().kind() == kind) {
            get();
            return true;
        }
        return false;
    }
    bool match(WasmToken::Kind expect, WasmToken* token, UniqueChars* error) {
        *token = get();
        if (token->kind() == expect)
            return true;
        generateError(*token, error);
        return false;
    }
    bool match(WasmToken::Kind expect, UniqueChars* error) {
        WasmToken token;
        return match(expect, &token, error);
    }
};


/*****************************************************************************/
// wasm text format parser

struct WasmParseContext
{
    WasmTokenStream ts;
    LifoAlloc& lifo;
    UniqueChars* error;

    WasmParseContext(const char16_t* text, LifoAlloc& lifo, UniqueChars* error)
      : ts(text, error),
        lifo(lifo),
        error(error)
    {}
};

static WasmAstExpr*
ParseExprInsideParens(WasmParseContext& c);

static WasmAstExpr*
ParseExpr(WasmParseContext& c)
{
    if (!c.ts.match(WasmToken::OpenParen, c.error))
        return nullptr;

    WasmAstExpr* expr = ParseExprInsideParens(c);
    if (!expr)
        return nullptr;

    if (!c.ts.match(WasmToken::CloseParen, c.error))
        return nullptr;

    return expr;
}

static WasmAstBlock*
ParseBlock(WasmParseContext& c)
{
    WasmAstExprVector exprs(c.lifo);

    while (c.ts.getIf(WasmToken::OpenParen)) {
        WasmAstExpr* expr = ParseExprInsideParens(c);
        if (!expr || !exprs.append(expr))
            return nullptr;
        if (!c.ts.match(WasmToken::CloseParen, c.error))
            return nullptr;
    }

    return new(c.lifo) WasmAstBlock(Move(exprs));
}

static WasmAstCall*
ParseCall(WasmParseContext& c, Expr expr)
{
    WasmToken index;
    if (!c.ts.match(WasmToken::Integer, &index, c.error))
        return nullptr;

    WasmAstExprVector args(c.lifo);
    while (c.ts.getIf(WasmToken::OpenParen)) {
        WasmAstExpr* arg = ParseExprInsideParens(c);
        if (!arg || !args.append(arg))
            return nullptr;
        if (!c.ts.match(WasmToken::CloseParen, c.error))
            return nullptr;
    }

    return new(c.lifo) WasmAstCall(expr, index.integer(), Move(args));
}

static WasmAstConst*
ParseConst(WasmParseContext& c, WasmToken constToken)
{
    switch (constToken.valueType()) {
      case ValType::I32: {
        WasmToken val;
        if (!c.ts.match(WasmToken::Integer, &val, c.error))
            return nullptr;
        return new(c.lifo) WasmAstConst(Val(val.integer()));
      }
      default:
        c.ts.generateError(constToken, c.error);
        return nullptr;
    }
}

static WasmAstGetLocal*
ParseGetLocal(WasmParseContext& c)
{
    WasmToken localIndex;
    if (!c.ts.match(WasmToken::Integer, &localIndex, c.error))
        return nullptr;

    return new(c.lifo) WasmAstGetLocal(localIndex.integer());
}

static WasmAstSetLocal*
ParseSetLocal(WasmParseContext& c)
{
    WasmToken localIndex;
    if (!c.ts.match(WasmToken::Integer, &localIndex, c.error))
        return nullptr;

    WasmAstExpr* value = ParseExpr(c);
    if (!value)
        return nullptr;

    return new(c.lifo) WasmAstSetLocal(localIndex.integer(), *value);
}

static WasmAstUnaryOperator*
ParseUnaryOperator(WasmParseContext& c, Expr expr)
{
    WasmAstExpr* op = ParseExpr(c);
    if (!op)
        return nullptr;

    return new(c.lifo) WasmAstUnaryOperator(expr, op);
}

static WasmAstBinaryOperator*
ParseBinaryOperator(WasmParseContext& c, Expr expr)
{
    WasmAstExpr* lhs = ParseExpr(c);
    if (!lhs)
        return nullptr;

    WasmAstExpr* rhs = ParseExpr(c);
    if (!rhs)
        return nullptr;

    return new(c.lifo) WasmAstBinaryOperator(expr, lhs, rhs);
}

static WasmAstComparisonOperator*
ParseComparisonOperator(WasmParseContext& c, Expr expr)
{
    WasmAstExpr* lhs = ParseExpr(c);
    if (!lhs)
        return nullptr;

    WasmAstExpr* rhs = ParseExpr(c);
    if (!rhs)
        return nullptr;

    return new(c.lifo) WasmAstComparisonOperator(expr, lhs, rhs);
}

static WasmAstConversionOperator*
ParseConversionOperator(WasmParseContext& c, Expr expr)
{
    WasmAstExpr* op = ParseExpr(c);
    if (!op)
        return nullptr;

    return new(c.lifo) WasmAstConversionOperator(expr, op);
}

static WasmAstIfElse*
ParseIfElse(WasmParseContext& c, Expr expr)
{
    WasmAstExpr* cond = ParseExpr(c);
    if (!cond)
        return nullptr;

    WasmAstExpr* ifBody = ParseExpr(c);
    if (!ifBody)
        return nullptr;

    WasmAstExpr* elseBody = nullptr;
    if (expr == Expr::IfElse) {
        elseBody = ParseExpr(c);
        if (!elseBody)
            return nullptr;
    }

    return new(c.lifo) WasmAstIfElse(expr, cond, ifBody, elseBody);
}

static WasmAstExpr*
ParseExprInsideParens(WasmParseContext& c)
{
    WasmToken token = c.ts.get();

    switch (token.kind()) {
      case WasmToken::Nop:
        return new(c.lifo) WasmAstNop;
      case WasmToken::BinaryOpcode:
        return ParseBinaryOperator(c, token.expr());
      case WasmToken::Block:
        return ParseBlock(c);
      case WasmToken::Call:
        return ParseCall(c, Expr::Call);
      case WasmToken::CallImport:
        return ParseCall(c, Expr::CallImport);
      case WasmToken::ComparisonOpcode:
        return ParseComparisonOperator(c, token.expr());
      case WasmToken::Const:
        return ParseConst(c, token);
      case WasmToken::ConversionOpcode:
        return ParseConversionOperator(c, token.expr());
      case WasmToken::If:
        return ParseIfElse(c, Expr::If);
      case WasmToken::IfElse:
        return ParseIfElse(c, Expr::IfElse);
      case WasmToken::GetLocal:
        return ParseGetLocal(c);
      case WasmToken::SetLocal:
        return ParseSetLocal(c);
      case WasmToken::UnaryOpcode:
        return ParseUnaryOperator(c, token.expr());
      default:
        c.ts.generateError(token, c.error);
        return nullptr;
    }
}

static bool
ParseValueType(WasmParseContext& c, WasmAstValTypeVector* vec)
{
    WasmToken valueType;
    return c.ts.match(WasmToken::ValueType, &valueType, c.error) &&
           vec->append(valueType.valueType());
}

static bool
ParseResult(WasmParseContext& c, ExprType* result)
{
    if (*result != ExprType::Void) {
        c.ts.generateError(c.ts.peek(), c.error);
        return false;
    }

    WasmToken valueType;
    if (!c.ts.match(WasmToken::ValueType, &valueType, c.error))
        return false;

    *result = ToExprType(valueType.valueType());
    return true;
}

static WasmAstFunc*
ParseFunc(WasmParseContext& c, WasmAstModule* module)
{
    WasmAstValTypeVector vars(c.lifo);
    WasmAstValTypeVector args(c.lifo);
    ExprType result = ExprType::Void;

    WasmAstExpr* maybeBody = nullptr;
    while (c.ts.getIf(WasmToken::OpenParen) && !maybeBody) {
        WasmToken token = c.ts.get();
        switch (token.kind()) {
          case WasmToken::Local:
            if (!ParseValueType(c, &vars))
                return nullptr;
            break;
          case WasmToken::Param:
            if (!ParseValueType(c, &args))
                return nullptr;
            break;
          case WasmToken::Result:
            if (!ParseResult(c, &result))
                return nullptr;
            break;
          default:
            c.ts.unget(token);
            maybeBody = ParseExprInsideParens(c);
            if (!maybeBody)
                return nullptr;
            break;
        }
        if (!c.ts.match(WasmToken::CloseParen, c.error))
            return nullptr;
    }

    uint32_t sigIndex;
    if (!module->declare(WasmAstSig(Move(args), result), &sigIndex))
        return nullptr;

    return new(c.lifo) WasmAstFunc(sigIndex, Move(vars), maybeBody);
}

static WasmAstSegment*
ParseSegment(WasmParseContext& c)
{
    if (!c.ts.match(WasmToken::Segment, c.error))
        return nullptr;

    WasmToken dstOffset;
    if (!c.ts.match(WasmToken::Integer, &dstOffset, c.error))
        return nullptr;

    WasmToken text;
    if (!c.ts.match(WasmToken::Text, &text, c.error))
        return nullptr;

    return new(c.lifo) WasmAstSegment(dstOffset.integer(), text.text());
}

static WasmAstMemory*
ParseMemory(WasmParseContext& c)
{
    WasmToken initialSize;
    if (!c.ts.match(WasmToken::Integer, &initialSize, c.error))
        return nullptr;

    WasmAstSegmentVector segments(c.lifo);
    while (c.ts.getIf(WasmToken::OpenParen)) {
        WasmAstSegment* segment = ParseSegment(c);
        if (!segment || !segments.append(segment))
            return nullptr;
        if (!c.ts.match(WasmToken::CloseParen, c.error))
            return nullptr;
    }

    return new(c.lifo) WasmAstMemory(initialSize.integer(), Move(segments));
}

static WasmAstImport*
ParseImport(WasmParseContext& c, WasmAstModule* module)
{
    WasmToken moduleName;
    if (!c.ts.match(WasmToken::Text, &moduleName, c.error))
        return nullptr;

    WasmToken funcName;
    if (!c.ts.match(WasmToken::Text, &funcName, c.error))
        return nullptr;

    WasmAstValTypeVector args(c.lifo);
    ExprType result = ExprType::Void;

    while (c.ts.getIf(WasmToken::OpenParen)) {
        WasmToken token = c.ts.get();
        switch (token.kind()) {
          case WasmToken::Param:
            if (!ParseValueType(c, &args))
                return nullptr;
            break;
          case WasmToken::Result:
            if (!ParseResult(c, &result))
                return nullptr;
            break;
          default:
            c.ts.generateError(token, c.error);
            return nullptr;
        }
        if (!c.ts.match(WasmToken::CloseParen, c.error))
            return nullptr;
    }

    uint32_t sigIndex;
    if (!module->declare(WasmAstSig(Move(args), result), &sigIndex))
        return nullptr;

    return new(c.lifo) WasmAstImport(moduleName.text(), funcName.text(), sigIndex);
}

static WasmAstExport*
ParseExport(WasmParseContext& c)
{
    WasmToken name;
    if (!c.ts.match(WasmToken::Text, &name, c.error))
        return nullptr;

    WasmToken exportee = c.ts.get();
    switch (exportee.kind()) {
      case WasmToken::Integer:
        return new(c.lifo) WasmAstExport(name.text(), exportee.integer());
      case WasmToken::Memory:
        return new(c.lifo) WasmAstExport(name.text());
      default:
        break;
    }

    c.ts.generateError(exportee, c.error);
    return nullptr;

}

static WasmAstModule*
ParseModule(const char16_t* text, LifoAlloc& lifo, UniqueChars* error)
{
    WasmParseContext c(text, lifo, error);

    if (!c.ts.match(WasmToken::OpenParen, c.error))
        return nullptr;
    if (!c.ts.match(WasmToken::Module, c.error))
        return nullptr;

    auto module = new(c.lifo) WasmAstModule(c.lifo);
    if (!module || !module->init())
        return nullptr;

    while (c.ts.getIf(WasmToken::OpenParen)) {
        WasmToken section = c.ts.get();

        switch (section.kind()) {
          case WasmToken::Memory: {
            WasmAstMemory* memory = ParseMemory(c);
            if (!memory)
                return nullptr;
            if (!module->setMemory(memory)) {
                c.ts.generateError(section, c.error);
                return nullptr;
            }
            break;
          }
          case WasmToken::Import: {
            WasmAstImport* imp = ParseImport(c, module);
            if (!imp || !module->append(imp))
                return nullptr;
            break;
          }
          case WasmToken::Export: {
            WasmAstExport* exp = ParseExport(c);
            if (!exp || !module->append(exp))
                return nullptr;
            break;
          }
          case WasmToken::Func: {
            WasmAstFunc* func = ParseFunc(c, module);
            if (!func || !module->append(func))
                return nullptr;
            break;
          }
          default:
            c.ts.generateError(section, c.error);
            return nullptr;
        }

        if (!c.ts.match(WasmToken::CloseParen, c.error))
            return nullptr;
    }

    if (!c.ts.match(WasmToken::CloseParen, c.error))
        return nullptr;
    if (!c.ts.match(WasmToken::EndOfFile, c.error))
        return nullptr;

    return module;
}

/*****************************************************************************/
// wasm function body serialization

static bool
EncodeExpr(Encoder& e, WasmAstExpr& expr);

static bool
EncodeBlock(Encoder& e, WasmAstBlock& b)
{
    if (!e.writeExpr(Expr::Block))
        return false;

    size_t numExprs = b.exprs().length();
    if (!e.writeVarU32(numExprs))
        return false;

    for (size_t i = 0; i < numExprs; i++) {
        if (!EncodeExpr(e, *b.exprs()[i]))
            return false;
    }

    return true;
}

static bool
EncodeCall(Encoder& e, WasmAstCall& c)
{
    if (!e.writeExpr(c.expr()))
        return false;

    if (!e.writeU32(c.index()))
        return false;

    for (WasmAstExpr* arg : c.args()) {
        if (!EncodeExpr(e, *arg))
            return false;
    }

    return true;
}

static bool
EncodeConst(Encoder& e, WasmAstConst& c)
{
    switch (c.val().type()) {
      case ValType::I32:
        return e.writeExpr(Expr::I32Const) &&
               e.writeVarU32(c.val().i32());
      default:
        break;
    }
    MOZ_CRASH("Bad value type");
}

static bool
EncodeGetLocal(Encoder& e, WasmAstGetLocal& gl)
{
    return e.writeExpr(Expr::GetLocal) &&
           e.writeVarU32(gl.localIndex());
}

static bool
EncodeSetLocal(Encoder& e, WasmAstSetLocal& sl)
{
    return e.writeExpr(Expr::SetLocal) &&
           e.writeVarU32(sl.localIndex()) &&
           EncodeExpr(e, sl.value());
}

static bool
EncodeUnaryOperator(Encoder& e, WasmAstUnaryOperator& b)
{
    return e.writeExpr(b.expr()) &&
           EncodeExpr(e, *b.op());
}

static bool
EncodeBinaryOperator(Encoder& e, WasmAstBinaryOperator& b)
{
    return e.writeExpr(b.expr()) &&
           EncodeExpr(e, *b.lhs()) &&
           EncodeExpr(e, *b.rhs());
}

static bool
EncodeComparisonOperator(Encoder& e, WasmAstComparisonOperator& b)
{
    return e.writeExpr(b.expr()) &&
           EncodeExpr(e, *b.lhs()) &&
           EncodeExpr(e, *b.rhs());
}

static bool
EncodeConversionOperator(Encoder& e, WasmAstConversionOperator& b)
{
    return e.writeExpr(b.expr()) &&
           EncodeExpr(e, *b.op());
}

static bool
EncodeIfElse(Encoder& e, WasmAstIfElse& ie)
{
    return e.writeExpr(ie.expr()) &&
           EncodeExpr(e, ie.cond()) &&
           EncodeExpr(e, ie.ifBody()) &&
           (!ie.hasElse() || EncodeExpr(e, ie.elseBody()));
}

static bool
EncodeExpr(Encoder& e, WasmAstExpr& expr)
{
    switch (expr.kind()) {
      case WasmAstExprKind::Nop:
        return e.writeExpr(Expr::Nop);
      case WasmAstExprKind::BinaryOperator:
        return EncodeBinaryOperator(e, expr.as<WasmAstBinaryOperator>());
      case WasmAstExprKind::Block:
        return EncodeBlock(e, expr.as<WasmAstBlock>());
      case WasmAstExprKind::Call:
        return EncodeCall(e, expr.as<WasmAstCall>());
      case WasmAstExprKind::ComparisonOperator:
        return EncodeComparisonOperator(e, expr.as<WasmAstComparisonOperator>());
      case WasmAstExprKind::Const:
        return EncodeConst(e, expr.as<WasmAstConst>());
      case WasmAstExprKind::ConversionOperator:
        return EncodeConversionOperator(e, expr.as<WasmAstConversionOperator>());
      case WasmAstExprKind::GetLocal:
        return EncodeGetLocal(e, expr.as<WasmAstGetLocal>());
      case WasmAstExprKind::IfElse:
        return EncodeIfElse(e, expr.as<WasmAstIfElse>());
      case WasmAstExprKind::SetLocal:
        return EncodeSetLocal(e, expr.as<WasmAstSetLocal>());
      case WasmAstExprKind::UnaryOperator:
        return EncodeUnaryOperator(e, expr.as<WasmAstUnaryOperator>());
      default:;
    }
    MOZ_CRASH("Bad expr kind");
}

/*****************************************************************************/
// wasm AST binary serialization

static bool
EncodeSignatureSection(Encoder& e, WasmAstModule& module)
{
    if (module.sigs().empty())
        return true;

    if (!e.writeCString(SigSection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(module.sigs().length()))
        return false;

    for (WasmAstSig* sig : module.sigs()) {
        if (!e.writeVarU32(sig->args().length()))
            return false;

        if (!e.writeExprType(sig->ret()))
            return false;

        for (ValType t : sig->args()) {
            if (!e.writeValType(t))
                return false;
        }
    }

    e.finishSection(offset);
    return true;
}

static bool
EncodeDeclarationSection(Encoder& e, WasmAstModule& module)
{
    if (module.funcs().empty())
        return true;

    if (!e.writeCString(DeclSection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(module.funcs().length()))
        return false;

    for (WasmAstFunc* func : module.funcs()) {
        if (!e.writeVarU32(func->sigIndex()))
            return false;
    }

    e.finishSection(offset);
    return true;
}

static bool
EncodeCString(Encoder& e, TwoByteChars twoByteChars)
{
    UniqueChars utf8(JS::CharsToNewUTF8CharsZ(nullptr, twoByteChars).c_str());
    return utf8 && e.writeCString(utf8.get());
}

static bool
EncodeImport(Encoder& e, WasmAstImport& imp)
{
    if (!e.writeCString(FuncSubsection))
        return false;

    if (!e.writeVarU32(imp.sigIndex()))
        return false;

    if (!EncodeCString(e, imp.module()))
        return false;

    if (!EncodeCString(e, imp.func()))
        return false;

    return true;
}

static bool
EncodeImportSection(Encoder& e, WasmAstModule& module)
{
    if (module.imports().empty())
        return true;

    if (!e.writeCString(ImportSection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(module.imports().length()))
        return false;

    for (WasmAstImport* imp : module.imports()) {
        if (!EncodeImport(e, *imp))
            return false;
    }

    e.finishSection(offset);
    return true;
}

static bool
EncodeMemorySection(Encoder& e, WasmAstModule& module)
{
    if (!module.maybeMemory())
        return true;

    if (!e.writeCString(MemorySection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    WasmAstMemory& memory = *module.maybeMemory();

    if (!e.writeCString(FieldInitial))
        return false;

    if (!e.writeVarU32(memory.initialSize()))
        return false;

    e.finishSection(offset);
    return true;
}

static bool
EncodeFunctionExport(Encoder& e, WasmAstExport& exp)
{
    if (!e.writeCString(FuncSubsection))
        return false;

    if (!e.writeVarU32(exp.funcIndex()))
        return false;

    if (!EncodeCString(e, exp.name()))
        return false;

    return true;
}

static bool
EncodeMemoryExport(Encoder& e, WasmAstExport& exp)
{
    if (!e.writeCString(MemorySubsection))
        return false;

    if (!EncodeCString(e, exp.name()))
        return false;

    return true;
}

static bool
EncodeExportSection(Encoder& e, WasmAstModule& module)
{
    if (module.exports().empty())
        return true;

    if (!e.writeCString(ExportSection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(module.exports().length()))
        return false;

    for (WasmAstExport* exp : module.exports()) {
        switch (exp->kind()) {
          case WasmAstExportKind::Func:
            if (!EncodeFunctionExport(e, *exp))
                return false;
            break;
          case WasmAstExportKind::Memory:
            if (!EncodeMemoryExport(e, *exp))
                return false;
            break;
        }
    }

    e.finishSection(offset);
    return true;
}

static bool
EncodeFunc(Encoder& e, WasmAstFunc& func)
{
    if (!e.writeCString(FuncSubsection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(func.varTypes().length()))
        return false;

    for (ValType type : func.varTypes()) {
        if (!e.writeValType(type))
            return false;
    }

    if (func.maybeBody()) {
        if (!EncodeExpr(e, *func.maybeBody()))
            return false;
    } else {
        if (!e.writeExpr(Expr::Nop))
            return false;
    }

    e.finishSection(offset);

    return true;
}

static bool
EncodeCodeSection(Encoder& e, WasmAstModule& module)
{
    if (module.funcs().empty())
        return true;

    if (!e.writeCString(CodeSection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(module.funcs().length()))
        return false;

    for (WasmAstFunc* func : module.funcs()) {
        if (!EncodeFunc(e, *func))
            return false;
    }

    e.finishSection(offset);
    return true;
}

static bool
EncodeDataSegment(Encoder& e, WasmAstSegment& segment)
{
    if (!e.writeCString(SegmentSubsection))
        return false;

    if (!e.writeVarU32(segment.offset()))
        return false;

    TwoByteChars text = segment.text();

    Vector<uint8_t, 0, SystemAllocPolicy> bytes;
    if (!bytes.reserve(text.length()))
        return false;

    const char16_t* cur = text.start().get();
    const char16_t* end = text.end().get();
    while (cur != end) {
        uint8_t byte;
        MOZ_ALWAYS_TRUE(ConsumeTextByte(&cur, end, &byte));
        bytes.infallibleAppend(byte);
    }

    if (!e.writeVarU32(bytes.length()))
        return false;

    if (!e.writeData(bytes.begin(), bytes.length()))
        return false;

    return true;
}

static bool
EncodeDataSection(Encoder& e, WasmAstModule& module)
{
    if (!module.maybeMemory() || module.maybeMemory()->segments().empty())
        return true;

    const WasmAstSegmentVector& segments = module.maybeMemory()->segments();

    if (!e.writeCString(DataSection))
        return false;

    size_t offset;
    if (!e.startSection(&offset))
        return false;

    if (!e.writeVarU32(segments.length()))
        return false;

    for (WasmAstSegment* segment : segments) {
        if (!EncodeDataSegment(e, *segment))
            return false;
    }

    e.finishSection(offset);
    return true;
}

static UniqueBytecode
EncodeModule(WasmAstModule& module)
{
    UniqueBytecode bytecode = MakeUnique<Bytecode>();
    if (!bytecode)
        return nullptr;

    Encoder e(*bytecode);

    if (!e.writeU32(MagicNumber))
        return nullptr;

    if (!e.writeU32(EncodingVersion))
        return nullptr;

    if (!EncodeSignatureSection(e, module))
        return nullptr;

    if (!EncodeImportSection(e, module))
        return nullptr;

    if (!EncodeDeclarationSection(e, module))
        return nullptr;

    if (!EncodeMemorySection(e, module))
        return nullptr;

    if (!EncodeExportSection(e, module))
        return nullptr;

    if (!EncodeCodeSection(e, module))
        return nullptr;

    if (!EncodeDataSection(e, module))
        return nullptr;

    if (!e.writeCString(EndSection))
        return nullptr;

    return Move(bytecode);
}

/*****************************************************************************/

UniqueBytecode
wasm::TextToBinary(const char16_t* text, UniqueChars* error)
{
    LifoAlloc lifo(AST_LIFO_DEFAULT_CHUNK_SIZE);
    WasmAstModule* module = ParseModule(text, lifo, error);
    if (!module)
        return nullptr;

    return EncodeModule(*module);
}
