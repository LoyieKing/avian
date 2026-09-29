/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include <avian/codegen/jit-debug.h>
#include <avian/system/debugger.h>

#include <stdlib.h>
#include <string.h>

#include <new>

// ---------------------------------------------------------------------------
// The GDB JIT interface.  These two symbols are the whole protocol: the
// debugger puts a breakpoint on __jit_debug_register_code, and each time
// it is called reads __jit_debug_descriptor to find the object file that
// was just added or is about to be removed.  On attach it walks the list
// of everything registered so far.  See "JIT Compilation Interface" in
// the gdb manual.  They must keep these exact names and C linkage, and
// be visible in the dynamic symbol table (release builds are stripped).

extern "C" {

enum JitActions { JIT_NOACTION = 0, JIT_REGISTER_FN, JIT_UNREGISTER_FN };

struct jit_code_entry {
  jit_code_entry* next_entry;
  jit_code_entry* prev_entry;
  const char* symfile_addr;
  uint64_t symfile_size;
};

struct jit_descriptor {
  uint32_t version;
  uint32_t action_flag;
  jit_code_entry* relevant_entry;
  jit_code_entry* first_entry;
};

__attribute__((noinline, used, visibility("default"))) void
    __jit_debug_register_code()
{
  // Must not be optimized away or merged; the debugger breaks here.
  __asm__ __volatile__("" ::: "memory");
}

__attribute__((used, visibility("default"))) jit_descriptor
    __jit_debug_descriptor = {1, JIT_NOACTION, 0, 0};

}  // extern "C"

