#include "display.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace epq {
namespace {

using Slots = std::unordered_map<std::string, std::size_t>;

void find_global_slots(const Node& node, Slots& slots) {
    switch (node.op) {
    case Op::BLK:
    case Op::FOR:
    case Op::DEF:
        return;
    case Op::IF:
        find_global_slots(node.children[0], slots);
        return;
    case Op::SET:
        // The original display reserved a slot even for an indexed target
        // (which has an empty name). Later globals expose that numbering.
        slots.emplace(node.children[0].name, slots.size());
        break;
    default:
        break;
    }
    for (const auto& child : node.children) find_global_slots(child, slots);
}

struct Line {
    std::size_t address;
    std::string instruction;
};

class Listing {
public:
    Listing(const Slots& globals, FunctionDisplays& displays)
        : globals_(globals), displays_(displays) {}

    const std::string& function(const Node& definition) {
        for (const auto& param : definition.children[0].children) bind(param.name);
        emit(definition.children[1]);
        op("NULL");
        op("RETURN");
        std::string text = "function: {\n";
        for (const auto& line : lines_) {
            text += std::to_string(line.address) + ":\tOP_" + line.instruction + "\n";
        }
        text += "}\n";
        return displays_.emplace(&definition, std::move(text)).first->second;
    }

private:
    const Slots& globals_;
    FunctionDisplays& displays_;
    Slots locals_;
    std::vector<std::string> local_names_;
    std::vector<Line> lines_;
    std::vector<std::string> constants_;
    std::size_t pc_ = 0;
    std::size_t range_count_ = 0;

    void bind(const std::string& name) {
        locals_.emplace(name, local_names_.size());
        local_names_.push_back(name);
    }

    void op(std::string instruction, std::size_t width = 1) {
        lines_.push_back({pc_, std::move(instruction)});
        pc_ += width;
    }

    void indexed(const char* name, std::size_t index) {
        op(std::string(name) + " " + std::to_string(static_cast<std::uint8_t>(index)), 2);
    }

    void constant(std::string value) {
        const auto index = static_cast<std::uint8_t>(constants_.size());
        constants_.push_back(std::move(value));
        op("CONST " + constants_[index], 2);
    }

    std::size_t jump(const char* name) {
        const auto index = lines_.size();
        op(name, 3);
        return index;
    }

    void patch(std::size_t index) {
        auto& line = lines_[index];
        line.instruction += " " + std::to_string(pc_ - line.address);
    }

    void close_scope(std::size_t start) {
        while (local_names_.size() > start) {
            locals_.erase(local_names_.back());
            local_names_.pop_back();
            op("POP");
        }
    }

    void scoped(const Node& node) {
        const auto start = local_names_.size();
        emit(node);
        close_scope(start);
    }

    void variable(const std::string& name, bool write) {
        const auto global = globals_.find(name);
        if (global != globals_.end()) {
            indexed(write ? "SET_GLOBAL" : "GET_GLOBAL", global->second);
            return;
        }
        const auto local = locals_.find(name);
        if (local != locals_.end()) {
            indexed(write ? "SET_LOCAL" : "GET_LOCAL", local->second);
            return;
        }
        bind(name);
        if (write) {
            // The trailing space appears in the original printable instruction.
            op("DEF_LOCAL ");
        } else {
            indexed("GET_LOCAL", locals_.at(name));
        }
    }

    void set_and_drop(const std::string& name) {
        variable(name, true);
        op("POP");
    }

    void loop(const Node& node) {
        const auto& c = node.children;
        const bool counted = node.op == Op::FOR;
        const bool range = node.op == Op::RNG;
        const auto scope = local_names_.size();
        std::string counter, list;
        if (range) {
            const auto id = std::to_string(range_count_++);
            counter = id + "i";
            list = id + "l";
            op("NULL");
            set_and_drop(counter);
            emit(c[1]);
            set_and_drop(list);
            op("NULL");
            set_and_drop(c[0].name);
        } else if (counted) {
            emit(c[0]);
        }

        const auto start = pc_;
        if (range) {
            variable(counter, false);
            variable(list, false);
            op("ABS");
            op("LESS");
        } else {
            emit(c[counted ? 1 : 0]);
        }
        const auto done = jump("JMP_F");
        op("POP");
        if (range) {
            variable(list, false);
            variable(counter, false);
            op("GET_IND");
            set_and_drop(c[0].name);
            scoped(c[2]);
            variable(counter, false);
            constant("1");
            op("ADD");
            set_and_drop(counter);
        } else {
            scoped(c[counted ? 3 : 1]);
            if (counted) emit(c[2]);
        }
        op("LOOP " + std::to_string(pc_ - start), 3);
        patch(done);
        op("POP");
        close_scope(scope);
    }

