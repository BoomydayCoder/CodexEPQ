#include "quick_arm.hpp"
#include "epq_runtime.hpp"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace epq::arm {
namespace {
constexpr unsigned scratch = 16;
constexpr unsigned sp = 31;
constexpr std::size_t unbound = std::numeric_limits<std::size_t>::max();

void checkWordRegister(unsigned reg) {
    if (reg >= 31) throw std::logic_error("Invalid AArch64 value register");
}

std::size_t byteOffset(std::size_t index) {
    if (index > std::numeric_limits<std::size_t>::max() / 8) {
        throw std::length_error("EPQ function frame too large");
    }
    return index * 8;
}

bool fitsSigned(std::int64_t value, unsigned bits) {
    const std::int64_t limit = std::int64_t{1} << (bits - 1);
    return value >= -limit && value < limit;
}

std::uint32_t branchOffset(std::int64_t delta, unsigned bits) {
    return static_cast<std::uint32_t>(delta) & ((std::uint32_t{1} << bits) - 1);
}
}

Image::Image(void* memory, std::size_t length, std::vector<std::size_t> expandedBranches)
    : memory_(memory), length_(length), expandedBranches_(std::move(expandedBranches)) {}

Image::Image(Image&& other) noexcept
    : memory_(std::exchange(other.memory_, nullptr)), length_(other.length_),
      expandedBranches_(std::move(other.expandedBranches_)) {}

Image::~Image() {
    if (memory_) munmap(memory_, length_);
}

void* Image::instruction(std::size_t offset) const {
    const auto index = offset / 4;
    if (!memory_ || index >= length_ / 4 - expandedBranches_.size() || (offset & 3) != 0) {
        throw std::out_of_range("Invalid EPQ function entry");
    }
    const auto extra = std::lower_bound(expandedBranches_.begin(), expandedBranches_.end(), index)
                     - expandedBranches_.begin();
    return static_cast<unsigned char*>(memory_) + offset + extra * 4;
}

void Assembler::immediate(unsigned reg, std::uint64_t value) {
    checkWordRegister(reg);
    // Use MOVN for predominantly negative values (especially common for EPQ
    // integers). Subsequent MOVK instructions replace individual halfwords.
    unsigned zeros = 0, ones = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto lane = static_cast<std::uint16_t>(value >> (16 * i));
        zeros += lane == 0;
        ones += lane == 0xffff;
    }
    const bool invert = ones > zeros;
    const unsigned fill = invert ? 0xffff : 0;
    bool started = false;
    for (unsigned i = 0; i < 4; ++i) {
        const auto lane = static_cast<std::uint16_t>(value >> (16 * i));
        if (lane == fill) continue;
        const auto imm = (invert && !started) ? static_cast<std::uint16_t>(~lane) : lane;
        const std::uint32_t opcode = started ? 0xf2800000 : invert ? 0x92800000 : 0xd2800000;
        word(opcode | (i << 21) | (std::uint32_t{imm} << 5) | reg);
        started = true;
    }
    if (!started) word((invert ? 0x92800000 : 0xd2800000) | reg);
}

void Assembler::mov(unsigned to, unsigned from) {
    checkWordRegister(to);
    checkWordRegister(from);
    if (to != from) word(0xaa0003e0 | (from << 16) | to); // ORR Xd, XZR, Xm
}

void Assembler::prologue(std::size_t slots, unsigned registerSlots) {
    if (registerSlots > 8 || registerSlots > slots) {
        throw std::logic_error("Invalid EPQ register allocation");
    }
    frameSlots_ = slots;
    registerSlots_ = registerSlots;
    const auto bytes = byteOffset(slots - registerSlots);
    if (bytes > std::numeric_limits<std::size_t>::max() - 15) {
        throw std::length_error("EPQ function frame too large");
    }
    const auto aligned = (bytes + 15) & ~std::size_t{15};
    word(0xa9bf7bfd); // STP X29, X30, [SP, #-16]!
    word(0x910003fd); // MOV X29, SP
    word(0xa9bf53f3); // STP X19, X20, [SP, #-16]!
    for (unsigned i = 0; i < registerSlots; i += 2) {
        const unsigned first = 21 + i;
        const unsigned next = i + 1 < registerSlots ? first + 1 : 31;
        word(0xa9bf03e0 | (next << 10) | first); // STP first,next,[SP,#-16]!
    }
    mov(19, 0);
    mov(20, 1);
    // Probe large frames one 4 KiB interval at a time so the stack guard
    // remains effective, including on systems with larger hardware pages.
    if (aligned >= 4096) {
        immediate(scratch, aligned >> 12);
        word(0xd14007ff); // SUB SP, SP, #1, LSL #12
        word(0xf90003ff); // STR XZR, [SP]
        word(0xf1000610); // SUBS X16, X16, #1
        word(0x54ffffa1); // B.NE -12
    }
    const unsigned tail = aligned & 4095;
    if (tail) word(0xd10003ff | (tail << 10)); // SUB SP, SP, #tail
}