namespace avian {
namespace codegen {

namespace {

// ---------------------------------------------------------------------------
// Host description.  Generated code always runs on the host, so this is
// fixed at build time.  DWARF register numbers are from each
// architecture's psABI.

struct Elf32 {
  typedef uint32_t Addr;
  typedef uint32_t Off;
  typedef uint32_t Xword;
  static const uint8_t Class = 1;
};

struct Elf64 {
  typedef uint64_t Addr;
  typedef uint64_t Off;
  typedef uint64_t Xword;
  static const uint8_t Class = 2;
};

#if defined(__x86_64__)
typedef Elf64 Elf;
const uint16_t Machine = 62;  // EM_X86_64
const uint32_t MachineFlags = 0;
const unsigned StackRegister = 7;          // rsp
const unsigned ReturnAddressRegister = 16;  // return address column
const unsigned CodeAlignment = 1;
const unsigned EntryCfaOffset = 8;  // the call pushed the return address
#define AVIAN_JIT_DEBUG_SUPPORTED 1
#elif defined(__i386__)
typedef Elf32 Elf;
const uint16_t Machine = 3;  // EM_386
const uint32_t MachineFlags = 0;
const unsigned StackRegister = 4;          // esp
const unsigned ReturnAddressRegister = 8;  // return address column
const unsigned CodeAlignment = 1;
const unsigned EntryCfaOffset = 4;
#define AVIAN_JIT_DEBUG_SUPPORTED 1
#elif defined(__aarch64__)
typedef Elf64 Elf;
const uint16_t Machine = 183;  // EM_AARCH64
const uint32_t MachineFlags = 0;
const unsigned StackRegister = 31;          // sp
const unsigned ReturnAddressRegister = 30;  // x30 (lr)
const unsigned CodeAlignment = 4;
const unsigned EntryCfaOffset = 0;  // return address is in lr
#define AVIAN_JIT_DEBUG_SUPPORTED 1
#elif defined(__arm__)
typedef Elf32 Elf;
const uint16_t Machine = 40;               // EM_ARM
const uint32_t MachineFlags = 0x05000000;  // EF_ARM_EABI_VER5
const unsigned StackRegister = 13;          // sp
const unsigned ReturnAddressRegister = 14;  // lr
const unsigned CodeAlignment = 4;           // ARM state
const unsigned EntryCfaOffset = 0;
#define AVIAN_JIT_DEBUG_SUPPORTED 1
#endif

#ifdef AVIAN_JIT_DEBUG_SUPPORTED

const unsigned WordSize = sizeof(Elf::Addr);
const bool ReturnAddressOnStackAtEntry = EntryCfaOffset != 0;

// ---------------------------------------------------------------------------
// ELF, the subset we need.  Field order and natural alignment give the
// on-disk layout for both classes.

struct Ehdr {
  uint8_t ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  Elf::Addr entry;
  Elf::Off phoff;
  Elf::Off shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
};

struct Shdr {
  uint32_t name;
  uint32_t type;
  Elf::Xword flags;
  Elf::Addr addr;
  Elf::Off offset;
  Elf::Xword size;
  uint32_t link;
  uint32_t info;
  Elf::Xword addralign;
  Elf::Xword entsize;
};

#if defined(__x86_64__) || defined(__aarch64__)
struct Sym {
  uint32_t name;
  uint8_t info;
  uint8_t other;
  uint16_t shndx;
  Elf::Addr value;
  Elf::Xword size;
};
#else
struct Sym {
  uint32_t name;
  Elf::Addr value;
  Elf::Xword size;
  uint8_t info;
  uint8_t other;
  uint16_t shndx;
};
#endif

const uint16_t ET_REL = 1;
const uint32_t SHT_PROGBITS = 1;
const uint32_t SHT_SYMTAB = 2;
const uint32_t SHT_STRTAB = 3;
const uint32_t SHT_NOBITS = 8;
const uint32_t SHF_ALLOC = 2;
const uint32_t SHF_EXECINSTR = 4;
const uint8_t STB_GLOBAL = 1;
const uint8_t STT_FUNC = 2;

enum Section { NullSection, Text, EhFrame, Symtab, Strtab, Shstrtab, SectionCount };

const char SectionNames[] = "\0.text\0.eh_frame\0.symtab\0.strtab\0.shstrtab";
const uint32_t SectionNameOffsets[SectionCount] = {0, 1, 7, 17, 25, 33};

// ---------------------------------------------------------------------------
// DWARF call frame information, in .eh_frame form.

const uint8_t DW_CFA_nop = 0x00;
const uint8_t DW_CFA_advance_loc = 0x40;
const uint8_t DW_CFA_offset = 0x80;
const uint8_t DW_CFA_advance_loc1 = 0x02;
const uint8_t DW_CFA_advance_loc2 = 0x03;
const uint8_t DW_CFA_advance_loc4 = 0x04;
const uint8_t DW_CFA_same_value = 0x08;
const uint8_t DW_CFA_def_cfa = 0x0c;
const uint8_t DW_CFA_def_cfa_offset = 0x0e;

const uint8_t DW_EH_PE_udata4 = 0x03;
const uint8_t DW_EH_PE_udata8 = 0x04;
const uint8_t DW_EH_PE_pcrel = 0x10;

// Code addresses are encoded relative to the field holding them, with
// the full word size: code and this object may be further apart than
// 32 bits reach.
const uint8_t AddressEncoding
    = DW_EH_PE_pcrel | (WordSize == 8 ? DW_EH_PE_udata8 : DW_EH_PE_udata4);

class Bytes {
 public:
  Bytes(util::Alloc* allocator)
      : allocator(allocator), data(0), size(0), capacity(0)
  {
  }

  ~Bytes()
  {
    if (data) {
      allocator->free(data, capacity);
    }
  }

  void append(const void* p, size_t n)
  {
    if (size + n > capacity) {
      size_t newCapacity = capacity ? capacity * 2 : 256;
      while (newCapacity < size + n) {
        newCapacity *= 2;
      }
      uint8_t* newData
          = static_cast<uint8_t*>(allocator->allocate(newCapacity));
      if (data) {
        memcpy(newData, data, size);
        allocator->free(data, capacity);
      }
      data = newData;
      capacity = newCapacity;
    }
    memcpy(data + size, p, n);
    size += n;
  }

  void u8(uint8_t v)
  {
    append(&v, 1);
  }

  void u16(uint16_t v)
  {
    append(&v, 2);
  }

  void u32(uint32_t v)
  {
    append(&v, 4);
  }

  void word(Elf::Addr v)
  {
    append(&v, sizeof(v));
  }

  void uleb(uint64_t v)
  {
    do {
      uint8_t b = v & 0x7f;
      v >>= 7;
      u8(v ? b | 0x80 : b);
    } while (v);
  }

  void sleb(int64_t v)
  {
    bool more = true;
    while (more) {
      uint8_t b = v & 0x7f;
      v >>= 7;
      more = not((v == 0 and (b & 0x40) == 0) or (v == -1 and (b & 0x40)));
      u8(more ? b | 0x80 : b);
    }
  }