    void emit(const Node& node) {
        const auto& c = node.children;
        const auto scope = local_names_.size();
        auto children = [&] { for (const auto& child : c) emit(child); };
        auto postfix = [&](const char* instruction) { children(); op(instruction); };
        switch (node.op) {
        case Op::ADD: postfix("ADD"); break;
        case Op::SUB: postfix("SUB"); break;
        case Op::MUL: postfix("MUL"); break;
        case Op::DIV: postfix("DIV"); break;
        case Op::NEG: postfix("NEG"); break;
        case Op::EQ: postfix("EQ"); break;
        case Op::GT: postfix("GRTR"); break;
        case Op::LT: postfix("LESS"); break;
        case Op::GE: postfix("LESS"); op("NOT"); break;
        case Op::LE: postfix("GRTR"); op("NOT"); break;
        case Op::NE: postfix("EQ"); op("NOT"); break;
        case Op::APP: postfix("APP"); break;
        case Op::NOT: postfix("NOT"); break;
        case Op::ABS: postfix("ABS"); break;
        case Op::PRINT: postfix("PRINT"); break;
        case Op::EXP: postfix("POP"); break;
        case Op::RET: postfix("RETURN"); break;
        case Op::IND: postfix("GET_IND"); break;
        case Op::INT: constant(std::to_string(node.number)); break;
        case Op::INP: op("INPUT"); break;
        case Op::ID: variable(node.name, false); break;
        case Op::SEQ: children(); break;
        case Op::BLK:
        case Op::LST:
            children();
            close_scope(scope);
            if (node.op == Op::LST) indexed("LIST", c.size());
            break;
        case Op::SET:
            if (c[0].op == Op::ID) {
                emit(c[1]);
                variable(c[0].name, true);
            } else if (c[0].op == Op::IND) {
                for (const auto& part : c[0].children) emit(part);
                emit(c[1]);
                op("SET_IND");
            }
            break;
        case Op::IF: {
            emit(c[0]);
            const auto otherwise = jump("JMP_F");
            op("POP");
            scoped(c[1]);
            const auto done = jump("JMP");
            patch(otherwise);
            op("POP");
            scoped(c[2]);
            patch(done);
            close_scope(scope);
            break;
        }
        case Op::AND:
        case Op::OR: {
            emit(c[0]);
            auto done = jump("JMP_F");
            if (node.op == Op::OR) {
                const auto second = done;
                done = jump("JMP");
                patch(second);
            }
            op("POP");
            emit(c[1]);
            patch(done);
            break;
        }
        case Op::WHL:
        case Op::FOR:
        case Op::RNG:
            loop(node);
            break;
        case Op::DEF:
            constant(Listing(globals_, displays_).function(node));
            break;
        case Op::CAL:
            for (const auto& arg : c[1].children) emit(arg);
            emit(c[0]);
            indexed("CALL", c[1].children.size());
            break;
        case Op::CLS:
        case Op::GET:
        case Op::SGET:
            break;
        }
    }
};

void find_functions(const Node& node, const Slots& globals, FunctionDisplays& displays) {
    if (node.op == Op::DEF) {
        Listing(globals, displays).function(node);
    } else {
        for (const auto& child : node.children) find_functions(child, globals, displays);
    }
}

} // namespace

FunctionDisplays function_displays(const Node& program) {
    Slots globals;
    find_global_slots(program, globals);
    FunctionDisplays displays;
    find_functions(program, globals, displays);
    return displays;
}

} // namespace epq
