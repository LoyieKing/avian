/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_COMPILE_CPP_INCLUDE
#error "include this file from src/compile.cpp"
#endif

// java.lang.String on the avian classpath stores Modified UTF-8.
// length, isEmpty, and equals are straight line. charAt and hashCode
// keep one out-of-line call for the slow case, using the two free
// bytes inside the invoke the way inlined new_ does. OpenJDK's String
// has a different layout, so none of this runs there. The thunks
// themselves stay in compile.cpp: the thunk table takes their address
// in every build.

#if TARGET_BYTES_PER_WORD == 8
bool methodAllowsInlineNew(Context* context);
unsigned takeSideIp(Context* context);
#endif

bool inTryBlock(MyThread* t, GcCode* code, unsigned ip);

bool stringNameIs(GcByteArray* name, const char* literal)
{
  return name->length() == strlen(literal) + 1
         and ::strcmp(reinterpret_cast<const char*>(name->body().begin()),
                      literal)
                == 0;
}

bool stringInvokeVirtual(Frame* frame)
{
  GcCode* code = frame->context->method->code();
  if (code == 0 or frame->ip >= code->length()) {
    return false;
  }
  return static_cast<unsigned>(
             static_cast<uint8_t>(code->body()[frame->ip]))
         == invokevirtual;
}

#if defined(HAVE_StringUnsafe_data) && TARGET_BYTES_PER_WORD == 8

class StringSlowPath {
 public:
  StringSlowPath* next;
  Compiler::State* edge;
  unsigned slowIp;
  unsigned contIp;
  TraceElement* trace;
  ir::Value* string;
  ir::Value* index;
  bool hashCode;
};

void queueStringSlowPath(Context* context, StringSlowPath* path)
{
  path->next = 0;
  if (context->stringSlowPathTail) {
    context->stringSlowPathTail->next = path;
  } else {
    context->stringSlowPaths = path;
  }
  context->stringSlowPathTail = path;
}

// invokevirtual is three bytes. The operand bytes are not targets, and
// the instruction is not the last byte of the method.
bool stringHolesFree(MyThread* t, Context* context, unsigned origin)
{
  GcCode* code = context->method->code();
  if (code == 0 or origin + 3 >= code->length()) {
    return false;
  }
  if (static_cast<unsigned>(static_cast<uint8_t>(code->body()[origin]))
      != invokevirtual) {
    return false;
  }
  if (context->visitTable[origin + 1] or context->visitTable[origin + 2]) {
    return false;
  }

  GcExceptionHandlerTable* eht = cast<GcExceptionHandlerTable>(
      t, code->exceptionHandlerTable());
  if (eht) {
    for (unsigned i = 0; i < eht->length(); ++i) {
      unsigned hip = exceptionHandlerIp(eht->body()[i]);
      if (hip == origin + 1 or hip == origin + 2) {
        return false;
      }
    }
  }
  return true;
}

ir::Value* stringSignBit(Compiler* c, ir::Value* value)
{
  return c->binaryOp(lir::ShiftRight,
                     ir::Type::iptr(),
                     c->constant(63, ir::Type::iptr()),
                     value);
}

// -1 when value != 0, else 0. value is a pointer-sized integer.
ir::Value* stringPresentMask(Compiler* c, ir::Value* value)
{
  ir::Value* negative = c->binaryOp(lir::Subtract,
                                     ir::Type::iptr(),
                                     value,
                                     c->constant(0, ir::Type::iptr()));
  return stringSignBit(
      c, c->binaryOp(lir::Or, ir::Type::iptr(), value, negative));
}

