#include "emit.hpp"
#include "display.hpp"
#include "loopopt.hpp"
#include "resolve.hpp"
#include "types.hpp"

#include <sstream>
#include <stdexcept>
#include <vector>

namespace epq {
namespace {

using Text = std::string;

Text slot(int n) { return "v" + std::to_string(n); }
Text flag(int n) { return "ready" + std::to_string(n); }
Text functionName(int n) { return "fn" + std::to_string(n); }

Text typeName(Kind type) {
    switch (type) {
    case Kind::Int:      return "int32_t";
    case Kind::List:     return "List*";
    case Kind::Function: return "int32_t";
    case Kind::Dynamic:  return "V";
    }
    throw std::logic_error("unknown native type");
}

struct Value {
    Kind kind;
    Text code;
};

Text box(const Value& value) {
    switch (value.kind) {
    case Kind::Int:      return "V::integer(" + value.code + ")";
    case Kind::List:     return "V::list(" + value.code + ")";
    case Kind::Function: return "V::function(" + value.code + ")";
    case Kind::Dynamic:  return value.code;
    }
    throw std::logic_error("unknown value representation");
}

Text convert(const Value& value, Kind wanted) {
    if (wanted == value.kind) return value.code;
    const Text boxed = box(value);
    switch (wanted) {
    case Kind::Int:      return boxed + ".asInt()";
    case Kind::List:     return boxed + ".asList()";
    case Kind::Function: return boxed + ".asFunction()";
    case Kind::Dynamic:  return boxed;
    }
    throw std::logic_error("unknown destination representation");
}

// The static value-flow analysis proves the tag returned by a dynamic operation.
Text unboxKnown(const Text& boxed, Kind wanted) {
    switch (wanted) {
    case Kind::Int:      return boxed + ".intUnchecked()";
    case Kind::List:     return boxed + ".listUnchecked()";
    case Kind::Function: return boxed + ".asFunction()";
    case Kind::Dynamic:  return boxed;
    }
    throw std::logic_error("unknown unboxing representation");
}

Text condition(const Value& value) {
    if (value.kind == Kind::Int) return value.code;
    return "rt::truth(" + box(value) + ")";
}

Text join(const std::vector<Text>& parts) {
    Text result;
    for (const auto& part : parts) {
        if (!result.empty()) result += ", ";
        result += part;
    }
    return result;
}

Text quote(const Text& data) {
    Text ret = "\"";
    for (auto c : data) {
        switch (c) {
        case '\n': ret += "\\n"; break;
        case '\t': ret += "\\t"; break;
        case '\\': ret += "\\\\"; break;
        case '"': ret += "\\\""; break;
        default: ret += c;
        }
    }
    return ret + "\"";
}

class Generator {
public:
    explicit Generator(const Node& program)
        : root(program), res(resolve(root)), types(infer(root, res)) {}

    Text run() {
        line("#include \"epq_runtime.hpp\"");
        line("using rt::V;");
        line("using rt::List;");
        line("using std::int32_t;");
        line("static V dispatch(int32_t, const V*, unsigned);");
        for (int i = 0; i < static_cast<int>(res.functions.size()); ++i)
            line(signature(i) + ";");
        declareSlots(-1, true);
        for (int i = 0; i < static_cast<int>(res.functions.size()); ++i) {
            current = i;
            open(signature(i));
            declareSlots(i, false);
            statement(res.functions[i]->children[1]);
            defaultReturn(types.result(i).kind);
            close();
        }
        current = -1;
        open("static void epq_main()");
        declareSlots(-1, false);
        statement(root);
        close();
        if (dynamicCalls) dispatcher();
        displays();
        line("int main() { return rt::run(epq_main); }");
        return output.str();
    }

private:
    void line(const Text& code) {
        output << Text(static_cast<std::size_t>(indent) * 2, ' ') << code << '\n';
    }
    void open(const Text& header) { line(header + " {"); ++indent; }
    void close() { --indent; line("}"); }
    Text fresh() { return "t" + std::to_string(next++); }
    Value temp(Kind type, const Text& code) {
        Text name = fresh();
        line(typeName(type) + " " + name + " = " + code + ";");
        return {type, name};
    }
    void declareSlots(int owner, bool globals) {
        for (int i = 0; i < static_cast<int>(res.bindings.size()); ++i) {
            const auto& b = res.bindings[i];
            if (b.owner != owner || b.global != globals || b.parameter) continue;
            const Text link = globals ? "static " : "";
            line(link + typeName(types.binding(i).kind) + " " + slot(i) + "{};");
            if (res.needs_init_check.count(i)) line(link + "bool " + flag(i) + " = false;");
        }
    }
    void enterScope(const Node& n) {
        auto it = res.declarations.find(&n);
        if (it == res.declarations.end()) return;
        for (int id : it->second)
            if (res.needs_init_check.count(id)) line(flag(id) + " = false;");
    }
    Text signature(int f) const {
        std::vector<Text> params;
        for (const Node& arg : res.functions[f]->children[0].children) {
            int id = res.id.at(&arg);
            params.push_back(typeName(types.binding(id).kind) + " " + slot(id));
        }
        return "static " + typeName(types.result(f).kind) + " " + functionName(f)
            + "(" + join(params) + ")";
    }
    void assign(int id, const Value& value) {
        line(slot(id) + " = " + convert(value, types.binding(id).kind) + ";");
        if (res.needs_init_check.count(id)) line(flag(id) + " = true;");
    }
    void defaultReturn(Kind kind) {
        if (kind == Kind::Int) line("return 0;");
        else if (kind == Kind::Dynamic) line("return V::integer(0);");
        else line("rt::fail(\"Unreachable function fallthrough\");");
    }

