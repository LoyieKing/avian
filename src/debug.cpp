/* Copyright (c) 2026, Avian Contributors
   See license.txt for details. */

#include "avian/debug.h"

#include "avian/machine.h"
#include "avian/util.h"
#include "avian/constants.h"

#include <avian/system/code-memory.h>
#include <avian/system/system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

using namespace vm;

namespace vm {
namespace debug {

namespace {

const unsigned kHeader = 16;
const unsigned kMaxFrames = 32;
const int kNotImplemented = 99;
const int kAbsent = 101;
const int kInvalidThread = 10;
const int kThreadNotSuspended = 13;
const int kInvalidClass = 21;
const int kInvalidMethod = 23;
const int kInvalidEvent = 102;
const int kIllegalArgument = 103;

const int kSuspendNone = 0;
const int kSuspendEvent = 1;
const int kSuspendAll = 2;

const int kStatusVerified = 1;
const int kStatusPrepared = 2;
const int kStatusInitialized = 4;
const int kStatusError = 8;

struct LineRec {
  int32_t bci;
  int32_t line;
};

struct FieldRec {
  uint64_t id;
  char* name;
  char* spec;
  int32_t flags;
};

struct CompiledSite {
  char* className;
  char* methodName;
  char* spec;
  uint8_t* bits;
  uint32_t bitsLength;
  avian::system::CodeMemory* memory;
  int32_t* bciByOffset;  // parallel to offset, sorted by offset
  int32_t* offset;
  int mapCount;
  uint64_t methodId;
  CompiledSite* next;
};

struct MethodRec {
  uint64_t id;
  char* name;
  char* spec;
  int32_t flags;
  int32_t codeLength;  // -1 native or absent
  LineRec* lines;
  int lineCount;
  CompiledSite* site;
};

struct ClassRec {
  uint64_t id;
  char* name;  // internal binary name
  char* signature;
  char* source;
  char* superName;
  int32_t flags;
  int32_t status;
  int typeTag;
  bool primitive;
  FieldRec* fields;
  int fieldCount;
  MethodRec* methods;
  int methodCount;
  ClassRec* next;
};

struct Request {
  uint32_t id;
  int kind;
  int policy;
  int count;  // 0 means unlimited; otherwise remaining
  bool hasCount;
  bool hasThread;
  uint64_t thread;
  bool hasClass;
  uint64_t classId;
  char* classMatch;
  char* classExclude;
  bool hasLocation;
  uint64_t locClass;
  uint64_t locMethod;
  int64_t locIndex;
  bool hasStep;
  uint64_t stepThread;
  int stepSize;
  int stepDepth;
  Request* next;
};

struct SnapFrame {
  uint64_t classId;
  uint64_t methodId;
  int64_t index;
  char* className;
  char* methodName;
  char* spec;
};

struct Snap {
  int count;
  int depth;
  SnapFrame frames[kMaxFrames];
};

struct Packet {
  uint8_t* data;
  unsigned length;
  Packet* next;
};

bool enabledFlag = false;
bool suspendOnConnect = false;
bool disposed = false;
bool serverAlive = false;
int requestedPort = 0;
volatile int boundPort = 0;
volatile int globalSuspend = 0;
volatile int breakpointCount = 0;
bool holdEvents = false;
bool logPackets = false;

avian::system::CodeMemory* codeMemory = 0;
System::Monitor* debugMonitor = 0;
System::Mutex* writeLock = 0;
System* system_ = 0;
Machine* machine_ = 0;

#ifndef _WIN32
pthread_mutex_t registryLock = PTHREAD_MUTEX_INITIALIZER;
#endif

ClassRec* classes = 0;
CompiledSite* sites = 0;
Request* requests = 0;
uint64_t nextClassId = 1;
uint64_t nextMemberId = 1;
uint32_t nextRequestId = 1;
uint32_t nextPacketId = 1;

int clientSocket = -1;
int listenSocket = -1;
Packet* held = 0;
Packet* heldTail = 0;

Walker walkerFn = 0;

char jdwpOptionCopy[512];

void logf(const char* fmt, ...)
{
  if (not logPackets)
    return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "jdwp: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

char* dupN(const char* s, unsigned n)
{
  char* p = static_cast<char*>(malloc(n + 1));
  if (p == 0)
    return 0;
  if (n)
    memcpy(p, s, n);
  p[n] = 0;
  return p;
}

char* dupZ(const char* s)
{
  if (s == 0)
    return dupN("", 0);
  return dupN(s, strlen(s));
}

void copyBytes(char* dst, int cap, GcByteArray* a)
{
  dst[0] = 0;
  if (a == 0 or cap <= 0)
    return;
  int len = a->length();
  if (len > 0 and a->body()[len - 1] == 0)
    --len;
  if (len >= cap)
    len = cap - 1;
  if (len > 0)
    memcpy(dst, a->body().begin(), len);
  dst[len] = 0;
}

char* dupBytes(GcByteArray* a)
{
  if (a == 0)
    return dupZ("");
  int len = a->length();
  if (len > 0 and a->body()[len - 1] == 0)
    --len;
  if (len < 0)
    len = 0;
  return dupN(reinterpret_cast<const char*>(a->body().begin()), len);
}

void lockReg()
{
#ifndef _WIN32
  pthread_mutex_lock(&registryLock);
#endif
}

void unlockReg()
{
#ifndef _WIN32
  pthread_mutex_unlock(&registryLock);
#endif
}

int loadSuspend()
{
  return __atomic_load_n(&globalSuspend, __ATOMIC_ACQUIRE);
}

void storeSuspend(int v)
{
  __atomic_store_n(&globalSuspend, v, __ATOMIC_RELEASE);
}

int loadThreadSuspend(Thread* t)
{
  return __atomic_load_n(&t->debugSuspend, __ATOMIC_ACQUIRE);
}

void storeThreadSuspend(Thread* t, int v)
{
  __atomic_store_n(&t->debugSuspend, v, __ATOMIC_RELEASE);
}

struct Buf {
  uint8_t* p;
  unsigned n;
  unsigned cap;
};

void bufInit(Buf* b)
{
  b->p = 0;
  b->n = 0;
  b->cap = 0;
}

void bufFree(Buf* b)
{
  free(b->p);
  b->p = 0;
  b->n = b->cap = 0;
}

bool bufGrow(Buf* b, unsigned more)
{
  if (b->n + more <= b->cap)
    return true;
  unsigned c = b->cap ? b->cap * 2 : 64;
  while (c < b->n + more)
    c *= 2;
  uint8_t* n = static_cast<uint8_t*>(realloc(b->p, c));
  if (n == 0)
    return false;
  b->p = n;
  b->cap = c;
  return true;
}

void b1(Buf* b, unsigned v)
{
  if (bufGrow(b, 1))
    b->p[b->n++] = static_cast<uint8_t>(v);
}

void b2(Buf* b, unsigned v)
{
  b1(b, v >> 8);
  b1(b, v);
}

void b4(Buf* b, unsigned v)
{
  b2(b, v >> 16);
  b2(b, v);
}

void b8(Buf* b, uint64_t v)
{
  b4(b, static_cast<unsigned>(v >> 32));
  b4(b, static_cast<unsigned>(v));
}

void bstr(Buf* b, const char* s)
{
  unsigned n = s ? strlen(s) : 0;
  b4(b, n);
  if (n and bufGrow(b, n)) {
    memcpy(b->p + b->n, s, n);
    b->n += n;
  }
}

struct Reader {
  const uint8_t* p;
  unsigned n;
  unsigned i;
  bool error;
};

unsigned r1(Reader* r)
{
  if (r->i >= r->n) {
    r->error = true;
    return 0;
  }
  return r->p[r->i++];
}

unsigned r2(Reader* r)
{
  unsigned a = r1(r);
  unsigned b = r1(r);
  return (a << 8) | b;
}

unsigned r4(Reader* r)
{
  unsigned a = r2(r);
  unsigned b = r2(r);
  return (a << 16) | b;
}

uint64_t r8(Reader* r)
{
  uint64_t a = r4(r);
  uint64_t b = r4(r);
  return (a << 32) | b;
}

char* rstr(Reader* r)
{
  unsigned n = r4(r);
  if (r->error or r->i + n > r->n) {
    r->error = true;
    return dupZ("");
  }
  char* s = dupN(reinterpret_cast<const char*>(r->p + r->i), n);
  r->i += n;
  return s ? s : dupZ("");
}

const char* dotted(const char* internal, char* buf, int cap)
{
  int i = 0;
  for (; internal[i] and i + 1 < cap; ++i)
    buf[i] = internal[i] == '/' ? '.' : internal[i];
  buf[i] = 0;
  return buf;
}

bool wildcardMatch(const char* pattern, const char* name)
{
  if (pattern == 0 or name == 0)
    return false;
  unsigned pn = strlen(pattern);
  unsigned nn = strlen(name);
  if (pn == 0)
    return nn == 0;
  if (pattern[0] == '*')
    return pn == 1 or (nn + 1 >= pn and ::strcmp(name + (nn - (pn - 1)), pattern + 1) == 0);
  if (pattern[pn - 1] == '*')
    return pn == 1 or (nn + 1 >= pn and strncmp(name, pattern, pn - 1) == 0);
  return ::strcmp(pattern, name) == 0;
}

bool classMatch(const char* pattern, ClassRec* c)
{
  if (pattern == 0)
    return true;
  char dot[512];
  dotted(c->name, dot, sizeof dot);
  return wildcardMatch(pattern, c->name) or wildcardMatch(pattern, dot)
         or wildcardMatch(pattern, c->signature);
}

ClassRec* findClass(uint64_t id)
{
  for (ClassRec* c = classes; c; c = c->next)
    if (c->id == id)
      return c;
  return 0;
}

ClassRec* findClassByName(const char* name)
{
  for (ClassRec* c = classes; c; c = c->next)
    if (::strcmp(c->name, name) == 0)
      return c;
  return 0;
}

MethodRec* findMethod(ClassRec* c, uint64_t id)
{
  if (c == 0)
    return 0;
  for (int i = 0; i < c->methodCount; ++i)
    if (c->methods[i].id == id)
      return &c->methods[i];
  return 0;
}

MethodRec* findMethodGlobal(uint64_t id, ClassRec** outClass)
{
  for (ClassRec* c = classes; c; c = c->next) {
    MethodRec* m = findMethod(c, id);
    if (m) {
      if (outClass)
        *outClass = c;
      return m;
    }
  }
  return 0;
}

MethodRec* findMethodByName(const char* cn, const char* mn, const char* spec)
{
  ClassRec* c = findClassByName(cn);
  if (c == 0)
    return 0;
  for (int i = 0; i < c->methodCount; ++i) {
    MethodRec* m = &c->methods[i];
    if (::strcmp(m->name, mn) == 0 and (spec == 0 or spec[0] == 0 or ::strcmp(m->spec, spec) == 0))
      return m;
  }
  return 0;
}

int lineFor(MethodRec* m, int32_t bci)
{
  int line = -1;
  if (m == 0)
    return -1;
  for (int i = 0; i < m->lineCount; ++i) {
    if (m->lines[i].bci <= bci)
      line = m->lines[i].line;
  }
  return line;
}

void eachThread(Thread* t, void (*fn)(Thread*, void*), void* arg)
{
  if (t == 0)
    return;
  fn(t, arg);
  for (Thread* c = t->child; c; c = c->peer)
    eachThread(c, fn, arg);
}

Thread* findThread(uint64_t id)
{
  struct Find {
    uint64_t id;
    Thread* found;
  };
  Find f;
  f.id = id;
  f.found = 0;
  struct Local {
    static void visit(Thread* t, void* arg)
    {
      Find* f = static_cast<Find*>(arg);
      if (reinterpret_cast<uint64_t>(t) == f->id)
        f->found = t;
    }
  };
  if (machine_)
    eachThread(machine_->rootThread, Local::visit, &f);
  return f.found;
}

void freeSnap(Snap* s)
{
  if (s == 0)
    return;
  for (int i = 0; i < s->count; ++i) {
    free(s->frames[i].className);
    free(s->frames[i].methodName);
    free(s->frames[i].spec);
  }
  free(s);
}

void setSnap(Thread* t, Snap* s)
{
  freeSnap(static_cast<Snap*>(t->debugSnap));
  t->debugSnap = s;
  t->debugDepth = s ? s->count : 0;
}

bool writeAll(int fd, const uint8_t* p, unsigned n)
{
  unsigned off = 0;
  while (off < n) {
    ssize_t w = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (w == 0)
      return false;
    off += static_cast<unsigned>(w);
  }
  return true;
}

bool readAll(int fd, uint8_t* p, unsigned n)
{
  unsigned off = 0;
  while (off < n) {
    ssize_t r = ::recv(fd, p + off, n - off, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (r == 0)
      return false;
    off += static_cast<unsigned>(r);
  }
  return true;
}

void lockWrite()
{
  if (writeLock)
    writeLock->acquire();
}

void unlockWrite()
{
  if (writeLock)
    writeLock->release();
}

bool sendRaw(const uint8_t* p, unsigned n)
{
  if (clientSocket < 0 or disposed)
    return false;
  lockWrite();
  bool ok = writeAll(clientSocket, p, n);
  unlockWrite();
  return ok;
}

void flushHeld()
{
  lockWrite();
  Packet* p = held;
  held = heldTail = 0;
  holdEvents = false;
  while (p) {
    Packet* n = p->next;
    if (clientSocket >= 0)
      writeAll(clientSocket, p->data, p->length);
    free(p->data);
    free(p);
    p = n;
  }
  unlockWrite();
}

void sendPacket(uint8_t flags, uint32_t id, unsigned extra, const uint8_t* body, unsigned bodyLen)
{
  unsigned length = 11 + bodyLen;
  uint8_t* raw = static_cast<uint8_t*>(malloc(length));
  if (raw == 0)
    return;
  raw[0] = static_cast<uint8_t>(length >> 24);
  raw[1] = static_cast<uint8_t>(length >> 16);
  raw[2] = static_cast<uint8_t>(length >> 8);
  raw[3] = static_cast<uint8_t>(length);
  raw[4] = static_cast<uint8_t>(id >> 24);
  raw[5] = static_cast<uint8_t>(id >> 16);
  raw[6] = static_cast<uint8_t>(id >> 8);
  raw[7] = static_cast<uint8_t>(id);
  raw[8] = flags;
  raw[9] = static_cast<uint8_t>(extra >> 8);
  raw[10] = static_cast<uint8_t>(extra);
  if (bodyLen)
    memcpy(raw + 11, body, bodyLen);

  if ((flags & 0x80) == 0 and holdEvents) {
    Packet* pkt = static_cast<Packet*>(malloc(sizeof(Packet)));
    if (pkt) {
      pkt->data = raw;
      pkt->length = length;
      pkt->next = 0;
      lockWrite();
      if (heldTail)
        heldTail->next = pkt;
      else
        held = pkt;
      heldTail = pkt;
      unlockWrite();
      return;
    }
  }
  sendRaw(raw, length);
  free(raw);
}

void sendReply(uint32_t id, unsigned error, Buf* body)
{
  sendPacket(0x80, id, error, body ? body->p : 0, body ? body->n : 0);
}

void sendEvent(int policy, Buf* body)
{
  // body is the composite payload AFTER the header's cmd bytes, i.e.
  // starting at suspendPolicy.  Command set 64, command 100.
  unsigned length = 11 + (body ? body->n : 0);
  uint8_t* raw = static_cast<uint8_t*>(malloc(length));
  if (raw == 0)
    return;
  uint32_t id = nextPacketId++;
  raw[0] = static_cast<uint8_t>(length >> 24);
  raw[1] = static_cast<uint8_t>(length >> 16);
  raw[2] = static_cast<uint8_t>(length >> 8);
  raw[3] = static_cast<uint8_t>(length);
  raw[4] = static_cast<uint8_t>(id >> 24);
  raw[5] = static_cast<uint8_t>(id >> 16);
  raw[6] = static_cast<uint8_t>(id >> 8);
  raw[7] = static_cast<uint8_t>(id);
  raw[8] = 0;
  raw[9] = 64;
  raw[10] = 100;
  if (body and body->n)
    memcpy(raw + 11, body->p, body->n);
  (void)policy;
  if (holdEvents) {
    Packet* pkt = static_cast<Packet*>(malloc(sizeof(Packet)));
    if (pkt) {
      pkt->data = raw;
      pkt->length = length;
      pkt->next = 0;
      lockWrite();
      if (heldTail)
        heldTail->next = pkt;
      else
        held = pkt;
      heldTail = pkt;
      unlockWrite();
      return;
    }
  }
  sendRaw(raw, length);
  free(raw);
}

System::Thread* serverSystemThread = 0;

void wakeWaiters()
{
  if (debugMonitor == 0 or serverSystemThread == 0)
    return;
  debugMonitor->acquire(serverSystemThread);
  debugMonitor->notifyAll(serverSystemThread);
  debugMonitor->release(serverSystemThread);
}

void clearThreadSuspend(Thread* t, void*)
{
  storeThreadSuspend(t, 0);
  __atomic_store_n(&t->debugStepping, 0, __ATOMIC_RELEASE);
}

void resumeAll()
{
  storeSuspend(0);
  if (machine_)
    eachThread(machine_->rootThread, clearThreadSuspend, 0);
  wakeWaiters();
}

void applyBits(CompiledSite* site)
{
  if (site == 0 or site->bits == 0)
    return;
  for (unsigned i = 0; i < site->bitsLength; ++i) {
    uint8_t want = 0;
    for (Request* r = requests; r; r = r->next) {
      if (r->kind == 2 and r->hasLocation and r->locMethod == site->methodId
          and r->locIndex == static_cast<int64_t>(i))
        want = 1;
    }
    uint8_t* addr = site->bits + i;
    if (site->memory)
      site->memory->patch(addr, &want, 1);
    else
      *addr = want;
  }
}

void patchMethodBits(MethodRec* m)
{
  if (m and m->site)
    applyBits(m->site);
}

CompiledSite* findSite(const char* cn, const char* mn, const char* spec)
{
  for (CompiledSite* s = sites; s; s = s->next) {
    if (::strcmp(s->className, cn) == 0 and ::strcmp(s->methodName, mn) == 0
        and (spec == 0 or spec[0] == 0 or ::strcmp(s->spec, spec) == 0))
      return s;
  }
  return 0;
}

void linkSite(MethodRec* m, const char* cn)
{
  if (m == 0 or m->site)
    return;
  CompiledSite* s = findSite(cn, m->name, m->spec);
  if (s == 0)
    return;
  s->methodId = m->id;
  m->site = s;
  // Publish the site pointer into the bitset header so the checkpoint
  // can name the method without touching the heap.
  if (s->bits) {
    uint64_t bits = reinterpret_cast<uint64_t>(s);
    uint8_t* header = s->bits - kHeader;
    if (s->memory)
      s->memory->patch(header, &bits, 8);
    else
      memcpy(header, &bits, 8);
  }
  applyBits(s);
}

int statusOf(GcClass* c)
{
  int st = kStatusVerified | kStatusPrepared;
  if ((c->vmFlags() & NeedInitFlag) == 0)
    st |= kStatusInitialized;
  if (c->vmFlags() & InitErrorFlag)
    st |= kStatusError;
  return st;
}

int tagOf(GcClass* c, const char* name)
{
  if (name[0] == '[')
    return 3;
  if (c->flags() & ACC_INTERFACE)
    return 2;
  return 1;
}

void readLines(Thread* t, GcMethod* method, MethodRec* mr)
{
  mr->codeLength = -1;
  mr->lines = 0;
  mr->lineCount = 0;
  if (method->flags() & ACC_NATIVE)
    return;
  GcCode* code = method->code();
  if (code == 0)
    return;
  // After JIT, length is 0 and the line table holds machine offsets.
  // Only snapshot real bytecode tables.
  if (code->length() == 0)
    return;
  mr->codeLength = static_cast<int32_t>(code->length());
  GcLineNumberTable* lnt = code->lineNumberTable();
  if (lnt == 0 or lnt->length() == 0)
    return;
  mr->lineCount = static_cast<int>(lnt->length());
  mr->lines = static_cast<LineRec*>(malloc(sizeof(LineRec) * mr->lineCount));
  if (mr->lines == 0) {
    mr->lineCount = 0;
    return;
  }
  for (int i = 0; i < mr->lineCount; ++i) {
    uint64_t ln = lnt->body()[i];
    mr->lines[i].bci = static_cast<int32_t>(lineNumberIp(ln));
    mr->lines[i].line = static_cast<int32_t>(lineNumberLine(ln));
  }
  (void)t;
}

ClassRec* ingest(Thread* t, GcClass* c)
{
  if (c == 0 or c->name() == 0)
    return 0;
  char* name = dupBytes(c->name());
  if (name == 0)
    return 0;

  char signature[600];
  if (name[0] == '[') {
    ::snprintf(signature, sizeof signature, "%s", name);
  } else {
    ::snprintf(signature, sizeof signature, "L%s;", name);
  }

  char* source = dupBytes(c->sourceFile());
  char* superName = 0;
  if (c->super() and c->super()->name())
    superName = dupBytes(c->super()->name());

  int flags = c->flags();
  int status = statusOf(c);
  int tag = tagOf(c, name);
  bool primitive = (c->vmFlags() & PrimitiveFlag) != 0;

  FieldRec* fields = 0;
  int fieldCount = 0;
  GcArray* ftable = cast<GcArray>(t, c->fieldTable());
  if (ftable) {
    fieldCount = static_cast<int>(ftable->length());
    fields = static_cast<FieldRec*>(calloc(fieldCount ? fieldCount : 1, sizeof(FieldRec)));
    int n = 0;
    for (int i = 0; i < fieldCount; ++i) {
      if (ftable->body()[i] == 0)
        continue;
      GcField* f = cast<GcField>(t, ftable->body()[i]);
      fields[n].name = dupBytes(f->name());
      fields[n].spec = dupBytes(f->spec());
      fields[n].flags = f->flags();
      ++n;
    }
    fieldCount = n;
  }

  MethodRec* methods = 0;
  int methodCount = 0;
  GcArray* mtable = cast<GcArray>(t, c->methodTable());
  if (mtable) {
    methodCount = static_cast<int>(mtable->length());
    GcClassAddendum* add = c->addendum();
    if (add) {
      int declared = static_cast<int>(add->declaredMethodCount());
      if (declared >= 0 and declared < methodCount)
        methodCount = declared;
    }
    methods = static_cast<MethodRec*>(calloc(methodCount ? methodCount : 1, sizeof(MethodRec)));
    int n = 0;
    for (int i = 0; i < methodCount; ++i) {
      if (mtable->body()[i] == 0)
        continue;
      GcMethod* m = cast<GcMethod>(t, mtable->body()[i]);
      methods[n].name = dupBytes(m->name());
      methods[n].spec = dupBytes(m->spec());
      methods[n].flags = m->flags();
      methods[n].site = 0;
      readLines(t, m, &methods[n]);
      ++n;
    }
    methodCount = n;
  }

  lockReg();
  ClassRec* existing = findClassByName(name);
  if (existing) {
    if (existing->methodCount == 0 and methodCount > 0) {
      existing->methods = methods;
      existing->methodCount = methodCount;
      existing->fields = fields;
      existing->fieldCount = fieldCount;
      free(existing->source);
      existing->source = source;
      free(existing->superName);
      existing->superName = superName;
      existing->flags = flags;
      existing->status = status;
      existing->typeTag = tag;
      existing->primitive = primitive;
      for (int i = 0; i < methodCount; ++i) {
        methods[i].id = nextMemberId++;
        linkSite(&methods[i], existing->name);
      }
      for (int i = 0; i < fieldCount; ++i)
        fields[i].id = nextMemberId++;
      methods = 0;
      fields = 0;
      source = 0;
      superName = 0;
    } else {
      existing->status = status;
    }
    unlockReg();
    free(name);
    free(source);
    free(superName);
    if (methods) {
      for (int i = 0; i < methodCount; ++i) {
        free(methods[i].name);
        free(methods[i].spec);
        free(methods[i].lines);
      }
      free(methods);
    }
    if (fields) {
      for (int i = 0; i < fieldCount; ++i) {
        free(fields[i].name);
        free(fields[i].spec);
      }
      free(fields);
    }
    return existing;
  }

  ClassRec* rec = static_cast<ClassRec*>(calloc(1, sizeof(ClassRec)));
  rec->id = nextClassId++;
  rec->name = name;
  rec->signature = dupZ(signature);
  rec->source = source;
  rec->superName = superName;
  rec->flags = flags;
  rec->status = status;
  rec->typeTag = tag;
  rec->primitive = primitive;
  rec->fields = fields;
  rec->fieldCount = fieldCount;
  rec->methods = methods;
  rec->methodCount = methodCount;
  for (int i = 0; i < methodCount; ++i) {
    methods[i].id = nextMemberId++;
    linkSite(&methods[i], rec->name);
  }
  for (int i = 0; i < fieldCount; ++i)
    fields[i].id = nextMemberId++;
  rec->next = classes;
  classes = rec;
  unlockReg();
  return rec;
}

void scanMap(Thread* t, GcHashMap* map)
{
  if (map == 0)
    return;
  for (HashMapIterator it(t, map); it.hasMore();) {
    GcTriple* n = it.next();
    if (n and n->second())
      ingest(t, cast<GcClass>(t, n->second()));
  }
}

void scanClasses(Thread* t)
{
  scanMap(t, roots(t)->bootstrapClassMap());
  GcClassLoader* loader = roots(t)->bootLoader();
  if (loader and loader->map())
    scanMap(t, cast<GcHashMap>(t, loader->map()));
}

// ---- suspend -----------------------------------------------------------

void block(Thread* t)
{
  if (t->debugInBlock)
    return;
  if (debugMonitor == 0 or t->systemThread == 0)
    return;
  t->debugInBlock = true;
  {
    ENTER(t, Thread::IdleState);
    debugMonitor->acquire(t->systemThread);
    while (not disposed
           and (loadSuspend() > 0 or loadThreadSuspend(t) > 0)) {
      debugMonitor->wait(t->systemThread, 0);
    }
    debugMonitor->release(t->systemThread);
  }
  t->debugInBlock = false;
}

bool stepping(Thread* t)
{
  return __atomic_load_n(&t->debugStepping, __ATOMIC_ACQUIRE) != 0;
}

uint64_t methodIdOf(GcMethod* method)
{
  if (method == 0 or method->class_() == 0)
    return 0;
  char cn[256], mn[256], sp[256];
  copyBytes(cn, sizeof cn, method->class_()->name());
  copyBytes(mn, sizeof mn, method->name());
  copyBytes(sp, sizeof sp, method->spec());
  lockReg();
  MethodRec* m = findMethodByName(cn, mn, sp);
  uint64_t id = m ? m->id : 0;
  unlockReg();
  return id;
}

void fillNames(GcMethod* method, char* cn, char* mn, char* sp, int cap)
{
  cn[0] = mn[0] = sp[0] = 0;
  if (method == 0)
    return;
  copyBytes(mn, cap, method->name());
  copyBytes(sp, cap, method->spec());
  if (method->class_())
    copyBytes(cn, cap, method->class_()->name());
}

Snap* makeSnap(Thread* t, int32_t bci, CompiledSite* site, GcMethod* method)
{
  Snap* s = static_cast<Snap*>(calloc(1, sizeof(Snap)));
  if (s == 0)
    return 0;

  char cn[256], mn[256], sp[256];
  cn[0] = mn[0] = sp[0] = 0;
  uint64_t mid = 0;
  uint64_t cid = 0;
  if (site) {
    ::snprintf(cn, sizeof cn, "%s", site->className);
    ::snprintf(mn, sizeof mn, "%s", site->methodName);
    ::snprintf(sp, sizeof sp, "%s", site->spec);
    mid = site->methodId;
  } else if (method) {
    fillNames(method, cn, mn, sp, 256);
    mid = methodIdOf(method);
  }
  lockReg();
  if (mid == 0) {
    MethodRec* m = findMethodByName(cn, mn, sp);
    if (m)
      mid = m->id;
  }
  ClassRec* cr = findClassByName(cn);
  if (cr)
    cid = cr->id;
  unlockReg();

  s->frames[0].classId = cid;
  s->frames[0].methodId = mid;
  s->frames[0].index = bci;
  s->frames[0].className = dupZ(cn);
  s->frames[0].methodName = dupZ(mn);
  s->frames[0].spec = dupZ(sp);
  s->count = 1;

  if (walkerFn) {
    WalkerFrame extra[kMaxFrames];
    int n = walkerFn(t, extra, kMaxFrames);
    for (int i = 0; i < n and s->count < static_cast<int>(kMaxFrames); ++i) {
      if (s->count == 1 and ::strcmp(extra[i].methodName, mn) == 0
          and ::strcmp(extra[i].className, cn) == 0)
        continue;
      SnapFrame* f = &s->frames[s->count];
      f->className = dupZ(extra[i].className);
      f->methodName = dupZ(extra[i].methodName);
      f->spec = dupZ(extra[i].spec);
      f->index = extra[i].index;
      lockReg();
      MethodRec* m = findMethodByName(extra[i].className, extra[i].methodName, extra[i].spec);
      ClassRec* c = findClassByName(extra[i].className);
      f->methodId = m ? m->id : 0;
      f->classId = c ? c->id : 0;
      unlockReg();
      if (f->methodId == 0 and f->className[0] == 0) {
        free(f->className);
        free(f->methodName);
        free(f->spec);
        continue;
      }
      ++s->count;
    }
  }
  s->depth = s->count;
  return s;
}

int currentDepth(Thread* t)
{
  if (walkerFn) {
    WalkerFrame extra[kMaxFrames];
    int n = walkerFn(t, extra, kMaxFrames);
    if (n > 0)
      return n;
  }
  return t->debugDepth > 0 ? t->debugDepth : 1;
}

bool breakpointOn(uint64_t methodId, int32_t bci)
{
  if (methodId == 0)
    return false;
  lockReg();
  bool hit = false;
  for (Request* r = requests; r; r = r->next) {
    if (r->kind == 2 and r->hasLocation and r->locMethod == methodId
        and r->locIndex == bci)
      hit = true;
  }
  unlockReg();
  return hit;
}

bool stepMatches(Thread* t, int32_t bci, uint64_t methodId)
{
  if (not stepping(t))
    return false;
  int size = t->debugStepSize;
  int depthKind = t->debugStepDepth;
  int base = t->debugStepBase;
  int here = currentDepth(t);
  if (size == 1) {
    // LINE.  Same line and method does not count.
    int line = -2;
    lockReg();
    ClassRec* cc = 0;
    MethodRec* m = findMethodGlobal(methodId, &cc);
    line = lineFor(m, bci);
    unlockReg();
    if (methodId == t->debugStepMethod and line == t->debugStepLine and line >= 0)
      return false;
  }
  if (depthKind == 1 and here > base)
    return false;
  if (depthKind == 2 and here >= base)
    return false;
  return true;
}

void suspendFor(Thread* t, int policy)
{
  if (policy == kSuspendAll) {
    debugMonitor->acquire(t->systemThread);
    storeSuspend(loadSuspend() + 1);
    debugMonitor->release(t->systemThread);
  } else if (policy == kSuspendEvent) {
    debugMonitor->acquire(t->systemThread);
    storeThreadSuspend(t, loadThreadSuspend(t) + 1);
    debugMonitor->release(t->systemThread);
  }
}

void fire(Thread* t, int32_t bci, CompiledSite* site, GcMethod* method, bool bp, bool step)
{
  Snap* snap = makeSnap(t, bci, site, method);
  setSnap(t, snap);
  uint64_t tid = reinterpret_cast<uint64_t>(t);
  uint64_t methodId = snap and snap->count ? snap->frames[0].methodId : 0;
  uint64_t classId = snap and snap->count ? snap->frames[0].classId : 0;
  int tag = 1;
  lockReg();
  ClassRec* cr = findClass(classId);
  if (cr)
    tag = cr->typeTag;

  int kinds[8];
  uint32_t ids[8];
  int mc = 0;
  int policy = kSuspendNone;
  for (Request* r = requests; r and mc < 8; r = r->next) {
    bool ok = false;
    if (bp and r->kind == 2 and r->hasLocation and r->locMethod == methodId
        and r->locIndex == bci)
      ok = true;
    if (step and r->kind == 1 and r->hasStep
        and (r->stepThread == 0 or r->stepThread == tid))
      ok = true;
    if (not ok)
      continue;
    if (r->hasThread and r->thread != tid)
      continue;
    if (r->hasCount) {
      if (r->count <= 0)
        continue;
      r->count -= 1;
    }
    kinds[mc] = r->kind;
    ids[mc] = r->id;
    ++mc;
    if (r->policy == kSuspendAll)
      policy = kSuspendAll;
    else if (r->policy == kSuspendEvent and policy == kSuspendNone)
      policy = kSuspendEvent;
  }
  // Drop exhausted count requests.
  Request** pp = &requests;
  while (*pp) {
    Request* r = *pp;
    if (r->hasCount and r->count <= 0) {
      *pp = r->next;
      if (r->kind == 1 and r->stepThread == tid)
        __atomic_store_n(&t->debugStepping, 0, __ATOMIC_RELEASE);
      if (r->kind == 2)
        __atomic_fetch_sub(&breakpointCount, 1, __ATOMIC_RELEASE);
      free(r->classMatch);
      free(r->classExclude);
      free(r);
      continue;
    }
    pp = &r->next;
  }
  unlockReg();

  if (mc == 0) {
    if (mustSuspend(t))
      block(t);
    return;
  }

  t->debugSuppressBci = bci;
  t->debugSuppressCookie = site ? reinterpret_cast<uintptr_t>(site)
                                : static_cast<uintptr_t>(methodId);

  suspendFor(t, policy);

  Buf body;
  bufInit(&body);
  b1(&body, policy);
  b4(&body, mc);
  for (int i = 0; i < mc; ++i) {
    b1(&body, kinds[i]);
    b4(&body, ids[i]);
    b8(&body, tid);
    b1(&body, tag);
    b8(&body, classId);
    b8(&body, methodId);
    b8(&body, static_cast<uint64_t>(bci));
  }
  sendEvent(policy, &body);
  bufFree(&body);

  block(t);
}

CompiledSite* siteFromBits(uintptr_t bits, uint32_t* length)
{
  if (bits == 0)
    return 0;
  uint8_t* base = reinterpret_cast<uint8_t*>(bits) - kHeader;
  uint64_t siteBits = 0;
  uint32_t len = 0;
  memcpy(&siteBits, base, 8);
  memcpy(&len, base + 8, 4);
  if (length)
    *length = len;
  return reinterpret_cast<CompiledSite*>(static_cast<uintptr_t>(siteBits));
}

void checkpointImpl(Thread* t, int32_t bci, uintptr_t bits, GcMethod* method)
{
  if (not enabledFlag or disposed or t == 0 or t->debugInBlock)
    return;

  bool sus = loadSuspend() > 0 or loadThreadSuspend(t) > 0;
  bool step = stepping(t);
  uint32_t length = 0;
  CompiledSite* site = siteFromBits(bits, &length);
  bool bp = false;
  if (bits and static_cast<uint32_t>(bci) < length) {
    bp = reinterpret_cast<volatile uint8_t*>(bits)[bci] != 0;
  }
  int bps = __atomic_load_n(&breakpointCount, __ATOMIC_ACQUIRE);
  // JIT bitsets are authoritative.  The interpreter has no bitset, so
  // it consults the request table when any breakpoint is armed.
  if (not sus and not step and not bp and (bits or bps == 0))
    return;

  uintptr_t cookie = site ? reinterpret_cast<uintptr_t>(site) : 0;
  if (cookie == 0 and method and (bp or step or sus or (bps and not bits)))
    cookie = static_cast<uintptr_t>(methodIdOf(method));
  if (not bp and not bits and method)
    bp = breakpointOn(cookie, bci);

  if (t->debugSuppressBci == bci and cookie != 0
      and t->debugSuppressCookie == cookie) {
    if (sus)
      block(t);
    return;
  }
  if (t->debugSuppressBci >= 0 and (t->debugSuppressBci != bci or (cookie and t->debugSuppressCookie != cookie))) {
    t->debugSuppressBci = -1;
    t->debugSuppressCookie = 0;
  }

  bool doStep = false;
  if (step) {
    uint64_t mid = site ? site->methodId : cookie;
    doStep = stepMatches(t, bci, mid);
  }

  if (bp or doStep)
    fire(t, bci, site, method, bp, doStep);
  else if (sus)
    block(t);
}

void postClassPrepare(Thread* t, ClassRec* rec)
{
  if (rec == 0 or not serverAlive or disposed)
    return;
  uint64_t tid = reinterpret_cast<uint64_t>(t);
  lockReg();
  uint32_t ids[8];
  int mc = 0;
  int policy = kSuspendNone;
  for (Request* r = requests; r and mc < 8; r = r->next) {
    if (r->kind != 8)
      continue;
    if (r->classMatch and not classMatch(r->classMatch, rec))
      continue;
    if (r->classExclude and classMatch(r->classExclude, rec))
      continue;
    if (r->hasClass and r->classId != rec->id)
      continue;
    if (r->hasThread and r->thread != tid)
      continue;
    if (r->hasCount) {
      if (r->count <= 0)
        continue;
      r->count -= 1;
    }
    ids[mc++] = r->id;
    if (r->policy == kSuspendAll)
      policy = kSuspendAll;
    else if (r->policy == kSuspendEvent and policy == kSuspendNone)
      policy = kSuspendEvent;
  }
  Request** pp = &requests;
  while (*pp) {
    Request* r = *pp;
    if (r->hasCount and r->count <= 0) {
      *pp = r->next;
      free(r->classMatch);
      free(r->classExclude);
      free(r);
      continue;
    }
    pp = &r->next;
  }
  int tag = rec->typeTag;
  uint64_t id = rec->id;
  int status = rec->status;
  char* sig = dupZ(rec->signature);
  unlockReg();

  if (mc == 0) {
    free(sig);
    return;
  }

  suspendFor(t, policy);
  Buf body;
  bufInit(&body);
  b1(&body, policy);
  b4(&body, mc);
  for (int i = 0; i < mc; ++i) {
    b1(&body, 8);
    b4(&body, ids[i]);
    b8(&body, tid);
    b1(&body, tag);
    b8(&body, id);
    bstr(&body, sig);
    b4(&body, status);
  }
  sendEvent(policy, &body);
  bufFree(&body);
  free(sig);
  if (policy != kSuspendNone)
    block(t);
}

// ---- commands ----------------------------------------------------------

void cmdVersion(uint32_t id)
{
  Buf b;
  bufInit(&b);
  bstr(&b, "Avian JDWP");
  b4(&b, 1);
  b4(&b, 6);
  bstr(&b, AVIAN_VERSION);
  bstr(&b, "Avian");
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdIdSizes(uint32_t id)
{
  Buf b;
  bufInit(&b);
  b4(&b, 8);
  b4(&b, 8);
  b4(&b, 8);
  b4(&b, 8);
  b4(&b, 8);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdCapabilities(uint32_t id, int n)
{
  Buf b;
  bufInit(&b);
  for (int i = 0; i < n; ++i)
    b1(&b, 0);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void writeClass(Buf* b, ClassRec* c, bool generic)
{
  b1(b, c->typeTag);
  b8(b, c->id);
  bstr(b, c->signature);
  if (generic)
    bstr(b, "");
  b4(b, c->status);
}

void cmdAllClasses(uint32_t id, bool generic)
{
  Buf b;
  bufInit(&b);
  lockReg();
  int n = 0;
  for (ClassRec* c = classes; c; c = c->next)
    if (not c->primitive)
      ++n;
  b4(&b, n);
  for (ClassRec* c = classes; c; c = c->next)
    if (not c->primitive)
      writeClass(&b, c, generic);
  unlockReg();
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdClassesBySignature(uint32_t id, Reader* r)
{
  char* sig = rstr(r);
  Buf b;
  bufInit(&b);
  lockReg();
  int n = 0;
  for (ClassRec* c = classes; c; c = c->next)
    if (::strcmp(c->signature, sig) == 0 or ::strcmp(c->name, sig) == 0)
      ++n;
  b4(&b, n);
  for (ClassRec* c = classes; c; c = c->next) {
    if (::strcmp(c->signature, sig) == 0 or ::strcmp(c->name, sig) == 0) {
      b1(&b, c->typeTag);
      b8(&b, c->id);
      b4(&b, c->status);
    }
  }
  unlockReg();
  free(sig);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdAllThreads(uint32_t id)
{
  struct Count {
    int n;
    uint64_t ids[64];
  } c;
  c.n = 0;
  struct Local {
    static void visit(Thread* t, void* arg)
    {
      Count* c = static_cast<Count*>(arg);
      if (c->n < 64 and t->state != Thread::ZombieState
          and t->state != Thread::JoinedState)
        c->ids[c->n++] = reinterpret_cast<uint64_t>(t);
    }
  };
  if (machine_)
    eachThread(machine_->rootThread, Local::visit, &c);
  Buf b;
  bufInit(&b);
  b4(&b, c.n);
  for (int i = 0; i < c.n; ++i)
    b8(&b, c.ids[i]);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdThreadGroups(uint32_t id)
{
  Buf b;
  bufInit(&b);
  b4(&b, 1);
  b8(&b, 1);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void adjustGlobal(int delta)
{
  if (debugMonitor and serverSystemThread)
    debugMonitor->acquire(serverSystemThread);
  int v = loadSuspend() + delta;
  if (v < 0)
    v = 0;
  storeSuspend(v);
  if (debugMonitor and serverSystemThread) {
    debugMonitor->notifyAll(serverSystemThread);
    debugMonitor->release(serverSystemThread);
  }
}

void cmdClassPaths(uint32_t id)
{
  Buf b;
  bufInit(&b);
  bstr(&b, "");
  b4(&b, 0);
  b4(&b, 0);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void writeMethods(Buf* b, ClassRec* c, bool generic)
{
  b4(b, c->methodCount);
  for (int i = 0; i < c->methodCount; ++i) {
    MethodRec* m = &c->methods[i];
    b8(b, m->id);
    bstr(b, m->name);
    bstr(b, m->spec);
    if (generic)
      bstr(b, "");
    b4(b, m->flags);
  }
}

void writeFields(Buf* b, ClassRec* c, bool generic)
{
  b4(b, c->fieldCount);
  for (int i = 0; i < c->fieldCount; ++i) {
    FieldRec* f = &c->fields[i];
    b8(b, f->id);
    bstr(b, f->name);
    bstr(b, f->spec);
    if (generic)
      bstr(b, "");
    b4(b, f->flags);
  }
}

void cmdReference(uint32_t id, unsigned cmd, Reader* r)
{
  uint64_t cid = r8(r);
  lockReg();
  ClassRec* c = findClass(cid);
  if (c == 0) {
    unlockReg();
    sendReply(id, kInvalidClass, 0);
    return;
  }
  Buf b;
  bufInit(&b);
  switch (cmd) {
  case 1:  // Signature
    bstr(&b, c->signature);
    break;
  case 2:  // ClassLoader
    b8(&b, 0);
    break;
  case 3:
    b4(&b, c->flags);
    break;
  case 4:
    writeFields(&b, c, false);
    break;
  case 5:
    writeMethods(&b, c, false);
    break;
  case 7:
    if (c->source == 0 or c->source[0] == 0) {
      unlockReg();
      bufFree(&b);
      sendReply(id, kAbsent, 0);
      return;
    }
    bstr(&b, c->source);
    break;
  case 8:  // NestedTypes
    b4(&b, 0);
    break;
  case 9:
    b4(&b, c->status);
    break;
  case 10:  // Interfaces
    b4(&b, 0);
    break;
  case 11:  // ClassObject
    b8(&b, 0);
    break;
  case 13:
    bstr(&b, c->signature);
    bstr(&b, "");
    break;
  case 14:
    writeFields(&b, c, true);
    break;
  case 15:
    writeMethods(&b, c, true);
    break;
  default:
    unlockReg();
    bufFree(&b);
    sendReply(id, kNotImplemented, 0);
    return;
  }
  unlockReg();
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdSuper(uint32_t id, Reader* r)
{
  uint64_t cid = r8(r);
  lockReg();
  ClassRec* c = findClass(cid);
  uint64_t sid = 0;
  if (c and c->superName) {
    ClassRec* s = findClassByName(c->superName);
    if (s)
      sid = s->id;
  }
  bool ok = c != 0;
  unlockReg();
  if (not ok) {
    sendReply(id, kInvalidClass, 0);
    return;
  }
  Buf b;
  bufInit(&b);
  b8(&b, sid);
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdLineTable(uint32_t id, Reader* r)
{
  // HotSpot JDI writes referenceTypeID then methodID.  A minimal client
  // may send only the methodID.  Accept both.
  unsigned remain = r->n - r->i;
  uint64_t mid;
  if (remain >= 16) {
    r8(r);
    mid = r8(r);
  } else {
    mid = r8(r);
  }
  lockReg();
  ClassRec* cc = 0;
  MethodRec* m = findMethodGlobal(mid, &cc);
  if (m == 0) {
    unlockReg();
    sendReply(id, kInvalidMethod, 0);
    return;
  }
  Buf b;
  bufInit(&b);
  if (m->codeLength < 0) {
    b8(&b, static_cast<uint64_t>(-1));
    b8(&b, static_cast<uint64_t>(-1));
    b4(&b, 0);
  } else {
    int64_t start = 0;
    int64_t end = m->codeLength > 0 ? m->codeLength - 1 : 0;
    b8(&b, static_cast<uint64_t>(start));
    b8(&b, static_cast<uint64_t>(end));
    b4(&b, m->lineCount);
    for (int i = 0; i < m->lineCount; ++i) {
      b8(&b, static_cast<uint64_t>(m->lines[i].bci));
      b4(&b, m->lines[i].line);
    }
  }
  unlockReg();
  sendReply(id, 0, &b);
  bufFree(&b);
}

const char* threadName(Thread* t, char* buf, int cap)
{
  if (t->parent == 0 or (machine_ and t == machine_->rootThread))
    return "main";
  ::snprintf(buf, cap, "Thread-%u", static_cast<unsigned>(reinterpret_cast<uintptr_t>(t) & 0xFFFF));
  return buf;
}

int jdwpThreadStatus(Thread* t)
{
  if (t->state == Thread::ZombieState or t->state == Thread::JoinedState
      or t->state == Thread::ExitState)
    return 0;
  if (t->debugInBlock)
    return 1;
  if (t->state == Thread::IdleState)
    return 4;
  return 1;
}

void cmdThread(uint32_t id, unsigned cmd, Reader* r)
{
  uint64_t tid = r8(r);
  Thread* t = findThread(tid);
  if (t == 0) {
    sendReply(id, kInvalidThread, 0);
    return;
  }
  Buf b;
  bufInit(&b);
  switch (cmd) {
  case 1: {
    char tmp[64];
    bstr(&b, threadName(t, tmp, sizeof tmp));
    break;
  }
  case 2:
    storeThreadSuspend(t, loadThreadSuspend(t) + 1);
    wakeWaiters();
    break;
  case 3: {
    int v = loadThreadSuspend(t) - 1;
    if (v < 0)
      v = 0;
    storeThreadSuspend(t, v);
    wakeWaiters();
    break;
  }
  case 4: {
    int st = jdwpThreadStatus(t);
    int sus = (loadSuspend() > 0 or loadThreadSuspend(t) > 0 or t->debugInBlock) ? 1 : 0;
    b4(&b, st);
    b4(&b, sus);
    break;
  }
  case 5:
    b8(&b, 1);
    break;
  case 6: {
    if (loadSuspend() <= 0 and loadThreadSuspend(t) <= 0 and not t->debugInBlock) {
      bufFree(&b);
      sendReply(id, kThreadNotSuspended, 0);
      return;
    }
    int start = static_cast<int>(r4(r));
    int length = static_cast<int>(r4(r));
    Snap* s = static_cast<Snap*>(t->debugSnap);
    int count = s ? s->count : 0;
    if (start < 0 or start > count) {
      bufFree(&b);
      sendReply(id, kIllegalArgument, 0);
      return;
    }
    int n = count - start;
    if (length >= 0 and length < n)
      n = length;
    b4(&b, n);
    for (int i = 0; i < n; ++i) {
      SnapFrame* f = &s->frames[start + i];
      b8(&b, static_cast<uint64_t>(start + i + 1));
      int tag = 1;
      lockReg();
      ClassRec* c = findClass(f->classId);
      if (c)
        tag = c->typeTag;
      unlockReg();
      b1(&b, tag);
      b8(&b, f->classId);
      b8(&b, f->methodId);
      b8(&b, static_cast<uint64_t>(f->index));
    }
    break;
  }
  case 7: {
    if (loadSuspend() <= 0 and loadThreadSuspend(t) <= 0 and not t->debugInBlock) {
      bufFree(&b);
      sendReply(id, kThreadNotSuspended, 0);
      return;
    }
    Snap* s = static_cast<Snap*>(t->debugSnap);
    b4(&b, s ? s->count : 0);
    break;
  }
  case 12: {
    int n = loadThreadSuspend(t);
    if (loadSuspend() > 0)
      n += loadSuspend();
    if (n == 0 and t->debugInBlock)
      n = 1;
    b4(&b, n);
    break;
  }
  default:
    bufFree(&b);
    sendReply(id, kNotImplemented, 0);
    return;
  }
  sendReply(id, 0, &b);
  bufFree(&b);
}

void cmdGroup(uint32_t id, unsigned cmd, Reader* r)
{
  uint64_t gid = r8(r);
  if (gid != 1) {
    sendReply(id, kInvalidThread, 0);
    return;
  }
  Buf b;
  bufInit(&b);
  if (cmd == 1) {
    bstr(&b, "main");
  } else if (cmd == 2) {
    b8(&b, 0);
  } else if (cmd == 3) {
    struct Count {
      int n;
      uint64_t ids[64];
    } c;
    c.n = 0;
    struct Local {
      static void visit(Thread* t, void* arg)
      {
        Count* c = static_cast<Count*>(arg);
        if (c->n < 64 and t->state != Thread::ZombieState
            and t->state != Thread::JoinedState)
          c->ids[c->n++] = reinterpret_cast<uint64_t>(t);
      }
    };
    if (machine_)
      eachThread(machine_->rootThread, Local::visit, &c);
    b4(&b, c.n);
    for (int i = 0; i < c.n; ++i)
      b8(&b, c.ids[i]);
    b4(&b, 0);
  } else {
    bufFree(&b);
    sendReply(id, kNotImplemented, 0);
    return;
  }
  sendReply(id, 0, &b);
  bufFree(&b);
}

bool consumeModifier(Reader* r, Request* req)
{
  unsigned kind = r1(r);
  switch (kind) {
  case 1:  // Count
    req->hasCount = true;
    req->count = static_cast<int>(r4(r));
    return true;
  case 3:  // ThreadOnly
    req->hasThread = true;
    req->thread = r8(r);
    return true;
  case 4:
    req->hasClass = true;
    req->classId = r8(r);
    return true;
  case 5:
    req->classMatch = rstr(r);
    return true;
  case 6:
    req->classExclude = rstr(r);
    return true;
  case 7:
    r1(r);  // type tag
    req->hasLocation = true;
    req->locClass = r8(r);
    req->locMethod = r8(r);
    req->locIndex = static_cast<int64_t>(r8(r));
    return true;
  case 2:  // Conditional (obsolete): exprID
    r4(r);
    return true;
  case 8:  // ExceptionOnly: type, caught, uncaught. Not delivered.
    r8(r);
    r1(r);
    r1(r);
    return true;
  case 9:  // FieldOnly: declaring type, field. Not delivered.
    r8(r);
    r8(r);
    return true;
  case 10:  // Step
    req->hasStep = true;
    req->stepThread = r8(r);
    req->stepSize = static_cast<int>(r4(r));
    req->stepDepth = static_cast<int>(r4(r));
    return true;
  case 11:  // InstanceOnly
    r8(r);
    return true;
  case 12:  // SourceNameMatch
    free(rstr(r));
    return true;
  default:
    logf("unknown event modifier %u", kind);
    r->error = true;
    return false;
  }
}

void armStep(Request* req)
{
  if (req->kind != 1 or not req->hasStep)
    return;
  Thread* t = findThread(req->stepThread);
  if (t == 0)
    return;
  t->debugStepSize = req->stepSize;
  t->debugStepDepth = req->stepDepth;
  Snap* s = static_cast<Snap*>(t->debugSnap);
  t->debugStepBase = s and s->count ? s->count : (t->debugDepth > 0 ? t->debugDepth : 1);
  t->debugStepMethod = s and s->count ? s->frames[0].methodId : 0;
  t->debugStepLine = -1;
  lockReg();
  ClassRec* cc = 0;
  MethodRec* m = findMethodGlobal(t->debugStepMethod, &cc);
  if (m and s and s->count)
    t->debugStepLine = lineFor(m, static_cast<int32_t>(s->frames[0].index));
  unlockReg();
  __atomic_store_n(&t->debugStepping, 1, __ATOMIC_RELEASE);
}

void cmdEventSet(uint32_t id, Reader* r)
{
  Request* req = static_cast<Request*>(calloc(1, sizeof(Request)));
  req->kind = static_cast<int>(r1(r));
  req->policy = static_cast<int>(r1(r));
  unsigned mods = r4(r);
  for (unsigned i = 0; i < mods; ++i) {
    if (not consumeModifier(r, req))
      break;
  }
  if (r->error) {
    free(req->classMatch);
    free(req->classExclude);
    free(req);
    sendReply(id, kIllegalArgument, 0);
    return;
  }
  lockReg();
  req->id = nextRequestId++;
  req->next = requests;
  requests = req;
  uint32_t rid = req->id;
  if (req->kind == 2 and req->hasLocation) {
    ClassRec* cc = 0;
    MethodRec* m = findMethodGlobal(req->locMethod, &cc);
    patchMethodBits(m);
    __atomic_fetch_add(&breakpointCount, 1, __ATOMIC_RELEASE);
  }
  unlockReg();
  if (req->kind == 1)
    armStep(req);
  Buf b;
  bufInit(&b);
  b4(&b, rid);
  sendReply(id, 0, &b);
  bufFree(&b);
  logf("event set kind %d id %u policy %d", req->kind, rid, req->policy);
}

void cmdEventClear(uint32_t id, Reader* r)
{
  int kind = static_cast<int>(r1(r));
  uint32_t rid = r4(r);
  lockReg();
  Request** pp = &requests;
  while (*pp) {
    if ((*pp)->id == rid and (*pp)->kind == kind) {
      Request* dead = *pp;
      *pp = dead->next;
      if (dead->kind == 2) {
        patchMethodBits(findMethodGlobal(dead->locMethod, 0));
        __atomic_fetch_sub(&breakpointCount, 1, __ATOMIC_RELEASE);
      }
      if (dead->kind == 1) {
        Thread* t = findThread(dead->stepThread);
        if (t)
          __atomic_store_n(&t->debugStepping, 0, __ATOMIC_RELEASE);
      }
      free(dead->classMatch);
      free(dead->classExclude);
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  unlockReg();
  sendReply(id, 0, 0);
}

void cmdClearBreakpoints(uint32_t id)
{
  lockReg();
  Request** pp = &requests;
  while (*pp) {
    if ((*pp)->kind == 2) {
      Request* dead = *pp;
      *pp = dead->next;
      free(dead->classMatch);
      free(dead->classExclude);
      free(dead);
      continue;
    }
    pp = &(*pp)->next;
  }
  for (CompiledSite* s = sites; s; s = s->next)
    applyBits(s);
  __atomic_store_n(&breakpointCount, 0, __ATOMIC_RELEASE);
  unlockReg();
  sendReply(id, 0, 0);
}

void cmdStack(uint32_t id, unsigned cmd, Reader* r)
{
  uint64_t tid = r8(r);
  r8(r);  // frame id, ignored: we don't have locals
  Thread* t = findThread(tid);
  if (t == 0) {
    sendReply(id, kInvalidThread, 0);
    return;
  }
  if (cmd == 3) {
    // ThisObject.  Real receiver identity is not tracked.
    Buf b;
    bufInit(&b);
    b1(&b, 'L');
    b8(&b, 0);
    sendReply(id, 0, &b);
    bufFree(&b);
    return;
  }
  sendReply(id, kAbsent, 0);
}

void dispatch(uint32_t id, unsigned set, unsigned cmd, Reader* r)
{
  logf("cmd set %u cmd %u id %u", set, cmd, id);
  switch (set) {
  case 1:
    switch (cmd) {
    case 1:
      cmdVersion(id);
      return;
    case 2:
      cmdClassesBySignature(id, r);
      return;
    case 3:
      cmdAllClasses(id, false);
      return;
    case 4:
      cmdAllThreads(id);
      return;
    case 5:
      cmdThreadGroups(id);
      return;
    case 6:
      sendReply(id, 0, 0);
      disposed = true;
      resumeAll();
      return;
    case 7:
      cmdIdSizes(id);
      return;
    case 8:
      adjustGlobal(+1);
      sendReply(id, 0, 0);
      return;
    case 9:
      adjustGlobal(-1);
      sendReply(id, 0, 0);
      return;
    case 10: {
      unsigned code = r4(r);
      sendReply(id, 0, 0);
      if (system_)
        system_->exit(static_cast<int>(code));
      ::exit(static_cast<int>(code));
      return;
    }
    case 12:
      cmdCapabilities(id, 7);
      return;
    case 13:
      cmdClassPaths(id);
      return;
    case 14:
      sendReply(id, 0, 0);
      return;
    case 15:
      holdEvents = true;
      sendReply(id, 0, 0);
      return;
    case 16:
      sendReply(id, 0, 0);
      flushHeld();
      return;
    case 17:
      cmdCapabilities(id, 32);
      return;
    case 20:
      cmdAllClasses(id, true);
      return;
    default:
      break;
    }
    break;
  case 2:
    cmdReference(id, cmd, r);
    return;
  case 3:
    if (cmd == 1) {
      cmdSuper(id, r);
      return;
    }
    break;
  case 6:
    if (cmd == 1) {
      cmdLineTable(id, r);
      return;
    }
    if (cmd == 2 or cmd == 5) {
      sendReply(id, kAbsent, 0);
      return;
    }
    break;
  case 11:
    cmdThread(id, cmd, r);
    return;
  case 12:
    cmdGroup(id, cmd, r);
    return;
  case 15:
    if (cmd == 1) {
      cmdEventSet(id, r);
      return;
    }
    if (cmd == 2) {
      cmdEventClear(id, r);
      return;
    }
    if (cmd == 3) {
      cmdClearBreakpoints(id);
      return;
    }
    break;
  case 16:
    cmdStack(id, cmd, r);
    return;
  default:
    break;
  }
  logf("not implemented set %u cmd %u", set, cmd);
  sendReply(id, kNotImplemented, 0);
}

void sendVMStart()
{
  uint64_t tid = 0;
  if (machine_ and machine_->rootThread)
    tid = reinterpret_cast<uint64_t>(machine_->rootThread);
  Buf b;
  bufInit(&b);
  b1(&b, kSuspendAll);
  b4(&b, 1);
  b1(&b, 90);
  b4(&b, 0);
  b8(&b, tid);
  // VMStart is not held; the debugger is not in HoldEvents yet, but
  // be safe and send directly.
  bool saved = holdEvents;
  holdEvents = false;
  sendEvent(kSuspendAll, &b);
  holdEvents = saved;
  bufFree(&b);
}

void closeClient()
{
  if (clientSocket >= 0) {
#ifndef _WIN32
    ::close(clientSocket);
#endif
    clientSocket = -1;
  }
  disposed = true;
  resumeAll();
}

void serveLoop()
{
#ifndef _WIN32
  const char* hello = "JDWP-Handshake";
  if (not writeAll(clientSocket, reinterpret_cast<const uint8_t*>(hello), 14))
    return;
  char got[14];
  if (not readAll(clientSocket, reinterpret_cast<uint8_t*>(got), 14))
    return;
  if (memcmp(got, hello, 14) != 0) {
    fprintf(stderr, "jdwp: bad handshake\n");
    return;
  }
  logf("handshake ok");
  sendVMStart();
  while (not disposed) {
    uint8_t hdr[11];
    if (not readAll(clientSocket, hdr, 11))
      break;
    unsigned length = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
    uint32_t id = (hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];
    unsigned flags = hdr[8];
    if (length < 11 or length > 16 * 1024 * 1024) {
      fprintf(stderr, "jdwp: bad packet length %u\n", length);
      break;
    }
    unsigned bodyLen = length - 11;
    uint8_t* body = bodyLen ? static_cast<uint8_t*>(malloc(bodyLen)) : 0;
    if (bodyLen and (body == 0 or not readAll(clientSocket, body, bodyLen))) {
      free(body);
      break;
    }
    if (flags & 0x80) {
      free(body);
      continue;
    }
    unsigned set = hdr[9];
    unsigned cmd = hdr[10];
    Reader reader;
    reader.p = body;
    reader.n = bodyLen;
    reader.i = 0;
    reader.error = false;
    dispatch(id, set, cmd, &reader);
    free(body);
    if (disposed)
      break;
  }
#endif
}

class Server : public System::Runnable {
 public:
  virtual void attach(System::Thread* st)
  {
    self = st;
    serverSystemThread = st;
  }
  virtual void run()
  {
    runServer();
  }
  virtual bool interrupted()
  {
    return false;
  }
  virtual void setInterrupted(bool)
  {
  }
  System::Thread* self;

  void runServer()
  {
#ifndef _WIN32
    listenSocket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) {
      perror("jdwp socket");
      __atomic_store_n(&boundPort, -1, __ATOMIC_RELEASE);
      return;
    }
    int yes = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#ifdef SO_NOSIGPIPE
    setsockopt(listenSocket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(requestedPort > 0 ? requestedPort : 0));
    if (::bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      perror("jdwp bind");
      __atomic_store_n(&boundPort, -1, __ATOMIC_RELEASE);
      return;
    }
    socklen_t alen = sizeof addr;
    if (::getsockname(listenSocket, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
      perror("jdwp getsockname");
      __atomic_store_n(&boundPort, -1, __ATOMIC_RELEASE);
      return;
    }
    int port = ntohs(addr.sin_port);
    if (::listen(listenSocket, 1) != 0) {
      perror("jdwp listen");
      __atomic_store_n(&boundPort, -1, __ATOMIC_RELEASE);
      return;
    }
    __atomic_store_n(&boundPort, port, __ATOMIC_RELEASE);
    fprintf(stderr, "Listening for transport dt_socket at address: %d\n", port);
    fflush(stderr);
    serverAlive = true;

    sockaddr_in peer;
    socklen_t plen = sizeof peer;
    clientSocket = ::accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (clientSocket < 0) {
      serverAlive = false;
      __atomic_store_n(&boundPort, -1, __ATOMIC_RELEASE);
      return;
    }
    int one = 1;
    setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(clientSocket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    serveLoop();
    closeClient();
    serverAlive = false;
#else
    fprintf(stderr, "jdwp: sockets are not implemented on this platform\n");
    __atomic_store_n(&boundPort, -1, __ATOMIC_RELEASE);
#endif
  }
};

Server* server = 0;

bool parseOptions(const char* options)
{
  bool transportOk = false;
  bool serverOk = false;
  suspendOnConnect = false;
  requestedPort = 0;
  char buf[512];
  ::snprintf(buf, sizeof buf, "%s", options ? options : "");
  char* save = 0;
  for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(0, ",", &save)) {
    char* eq = strchr(tok, '=');
    if (eq == 0)
      continue;
    *eq = 0;
    const char* key = tok;
    const char* val = eq + 1;
    if (::strcmp(key, "transport") == 0) {
      if (::strcmp(val, "dt_socket") != 0) {
        fprintf(stderr, "jdwp: unsupported transport %s\n", val);
        return false;
      }
      transportOk = true;
    } else if (::strcmp(key, "server") == 0) {
      if (::strcmp(val, "y") != 0) {
        fprintf(stderr, "jdwp: only server=y is supported\n");
        return false;
      }
      serverOk = true;
    } else if (::strcmp(key, "suspend") == 0) {
      suspendOnConnect = ::strcmp(val, "y") == 0;
    } else if (::strcmp(key, "address") == 0) {
      const char* colon = strrchr(val, ':');
      const char* portStr = colon ? colon + 1 : val;
      requestedPort = atoi(portStr);
    } else if (::strcmp(key, "onthrow") == 0 or ::strcmp(key, "onuncaught") == 0
               or ::strcmp(key, "launch") == 0 or ::strcmp(key, "timeout") == 0) {
      // accepted and ignored
    } else {
      logf("ignoring option %s", key);
    }
  }
  if (not transportOk)
    transportOk = strstr(options ? options : "", "dt_socket") != 0;
  return transportOk and serverOk;
}

}  // namespace

bool enabled()
{
  return enabledFlag;
}

void configure(const char* options)
{
  const char* env = getenv("AVIAN_JDWP_LOG");
  logPackets = env and env[0] == '1';
  if (options == 0)
    return;
  ::snprintf(jdwpOptionCopy, sizeof jdwpOptionCopy, "%s", options);
  if (parseOptions(options)) {
    enabledFlag = true;
    fprintf(stderr, "jdwp: enabled (%s)\n", jdwpOptionCopy);
  }
}

bool mustSuspend(Thread* t)
{
  if (not enabledFlag or disposed or t == 0)
    return false;
  return loadSuspend() > 0 or loadThreadSuspend(t) > 0;
}

void safepoint(Thread* t)
{
  if (mustSuspend(t))
    block(t);
}

void checkpoint(Thread* t, int32_t bci, uintptr_t bits, void* method)
{
  checkpointImpl(t, bci, bits, static_cast<GcMethod*>(method));
}

void setCodeMemory(void* memory)
{
  codeMemory = static_cast<avian::system::CodeMemory*>(memory);
}

uint8_t* allocateBits(Thread* t, unsigned length)
{
  if (length == 0)
    return 0;
  unsigned size = kHeader + length;
  uint8_t* block = 0;
  avian::system::CodeMemory* mem = codeMemory;
  if (mem)
    block = mem->allocate(size);
  bool plain = false;
  if (block == 0) {
    block = static_cast<uint8_t*>(malloc(size));
    plain = true;
    mem = 0;
  }
  if (block == 0)
    return 0;
  uint8_t* stage = static_cast<uint8_t*>(calloc(1, size));
  if (stage == 0) {
    if (plain)
      free(block);
    return 0;
  }
  uint32_t len = length;
  memcpy(stage + 8, &len, 4);
  if (mem) {
    mem->commit(block, stage, size);
    free(stage);
  } else {
    memcpy(block, stage, size);
    free(stage);
  }
  (void)t;
  // Remember that this block's patches go through `mem` (null = plain).
  // Stash it in a tiny side record keyed by the bytes pointer.  The
  // CompiledSite created in publishCompiled is the long-term owner.
  return block + kHeader;
}

void publishCompiled(Thread* t, GcMethod* method, uint8_t* bits, unsigned length, int32_t* map)
{
  if (method == 0 or bits == 0) {
    free(map);
    return;
  }
  CompiledSite* site = static_cast<CompiledSite*>(calloc(1, sizeof(CompiledSite)));
  if (site == 0) {
    free(map);
    return;
  }
  char cn[256], mn[256], sp[256];
  fillNames(method, cn, mn, sp, 256);
  site->className = dupZ(cn);
  site->methodName = dupZ(mn);
  site->spec = dupZ(sp);
  site->bits = bits;
  site->bitsLength = length;
  site->memory = codeMemory;
  // If the header was malloc'd (commit wasn't used), codeMemory may
  // still be non-null.  Detect a plain block by seeing whether the
  // length word is readable and the pointer lies outside code memory.
  if (site->memory and not site->memory->contains(bits))
    site->memory = 0;

  int n = 0;
  for (unsigned i = 0; i < length; ++i)
    if (map and map[i] >= 0)
      ++n;
  site->mapCount = n;
  if (n) {
    site->offset = static_cast<int32_t*>(malloc(sizeof(int32_t) * n));
    site->bciByOffset = static_cast<int32_t*>(malloc(sizeof(int32_t) * n));
    int k = 0;
    for (unsigned i = 0; i < length; ++i) {
      if (map and map[i] >= 0) {
        site->offset[k] = map[i];
        site->bciByOffset[k] = static_cast<int32_t>(i);
        ++k;
      }
    }
    // insertion sort by offset
    for (int i = 1; i < n; ++i) {
      int32_t o = site->offset[i];
      int32_t b = site->bciByOffset[i];
      int j = i;
      while (j > 0 and site->offset[j - 1] > o) {
        site->offset[j] = site->offset[j - 1];
        site->bciByOffset[j] = site->bciByOffset[j - 1];
        --j;
      }
      site->offset[j] = o;
      site->bciByOffset[j] = b;
    }
  }
  free(map);

  lockReg();
  site->next = sites;
  sites = site;
  MethodRec* mr = findMethodByName(cn, mn, sp);
  if (mr) {
    mr->site = site;
    site->methodId = mr->id;
    uint64_t bits64 = reinterpret_cast<uint64_t>(site);
    uint8_t* header = site->bits - kHeader;
    if (site->memory)
      site->memory->patch(header, &bits64, 8);
    else
      memcpy(header, &bits64, 8);
    applyBits(site);
  }
  unlockReg();
  (void)t;
}

int machineOffsetToBci(const char* className, const char* methodName,
                       const char* spec, int32_t machineOffset)
{
  lockReg();
  CompiledSite* s = findSite(className, methodName, spec);
  int bci = -1;
  if (s and s->mapCount > 0) {
    // greatest offset <= machineOffset
    int lo = 0;
    int hi = s->mapCount - 1;
    int best = -1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (s->offset[mid] <= machineOffset) {
        best = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    if (best >= 0)
      bci = s->bciByOffset[best];
  }
  unlockReg();
  return bci;
}

void registerWalker(Walker walker)
{
  walkerFn = walker;
}

ClassPrepareNotifier::ClassPrepareNotifier(Thread* t, GcClass** slot)
    : t(t), slot(slot), armed(false)
{
}

ClassPrepareNotifier::~ClassPrepareNotifier()
{
  if (not armed or slot == 0 or *slot == 0 or not enabledFlag)
    return;
  ClassRec* rec = ingest(t, *slot);
  postClassPrepare(t, rec);
}

void ClassPrepareNotifier::arm()
{
  if (enabledFlag)
    armed = true;
}

void noteClassStatus(Thread* t, GcClass* c, bool initialized, bool error)
{
  if (not enabledFlag or c == 0 or c->name() == 0)
    return;
  char* name = dupBytes(c->name());
  lockReg();
  ClassRec* rec = findClassByName(name);
  if (rec) {
    if (initialized)
      rec->status |= kStatusInitialized;
    if (error)
      rec->status |= kStatusError;
  }
  unlockReg();
  free(name);
  (void)t;
}

void boot(Thread* t)
{
  if (not enabledFlag)
    return;
  system_ = t->m->system;
  machine_ = t->m;
  const char* env = getenv("AVIAN_JDWP_LOG");
  if (env and env[0] == '1')
    logPackets = true;

  scanClasses(t);

#ifndef _WIN32
  if (not t->m->system->success(t->m->system->make(&debugMonitor))) {
    fprintf(stderr, "jdwp: cannot create monitor\n");
    enabledFlag = false;
    return;
  }
  if (not t->m->system->success(t->m->system->make(&writeLock))) {
    fprintf(stderr, "jdwp: cannot create mutex\n");
    enabledFlag = false;
    return;
  }
  // Arm suspend before the socket accepts.  A debugger that resumes
  // during the handshake must decrement a count that is already 1,
  // otherwise that resume is lost and boot blocks forever.
  if (suspendOnConnect)
    storeSuspend(1);
  server = new (t->m->system->tryAllocate(sizeof(Server))) Server;
  if (server == 0 or not t->m->system->success(t->m->system->start(server))) {
    fprintf(stderr, "jdwp: cannot start server thread\n");
    storeSuspend(0);
    enabledFlag = false;
    return;
  }
  while (__atomic_load_n(&boundPort, __ATOMIC_ACQUIRE) == 0)
    t->m->system->yield();
  if (boundPort < 0) {
    fprintf(stderr, "jdwp: failed to listen\n");
    storeSuspend(0);
    enabledFlag = false;
    return;
  }
  if (suspendOnConnect)
    block(t);
#else
  fprintf(stderr, "jdwp: not supported on this platform\n");
  enabledFlag = false;
#endif
}

void shutdown(Thread* t)
{
  if (not enabledFlag)
    return;
  if (clientSocket >= 0 and not disposed) {
    Buf b;
    bufInit(&b);
    b1(&b, kSuspendNone);
    b4(&b, 1);
    b1(&b, 99);
    b4(&b, 0);
    bool saved = holdEvents;
    holdEvents = false;
    sendEvent(kSuspendNone, &b);
    holdEvents = saved;
    bufFree(&b);
  }
  disposed = true;
  resumeAll();
  (void)t;
}

}  // namespace debug
}  // namespace vm
