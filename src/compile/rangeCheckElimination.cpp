/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "compile/rangeCheckElimination.h"

#include "avian/machine.h"
#include "avian/zone.h"

#include <string.h>

namespace vm {

namespace {

// RangeCheckElimination::eliminate is the only entry the compiler calls.
// RangeCheckEliminator owns the pass. The helpers are the predicate: the
// control-flow graph, the stack tokens, and the counted-loop shape. A
// bytecode is marked only when the proof on RangeCheckElimination holds
// for every execution.

// --- control-flow graph ---

struct RceEdge {
  uint16_t from;
  uint16_t to;
};

struct RceHandler {
  uint16_t start;
  uint16_t end;
  uint16_t ip;
};

struct RceCfg {
  const uint8_t* code;
  unsigned length;
  unsigned maxLocals;
  uint16_t* streamNext;
  uint16_t* branch;
  uint8_t* falls;
  uint16_t* predCount;
  uint16_t* pred0;
  uint16_t* pred1;
  RceEdge* edges;
  unsigned edgeCount;
  RceHandler* handlers;
  unsigned handlerCount;
  uint8_t* seen;
  uint16_t* work;
};

const unsigned RceNoTarget = 0xFFFF;
const unsigned RceFall = 0;
const unsigned RceFallBranch = 1;
const unsigned RceBranch = 2;
const unsigned RceTerm = 3;
const unsigned RceSwitch = 4;

const uint8_t RceUnk = 0;
const uint8_t RceInt = 1;
const uint8_t RceObj = 2;
const uint8_t RceC2L = 3;
const uint8_t RceC2H = 4;

struct RceSlot {
  uint8_t kind;
  uint16_t local;
};

struct RceStack {
  RceSlot* slot;
  unsigned sp;
  unsigned cap;
};

// --- bytecode readers ---

unsigned rceReadU16(const uint8_t* code, unsigned ip)
{
  return (unsigned(code[ip]) << 8) | unsigned(code[ip + 1]);
}

int rceReadS16(const uint8_t* code, unsigned ip)
{
  return static_cast<int16_t>(rceReadU16(code, ip));
}

int rceReadS32(const uint8_t* code, unsigned ip)
{
  uint32_t v = (uint32_t(code[ip]) << 24) | (uint32_t(code[ip + 1]) << 16)
               | (uint32_t(code[ip + 2]) << 8) | uint32_t(code[ip + 3]);
  return static_cast<int32_t>(v);
}

bool rceTarget(unsigned ip, int offset, unsigned length, unsigned* target)
{
  int64_t t = static_cast<int64_t>(ip) + offset;
  if (t < 0 or t >= static_cast<int64_t>(length)) {
    return false;
  }
  *target = static_cast<unsigned>(t);
  return true;
}

// --- local and constant patterns ---

bool rceIsIload(const uint8_t* code, unsigned length, unsigned ip, unsigned* local)
{
  if (ip >= length) {
    return false;
  }
  unsigned op = code[ip];
  if (op >= iload_0 and op <= iload_3) {
    *local = op - iload_0;
    return true;
  }
  if (op == iload and ip + 2 <= length) {
    *local = code[ip + 1];
    return true;
  }
  if (op == wide and ip + 4 <= length and code[ip + 1] == iload) {
    *local = rceReadU16(code, ip + 2);
    return true;
  }
  return false;
}

bool rceIsAload(const uint8_t* code, unsigned length, unsigned ip, unsigned* local)
{
  if (ip >= length) {
    return false;
  }
  unsigned op = code[ip];
  if (op >= aload_0 and op <= aload_3) {
    *local = op - aload_0;
    return true;
  }
  if (op == aload and ip + 2 <= length) {
    *local = code[ip + 1];
    return true;
  }
  if (op == wide and ip + 4 <= length and code[ip + 1] == aload) {
    *local = rceReadU16(code, ip + 2);
    return true;
  }
  return false;
}

bool rceIsIstore(const uint8_t* code, unsigned length, unsigned ip, unsigned* local)
{
  if (ip >= length) {
    return false;
  }
  unsigned op = code[ip];
  if (op >= istore_0 and op <= istore_3) {
    *local = op - istore_0;
    return true;
  }
  if (op == istore and ip + 2 <= length) {
    *local = code[ip + 1];
    return true;
  }
  if (op == wide and ip + 4 <= length and code[ip + 1] == istore) {
    *local = rceReadU16(code, ip + 2);
    return true;
  }
  return false;
}

bool rceIsIinc(const uint8_t* code,
               unsigned length,
               unsigned ip,
               unsigned* local,
               int* delta)
{
  if (ip >= length) {
    return false;
  }
  if (code[ip] == iinc and ip + 3 <= length) {
    *local = code[ip + 1];
    *delta = static_cast<int8_t>(code[ip + 2]);
    return true;
  }
  if (code[ip] == wide and ip + 6 <= length and code[ip + 1] == iinc) {
    *local = rceReadU16(code, ip + 2);
    *delta = rceReadS16(code, ip + 4);
    return true;
  }
  return false;
}

bool rceNonNegConst(const uint8_t* code, unsigned length, unsigned ip)
{
  if (ip >= length) {
    return false;
  }
  unsigned op = code[ip];
  if (op >= iconst_0 and op <= iconst_5) {
    return true;
  }
  if (op == bipush and ip + 2 <= length) {
    return static_cast<int8_t>(code[ip + 1]) >= 0;
  }
  if (op == sipush and ip + 3 <= length) {
    return rceReadS16(code, ip + 1) >= 0;
  }
  return false;
}

// Writes of a local. Long and double stores occupy two slots. iinc counts
// as a write because it changes the value the header compared.
bool rceMods(const uint8_t* code,
             unsigned length,
             unsigned ip,
             unsigned* slot,
             unsigned* count)
{
  if (ip >= length) {
    return false;
  }
  unsigned op = code[ip];
  unsigned local = 0;
  unsigned n = 1;
  bool isWide = false;
  if (op == wide) {
    if (ip + 4 > length) {
      return false;
    }
    isWide = true;
    op = code[ip + 1];
    local = rceReadU16(code, ip + 2);
    if (op == iinc and ip + 6 > length) {
      return false;
    }
  }

  if (not isWide) {
    if (op >= istore_0 and op <= istore_3) {
      local = op - istore_0;
    } else if (op >= lstore_0 and op <= lstore_3) {
      local = op - lstore_0;
      n = 2;
    } else if (op >= fstore_0 and op <= fstore_3) {
      local = op - fstore_0;
    } else if (op >= dstore_0 and op <= dstore_3) {
      local = op - dstore_0;
      n = 2;
    } else if (op >= astore_0 and op <= astore_3) {
      local = op - astore_0;
    } else if (op == istore or op == fstore or op == astore) {
      if (ip + 2 > length) {
        return false;
      }
      local = code[ip + 1];
    } else if (op == lstore or op == dstore) {
      if (ip + 2 > length) {
        return false;
      }
      local = code[ip + 1];
      n = 2;
    } else if (op == iinc) {
      if (ip + 3 > length) {
        return false;
      }
      local = code[ip + 1];
    } else {
      return false;
    }
  } else if (op == lstore or op == dstore) {
    n = 2;
  } else if (op != istore and op != fstore and op != astore and op != iinc) {
    return false;
  }

  *slot = local;
  *count = n;
  return true;
}

bool rceOverlap(unsigned slot, unsigned count, unsigned local)
{
  return local < slot + count and slot < local + 1;
}

unsigned rcePrev(const uint16_t* streamNext, unsigned length, unsigned ip)
{
  for (unsigned p = 0; p < ip; ++p) {
    if (streamNext[p] == ip) {
      return p;
    }
  }
  return length;
}

// --- instruction length and decode ---

bool rceSimpleLength(unsigned op, unsigned* len)
{
  switch (op) {
  case nop:
  case aconst_null:
  case iconst_m1:
  case iconst_0:
  case iconst_1:
  case iconst_2:
  case iconst_3:
  case iconst_4:
  case iconst_5:
  case lconst_0:
  case lconst_1:
  case fconst_0:
  case fconst_1:
  case fconst_2:
  case dconst_0:
  case dconst_1:
  case iload_0:
  case iload_1:
  case iload_2:
  case iload_3:
  case lload_0:
  case lload_1:
  case lload_2:
  case lload_3:
  case fload_0:
  case fload_1:
  case fload_2:
  case fload_3:
  case dload_0:
  case dload_1:
  case dload_2:
  case dload_3:
  case aload_0:
  case aload_1:
  case aload_2:
  case aload_3:
  case iaload:
  case laload:
  case faload:
  case daload:
  case aaload:
  case baload:
  case caload:
  case saload:
  case istore_0:
  case istore_1:
  case istore_2:
  case istore_3:
  case lstore_0:
  case lstore_1:
  case lstore_2:
  case lstore_3:
  case fstore_0:
  case fstore_1:
  case fstore_2:
  case fstore_3:
  case dstore_0:
  case dstore_1:
  case dstore_2:
  case dstore_3:
  case astore_0:
  case astore_1:
  case astore_2:
  case astore_3:
  case iastore:
  case lastore:
  case fastore:
  case dastore:
  case aastore:
  case bastore:
  case castore:
  case sastore:
  case pop_:
  case pop2:
  case dup:
  case dup_x1:
  case dup_x2:
  case dup2:
  case dup2_x1:
  case dup2_x2:
  case swap:
  case iadd:
  case ladd:
  case fadd:
  case dadd:
  case isub:
  case lsub:
  case fsub:
  case dsub:
  case imul:
  case lmul:
  case fmul:
  case dmul:
  case idiv:
  case ldiv_:
  case fdiv:
  case ddiv:
  case irem:
  case lrem:
  case frem:
  case drem:
  case ineg:
  case lneg:
  case fneg:
  case dneg:
  case ishl:
  case lshl:
  case ishr:
  case lshr:
  case iushr:
  case lushr:
  case iand:
  case land:
  case ior:
  case lor:
  case ixor:
  case lxor:
  case i2l:
  case i2f:
  case i2d:
  case l2i:
  case l2f:
  case l2d:
  case f2i:
  case f2l:
  case f2d:
  case d2i:
  case d2l:
  case d2f:
  case i2b:
  case i2c:
  case i2s:
  case lcmp:
  case fcmpl:
  case fcmpg:
  case dcmpl:
  case dcmpg:
  case ireturn:
  case lreturn:
  case freturn:
  case dreturn:
  case areturn:
  case return_:
  case arraylength:
  case athrow:
  case monitorenter:
  case monitorexit:
    *len = 1;
    return true;

  case bipush:
  case ldc:
  case iload:
  case lload:
  case fload:
  case dload:
  case aload:
  case istore:
  case lstore:
  case fstore:
  case dstore:
  case astore:
  case ret:
  case newarray:
    *len = 2;
    return true;

  case sipush:
  case ldc_w:
  case ldc2_w:
  case iinc:
  case ifeq:
  case ifne:
  case iflt:
  case ifge:
  case ifgt:
  case ifle:
  case if_icmpeq:
  case if_icmpne:
  case if_icmplt:
  case if_icmpge:
  case if_icmpgt:
  case if_icmple:
  case if_acmpeq:
  case if_acmpne:
  case goto_:
  case jsr:
  case getstatic:
  case putstatic:
  case getfield:
  case putfield:
  case invokevirtual:
  case invokespecial:
  case invokestatic:
  case new_:
  case anewarray:
  case checkcast:
  case instanceof:
  case ifnull:
  case ifnonnull:
    *len = 3;
    return true;

  case multianewarray:
    *len = 4;
    return true;

  case invokeinterface:
  case invokedynamic:
  case goto_w:
  case jsr_w:
    *len = 5;
    return true;

  default:
    return false;
  }
}

bool rceIsBranch(unsigned op)
{
  switch (op) {
  case ifeq:
  case ifne:
  case iflt:
  case ifge:
  case ifgt:
  case ifle:
  case if_icmpeq:
  case if_icmpne:
  case if_icmplt:
  case if_icmpge:
  case if_icmpgt:
  case if_icmple:
  case if_acmpeq:
  case if_acmpne:
  case ifnull:
  case ifnonnull:
  case goto_:
  case goto_w:
    return true;
  default:
    return false;
  }
}

bool rceIsTerminal(unsigned op)
{
  switch (op) {
  case ireturn:
  case lreturn:
  case freturn:
  case dreturn:
  case areturn:
  case return_:
  case athrow:
    return true;
  default:
    return false;
  }
}

// Fills targets when the pointer is non-null. targetCount includes default.
bool rceSwitch(const uint8_t* code,
               unsigned length,
               unsigned ip,
               unsigned* end,
               unsigned* targetCount,
               unsigned* targets)
{
  if (ip >= length) {
    return false;
  }
  unsigned base = (ip + 4) & ~3u;
  if (base < ip or base + 8 > length) {
    return false;
  }
  int def = rceReadS32(code, base);
  unsigned defTarget = 0;
  if (not rceTarget(ip, def, length, &defTarget)) {
    return false;
  }

  if (code[ip] == tableswitch) {
    if (base + 12 > length) {
      return false;
    }
    int low = rceReadS32(code, base + 4);
    int high = rceReadS32(code, base + 8);
    if (high < low) {
      return false;
    }
    uint64_t n = static_cast<uint64_t>(static_cast<int64_t>(high) - low) + 1;
    uint64_t bytes = 12 + n * 4;
    if (n > length or static_cast<uint64_t>(base) + bytes > length) {
      return false;
    }
    *targetCount = static_cast<unsigned>(n) + 1;
    *end = base + static_cast<unsigned>(bytes);
    if (targets) {
      targets[0] = defTarget;
      for (unsigned i = 0; i < static_cast<unsigned>(n); ++i) {
        int off = rceReadS32(code, base + 12 + i * 4);
        if (not rceTarget(ip, off, length, targets + i + 1)) {
          return false;
        }
      }
    }
    return true;
  }

  if (code[ip] == lookupswitch) {
    int npairs = rceReadS32(code, base + 4);
    if (npairs < 0) {
      return false;
    }
    uint64_t bytes = 8 + static_cast<uint64_t>(npairs) * 8;
    if (static_cast<uint64_t>(npairs) > length
        or static_cast<uint64_t>(base) + bytes > length) {
      return false;
    }
    *targetCount = static_cast<unsigned>(npairs) + 1;
    *end = base + static_cast<unsigned>(bytes);
    if (targets) {
      targets[0] = defTarget;
      for (int i = 0; i < npairs; ++i) {
        int off = rceReadS32(code, base + 8 + static_cast<unsigned>(i) * 8 + 4);
        if (not rceTarget(ip, off, length, targets + i + 1)) {
          return false;
        }
      }
    }
    return true;
  }

  return false;
}

// False means the method cannot be analyzed safely (malformed, or jsr/ret,
// whose successors are not explicit).
bool rceDecode(const uint8_t* code,
               unsigned length,
               unsigned ip,
               unsigned* next,
               unsigned* target,
               unsigned* kind,
               unsigned* switchCount)
{
  *target = 0;
  *kind = RceFall;
  *switchCount = 0;
  if (ip >= length) {
    return false;
  }
  unsigned op = code[ip];

  if (op == jsr or op == jsr_w or op == ret) {
    return false;
  }

  if (op == wide) {
    if (ip + 2 > length) {
      return false;
    }
    unsigned wop = code[ip + 1];
    if (wop == ret) {
      return false;
    }
    unsigned len = (wop == iinc) ? 6 : 4;
    if (wop != iinc and wop != iload and wop != fload and wop != aload
        and wop != lload and wop != dload and wop != istore and wop != fstore
        and wop != astore and wop != lstore and wop != dstore) {
      return false;
    }
    if (ip + len > length) {
      return false;
    }
    *next = ip + len;
    return true;
  }

  if (op == tableswitch or op == lookupswitch) {
    unsigned end = 0;
    unsigned count = 0;
    if (not rceSwitch(code, length, ip, &end, &count, 0)) {
      return false;
    }
    if (end <= ip) {
      return false;
    }
    *next = end;
    *kind = RceSwitch;
    *switchCount = count;
    return true;
  }

  unsigned len = 0;
  if (not rceSimpleLength(op, &len) or ip + len > length) {
    return false;
  }
  *next = ip + len;

  if (rceIsBranch(op)) {
    int offset;
    if (op == goto_w) {
      offset = rceReadS32(code, ip + 1);
    } else {
      offset = rceReadS16(code, ip + 1);
    }
    unsigned to = 0;
    if (not rceTarget(ip, offset, length, &to)) {
      return false;
    }
    *target = to;
    *kind = (op == goto_ or op == goto_w) ? RceBranch : RceFallBranch;
    return true;
  }

  if (rceIsTerminal(op)) {
    *kind = RceTerm;
  }
  return true;
}

// --- predecessors and reachability ---

void rceAddPred(RceCfg* cfg, unsigned to, unsigned from)
{
  if (cfg->predCount[to] == 0) {
    cfg->pred0[to] = static_cast<uint16_t>(from);
  } else if (cfg->predCount[to] == 1) {
    cfg->pred1[to] = static_cast<uint16_t>(from);
  }
  if (cfg->predCount[to] != 0xFFFF) {
    ++cfg->predCount[to];
  }
}

bool rcePred(RceCfg* cfg, unsigned to, unsigned from)
{
  if (to >= cfg->length or cfg->streamNext[to] == 0) {
    return false;
  }
  rceAddPred(cfg, to, from);
  return true;
}

void rceEnq(RceCfg* cfg, unsigned* qw, unsigned ip)
{
  if (ip >= cfg->length or cfg->seen[ip]) {
    return;
  }
  cfg->seen[ip] = 1;
  cfg->work[(*qw)++] = static_cast<uint16_t>(ip);
}

void rceReach(RceCfg* cfg, unsigned start, unsigned block)
{
  memset(cfg->seen, 0, cfg->length);
  unsigned qr = 0;
  unsigned qw = 0;
  if (start >= cfg->length) {
    return;
  }
  cfg->seen[start] = 1;
  cfg->work[qw++] = static_cast<uint16_t>(start);
  while (qr < qw) {
    unsigned ip = cfg->work[qr++];
    if (ip == block) {
      continue;
    }
    if (cfg->falls[ip] and cfg->streamNext[ip] < cfg->length) {
      rceEnq(cfg, &qw, cfg->streamNext[ip]);
    }
    if (cfg->branch[ip] != RceNoTarget) {
      rceEnq(cfg, &qw, cfg->branch[ip]);
    }
    for (unsigned e = 0; e < cfg->edgeCount; ++e) {
      if (cfg->edges[e].from == ip) {
        rceEnq(cfg, &qw, cfg->edges[e].to);
      }
    }
    for (unsigned h = 0; h < cfg->handlerCount; ++h) {
      if (ip >= cfg->handlers[h].start and ip < cfg->handlers[h].end) {
        rceEnq(cfg, &qw, cfg->handlers[h].ip);
      }
    }
  }
}

// --- operand stack ---

bool rcePush(RceStack* st, uint8_t kind, unsigned local)
{
  if (st->sp >= st->cap) {
    return false;
  }
  st->slot[st->sp].kind = kind;
  st->slot[st->sp].local = static_cast<uint16_t>(local);
  ++st->sp;
  return true;
}

bool rcePushUnk(RceStack* st)
{
  return rcePush(st, RceUnk, 0);
}

bool rcePushCat2(RceStack* st)
{
  return rcePush(st, RceC2L, 0) and rcePush(st, RceC2H, 0);
}

bool rcePop1(RceStack* st, RceSlot* out)
{
  if (st->sp == 0) {
    return false;
  }
  RceSlot top = st->slot[st->sp - 1];
  if (top.kind == RceC2L or top.kind == RceC2H) {
    return false;
  }
  --st->sp;
  *out = top;
  return true;
}

bool rcePopCat2(RceStack* st)
{
  if (st->sp < 2) {
    return false;
  }
  if (st->slot[st->sp - 1].kind != RceC2H
      or st->slot[st->sp - 2].kind != RceC2L) {
    return false;
  }
  st->sp -= 2;
  return true;
}

bool rcePop2Instr(RceStack* st)
{
  if (st->sp == 0) {
    return false;
  }
  if (st->slot[st->sp - 1].kind == RceC2H) {
    return rcePopCat2(st);
  }
  if (st->slot[st->sp - 1].kind == RceC2L or st->sp < 2) {
    return false;
  }
  if (st->slot[st->sp - 2].kind == RceC2L
      or st->slot[st->sp - 2].kind == RceC2H) {
    return false;
  }
  st->sp -= 2;
  return true;
}

bool rceDup2(RceStack* st)
{
  if (st->sp >= 2 and st->slot[st->sp - 1].kind == RceC2H
      and st->slot[st->sp - 2].kind == RceC2L) {
    return rcePushCat2(st);
  }
  if (st->sp < 2) {
    return false;
  }
  RceSlot b = st->slot[st->sp - 1];
  RceSlot a = st->slot[st->sp - 2];
  if (a.kind == RceC2L or a.kind == RceC2H or b.kind == RceC2L
      or b.kind == RceC2H) {
    return false;
  }
  return rcePush(st, a.kind, a.local) and rcePush(st, b.kind, b.local);
}

bool rceMatch(RceSlot array, RceSlot index, unsigned arrLocal, unsigned iv)
{
  return array.kind == RceObj and array.local == arrLocal and index.kind == RceInt
         and index.local == iv;
}

bool rceNote(unsigned ip, unsigned* marks, unsigned* markCount, unsigned markCap)
{
  if (*markCount >= markCap) {
    return false;
  }
  marks[(*markCount)++] = ip;
  return true;
}

bool rceArrayLoad(RceStack* st,
                  unsigned ip,
                  bool cat2,
                  unsigned arrLocal,
                  unsigned iv,
                  unsigned* marks,
                  unsigned* markCount,
                  unsigned markCap)
{
  RceSlot index;
  RceSlot array;
  if (not rcePop1(st, &index) or not rcePop1(st, &array)) {
    return false;
  }
  if (rceMatch(array, index, arrLocal, iv)
      and not rceNote(ip, marks, markCount, markCap)) {
    return false;
  }
  if (cat2) {
    return rcePushCat2(st);
  }
  return rcePushUnk(st);
}

bool rceArrayStore(RceStack* st,
                   unsigned ip,
                   bool cat2,
                   unsigned arrLocal,
                   unsigned iv,
                   unsigned* marks,
                   unsigned* markCount,
                   unsigned markCap)
{
  if (cat2) {
    if (not rcePopCat2(st)) {
      return false;
    }
  } else {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
  }
  RceSlot index;
  RceSlot array;
  if (not rcePop1(st, &index) or not rcePop1(st, &array)) {
    return false;
  }
  if (rceMatch(array, index, arrLocal, iv)
      and not rceNote(ip, marks, markCount, markCap)) {
    return false;
  }
  return true;
}

bool rceExec(const uint8_t* code,
             unsigned length,
             unsigned ip,
             RceStack* st,
             unsigned arrLocal,
             unsigned iv,
             unsigned* marks,
             unsigned* markCount,
             unsigned markCap)
{
  unsigned op = code[ip];
  if (op == wide) {
    unsigned wop = code[ip + 1];
    unsigned local = rceReadU16(code, ip + 2);
    if (wop == iload) {
      return rcePush(st, RceInt, local);
    }
    if (wop == aload) {
      return rcePush(st, RceObj, local);
    }
    if (wop == fload) {
      return rcePushUnk(st);
    }
    if (wop == lload or wop == dload) {
      return rcePushCat2(st);
    }
    if (wop == iinc) {
      return true;
    }
    if (wop == istore or wop == fstore or wop == astore) {
      RceSlot value;
      return rcePop1(st, &value);
    }
    if (wop == lstore or wop == dstore) {
      return rcePopCat2(st);
    }
    return false;
  }

  unsigned local = 0;
  if (rceIsIload(code, length, ip, &local)) {
    return rcePush(st, RceInt, local);
  }
  if (rceIsAload(code, length, ip, &local)) {
    return rcePush(st, RceObj, local);
  }

  switch (op) {
  case nop:
  case iinc:
    return true;

  case aconst_null:
  case iconst_m1:
  case iconst_0:
  case iconst_1:
  case iconst_2:
  case iconst_3:
  case iconst_4:
  case iconst_5:
  case fconst_0:
  case fconst_1:
  case fconst_2:
  case bipush:
  case sipush:
  case ldc:
  case ldc_w:
  case fload:
  case fload_0:
  case fload_1:
  case fload_2:
  case fload_3:
    return rcePushUnk(st);

  case lconst_0:
  case lconst_1:
  case dconst_0:
  case dconst_1:
  case ldc2_w:
  case lload:
  case lload_0:
  case lload_1:
  case lload_2:
  case lload_3:
  case dload:
  case dload_0:
  case dload_1:
  case dload_2:
  case dload_3:
    return rcePushCat2(st);

  case istore:
  case istore_0:
  case istore_1:
  case istore_2:
  case istore_3:
  case fstore:
  case fstore_0:
  case fstore_1:
  case fstore_2:
  case fstore_3:
  case astore:
  case astore_0:
  case astore_1:
  case astore_2:
  case astore_3: {
    RceSlot value;
    return rcePop1(st, &value);
  }

  case lstore:
  case lstore_0:
  case lstore_1:
  case lstore_2:
  case lstore_3:
  case dstore:
  case dstore_0:
  case dstore_1:
  case dstore_2:
  case dstore_3:
    return rcePopCat2(st);

  case iaload:
  case faload:
  case aaload:
  case baload:
  case caload:
  case saload:
    return rceArrayLoad(
        st, ip, false, arrLocal, iv, marks, markCount, markCap);

  case laload:
  case daload:
    return rceArrayLoad(
        st, ip, true, arrLocal, iv, marks, markCount, markCap);

  case iastore:
  case fastore:
  case aastore:
  case bastore:
  case castore:
  case sastore:
    return rceArrayStore(
        st, ip, false, arrLocal, iv, marks, markCount, markCap);

  case lastore:
  case dastore:
    return rceArrayStore(
        st, ip, true, arrLocal, iv, marks, markCount, markCap);

  case pop_: {
    RceSlot value;
    return rcePop1(st, &value);
  }

  case pop2:
    return rcePop2Instr(st);

  case dup: {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
    return rcePush(st, value.kind, value.local)
           and rcePush(st, value.kind, value.local);
  }

  case dup_x1: {
    RceSlot b;
    RceSlot a;
    if (not rcePop1(st, &b) or not rcePop1(st, &a)) {
      return false;
    }
    return rcePush(st, b.kind, b.local) and rcePush(st, a.kind, a.local)
           and rcePush(st, b.kind, b.local);
  }

  case dup2:
    return rceDup2(st);

  case swap: {
    if (st->sp < 2) {
      return false;
    }
    RceSlot b = st->slot[st->sp - 1];
    RceSlot a = st->slot[st->sp - 2];
    if (a.kind == RceC2L or a.kind == RceC2H or b.kind == RceC2L
        or b.kind == RceC2H) {
      return false;
    }
    st->slot[st->sp - 1] = a;
    st->slot[st->sp - 2] = b;
    return true;
  }

  case iadd:
  case isub:
  case imul:
  case idiv:
  case irem:
  case ishl:
  case ishr:
  case iushr:
  case iand:
  case ior:
  case ixor:
  case fadd:
  case fsub:
  case fmul:
  case fdiv:
  case frem:
  case fcmpl:
  case fcmpg: {
    RceSlot a;
    RceSlot b;
    if (not rcePop1(st, &a) or not rcePop1(st, &b)) {
      return false;
    }
    return rcePushUnk(st);
  }

  case ladd:
  case lsub:
  case lmul:
  case ldiv_:
  case lrem:
  case land:
  case lor:
  case lxor:
  case lcmp:
  case dadd:
  case dsub:
  case dmul:
  case ddiv:
  case drem:
  case dcmpl:
  case dcmpg:
    if (not rcePopCat2(st) or not rcePopCat2(st)) {
      return false;
    }
    if (op == lcmp or op == dcmpl or op == dcmpg) {
      return rcePushUnk(st);
    }
    return rcePushCat2(st);

  case lshl:
  case lshr:
  case lushr: {
    RceSlot shift;
    if (not rcePop1(st, &shift) or not rcePopCat2(st)) {
      return false;
    }
    return rcePushCat2(st);
  }

  case ineg:
  case fneg: {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
    return rcePushUnk(st);
  }

  case lneg:
  case dneg:
    if (not rcePopCat2(st)) {
      return false;
    }
    return rcePushCat2(st);

  case i2l:
  case i2d:
  case f2l:
  case f2d: {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
    return rcePushCat2(st);
  }

  case i2f:
  case i2b:
  case i2c:
  case i2s:
  case f2i: {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
    return rcePushUnk(st);
  }

  case l2i:
  case l2f:
  case d2i:
  case d2f:
    if (not rcePopCat2(st)) {
      return false;
    }
    return rcePushUnk(st);

  case l2d:
  case d2l:
    if (not rcePopCat2(st)) {
      return false;
    }
    return rcePushCat2(st);

  case arraylength:
  case instanceof:
  case newarray:
  case anewarray: {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
    return rcePushUnk(st);
  }

  case checkcast: {
    RceSlot value;
    if (not rcePop1(st, &value)) {
      return false;
    }
    return rcePush(st, value.kind, value.local);
  }

  case new_:
    return rcePushUnk(st);

  case monitorenter:
  case monitorexit: {
    RceSlot value;
    return rcePop1(st, &value);
  }

  case multianewarray: {
    if (ip + 4 > length) {
      return false;
    }
    unsigned dims = code[ip + 3];
    if (dims == 0) {
      return false;
    }
    for (unsigned d = 0; d < dims; ++d) {
      RceSlot value;
      if (not rcePop1(st, &value)) {
        return false;
      }
    }
    return rcePushUnk(st);
  }

  default:
    return false;
  }
}

// --- counted-loop predicate ---

bool rceSeenMods(RceCfg* cfg, unsigned local)
{
  for (unsigned ip = 0; ip < cfg->length; ++ip) {
    if (not cfg->seen[ip] or cfg->streamNext[ip] == 0) {
      continue;
    }
    unsigned slot;
    unsigned count;
    if (rceMods(cfg->code, cfg->length, ip, &slot, &count)
        and rceOverlap(slot, count, local)) {
      return true;
    }
  }
  return false;
}


bool rceTryLoop(RceCfg* cfg,
                RceStack* st,
                unsigned* marks,
                unsigned* markCount,
                unsigned markCap,
                uint8_t* safe,
                unsigned gotoIp,
                unsigned header,
                uint16_t* ivAtHeader)
{
  const uint8_t* code = cfg->code;
  unsigned length = cfg->length;
  if (header >= gotoIp or cfg->streamNext[header] == 0 or cfg->falls[gotoIp]) {
    return false;
  }
  unsigned loopEnd = cfg->streamNext[gotoIp];
  if (cfg->branch[gotoIp] != header) {
    return false;
  }

  unsigned iv = 0;
  if (not rceIsIload(code, length, header, &iv) or iv >= cfg->maxLocals) {
    return false;
  }

  unsigned cursor = cfg->streamNext[header];
  if (cursor >= length) {
    return false;
  }

  bool haveLim = false;
  bool haveArr = false;
  unsigned limLocal = 0;
  unsigned arrLocal = 0;
  unsigned limTmp = 0;
  unsigned arrTmp = 0;
  if (rceIsIload(code, length, cursor, &limTmp)) {
    if (limTmp >= cfg->maxLocals) {
      return false;
    }
    haveLim = true;
    limLocal = limTmp;
    cursor = cfg->streamNext[cursor];
  } else if (rceIsAload(code, length, cursor, &arrTmp)) {
    if (arrTmp >= cfg->maxLocals) {
      return false;
    }
    cursor = cfg->streamNext[cursor];
    if (cursor >= length or code[cursor] != arraylength) {
      return false;
    }
    haveArr = true;
    arrLocal = arrTmp;
    cursor = cfg->streamNext[cursor];
  } else {
    return false;
  }

  if (cursor >= length or code[cursor] != if_icmpge or cfg->branch[cursor] == RceNoTarget
      or not cfg->falls[cursor]) {
    return false;
  }
  unsigned exitTarget = cfg->branch[cursor];
  unsigned body = cfg->streamNext[cursor];
  if (exitTarget >= header and exitTarget < loopEnd) {
    return false;
  }

  unsigned iincIp = rcePrev(cfg->streamNext, length, gotoIp);
  if (iincIp >= length or iincIp <= header or iincIp >= gotoIp) {
    return false;
  }
  if (body > iincIp) {
    return false;
  }
  unsigned incLocal = 0;
  int incDelta = 0;
  if (not rceIsIinc(code, length, iincIp, &incLocal, &incDelta) or incLocal != iv
      or incDelta != 1) {
    return false;
  }
  if (cfg->streamNext[iincIp] != gotoIp) {
    return false;
  }

  // Nothing may land in the middle of the header. Otherwise the compare
  // might not be "i < array.length".
  {
    unsigned seq = cfg->streamNext[header];
    unsigned seqGuard = 0;
    while (seq != body) {
      if (seq >= length or cfg->streamNext[seq] == 0 or cfg->predCount[seq] != 1
          or seq >= loopEnd or ++seqGuard > length) {
        return false;
      }
      seq = cfg->streamNext[seq];
    }
  }

  if (cfg->predCount[header] != 2) {
    return false;
  }
  unsigned predA = cfg->pred0[header];
  unsigned predB = cfg->pred1[header];
  if (predA != gotoIp and predB != gotoIp) {
    return false;
  }
  unsigned initIp = (predA == gotoIp) ? predB : predA;
  if (initIp == RceNoTarget) {
    return false;
  }
  unsigned stored = 0;
  if (not rceIsIstore(code, length, initIp, &stored) or stored != iv) {
    return false;
  }
  if (cfg->predCount[initIp] != 1) {
    return false;
  }
  unsigned constIp = cfg->pred0[initIp];
  if (constIp == RceNoTarget or cfg->streamNext[constIp] != initIp
      or not rceNonNegConst(code, length, constIp)) {
    return false;
  }

  if (haveLim) {
    unsigned caps = 0;
    unsigned capIp = 0;
    unsigned capCount = 0;
    for (unsigned ip = 0; ip < length; ++ip) {
      if (cfg->streamNext[ip] == 0) {
        continue;
      }
      unsigned slot;
      unsigned count;
      if (not rceMods(code, length, ip, &slot, &count)) {
        continue;
      }
      if (not rceOverlap(slot, count, limLocal)) {
        continue;
      }
      ++caps;
      capIp = ip;
      capCount = count;
    }
    if (caps != 1 or capCount != 1) {
      return false;
    }
    unsigned capLocal = 0;
    if (not rceIsIstore(code, length, capIp, &capLocal) or capLocal != limLocal) {
      return false;
    }
    unsigned lenIp = rcePrev(cfg->streamNext, length, capIp);
    unsigned aloadIp = (lenIp < length) ? rcePrev(cfg->streamNext, length, lenIp)
                                        : length;
    unsigned capArr = 0;
    if (lenIp >= length or aloadIp >= length or code[lenIp] != arraylength
        or not rceIsAload(code, length, aloadIp, &capArr)
        or capArr >= cfg->maxLocals) {
      return false;
    }
    if (cfg->predCount[capIp] != 1 or cfg->pred0[capIp] != lenIp
        or cfg->predCount[lenIp] != 1 or cfg->pred0[lenIp] != aloadIp
        or cfg->streamNext[aloadIp] != lenIp or cfg->streamNext[lenIp] != capIp) {
      return false;
    }

    rceReach(cfg, cfg->streamNext[capIp], length);
    if (rceSeenMods(cfg, capArr) or rceSeenMods(cfg, limLocal)
        or not cfg->seen[initIp]) {
      return false;
    }
    rceReach(cfg, 0, capIp);
    if (cfg->seen[initIp]) {
      return false;
    }
    haveArr = true;
    arrLocal = capArr;
  }

  if (not haveArr or arrLocal == iv or (haveLim and limLocal == iv)) {
    return false;
  }

  unsigned ivMods = 0;
  bool initOk = false;
  bool incOk = false;
  for (unsigned ip = 0; ip < length; ++ip) {
    if (cfg->streamNext[ip] == 0) {
      continue;
    }
    unsigned slot;
    unsigned count;
    if (not rceMods(code, length, ip, &slot, &count)
        or not rceOverlap(slot, count, iv)) {
      continue;
    }
    ++ivMods;
    unsigned stLocal = 0;
    if (ip == initIp and count == 1 and rceIsIstore(code, length, ip, &stLocal)
        and stLocal == iv) {
      initOk = true;
    } else {
      unsigned loc = 0;
      int delta = 0;
      if (ip == iincIp and rceIsIinc(code, length, ip, &loc, &delta) and loc == iv
          and delta == 1) {
        incOk = true;
      } else {
        return false;
      }
    }
  }
  if (ivMods != 2 or not initOk or not incOk) {
    return false;
  }

  unsigned base = *markCount;
  st->sp = 0;
  unsigned ip = body;
  unsigned guard = 0;
  while (ip != iincIp) {
    if (ip >= length or cfg->streamNext[ip] == 0 or not cfg->falls[ip]
        or cfg->predCount[ip] != 1 or ++guard > length) {
      *markCount = base;
      return false;
    }
    unsigned slot;
    unsigned count;
    if (rceMods(code, length, ip, &slot, &count)) {
      if (rceOverlap(slot, count, iv) or rceOverlap(slot, count, arrLocal)
          or (haveLim and rceOverlap(slot, count, limLocal))) {
        *markCount = base;
        return false;
      }
    }
    if (not rceExec(code, length, ip, st, arrLocal, iv, marks, markCount, markCap)) {
      *markCount = base;
      return false;
    }
    ip = cfg->streamNext[ip];
  }

  bool any = false;
  for (unsigned m = base; m < *markCount; ++m) {
    unsigned at = marks[m];
    unsigned op = code[at];
    if (op == iaload or op == laload or op == faload or op == daload
        or op == aaload or op == baload or op == caload or op == saload
        or op == iastore or op == lastore or op == fastore or op == dastore
        or op == aastore or op == bastore or op == castore or op == sastore) {
      safe[at] = 1;
      any = true;
    }
  }
  if (any and ivAtHeader and iv < RangeCheckElimination::NoCountedLoop) {
    ivAtHeader[header] = static_cast<uint16_t>(iv);
  }
  return any;
}


void* rceAlloc(Zone* zone, size_t bytes)
{
  void* p = zone->allocate(bytes);
  memset(p, 0, bytes);
  return p;
}

// Public entry is RangeCheckElimination::eliminate. This class is the pass,
// private to this translation unit, the way C1 keeps RangeCheckEliminator
// out of the rest of the compiler.
class RangeCheckEliminator {
 public:
  RangeCheckEliminator(Thread* thread, Zone* zone, GcMethod* method)
      : thread(thread),
        zone(zone),
        method(method),
        code(0),
        bytes(0),
        length(0),
        maxStack(0),
        maxLocals(0),
        handlerCount(0),
        handlers(0),
        streamNext(0),
        branch(0),
        falls(0),
        predCount(0),
        pred0(0),
        pred1(0),
        seen(0),
        work(0),
        edges(0),
        edgeCount(0),
        switchTargets(0),
        cfg()
  {
  }

