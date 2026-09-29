/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_CODEGEN_ASSEMBLER_H
#define AVIAN_CODEGEN_ASSEMBLER_H

#include <avian/system/system.h>
#include "avian/zone.h"

#include <avian/codegen/lir.h>
#include <avian/codegen/registers.h>
#include <avian/codegen/promise.h>

namespace avian {
namespace codegen {

class Architecture;

class OperandInfo {
 public:
  const unsigned size;
  const lir::Operand::Type type;
  lir::Operand* const operand;

  inline OperandInfo(unsigned size,
                     lir::Operand::Type type,
                     lir::Operand* operand)
      : size(size), type(type), operand(operand)
  {
  }
};

#ifdef AVIAN_TAILS
const bool TailCalls = true;
#else
const bool TailCalls = false;
#endif

#ifdef AVIAN_USE_FRAME_POINTER
const bool UseFramePointer = true;
#else
const bool UseFramePointer = false;
#endif

class Assembler {
 public:
  class Client {
   public:
    virtual Register acquireTemporary(RegisterMask mask = AnyRegisterMask) = 0;
    virtual void releaseTemporary(Register r) = 0;

    virtual void save(Register r) = 0;
  };

  class Block {
   public:
    virtual unsigned resolve(unsigned start, Block* next) = 0;
  };

  virtual void setClient(Client* client) = 0;

  virtual Architecture* arch() = 0;

  virtual void checkStackOverflow(uintptr_t handler,
                                  unsigned stackLimitOffsetFromThread) = 0;
  virtual void saveFrame(unsigned stackOffset, unsigned ipOffset) = 0;
  virtual void pushFrame(unsigned argumentCount, ...) = 0;
  virtual void allocateFrame(unsigned footprint) = 0;
  virtual void adjustFrame(unsigned difference) = 0;
  virtual void popFrame(unsigned footprint) = 0;
  virtual void popFrameForTailCall(unsigned footprint,
                                   int offset,
                                   Register returnAddressSurrogate,
                                   Register framePointerSurrogate) = 0;
  virtual void popFrameAndPopArgumentsAndReturn(unsigned frameFootprint,
                                                unsigned argumentFootprint) = 0;
  virtual void popFrameAndUpdateStackAndReturn(unsigned frameFootprint,
                                               unsigned stackOffsetFromThread)
      = 0;

  virtual void apply(lir::Operation op) = 0;
  virtual void apply(lir::UnaryOperation op, OperandInfo a) = 0;
  virtual void apply(lir::BinaryOperation op, OperandInfo a, OperandInfo b) = 0;
  virtual void apply(lir::TernaryOperation op,
                     OperandInfo a,
                     OperandInfo b,
                     OperandInfo c) = 0;

  // Emits the code laid out by the preceding Block::resolve calls into
  // `buffer`, linked to run at `address`.
  //
  // Every PC-relative displacement and every absolute code address is
  // computed from `address`; `buffer` is only ever stored to.  The two
  // may coincide (assembling in place, e.g. into a boot image) or not
  // (assembling into a staging buffer that the code memory subsystem
  // later copies to `address`).  `buffer` must be ordinary writable
  // memory: the assembler never writes to executable memory itself.
  //
  // Fixups whose values are not yet known register promise listeners
  // that write into `buffer` later.  That only makes sense when
  // `buffer` is the code's final storage, i.e. for boot image
  // generation, which is the only producer of such promises.
  virtual void write(uint8_t* buffer, uint8_t* address) = 0;

  virtual Promise* offset(bool forTrace = false) = 0;

  virtual Block* endBlock(bool startNew) = 0;

  virtual void endEvent() = 0;

  virtual unsigned length() = 0;

  virtual unsigned footerSize() = 0;

  virtual void dispose() = 0;
};

}  // namespace codegen
}  // namespace avian

#endif  // AVIAN_CODEGEN_ASSEMBLER_H