bool tryStringCharAt(MyThread* t, Frame* frame)
{
  Context* context = frame->context;
  if (frame->subroutine or not methodAllowsInlineNew(context)) {
    return false;
  }
  unsigned origin = frame->ip;
  // The out-of-line call is emitted after the method body, so its
  // return address is outside every try. A catch around this invoke
  // has to see the real call.
  if (inTryBlock(t, context->method->code(), origin)) {
    return false;
  }
  if (not stringHolesFree(t, context, origin)) {
    return false;
  }

  Compiler* c = context->compiler;
  // Top of stack is the index. The receiver is under it.
  ir::Value* index = c->peek(1, 0);
  ir::Value* string = c->peek(1, 1);
  TraceElement* trace = frame->trace(0, 0);

  unsigned fastIp = origin + 1;
  unsigned contIp = origin + 2;
  unsigned slowIp = takeSideIp(context);
  ir::Value* slow = c->promiseConstant(c->machineIp(slowIp), ir::Type::iptr());

  // A null receiver faults on this load and becomes a NullPointerException.
  // A null data field is an unsafe string: use the receiver as a stand-in
  // base so the length load does not fault, then mask the length to 0.
  ir::Value* data = c->load(ir::ExtendMode::Signed,
                            c->memory(string, ir::Type::iptr(), StringData),
                            ir::Type::iptr());
  ir::Value* present = stringPresentMask(c, data);
  ir::Value* absent = c->binaryOp(
      lir::Xor, ir::Type::iptr(), present, c->constant(-1, ir::Type::iptr()));
  ir::Value* addr = c->binaryOp(
      lir::Or,
      ir::Type::iptr(),
      c->binaryOp(lir::And, ir::Type::iptr(), data, present),
      c->binaryOp(lir::And, ir::Type::iptr(), string, absent));
  ir::Value* rawLen
      = c->load(ir::ExtendMode::Unsigned,
                c->memory(addr, ir::Type::iptr(), TargetArrayLength),
                ir::Type::iptr());
  ir::Value* len = c->binaryOp(lir::And, ir::Type::iptr(), rawLen, present);
  // A shared symbol array counts a trailing 0. Drop it when the last
  // counted byte is 0. An empty array, or a null data field, loads a
  // byte of the object header and the mask throws that load away.
  ir::Value* lenPresent = stringPresentMask(c, len);
  ir::Value* lastOff = c->binaryOp(
      lir::And,
      ir::Type::iptr(),
      lenPresent,
      c->binaryOp(lir::Add,
                  ir::Type::iptr(),
                  len,
                  c->constant(TargetArrayBody - 1, ir::Type::iptr())));
  ir::Value* lastAddr
      = c->binaryOp(lir::Add, ir::Type::iptr(), lastOff, addr);
  ir::Value* last = c->load(ir::ExtendMode::Signed,
                            c->memory(lastAddr, ir::Type::i1(), 0),
                            ir::Type::iptr());
  ir::Value* lastIsNul = c->binaryOp(lir::Xor,
                                     ir::Type::iptr(),
                                     stringPresentMask(c, last),
                                     c->constant(-1, ir::Type::iptr()));
  ir::Value* adjust = c->binaryOp(
      lir::And,
      ir::Type::iptr(),
      c->constant(1, ir::Type::iptr()),
      c->binaryOp(lir::And, ir::Type::iptr(), lastIsNul, lenPresent));
  ir::Value* byteLen
      = c->binaryOp(lir::Subtract, ir::Type::iptr(), adjust, len);
  ir::Value* charLen
      = c->load(ir::ExtendMode::Signed,
                c->memory(string, ir::Type::i4(), StringLength),
                ir::Type::i4());
  // The x86 backend cannot zero-extend a 32-bit register to a pointer
  // (moveZRR only implements 16-bit sources). Length is non-negative,
  // so a sign extend is a zero extend for the ASCII test. The bounds
  // mask stays in 32 bits: index>>31 is -1 when index<0, and
  // (length-index-1)>>31 is -1 when index>=length. Sign-extending
  // that mask to a pointer keeps the single out-of-line branch.
  ir::Value* negativeIndex = c->binaryOp(lir::ShiftRight,
                                         ir::Type::i4(),
                                         c->constant(31, ir::Type::i4()),
                                         index);
  ir::Value* above
      = c->binaryOp(lir::Subtract, ir::Type::i4(), index, charLen);
  ir::Value* aboveMinusOne = c->binaryOp(
      lir::Subtract, ir::Type::i4(), c->constant(1, ir::Type::i4()), above);
  ir::Value* outOfRange = c->binaryOp(lir::ShiftRight,
                                      ir::Type::i4(),
                                      c->constant(31, ir::Type::i4()),
                                      aboveMinusOne);
  ir::Value* boundsFail
      = c->binaryOp(lir::Or, ir::Type::i4(), negativeIndex, outOfRange);
  ir::Value* boundsWide
      = c->load(ir::ExtendMode::Signed, boundsFail, ir::Type::iptr());
  ir::Value* charLenWide
      = c->load(ir::ExtendMode::Signed, charLen, ir::Type::iptr());
  ir::Value* notAscii
      = c->binaryOp(lir::Xor, ir::Type::iptr(), byteLen, charLenWide);
  // Unmanaged ASCII stores a big-endian u16 byte count, then the bytes.
  // A null header reads two bytes of the receiver and the mask drops them.
  // Either the managed array or that header matching the char length is
  // enough; multibyte payloads fail both and stay on the slow call.
  ir::Value* header = c->load(
      ir::ExtendMode::Signed,
      c->memory(string, ir::Type::iptr(), StringUnsafe_data),
      ir::Type::iptr());
  ir::Value* headerPresent = stringPresentMask(c, header);
  ir::Value* headerAbsent = c->binaryOp(
      lir::Xor, ir::Type::iptr(), headerPresent, c->constant(-1, ir::Type::iptr()));
  ir::Value* headerAddr = c->binaryOp(
      lir::Or,
      ir::Type::iptr(),
      c->binaryOp(lir::And, ir::Type::iptr(), header, headerPresent),
      c->binaryOp(lir::And, ir::Type::iptr(), string, headerAbsent));
  ir::Value* lenHi = c->load(ir::ExtendMode::Signed,
                             c->memory(headerAddr, ir::Type::i1(), 0),
                             ir::Type::iptr());
  ir::Value* lenLo = c->load(ir::ExtendMode::Signed,
                             c->memory(headerAddr, ir::Type::i1(), 1),
                             ir::Type::iptr());
  ir::Value* hi = c->binaryOp(
      lir::And, ir::Type::iptr(), lenHi, c->constant(0xff, ir::Type::iptr()));
  ir::Value* lo = c->binaryOp(
      lir::And, ir::Type::iptr(), lenLo, c->constant(0xff, ir::Type::iptr()));
  ir::Value* headerRaw = c->binaryOp(
      lir::Or,
      ir::Type::iptr(),
      c->binaryOp(lir::ShiftLeft,
                  ir::Type::iptr(),
                  c->constant(8, ir::Type::iptr()),
                  hi),
      lo);
  ir::Value* headerLen
      = c->binaryOp(lir::And, ir::Type::iptr(), headerRaw, headerPresent);
  ir::Value* headerMismatch
      = c->binaryOp(lir::Xor, ir::Type::iptr(), headerLen, charLenWide);
  ir::Value* managedMiss = c->binaryOp(lir::Or, ir::Type::iptr(), absent, notAscii);
  ir::Value* unmanagedMiss
      = c->binaryOp(lir::Or, ir::Type::iptr(), headerAbsent, headerMismatch);
  ir::Value* neither
      = c->binaryOp(lir::And, ir::Type::iptr(), managedMiss, unmanagedMiss);
  ir::Value* blocked
      = c->binaryOp(lir::Or, ir::Type::iptr(), neither, boundsWide);
  c->condJump(lir::JumpIfNotEqual, c->constant(0, ir::Type::iptr()), blocked, slow);
  Compiler::State* edge = c->saveState();
  c->startLogicalIp(fastIp);

  // Values above the jump have no site on this side. The receiver and
  // the index are stack homes. Managed bytes sit at array body + index.
  // Unmanaged bytes sit at header + 2 + index. The branch already
  // proved exactly one of those bases is live.
  ir::Value* fastData
      = c->load(ir::ExtendMode::Signed,
                c->memory(string, ir::Type::iptr(), StringData),
                ir::Type::iptr());
  ir::Value* fastHeader
      = c->load(ir::ExtendMode::Signed,
                c->memory(string, ir::Type::iptr(), StringUnsafe_data),
                ir::Type::iptr());
  ir::Value* fastPresent = stringPresentMask(c, fastData);
  ir::Value* fastAbsent = c->binaryOp(
      lir::Xor, ir::Type::iptr(), fastPresent, c->constant(-1, ir::Type::iptr()));
  ir::Value* managedBase = c->binaryOp(
      lir::Add, ir::Type::iptr(), c->constant(TargetArrayBody, ir::Type::iptr()), fastData);
  ir::Value* unmanagedBase = c->binaryOp(
      lir::Add, ir::Type::iptr(), c->constant(2, ir::Type::iptr()), fastHeader);
  ir::Value* base = c->binaryOp(
      lir::Or,
      ir::Type::iptr(),
      c->binaryOp(lir::And, ir::Type::iptr(), managedBase, fastPresent),
      c->binaryOp(lir::And, ir::Type::iptr(), unmanagedBase, fastAbsent));
  // ASCII bytes are below 0x80, so a sign-extending load matches a
  // zero-extending one. moveZ has no 8-bit source on x86.
  ir::Value* ch = c->load(
      ir::ExtendMode::Signed,
      c->memory(base, ir::Type::i1(), 0, index),
      ir::Type::i4());
  c->store(ch,
           c->memory(c->threadRegister(), ir::Type::i4(), AllocationResultOffset));

  c->startLogicalIp(contIp);
  frame->pop(ir::Type::i4());
  frame->pop(ir::Type::object());
  frame->push(ir::Type::i4(),
              c->load(ir::ExtendMode::Signed,
                      c->memory(c->threadRegister(),
                                ir::Type::i4(),
                                AllocationResultOffset),
                      ir::Type::i4()));

  StringSlowPath* path
      = new (context->zone.allocate(sizeof(StringSlowPath))) StringSlowPath;
  path->edge = edge;
  path->slowIp = slowIp;
  path->contIp = contIp;
  path->trace = trace;
  path->string = string;
  path->index = index;
  path->hashCode = false;
  queueStringSlowPath(context, path);
  return true;
}