  void string(const char* s)
  {
    append(s, strlen(s) + 1);
  }

  void setU32(size_t at, uint32_t v)
  {
    memcpy(data + at, &v, 4);
  }

  util::Alloc* allocator;
  uint8_t* data;
  size_t size;
  size_t capacity;
};

// Opens a CIE or FDE: reserves its length field.
size_t beginEntry(Bytes* b)
{
  size_t start = b->size;
  b->u32(0);
  return start;
}

// Pads to the word size with DW_CFA_nop and fills in the length.
void endEntry(Bytes* b, size_t start)
{
  while ((b->size - start) % WordSize) {
    b->u8(DW_CFA_nop);
  }
  b->setU32(start, b->size - start - 4);
}

void advanceLocation(Bytes* b, unsigned bytes)
{
  unsigned delta = bytes / CodeAlignment;
  if (delta == 0) {
    return;
  } else if (delta < 0x40) {
    b->u8(DW_CFA_advance_loc | delta);
  } else if (delta <= 0xff) {
    b->u8(DW_CFA_advance_loc1);
    b->u8(delta);
  } else if (delta <= 0xffff) {
    b->u8(DW_CFA_advance_loc2);
    b->u16(delta);
  } else {
    b->u8(DW_CFA_advance_loc4);
    b->u32(delta);
  }
}

// A pc-relative field in .eh_frame, filled in once the object's final
// address is known.
struct Fixup {
  size_t offset;
  const uint8_t* target;
};

// Builds .eh_frame for `symbols` into `b`; `fixups` (count entries)
// receives the position of each FDE's initial location.
void buildEhFrame(Bytes* b, const JitSymbol* symbols, unsigned count,
                  Fixup* fixups)
{
  // CIE: the state at entry to any piece of generated code.
  size_t cie = beginEntry(b);
  b->u32(0);  // CIE id
  b->u8(1);   // version
  b->string("zR");
  b->uleb(CodeAlignment);
  b->sleb(-static_cast<int>(WordSize));
  b->u8(ReturnAddressRegister);
  b->uleb(1);  // augmentation data length
  b->u8(AddressEncoding);
  b->u8(DW_CFA_def_cfa);
  b->uleb(StackRegister);
  b->uleb(EntryCfaOffset);
  if (ReturnAddressOnStackAtEntry) {
    b->u8(DW_CFA_offset | ReturnAddressRegister);
    b->uleb(1);  // at CFA - 1 word
  } else {
    b->u8(DW_CFA_same_value);
    b->uleb(ReturnAddressRegister);
  }
  endEntry(b, cie);

  for (unsigned i = 0; i < count; ++i) {
    const JitSymbol* s = symbols + i;

    size_t fde = beginEntry(b);
    b->u32(b->size - cie);  // CIE pointer: distance back to the CIE
    fixups[i].offset = b->size;
    fixups[i].target = s->start;
    b->word(0);        // initial location, pc-relative (fixed up)
    b->word(s->size);  // address range
    b->uleb(0);        // augmentation data length

    if (s->frameSize) {
      advanceLocation(b, s->prologueSize);
      b->u8(DW_CFA_def_cfa_offset);
      b->uleb(s->frameSize);
      b->u8(DW_CFA_offset | ReturnAddressRegister);
      b->uleb(1);
    }

    endEntry(b, fde);
  }

  b->u32(0);  // terminator
}

size_t alignUp(size_t n, size_t alignment)
{
  return (n + alignment - 1) & ~(alignment - 1);
}

// ---------------------------------------------------------------------------
// Registration

// One registered object: the descriptor entry, then the ELF image.
struct Registration {
  jit_code_entry entry;
  Registration* nextOwned;
  size_t allocationSize;
};

// Serializes all changes to __jit_debug_descriptor in the process.
int registrationLock;

void lock()
{
  while (__atomic_test_and_set(&registrationLock, __ATOMIC_ACQUIRE)) {
  }
}

void unlock()
{
  __atomic_clear(&registrationLock, __ATOMIC_RELEASE);
}

class MyJitDebugInfo final : public JitDebugInfo {
 public:
  MyJitDebugInfo(util::Alloc* allocator) : allocator(allocator), owned(0)
  {
  }

