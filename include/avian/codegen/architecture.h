/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_CODEGEN_ARCHITECTURE_H
#define AVIAN_CODEGEN_ARCHITECTURE_H

#include "ir.h"
#include "registers.h"

namespace vm {
class Zone;
}

namespace avian {

namespace util {
class Alloc;
}

namespace codegen {

class Assembler;

// A store that retargets live code, e.g. a call site being pointed at
// a freshly compiled method.  An Architecture computes it from the live
// code (see Architecture::callPatch) but never performs it: applying it
// is the job of the owner of code memory (avian::system::CodeMemory),
// which knows how to make executable memory writable and how to make
// the result visible to other threads' instruction fetch.
class CodePatch {
 public:
  static const unsigned MaxSize = 8;

  CodePatch() : address(0), size(0)
  {
  }

  CodePatch(void* address, const void* src, unsigned size)
      : address(address), size(size)
  {
    for (unsigned i = 0; i < size; ++i) {
      bytes[i] = static_cast<const uint8_t*>(src)[i];
    }
  }

  // Where the bytes go.
  void* address;
  // 4 or 8.  For call sites emitted as lir::Aligned* the store is
  // naturally aligned, so it can be done atomically with respect to
  // other threads executing the code.
  unsigned size;
  uint8_t bytes[MaxSize];
};

class OperandMask {
 public:
  uint8_t typeMask;
  RegisterMask lowRegisterMask;
  RegisterMask highRegisterMask;

  OperandMask(uint8_t typeMask,
              RegisterMask lowRegisterMask,
              RegisterMask highRegisterMask)
      : typeMask(typeMask),
        lowRegisterMask(lowRegisterMask),
        highRegisterMask(highRegisterMask)
  {
  }

  OperandMask() : typeMask(~0), lowRegisterMask(AnyRegisterMask), highRegisterMask(AnyRegisterMask)
  {
  }

  void setLowHighRegisterMasks(RegisterMask lowRegisterMask, RegisterMask highRegisterMask) {
    this->lowRegisterMask = lowRegisterMask;
    this->highRegisterMask = highRegisterMask;
  }
};

class Architecture {
 public:
  virtual unsigned floatRegisterSize() = 0;

  virtual const RegisterFile* registerFile() = 0;

  virtual Register scratch() = 0;
  virtual Register stack() = 0;
  virtual Register thread() = 0;
  virtual Register returnLow() = 0;
  virtual Register returnHigh() = 0;
  virtual Register virtualCallTarget() = 0;
  virtual Register virtualCallIndex() = 0;

  virtual ir::TargetInfo targetInfo() = 0;

  virtual bool bigEndian() = 0;

  virtual uintptr_t maximumImmediateJump() = 0;

  virtual bool alwaysCondensed(lir::BinaryOperation op) = 0;
  virtual bool alwaysCondensed(lir::TernaryOperation op) = 0;

  virtual bool reserved(Register register_) = 0;

  virtual unsigned frameFootprint(unsigned footprint) = 0;
  virtual unsigned argumentFootprint(unsigned footprint) = 0;
  virtual bool argumentAlignment() = 0;
  virtual bool argumentRegisterAlignment() = 0;
  virtual unsigned argumentRegisterCount() = 0;
  virtual Register argumentRegister(unsigned index) = 0;

  virtual bool hasLinkRegister() = 0;

  virtual unsigned stackAlignmentInWords() = 0;

  virtual bool matchCall(void* returnAddress, void* target) = 0;

  // Describes the store that makes the call or jump `op` which returns
  // to `returnAddress` go to `newTarget` instead.  This only reads the
  // live code; it is up to the caller to apply the patch.
  virtual CodePatch callPatch(lir::UnaryOperation op,
                              void* returnAddress,
                              void* newTarget) = 0;

  virtual unsigned alignFrameSize(unsigned sizeInWords) = 0;

  virtual void nextFrame(void* start,
                         unsigned size,
                         unsigned footprint,
                         void* link,
                         bool mostRecent,
                         int targetParameterFootprint,
                         void** ip,
                         void** stack) = 0;
  virtual void* frameIp(void* stack) = 0;
  virtual unsigned frameHeaderSize() = 0;
  virtual unsigned frameReturnAddressSize() = 0;
  virtual unsigned frameFooterSize() = 0;
  virtual int returnAddressOffset() = 0;
  virtual int framePointerOffset() = 0;

  virtual void plan(lir::UnaryOperation op,
                    unsigned aSize,
                    OperandMask& aMask,
                    bool* thunk) = 0;

  virtual void planSource(lir::BinaryOperation op,
                          unsigned aSize,
                          OperandMask& aMask,
                          unsigned bSize,
                          bool* thunk) = 0;

  virtual void planDestination(lir::BinaryOperation op,
                               unsigned aSize,
                               const OperandMask& aMask,
                               unsigned bSize,
                               OperandMask& bMask) = 0;

  virtual void planMove(unsigned size,
                        OperandMask& src,
                        OperandMask& tmp,
                        const OperandMask& dst) = 0;

  virtual void planSource(lir::TernaryOperation op,
                          unsigned aSize,
                          OperandMask& aMask,
                          unsigned bSize,
                          OperandMask& bMask,
                          unsigned cSize,
                          bool* thunk) = 0;

  virtual void planDestination(lir::TernaryOperation op,
                               unsigned aSize,
                               const OperandMask& aMask,
                               unsigned bSize,
                               const OperandMask& bMask,
                               unsigned cSize,
                               OperandMask& cMask) = 0;

  virtual Assembler* makeAssembler(util::Alloc*, vm::Zone*) = 0;

  virtual void acquire() = 0;
  virtual void release() = 0;
};

}  // namespace codegen
}  // namespace avian

#endif  // AVIAN_CODEGEN_ARCHITECTURE_H