bool tryStringHashCode(MyThread* t, Frame* frame)
{
  Context* context = frame->context;
  if (frame->subroutine or not methodAllowsInlineNew(context)) {
    return false;
  }
  unsigned origin = frame->ip;
  if (not stringHolesFree(t, context, origin)) {
    return false;
  }

  Compiler* c = context->compiler;
  ir::Value* string = c->peek(1, 0);
  TraceElement* trace = frame->trace(0, 0);

  unsigned fastIp = origin + 1;
  unsigned contIp = origin + 2;
  unsigned slowIp = takeSideIp(context);
  ir::Value* slow = c->promiseConstant(c->machineIp(slowIp), ir::Type::iptr());

  ir::Value* cached
      = c->load(ir::ExtendMode::Signed,
                c->memory(string, ir::Type::i4(), StringHashCode),
                ir::Type::i4());
  c->condJump(lir::JumpIfEqual, c->constant(0, ir::Type::i4()), cached, slow);
  Compiler::State* edge = c->saveState();
  c->startLogicalIp(fastIp);

  ir::Value* cachedNow
      = c->load(ir::ExtendMode::Signed,
                c->memory(string, ir::Type::i4(), StringHashCode),
                ir::Type::i4());
  c->store(cachedNow,
           c->memory(c->threadRegister(), ir::Type::i4(), AllocationResultOffset));

  c->startLogicalIp(contIp);
  frame->pop(ir::Type::object());
  frame->push(ir::Type::i4(),
              c->load(ir::ExtendMode::Signed,
                      c->memory(c->threadRegister(),
                                ir::Type::i4(),
                                AllocationResultOffset),
                      ir::Type::i4()));

  StringSlowPath* path
      = new (context->zone.allocate(sizeof(StringSlowPath))) StringSlowPath;
  path->edge = edge;
  path->slowIp = slowIp;
  path->contIp = contIp;
  path->trace = trace;
  path->string = string;
  path->index = 0;
  path->hashCode = true;
  queueStringSlowPath(context, path);
  return true;
}