void Assembler::epilogue() {
    for (unsigned i = 0; i < registerSlots_; i += 2) {
        const unsigned first = 21 + i;
        const unsigned next = i + 1 < registerSlots_ ? first + 1 : 31;
        const unsigned scaledOffset = (0u - 4u - i) & 127u;
        word(0xa94003a0 | (scaledOffset << 15) | (next << 10) | first); // LDP from X29
    }
    word(0xa97f53b3); // LDP X19, X20, [X29, #-16]
    word(0x910003bf); // MOV SP, X29
    word(0xa8c17bfd); // LDP X29, X30, [SP], #16
    word(0xd65f03c0); // RET
}

void Assembler::offsetAddress(unsigned reg, unsigned base, std::size_t index) {
    const auto bytes = byteOffset(index);
    if (bytes <= 4095) {
        word(0x91000000 | (static_cast<unsigned>(bytes) << 10) | (base << 5) | reg);
        return;
    }
    // ADD (extended register) permits SP as the base unlike its shifted form.
    const unsigned tmp = base == scratch ? 17 : scratch;
    immediate(tmp, bytes);
    word(0x8b206000 | (tmp << 16) | (base << 5) | reg);
}

void Assembler::loadFrom(unsigned reg, unsigned base, std::size_t index) {
    checkWordRegister(reg);
    if (index <= 4095) {
        word(0xf9400000 | (static_cast<unsigned>(index) << 10) | (base << 5) | reg);
    } else {
        offsetAddress(scratch, base, index);
        word(0xf9400000 | (scratch << 5) | reg);
    }
}

void Assembler::load(std::size_t slot, unsigned reg) {
    if (slot >= frameSlots_) throw std::logic_error("EPQ load outside frame");
    if (slot < registerSlots_) mov(reg, 21 + static_cast<unsigned>(slot));
    else loadFrom(reg, sp, slot - registerSlots_);
}

void Assembler::store(unsigned reg, std::size_t slot) {
    if (slot >= frameSlots_) throw std::logic_error("EPQ store outside frame");
    if (slot < registerSlots_) mov(21 + static_cast<unsigned>(slot), reg);
    else storeAt(reg, sp, byteOffset(slot - registerSlots_));
}

void Assembler::saveTemp(unsigned reg, std::size_t slot) {
    if (slot >= frameSlots_ || slot < registerSlots_) throw std::logic_error("Invalid EPQ temporary slot");
    // Nested evaluations reserve consecutive slots. Modulo assignment keeps
    // their register choices independent of the surrounding expression tree.
    unsigned i = slot % 3;
    if (temps_[i] != unused && temps_[i] != slot) store(5 + i, temps_[i]);
    mov(5 + i, reg);
    temps_[i] = slot;
}

void Assembler::takeTemp(std::size_t slot, unsigned reg) {
    unsigned i = slot % 3;
    if (temps_[i] == slot) {
        mov(reg, 5 + i);
        temps_[i] = unused;
    } else {
        load(slot, reg);
    }
}

void Assembler::spillTemps() {
    for (unsigned i = 0; i < 3; ++i) {
        if (temps_[i] == unused) continue;
        store(5 + i, temps_[i]);
        temps_[i] = unused;
    }
}

void Assembler::context(unsigned reg) { mov(reg, 19); }

void Assembler::argument(unsigned reg, std::size_t index) { loadFrom(reg, 20, index); }

void Assembler::addressOfSlot(unsigned reg, std::size_t slot) {
    checkWordRegister(reg);
    // The one-past address is valid for the empty argument vector.
    if (slot > frameSlots_ || slot < registerSlots_) throw std::logic_error("EPQ slot has no memory address");
    offsetAddress(reg, sp, slot - registerSlots_);
}