    Value expr(const Node& n) {
        enterScope(n);
        const auto& c = n.children;
        const Kind kind = types.expression(n).kind;
        switch (n.op) {
        case Op::INT: return {Kind::Int, std::to_string(n.number)};
        case Op::INP: return temp(Kind::Int, "rt::input()");
        case Op::ID: {
            const int id = res.id.at(&n);
            if (!res.initialized.count(&n)) {
                const Text message = res.bindings[id].global ? "Undefined global" : "Undefined local";
                line("if (!" + flag(id) + ") rt::fail(" + quote(message) + ");");
            }
            // Copy here so a later operand can freely assign the same binding.
            return temp(kind, convert({types.binding(id).kind, slot(id)}, kind));
        }
        case Op::DEF: return {Kind::Function, std::to_string(res.function.at(&n))};
        case Op::LST: {
            std::vector<Text> items;
            for (const Node& arg : c) items.push_back(box(expr(arg)));
            return temp(Kind::List, "rt::makeList({" + join(items) + "})");
        }
        case Op::IND: {
            Value list = expr(c[0]);
            Value index = expr(c[1]);
            Text read = "rt::get(" + convert(list, Kind::List) + ", " + convert(index, Kind::Int) + ")";
            return temp(kind, unboxKnown(read, kind));
        }
        case Op::CLS: case Op::GET: case Op::SGET: return unsupported();
        case Op::SET: {
            if (c[0].op != Op::IND && c[0].op != Op::ID) return unsupported();
            if (c[0].op == Op::ID) {
                Value value = expr(c[1]);
                assign(res.id.at(&c[0]), value);
                return value;
            }
            const auto& indexed = c[0].children;
            Value list = expr(indexed.at(0));
            Value index = expr(indexed.at(1));
            Value value = expr(c[1]);
            line("rt::set(" + convert(list, Kind::List) + ", " + convert(index, Kind::Int)
                 + ", " + box(value) + ");");
            return value;
        }
        case Op::APP: {
            Value list = expr(c[0]);
            Value value = expr(c[1]);
            return temp(Kind::List, "rt::append(" + convert(list, Kind::List) + ", " + box(value) + ")");
        }
        case Op::CAL: return call(n);
        case Op::AND: case Op::OR: {
            Value left = expr(c[0]);
            Text selected = fresh();
            line(typeName(kind) + " " + selected + ";");
            open("if (" + Text(n.op == Op::AND ? "!(" : "(") + condition(left) + "))");
            line(selected + " = " + convert(left, kind) + ";");
            close();
            open("else");
            Value right = expr(c[1]);
            line(selected + " = " + convert(right, kind) + ";");
            close();
            return {kind, selected};
        }
        case Op::NEG: {
            Value arg = expr(c[0]);
            if (arg.kind == Kind::Int) return temp(Kind::Int, "rt::negate(" + arg.code + ")");
            if (arg.kind == Kind::List) return temp(kind, unboxKnown("rt::pop(" + arg.code + ")", kind));
            return temp(kind, unboxKnown("rt::neg(" + box(arg) + ")", kind));
        }
        case Op::ABS: {
            Value arg = expr(c[0]);
            if (arg.kind == Kind::Int) return temp(Kind::Int, "rt::absolute(" + arg.code + ")");
            if (arg.kind == Kind::List) return temp(Kind::Int, "rt::length(" + arg.code + ")");
            return temp(Kind::Int, "rt::abs(" + box(arg) + ")");
        }
        case Op::NOT: return temp(Kind::Int, "!(" + convert(expr(c[0]), Kind::Int) + ")");
        default: return binary(n);
        }
    }