#endif  // 64-bit avian string

bool tryStringIntrinsic(MyThread* t, Frame* frame, GcMethod* target)
{
#ifdef HAVE_StringUnsafe_data
  if (not stringInvokeVirtual(frame)) {
    return false;
  }
  GcByteArray* className = target->class_()->name();
  if (not stringNameIs(className, "java/lang/String")) {
    return false;
  }

  Compiler* c = frame->c;
  GcByteArray* name = target->name();
  GcByteArray* spec = target->spec();
  if (stringNameIs(name, "length") and stringNameIs(spec, "()I")) {
    ir::Value* self = frame->pop(ir::Type::object());
    frame->push(ir::Type::i4(),
                c->load(ir::ExtendMode::Signed,
                        c->memory(self, ir::Type::i4(), StringLength),
                        ir::Type::i4()));
    return true;
  }
  if (stringNameIs(name, "isEmpty") and stringNameIs(spec, "()Z")) {
    ir::Value* self = frame->pop(ir::Type::object());
    ir::Value* len = c->load(ir::ExtendMode::Signed,
                             c->memory(self, ir::Type::i4(), StringLength),
                             ir::Type::i4());
    // (len | -len) >> 31 is 0 for zero and -1 otherwise. Adding 1 makes
    // the boolean.
    ir::Value* negative = c->binaryOp(
        lir::Subtract, ir::Type::i4(), len, c->constant(0, ir::Type::i4()));
    ir::Value* bits = c->binaryOp(lir::Or, ir::Type::i4(), len, negative);
    ir::Value* sign = c->binaryOp(lir::ShiftRight,
                                  ir::Type::i4(),
                                  c->constant(31, ir::Type::i4()),
                                  bits);
    frame->push(ir::Type::i4(),
                c->binaryOp(lir::Add,
                            ir::Type::i4(),
                            c->constant(1, ir::Type::i4()),
                            sign));
    return true;
  }
  if (stringNameIs(name, "equals") and stringNameIs(spec, "(Ljava/lang/Object;)Z")) {
    ir::Value* other = c->peek(1, 0);
    ir::Value* self = c->peek(1, 1);
    ir::Value* result = c->nativeCall(
        c->constant(getThunk(t, stringEqualsThunk), ir::Type::iptr()),
        0,
        frame->trace(0, 0),
        ir::Type::i4(),
        args(c->threadRegister(), self, other));
    frame->pop(ir::Type::object());
    frame->pop(ir::Type::object());
    frame->push(ir::Type::i4(), result);
    return true;
  }
#if TARGET_BYTES_PER_WORD == 8
  if (stringNameIs(name, "charAt") and stringNameIs(spec, "(I)C")) {
    return tryStringCharAt(t, frame);
  }
  if (stringNameIs(name, "hashCode") and stringNameIs(spec, "()I")) {
    return tryStringHashCode(t, frame);
  }
#endif
  return false;
#else
  (void)t;
  (void)frame;
  (void)target;
  return false;
#endif
}