void Assembler::loadAt(unsigned base, std::size_t offset, unsigned dest) {
    if (base > sp || (offset & 7)) throw std::logic_error("Unaligned EPQ word load");
    loadFrom(dest, base, offset / 8);
}

void Assembler::storeAt(unsigned src, unsigned base, std::size_t offset) {
    checkWordRegister(src);
    if (base > sp || (offset & 7)) throw std::logic_error("Unaligned EPQ word store");
    const auto index = offset / 8;
    if (index <= 4095) {
        word(0xf9000000 | (static_cast<unsigned>(index) << 10) | (base << 5) | src);
    } else {
        if (src == scratch || src == 17) throw std::logic_error("Scratch register used as EPQ value");
        offsetAddress(scratch, base, index);
        word(0xf9000000 | (scratch << 5) | src);
    }
}

void Assembler::callHost(const void* function) {
    if (!function) throw std::logic_error("Null EPQ runtime entry");
    spillTemps(); // Spilling an unusually large frame can use x16 to address it.
    immediate(scratch, reinterpret_cast<std::uintptr_t>(function));
    callRegister(scratch);
}

void Assembler::callRegister(unsigned reg) {
    checkWordRegister(reg);
    spillTemps();
    word(0xd63f0000 | (reg << 5)); // BLR Xn
}

void Assembler::integer(IntOp op) {
    // Encoding a signed EPQ integer shifts its int32 value left by two. The
    // final SBFX sign-extends the low 34 bits and gives arithmetic int32 wrap.
    switch (op) {
        case IntOp::Add: word(0x8b010000); break; // ADD X0,X0,X1
        case IntOp::Sub: word(0xcb010000); break; // SUB X0,X0,X1
        case IntOp::Mul:
            word(0x9342fc21); // ASR X1,X1,#2
            word(0x9b017c00); // MUL X0,X0,X1
            break;
        case IntOp::Neg: word(0xcb0003e0); break; // NEG X0,X0
        case IntOp::Abs:
            word(0xf100001f); // CMP X0,#0
            word(0xda805400); // CNEG X0,X0,MI
            break;
        default: {
            unsigned condition;
            switch (op) {
                case IntOp::Eq: case IntOp::Not: condition = 0; break;
                case IntOp::Ne: condition = 1; break;
                case IntOp::Lt: condition = 11; break;
                case IntOp::Le: condition = 13; break;
                case IntOp::Gt: condition = 12; break;
                case IntOp::Ge: condition = 10; break;
                default: throw std::logic_error("Unknown EPQ integer operation");
            }
            word(op == IntOp::Not ? 0xf100001f : 0xeb01001f); // CMP X0, #0 or X1
            word(0x1a9f07e0 | ((condition ^ 1) << 12)); // CSET W0, condition
            word(0x531e7400); // LSL W0,W0,#2
            return;
        }
    }
    word(0x93408400); // SBFX X0,X0,#0,#34
}

void Assembler::addSmallInteger(unsigned reg, int offset) {
    checkWordRegister(reg);
    if (offset < -1023 || offset > 1023) throw std::logic_error("EPQ add immediate out of range");
    if (offset == 0) return;
    const unsigned magnitude = static_cast<unsigned>(offset < 0 ? -offset : offset);
    const auto opcode = offset < 0 ? 0xd1000000 : 0x91000000;
    word(opcode | ((4 * magnitude) << 10) | (reg << 5) | reg);
    word(0x93408400 | (reg << 5) | reg); // SBFX Xd,Xd,#0,#34
}

void Assembler::incrementLocal(std::size_t slot, int offset) {
    if (slot < registerSlots_) return addSmallInteger(21 + static_cast<unsigned>(slot), offset);
    load(slot, 0);
    addSmallInteger(0, offset);
    store(0, slot);
}

void Assembler::knownIntDivide(Label zeroDivisor) {
    branch(Branch::Zero, zeroDivisor, 1, true);
    // The common scale factor cancels out in division. SBFIZ truncates the
    // quotient to int32 and reapplies the tag, including INT_MIN / -1.
    word(0x9ac10c00); // SDIV X0,X0,X1
    word(0x937e7c00); // SBFIZ X0,X0,#2,#32
}