    Value unsupported() {
        line("rt::fail(\"Class and member syntax has no defined execution semantics\");");
        return {Kind::Int, "0"}; // Allows statically unreachable code to compile.
    }

    Value binary(const Node& n) {
        Value a = expr(n.children.at(0));
        Value b = expr(n.children.at(1));
        const char* arithmetic = nullptr;
        switch (n.op) {
        case Op::ADD: arithmetic = "add"; break;
        case Op::SUB: arithmetic = "sub"; break;
        case Op::MUL: arithmetic = "mul"; break;
        case Op::DIV: arithmetic = "divide"; break;
        default: break;
        }
        if (arithmetic) return temp(Kind::Int, Text("rt::") + arithmetic + "("
            + convert(a, Kind::Int) + ", " + convert(b, Kind::Int) + ")");

        bool native = a.kind == Kind::Int && b.kind == Kind::Int;
        const bool intLists = a.kind == Kind::List && b.kind == Kind::List
            && types.element(n.children[0]).kind == Kind::Int && types.element(n.children[1]).kind == Kind::Int;
        Text comparison;
        if (n.op == Op::EQ || n.op == Op::NE) {
            native = native || (a.kind == Kind::Function && b.kind == Kind::Function);
            if (native) comparison = a.code + " == " + b.code;
            else if (intLists) comparison = "rt::intListsEqual(" + a.code + ", " + b.code + ")";
            else comparison = "rt::equal(" + box(a) + ", " + box(b) + ")";
            if (n.op == Op::NE) comparison = "!(" + comparison + ")";
        } else {
            if (n.op == Op::GT || n.op == Op::LE) std::swap(a, b);
            if (native) comparison = a.code + " < " + b.code;
            else if (intLists) comparison = "rt::intListsLess(" + a.code + ", " + b.code + ")";
            else comparison = "rt::less(" + box(a) + ", " + box(b) + ")";
            if (n.op == Op::GE || n.op == Op::LE) comparison = "!(" + comparison + ")";
        }
        return temp(Kind::Int, comparison);
    }

    Value call(const Node& n) {
        std::vector<Value> args;
        for (const auto& arg : n.children[1].children) args.push_back(expr(arg));
        // EPQ evaluates the callee last, including assignments and global lookups.
        Value callee = expr(n.children[0]);
        Type target = types.expression(n.children[0]);
        Kind kind = types.expression(n).kind;
        if (target.kind == Kind::Function && target.function >= 0) {
            const auto& params = res.functions.at(target.function)->children[0].children;
            if (params.size() != args.size()) {
                line("rt::fail(\"Invalid arity\");");
                return {Kind::Int, "0"}; // Unreachable.
            }
            std::vector<Text> actual;
            for (std::size_t i = 0; i < args.size(); ++i)
                actual.push_back(convert(args[i], types.binding(res.id.at(&params[i])).kind));
            Text invoke = functionName(target.function) + "(" + join(actual) + ")";
            return temp(kind, convert({types.result(target.function).kind, invoke}, kind));
        }
        dynamicCalls = true;
        Text argv = "nullptr";
        if (!args.empty()) {
            argv = fresh();
            std::vector<Text> actual;
            for (const auto& arg : args) actual.push_back(box(arg));
            line("const V " + argv + "[] = {" + join(actual) + "};");
        }
        Text invoke = "dispatch(" + convert(callee, Kind::Function) + ", " + argv
            + ", " + std::to_string(args.size()) + ")";
        return temp(kind, unboxKnown(invoke, kind));
    }