  virtual void add(const JitSymbol* symbols, unsigned count)
  {
    if (count == 0) {
      return;
    }

    // The extent of .text.
    const uint8_t* low = symbols[0].start;
    const uint8_t* high = symbols[0].start + symbols[0].size;
    for (unsigned i = 0; i < count; ++i) {
      if (symbols[i].start < low) {
        low = symbols[i].start;
      }
      if (symbols[i].start + symbols[i].size > high) {
        high = symbols[i].start + symbols[i].size;
      }
    }

    Bytes ehFrame(allocator);
    Fixup* fixups
        = static_cast<Fixup*>(allocator->allocate(sizeof(Fixup) * count));
    buildEhFrame(&ehFrame, symbols, count, fixups);

    // Layout: header, .eh_frame, .symtab, .strtab, .shstrtab, section
    // headers.
    size_t symtabCount = count + 1;
    size_t ehFrameOffset = alignUp(sizeof(Ehdr), WordSize);
    size_t symtabOffset = alignUp(ehFrameOffset + ehFrame.size, WordSize);
    size_t strtabOffset = symtabOffset + symtabCount * sizeof(Sym);

    size_t strtabSize = 1;
    for (unsigned i = 0; i < count; ++i) {
      strtabSize += strlen(symbols[i].name) + 1;
    }

    size_t shstrtabOffset = strtabOffset + strtabSize;
    size_t shdrOffset
        = alignUp(shstrtabOffset + sizeof(SectionNames), WordSize);
    size_t imageSize = shdrOffset + SectionCount * sizeof(Shdr);

    size_t headerSize = alignUp(sizeof(Registration), 16);
    size_t allocationSize = headerSize + imageSize;
    uint8_t* memory = static_cast<uint8_t*>(allocator->allocate(allocationSize));
    memset(memory, 0, allocationSize);

    Registration* r = reinterpret_cast<Registration*>(memory);
    uint8_t* image = memory + headerSize;
    r->allocationSize = allocationSize;

    // ELF header.
    Ehdr* ehdr = reinterpret_cast<Ehdr*>(image);
    static const uint8_t Magic[4] = {0x7f, 'E', 'L', 'F'};
    memcpy(ehdr->ident, Magic, 4);
    ehdr->ident[4] = Elf::Class;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    ehdr->ident[5] = 2;
#else
    ehdr->ident[5] = 1;
#endif
    ehdr->ident[6] = 1;  // EV_CURRENT
    ehdr->type = ET_REL;
    ehdr->machine = Machine;
    ehdr->version = 1;
    ehdr->shoff = shdrOffset;
    ehdr->flags = MachineFlags;
    ehdr->ehsize = sizeof(Ehdr);
    ehdr->shentsize = sizeof(Shdr);
    ehdr->shnum = SectionCount;
    ehdr->shstrndx = Shstrtab;

    // .eh_frame, with its pc-relative fields resolved against where it
    // now lives.
    uint8_t* ehFrameData = image + ehFrameOffset;
    memcpy(ehFrameData, ehFrame.data, ehFrame.size);
    for (unsigned i = 0; i < count; ++i) {
      Elf::Addr value = reinterpret_cast<uintptr_t>(fixups[i].target)
                        - reinterpret_cast<uintptr_t>(ehFrameData
                                                      + fixups[i].offset);
      memcpy(ehFrameData + fixups[i].offset, &value, sizeof(value));
    }
    allocator->free(fixups, sizeof(Fixup) * count);

    // .symtab and .strtab.  In a relocatable object a symbol's value is
    // its offset in its section.
    Sym* syms = reinterpret_cast<Sym*>(image + symtabOffset);
    char* strings = reinterpret_cast<char*>(image + strtabOffset);
    size_t stringOffset = 1;
    for (unsigned i = 0; i < count; ++i) {
      Sym* sym = syms + i + 1;
      size_t length = strlen(symbols[i].name) + 1;
      memcpy(strings + stringOffset, symbols[i].name, length);
      sym->name = stringOffset;
      sym->info = (STB_GLOBAL << 4) | STT_FUNC;
      sym->shndx = Text;
      sym->value = symbols[i].start - low;
      sym->size = symbols[i].size;
      stringOffset += length;
    }

    memcpy(image + shstrtabOffset, SectionNames, sizeof(SectionNames));

    // Section headers.  .text holds no bytes (the code is in the
    // process already); its address is where the code is.
    Shdr* shdrs = reinterpret_cast<Shdr*>(image + shdrOffset);
    for (unsigned i = 0; i < SectionCount; ++i) {
      shdrs[i].name = SectionNameOffsets[i];
    }

    shdrs[Text].type = SHT_NOBITS;
    shdrs[Text].flags = SHF_ALLOC | SHF_EXECINSTR;
    shdrs[Text].addr = reinterpret_cast<uintptr_t>(low);
    shdrs[Text].offset = sizeof(Ehdr);
    shdrs[Text].size = high - low;
    shdrs[Text].addralign = 16;

    shdrs[EhFrame].type = SHT_PROGBITS;
    shdrs[EhFrame].flags = SHF_ALLOC;
    shdrs[EhFrame].addr = reinterpret_cast<uintptr_t>(ehFrameData);
    shdrs[EhFrame].offset = ehFrameOffset;
    shdrs[EhFrame].size = ehFrame.size;
    shdrs[EhFrame].addralign = WordSize;

    shdrs[Symtab].type = SHT_SYMTAB;
    shdrs[Symtab].offset = symtabOffset;
    shdrs[Symtab].size = symtabCount * sizeof(Sym);
    shdrs[Symtab].link = Strtab;
    shdrs[Symtab].info = 1;  // index of the first global symbol
    shdrs[Symtab].addralign = WordSize;
    shdrs[Symtab].entsize = sizeof(Sym);

    shdrs[Strtab].type = SHT_STRTAB;
    shdrs[Strtab].offset = strtabOffset;
    shdrs[Strtab].size = strtabSize;
    shdrs[Strtab].addralign = 1;

    shdrs[Shstrtab].type = SHT_STRTAB;
    shdrs[Shstrtab].offset = shstrtabOffset;
    shdrs[Shstrtab].size = sizeof(SectionNames);
    shdrs[Shstrtab].addralign = 1;

    r->entry.symfile_addr = reinterpret_cast<const char*>(image);
    r->entry.symfile_size = imageSize;

    lock();

    r->nextOwned = owned;
    owned = r;

    r->entry.prev_entry = 0;
    r->entry.next_entry = __jit_debug_descriptor.first_entry;
    if (r->entry.next_entry) {
      r->entry.next_entry->prev_entry = &(r->entry);
    }
    __jit_debug_descriptor.first_entry = &(r->entry);
    __jit_debug_descriptor.relevant_entry = &(r->entry);
    __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
    __jit_debug_register_code();

    unlock();
  }