  uint8_t* eliminate(uint16_t** countedLoopIv);

 private:
  bool copyCode();
  bool buildCfg();
  uint8_t* markCountedLoops(uint16_t** countedLoopIv);

  Thread* thread;
  Zone* zone;
  GcMethod* method;
  GcCode* code;
  uint8_t* bytes;
  unsigned length;
  unsigned maxStack;
  unsigned maxLocals;
  unsigned handlerCount;
  RceHandler* handlers;
  uint16_t* streamNext;
  uint16_t* branch;
  uint8_t* falls;
  uint16_t* predCount;
  uint16_t* pred0;
  uint16_t* pred1;
  uint8_t* seen;
  uint16_t* work;
  RceEdge* edges;
  unsigned edgeCount;
  unsigned* switchTargets;
  RceCfg cfg;
};

uint8_t* RangeCheckEliminator::eliminate(uint16_t** countedLoopIv)
{
  if (not copyCode() or not buildCfg()) {
    return 0;
  }
  return markCountedLoops(countedLoopIv);
}

bool RangeCheckEliminator::copyCode()
{
  if (method == 0 or method->code() == 0) {
    return false;
  }
  code = method->code();
  PROTECT(thread, code);

  length = code->length();
  if (length == 0 or length > 65535) {
    return false;
  }
  maxStack = code->maxStack();
  maxLocals = code->maxLocals();

  GcExceptionHandlerTable* table
      = cast<GcExceptionHandlerTable>(thread, code->exceptionHandlerTable());
  handlerCount = table ? table->length() : 0;
  if (handlerCount > length) {
    return false;
  }

  bytes = static_cast<uint8_t*>(rceAlloc(zone, length));
  streamNext
      = static_cast<uint16_t*>(rceAlloc(zone, length * sizeof(uint16_t)));
  branch
      = static_cast<uint16_t*>(rceAlloc(zone, length * sizeof(uint16_t)));
  falls = static_cast<uint8_t*>(rceAlloc(zone, length));
  predCount
      = static_cast<uint16_t*>(rceAlloc(zone, length * sizeof(uint16_t)));
  pred0
      = static_cast<uint16_t*>(rceAlloc(zone, length * sizeof(uint16_t)));
  pred1
      = static_cast<uint16_t*>(rceAlloc(zone, length * sizeof(uint16_t)));
  seen = static_cast<uint8_t*>(rceAlloc(zone, length));
  work
      = static_cast<uint16_t*>(rceAlloc(zone, length * sizeof(uint16_t)));
  handlers = 0;
  if (handlerCount) {
    handlers = static_cast<RceHandler*>(
        rceAlloc(zone, handlerCount * sizeof(RceHandler)));
  }

  memset(branch, 0xFF, length * sizeof(uint16_t));

  // Zone allocation can trigger a collection. Snapshot the bytecode and the
  // handler table only after every allocation that precedes the snapshot,
  // so these copies are not followed by a move of the Java objects. Later
  // edge and stack allocations happen after this returns, and they read
  // only the copies.
  memcpy(bytes, code->body().begin(), length);
  if (handlerCount) {
    GcExceptionHandlerTable* live
        = cast<GcExceptionHandlerTable>(thread, code->exceptionHandlerTable());
    if (live == 0 or live->length() != handlerCount) {
      return false;
    }
    for (unsigned i = 0; i < handlerCount; ++i) {
      uint64_t eh = live->body()[i];
      unsigned start = exceptionHandlerStart(eh);
      unsigned end = exceptionHandlerEnd(eh);
      unsigned hip = exceptionHandlerIp(eh);
      if (start > end or end > length or hip >= length) {
        return false;
      }
      handlers[i].start = static_cast<uint16_t>(start);
      handlers[i].end = static_cast<uint16_t>(end);
      handlers[i].ip = static_cast<uint16_t>(hip);
    }
  }
  return true;
}

bool RangeCheckEliminator::buildCfg()
{
  edgeCount = 0;
  unsigned ip = 0;
  while (ip < length) {
    unsigned next = 0;
    unsigned target = 0;
    unsigned kind = 0;
    unsigned sw = 0;
    if (not rceDecode(bytes, length, ip, &next, &target, &kind, &sw)
        or next <= ip or next > length) {
      return false;
    }
    streamNext[ip] = static_cast<uint16_t>(next);
    if (kind == RceSwitch) {
      if (edgeCount > length * 2 or sw > length * 2 - edgeCount) {
        return false;
      }
      edgeCount += sw;
    }
    ip = next;
  }
  if (ip != length) {
    return false;
  }

  edges = 0;
  switchTargets = 0;
  if (edgeCount) {
    edges = static_cast<RceEdge*>(rceAlloc(zone, edgeCount * sizeof(RceEdge)));
    switchTargets = static_cast<unsigned*>(
        rceAlloc(zone, edgeCount * sizeof(unsigned)));
  }

  cfg.code = bytes;
  cfg.length = length;
  cfg.maxLocals = maxLocals;
  cfg.streamNext = streamNext;
  cfg.branch = branch;
  cfg.falls = falls;
  cfg.predCount = predCount;
  cfg.pred0 = pred0;
  cfg.pred1 = pred1;
  cfg.edges = edges;
  cfg.edgeCount = edgeCount;
  cfg.handlers = handlers;
  cfg.handlerCount = handlerCount;
  cfg.seen = seen;
  cfg.work = work;

  unsigned edgeCursor = 0;
  for (ip = 0; ip < length; ++ip) {
    if (streamNext[ip] == 0) {
      continue;
    }
    unsigned next = 0;
    unsigned target = 0;
    unsigned kind = 0;
    unsigned sw = 0;
    if (not rceDecode(bytes, length, ip, &next, &target, &kind, &sw)
        or next != streamNext[ip]) {
      return false;
    }
    if (kind == RceFall or kind == RceFallBranch) {
      falls[ip] = 1;
      if (next < length and not rcePred(&cfg, next, ip)) {
        return false;
      }
    }
    if (kind == RceFallBranch or kind == RceBranch) {
      branch[ip] = static_cast<uint16_t>(target);
      if (not rcePred(&cfg, target, ip)) {
        return false;
      }
    } else if (kind == RceSwitch) {
      unsigned end = 0;
      unsigned count = 0;
      if (not rceSwitch(bytes, length, ip, &end, &count, switchTargets)
          or count != sw or edgeCursor + count > edgeCount) {
        return false;
      }
      for (unsigned s = 0; s < count; ++s) {
        edges[edgeCursor].from = static_cast<uint16_t>(ip);
        edges[edgeCursor].to = static_cast<uint16_t>(switchTargets[s]);
        if (not rcePred(&cfg, switchTargets[s], ip)) {
          return false;
        }
        ++edgeCursor;
      }
    }
  }
  if (edgeCursor != edgeCount) {
    return false;
  }

  for (unsigned h = 0; h < handlerCount; ++h) {
    if (streamNext[handlers[h].ip] == 0) {
      return false;
    }
    rceAddPred(&cfg, handlers[h].ip, RceNoTarget);
  }
  return true;
}

uint8_t* RangeCheckEliminator::markCountedLoops(uint16_t** countedLoopIv)
{
  unsigned stackCap = maxStack + 4;
  RceStack stack;
  stack.slot = static_cast<RceSlot*>(
      rceAlloc(zone, stackCap * sizeof(RceSlot)));
  stack.sp = 0;
  stack.cap = stackCap;
  unsigned* marks
      = static_cast<unsigned*>(rceAlloc(zone, length * sizeof(unsigned)));
  unsigned markCount = 0;
  uint8_t* safe = static_cast<uint8_t*>(rceAlloc(zone, length));
  // NoCountedLoop is 0xFFFF, so a byte fill is the sentinel in each slot.
  uint16_t* ivAtHeader = 0;
  if (countedLoopIv) {
    ivAtHeader = static_cast<uint16_t*>(
        rceAlloc(zone, length * sizeof(uint16_t)));
    memset(ivAtHeader, 0xFF, length * sizeof(uint16_t));
  }
  bool any = false;

  for (unsigned ip = 0; ip < length; ++ip) {
    if (streamNext[ip] == 0) {
      continue;
    }
    unsigned op = bytes[ip];
    if ((op == goto_ or op == goto_w) and branch[ip] != RceNoTarget
        and branch[ip] < ip) {
      if (rceTryLoop(&cfg,
                     &stack,
                     marks,
                     &markCount,
                     length,
                     safe,
                     ip,
                     branch[ip],
                     ivAtHeader)) {
        any = true;
      }
    }
  }

  if (any) {
    if (countedLoopIv) {
      *countedLoopIv = ivAtHeader;
    }
    return safe;
  }
  return 0;
}

}  // namespace

uint8_t* RangeCheckElimination::eliminate(Thread* t,
                                        Zone* zone,
                                        GcMethod* method,
                                        uint16_t** countedLoopIv)
{
  if (countedLoopIv) {
    *countedLoopIv = 0;
  }
  if (method == 0 or method->code() == 0) {
    return 0;
  }
  return RangeCheckEliminator(t, zone, method).eliminate(countedLoopIv);
}

}  // namespace vm