    void statement(const Node& n) {
        enterScope(n);
        const auto& c = n.children;
        switch (n.op) {
        case Op::SEQ: case Op::BLK:
            for (const Node& child : c) statement(child);
            break;
        case Op::EXP: expr(c[0]); break;
        case Op::PRINT: {
            Value value = expr(c[0]);
            line("rt::print(" + box(value) + ");");
            if (value.kind != Kind::Int && (value.kind != Kind::List || types.element(c[0]).kind != Kind::Int))
                mayPrintFunction = true;
            break;
        }
        case Op::RET: {
            Value value = expr(c[0]);
            if (current < 0) line("return;");
            else line("return " + convert(value, types.result(current).kind) + ";");
            break;
        }
        case Op::IF: {
            Value test = expr(c[0]);
            open("if (" + condition(test) + ")");
            statement(c[1]);
            close();
            if (c[2].op != Op::SEQ || !c[2].children.empty()) {
                open("else");
                statement(c[2]);
                close();
            }
            break;
        }
        case Op::FOR: case Op::WHL: {
            const bool counted = n.op == Op::FOR;
            if (counted) {
                statement(c[0]);
                if (auto fill = match_fill_loop(n, res, types)) {
                    Value start = expr(*fill->start_read);
                    Value bound = expr(*fill->bound);
                    open("if (" + start.code + (fill->inclusive ? " <= " : " < ") + bound.code + ")");
                    Value list = expr(*fill->list);
                    Value item = expr(*fill->value);
                    Text invoke = "rt::fillLoop(" + convert(list, Kind::List) + ", " + box(item) + ", "
                        + start.code + ", " + bound.code + ", " + (fill->inclusive ? "true" : "false") + ")";
                    assign(fill->counter, {Kind::Int, invoke});
                    close();
                    break;
                }
            }
            open("for (;;)");
            Value test = expr(c[counted ? 1 : 0]);
            line("if (!(" + condition(test) + ")) break;");
            statement(c[counted ? 3 : 1]);
            if (counted) statement(c[2]);
            close();
            break;
        }
        case Op::RNG: {
            Value items = expr(c[1]);
            int iterator = res.id.at(&c[0]);
            if (!res.fresh_iterators.count(&n)) assign(iterator, {Kind::Int, "0"});
            Text i = fresh();
            // A zero integer has length zero under the upstream range expansion.
            // Nonzero integers fail indexing on the first attempted iteration.
            Text size = items.kind == Kind::List ? "rt::length(" + items.code + ")"
                : "rt::abs(" + box(items) + ")";
            open("for (int32_t " + i + " = 0; " + i + " < " + size + "; ++" + i + ")");
            Kind elem = types.element(c[1]).kind;
            Text get = "rt::get(" + convert(items, Kind::List) + ", " + i + ")";
            assign(iterator, {elem, unboxKnown(get, elem)});
            statement(c[2]);
            close();
            break;
        }
        default: throw std::logic_error("unsupported EPQ statement");
        }
    }

    void dispatcher() {
        open("static V dispatch(int32_t id, const V* args, unsigned n)");
        open("switch (id)");
        for (int i = 0; i < static_cast<int>(res.functions.size()); ++i) {
            const auto& params = res.functions[i]->children[0].children;
            line("case " + std::to_string(i) + ":");
            line("  if (n != " + std::to_string(params.size()) + ") rt::fail(\"Invalid arity\");");
            std::vector<Text> actual;
            for (std::size_t j = 0; j < params.size(); ++j) {
                actual.push_back(convert({Kind::Dynamic, "args[" + std::to_string(j) + "]"},
                    types.binding(res.id.at(&params[j])).kind));
            }
            line("  return " + box({types.result(i).kind, functionName(i) + "(" + join(actual) + ")"}) + ";");
        }
        line("default: rt::fail(\"Invalid function\");");
        close();
        close();
    }

    void displays() {
        open("const char* rt::functionDisplay(int32_t id)");
        if (mayPrintFunction && !res.functions.empty()) {
            const auto text = function_displays(root);
            open("switch (id)");
            for (int i = 0; i < static_cast<int>(res.functions.size()); ++i)
                line("case " + std::to_string(i) + ": return " + quote(text.at(res.functions[i])) + ";");
            close();
        }
        line("rt::fail(\"Invalid printable function\");");
        close();
    }

    const Node& root;
    Resolution res;
    Types types;
    std::ostringstream output;
    int current = -1;
    int indent = 0;
    unsigned next = 0;
    bool dynamicCalls = false;
    bool mayPrintFunction = false;
};

} // namespace

std::string emit(const Node& root) { return Generator(root).run(); }

} // namespace epq