  virtual void dispose()
  {
    while (owned) {
      Registration* r = owned;

      lock();

      owned = r->nextOwned;

      jit_code_entry* e = &(r->entry);
      if (e->prev_entry) {
        e->prev_entry->next_entry = e->next_entry;
      } else {
        __jit_debug_descriptor.first_entry = e->next_entry;
      }
      if (e->next_entry) {
        e->next_entry->prev_entry = e->prev_entry;
      }
      __jit_debug_descriptor.relevant_entry = e;
      __jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
      __jit_debug_register_code();

      unlock();

      allocator->free(r, r->allocationSize);
    }

    util::Alloc* a = allocator;
    this->~MyJitDebugInfo();
    a->free(this, sizeof(*this));
  }

 private:
  util::Alloc* allocator;
  Registration* owned;
};

#endif  // AVIAN_JIT_DEBUG_SUPPORTED

bool wanted()
{
  const char* setting = getenv("AVIAN_JIT_DEBUG_INFO");
  if (setting and *setting) {
    return strcmp(setting, "0") != 0;
  }

#ifndef NDEBUG
  return true;
#else
  return system::debuggerAttached();
#endif
}

}  // namespace

JitDebugInfo* makeJitDebugInfo(util::Alloc* allocator)
{
#ifdef AVIAN_JIT_DEBUG_SUPPORTED
  if (wanted()) {
    return new (allocator->allocate(sizeof(MyJitDebugInfo)))
        MyJitDebugInfo(allocator);
  }
#else
  (void)allocator;
  (void)wanted;
#endif
  return 0;
}

}  // namespace codegen
}  // namespace avian