// A proven list value is an aligned List address plus one. Unscaled loads
// with offset -1 or +7 access the aligned fields without untagging first.
static_assert(offsetof(rt::List, data) == 0 && offsetof(rt::List, size) == 8);
static_assert(sizeof(rt::V) == 8 && sizeof(rt::List::size) == 4);

void Assembler::knownListLength() {
    word(0xb8407010); // LDUR W16,[X0,#7]
    word(0xd37ef600); // LSL X0,X16,#2
}

void Assembler::checkedListIndex(Label outOfBounds) {
    word(0xb8407010); // LDUR W16,[X0,#7]
    word(0x9342fc21); // ASR X1,X1,#2
    word(0xeb10003f); // CMP X1,X16
    branch(Branch::Condition, outOfBounds, 2, true); // B.HS also rejects negative indices
    word(0xf85ff010); // LDUR X16,[X0,#-1]
}

void Assembler::knownListGet(Label outOfBounds, unsigned width) {
    checkedListIndex(outOfBounds);
    switch (width) {
        case 8: word(0xf8617a00); return; // LDR X0,[X16,X1,LSL #3]
        case 4: word(0xb8a17a00); break;  // LDRSW X0,[X16,X1,LSL #2]
        case 1: word(0x38616a00); break;  // LDRB W0,[X16,X1]
        default: throw std::logic_error("Invalid EPQ list read width");
    }
    word(0xd37ef400); // LSL X0,X0,#2, restoring the EPQ integer tag.
}

void Assembler::knownListAddress(Label outOfBounds, unsigned dest) {
    checkWordRegister(dest);
    checkedListIndex(outOfBounds);
    word(0x8b010e00 | dest); // ADD Xd,X16,X1,LSL #3
}

void Assembler::loadListData(unsigned dest) {
    checkWordRegister(dest);
    word(0xf85ff000 | dest); // LDUR Xd,[X0,#-1]
}

void Assembler::uncheckedListAddress(unsigned dest, unsigned data, unsigned taggedIndex) {
    checkWordRegister(dest);
    checkWordRegister(data);
    checkWordRegister(taggedIndex);
    // Eight-byte values and four-times-scaled indices leave one address shift.
    word(0x8b000400 | (taggedIndex << 16) | (data << 5) | dest);
}

void Assembler::knownListSet(Label outOfBounds, unsigned width) {
    checkedListIndex(outOfBounds);
    switch (width) {
        case 8:
            word(0xf8217a02); // STR X2,[X16,X1,LSL #3]
            break;
        case 4: case 1:
            word(0x9342fc51); // ASR X17,X2,#2
            word(width == 1 ? 0x38216a11 : 0xb8217a11); // STRB or STR W17
            break;
        case 0: break;
        default: throw std::logic_error("Invalid EPQ list write width");
    }
    mov(0, 2);
}

void Assembler::knownListPop(Label empty) {
    word(0xb8407010); // LDUR W16,[X0,#7]
    branch(Branch::Zero, empty, 16, true);
    word(0x51000610); // SUB W16,W16,#1
    word(0xb8007010); // STUR W16,[X0,#7]
    word(0xf85ff011); // LDUR X17,[X0,#-1]
    word(0xf8707a20); // LDR X0,[X17,X16,LSL #3]
}

Assembler::Label Assembler::label() {
    if (labels_.size() >= std::numeric_limits<Label>::max()) {
        throw std::length_error("Too many EPQ labels");
    }
    labels_.push_back(unbound);
    return static_cast<Label>(labels_.size() - 1);
}

void Assembler::bind(Label label) {
    spillTemps();
    auto& target = labels_.at(label);
    if (target != unbound) throw std::logic_error("EPQ branch label bound twice");
    target = words_.size();
}

void Assembler::branch(Branch kind, Label target, unsigned reg, bool fatal) {
    if (target >= labels_.size()) throw std::logic_error("Invalid EPQ branch label");
    checkWordRegister(reg);
    if (!fatal) spillTemps();
    fixups_.push_back({words_.size(), target, kind, reg});
    word(0);
}

void Assembler::jump(Label label) { branch(Branch::Jump, label); }
void Assembler::callLabel(Label label) { branch(Branch::Call, label); }
void Assembler::branchZero(unsigned reg, Label label) { branch(Branch::Zero, label, reg); }
void Assembler::branchNonzero(unsigned reg, Label label) { branch(Branch::Nonzero, label, reg); }

