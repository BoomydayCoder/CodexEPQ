#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace epq::arm {

enum class IntOp { Add, Sub, Mul, Eq, Ne, Lt, Le, Gt, Ge, Neg, Abs, Not };

// Executable code with thread-local write protection, owned by a single run.
class Image {
public:
    Image(Image&& other) noexcept;
    Image& operator=(Image&&) = delete;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    template<typename Function>
    Function entry(std::size_t byteOffset) const {
        return reinterpret_cast<Function>(instruction(byteOffset));
    }

private:
    friend class Assembler;
    Image(void* memory, std::size_t length, std::vector<std::size_t> expandedBranches);
    void* instruction(std::size_t byteOffset) const;
    void* memory_;
    std::size_t length_;
    std::vector<std::size_t> expandedBranches_;
};

// Each EPQ function receives its run context in x0 and argument array in x1.
// x16 and x17 are scratch registers. Stack slots and arguments use 8-byte words.
class Assembler {
public:
    using Label = unsigned;
    // Up to eight leading local slots can stay in callee-saved registers.
    // Pass only locals whose address is never taken; argument scratch follows.
    void prologue(std::size_t slots, unsigned registerSlots = 0);
    void epilogue();
    void load(std::size_t slot, unsigned reg);
    void store(unsigned reg, std::size_t slot);
    // A saved operand has a single consuming read. It can stay in a volatile
    // register until an intervening call or alternate control path needs memory.
    void saveTemp(unsigned reg, std::size_t slot);
    void takeTemp(std::size_t slot, unsigned reg);
    void immediate(unsigned reg, std::uint64_t value);
    void mov(unsigned to, unsigned from);
    void context(unsigned reg);
    void argument(unsigned reg, std::size_t index);
    void addressOfSlot(unsigned reg, std::size_t slot);
    void loadAt(unsigned base, std::size_t byteOffset, unsigned dest);
    void storeAt(unsigned src, unsigned base, std::size_t byteOffset);
    void callHost(const void* function);
    void callRegister(unsigned reg);
    void integer(IntOp op);
    void addSmallInteger(unsigned reg, int offset);
    void incrementLocal(std::size_t slot, int offset);
    void knownIntDivide(Label zeroDivisor);
    void knownListLength();
    // Element widths are eight for tagged values, four for signed integers,
    // one for bytes, and zero for a size-only list (write checks only).
    void knownListGet(Label outOfBounds, unsigned width = 8);
    void knownListAddress(Label outOfBounds, unsigned dest);
    void loadListData(unsigned dest);
    void uncheckedListAddress(unsigned dest, unsigned data, unsigned taggedIndex);
    void knownListSet(Label outOfBounds, unsigned width = 8);
    void knownListPop(Label empty);

    Label label();
    void bind(Label label);
    void jump(Label label);
    void branchZero(unsigned reg, Label label);
    void branchNonzero(unsigned reg, Label label);
    void branchEqualImmediate(unsigned reg, std::uint16_t imm, Label label);
    void branchIntFalse(IntOp comparison, Label label);
    void callLabel(Label label);
    std::size_t position() const { return words_.size() * sizeof(std::uint32_t); }
    Image seal();

private:
    enum class Branch { Jump, Call, Zero, Nonzero, Condition };
    struct Fixup { std::size_t word; Label target; Branch kind; unsigned reg; };
    void branch(Branch kind, Label target, unsigned reg = 0, bool fatal = false);
    void spillTemps();
    void checkedListIndex(Label outOfBounds);
    void offsetAddress(unsigned reg, unsigned base, std::size_t index);
    void loadFrom(unsigned reg, unsigned base, std::size_t index);
    void word(std::uint32_t bits) { words_.push_back(bits); }
    std::vector<std::uint32_t> words_;
    std::vector<std::size_t> labels_;
    std::vector<Fixup> fixups_;
    std::size_t frameSlots_ = 0;
    unsigned registerSlots_ = 0;
    static constexpr std::size_t unused = static_cast<std::size_t>(-1);
    std::size_t temps_[3] = {unused, unused, unused}; // x5, x6, x7
};

} // namespace epq::arm