void flushStringIntrinsics(MyThread* t, Context* context)
{
#if defined(HAVE_StringUnsafe_data) && TARGET_BYTES_PER_WORD == 8
  Compiler* c = context->compiler;
  for (StringSlowPath* path = context->stringSlowPaths; path; path = path->next) {
    c->restoreState(path->edge);
    c->startLogicalIp(path->slowIp);

    ir::Value* result;
    if (path->hashCode) {
      result = c->nativeCall(
          c->constant(getThunk(t, stringHashCodeSlowThunk), ir::Type::iptr()),
          0,
          path->trace,
          ir::Type::i4(),
          args(c->threadRegister(), path->string));
    } else {
      result = c->nativeCall(
          c->constant(getThunk(t, stringCharAtSlowThunk), ir::Type::iptr()),
          0,
          path->trace,
          ir::Type::i4(),
          args(c->threadRegister(), path->string, path->index));
    }
    c->store(result,
             c->memory(
                 c->threadRegister(), ir::Type::i4(), AllocationResultOffset));
    c->jmp(c->promiseConstant(c->machineIp(path->contIp), ir::Type::iptr()));
    c->visitLogicalIp(path->contIp);
  }
#else
  (void)t;
  (void)context;
#endif
}

unsigned stringSlowPathLineCount(Context* context)
{
#if defined(HAVE_StringUnsafe_data) && TARGET_BYTES_PER_WORD == 8
  unsigned count = 0;
  for (StringSlowPath* path = context->stringSlowPaths; path;
       path = path->next) {
    ++count;
  }
  return count;
#else
  (void)context;
  return 0;
#endif
}

// The out-of-line calls sit after the method body. Give them the
// invoke's line, or the trace reports the method's last line.
unsigned appendStringSlowPathLines(MyThread* t UNUSED,
                                   Context* context,
                                   uint64_t* table,
                                   unsigned count,
                                   intptr_t start)
{
#if defined(HAVE_StringUnsafe_data) && TARGET_BYTES_PER_WORD == 8
  GcLineNumberTable* oldTable = context->method->code()->lineNumberTable();
  for (StringSlowPath* path = context->stringSlowPaths; path;
       path = path->next) {
    Promise* where = context->compiler->machineIp(path->slowIp);
    if (where == 0 or not where->resolved()) {
      continue;
    }
    unsigned source = path->trace ? path->trace->ip : 0;
    unsigned line = 0;
    if (oldTable) {
      for (unsigned i = 0; i < oldTable->length(); ++i) {
        uint64_t entry = oldTable->body()[i];
        if (lineNumberIp(entry) > source) {
          break;
        }
        line = lineNumberLine(entry);
      }
    }
    intptr_t ip = where->value() - start;
    if (ip < 0) {
      continue;
    }
    if (count
        and lineNumberIp(table[count - 1]) > static_cast<unsigned>(ip)) {
      continue;
    }
    table[count++] = lineNumber(static_cast<uint64_t>(ip), line);
  }
  context->stringSlowPaths = 0;
  context->stringSlowPathTail = 0;
#else
  (void)context;
  (void)table;
  (void)start;
#endif
  return count;
}