void Assembler::branchEqualImmediate(unsigned reg, std::uint16_t imm, Label label) {
    checkWordRegister(reg);
    if (imm <= 4095) {
        word(0xf100001f | (std::uint32_t{imm} << 10) | (reg << 5)); // CMP Xn,#imm
    } else {
        const auto tmp = reg == scratch ? 17u : scratch;
        immediate(tmp, imm);
        word(0xeb00001f | (tmp << 16) | (reg << 5)); // CMP Xn,Xm
    }
    branch(Branch::Condition, label, 0); // B.EQ
}

void Assembler::branchIntFalse(IntOp comparison, Label label) {
    unsigned opposite;
    switch (comparison) {
        case IntOp::Eq: opposite = 1; break;  // NE
        case IntOp::Ne: opposite = 0; break;  // EQ
        case IntOp::Lt: opposite = 10; break; // GE
        case IntOp::Le: opposite = 12; break; // GT
        case IntOp::Gt: opposite = 13; break; // LE
        case IntOp::Ge: opposite = 11; break; // LT
        default: throw std::logic_error("Expected an EPQ comparison");
    }
    word(0xeb01001f); // CMP X0,X1; signed ordering of tagged integers is preserved
    branch(Branch::Condition, label, opposite);
}

Image Assembler::seal() {
    if (words_.empty()) throw std::logic_error("Empty EPQ code");
    // A conditional jump reaches +/-1 MiB. Programs inside that range use
    // a single instruction per condition. Only large functions with a far
    // target need extra slots for an inverted condition plus a long branch.
    const auto conditional = [](Branch kind) { return kind != Branch::Jump && kind != Branch::Call; };
    std::vector<std::size_t> expandedBranches;
    bool needsLongBranches = false;
    for (const auto& fixup : fixups_) {
        const auto target = labels_.at(fixup.target);
        if (target == unbound || target >= words_.size()) throw std::logic_error("Missing EPQ branch target");
        auto distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(fixup.word);
        if (conditional(fixup.kind) && !fitsSigned(distance, 19)) needsLongBranches = true;
    }
    if (needsLongBranches) {
        for (const auto& fixup : fixups_)
            if (conditional(fixup.kind)) expandedBranches.push_back(fixup.word);
        std::vector<std::uint32_t> padded;
        padded.reserve(words_.size() + expandedBranches.size());
        auto next = expandedBranches.begin();
        for (std::size_t i = 0; i < words_.size(); ++i) {
            padded.push_back(words_[i]);
            if (next != expandedBranches.end() && i == *next) {
                padded.push_back(0xd503201f); // NOP if this particular branch still fits.
                ++next;
            }
        }
        words_.swap(padded);
    }
    const auto relocated = [&](std::size_t index) {
        return index + (std::lower_bound(expandedBranches.begin(), expandedBranches.end(), index)
                        - expandedBranches.begin());
    };
    for (const auto& fixup : fixups_) {
        auto at = relocated(fixup.word);
        auto delta = static_cast<std::int64_t>(relocated(labels_[fixup.target])) - static_cast<std::int64_t>(at);
        if (conditional(fixup.kind)) {
            const bool condition = fixup.kind == Branch::Condition;
            const std::uint32_t opcode = condition ? 0x54000000 | fixup.reg :
                (fixup.kind == Branch::Zero ? 0xb4000000 : 0xb5000000) | fixup.reg;
            if (fitsSigned(delta, 19)) {
                words_[at] = opcode | (branchOffset(delta, 19) << 5);
                continue;
            }
            words_[at++] = (opcode ^ (condition ? 1u : (1u << 24))) | (2u << 5);
            --delta;
        }
        if (!fitsSigned(delta, 26)) throw std::length_error("EPQ native branch exceeds 128 MiB");
        words_[at] = (fixup.kind == Branch::Call ? 0x94000000 : 0x14000000) | branchOffset(delta, 26);
    }
    const auto length = position();
    if (!pthread_jit_write_protect_supported_np()) {
        throw std::runtime_error("Apple Silicon JIT write protection unavailable");
    }
    void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (address == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "Cannot map EPQ JIT code");

    // No allocation, callbacks or exception-producing operations while writable.
    pthread_jit_write_protect_np(0);
    std::memcpy(address, words_.data(), length);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(address, length);
    return Image(address, length, std::move(expandedBranches));
}
} // namespace epq::arm
