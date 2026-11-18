#include "ast.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace epq {
namespace {

template<class... Children>
Node make_node(Op op, Children&&... children) {
    Node result{op, {}, 0, {}};
    result.children.reserve(sizeof...(children));
    (result.children.push_back(std::forward<Children>(children)), ...);
    return result;
}

Node append(Node&& node, Node&& child) {
    node.children.push_back(std::move(child));
    return std::move(node);
}

#include "parse_tables.inc"

bool digit(char c) {
    return c >= '0' && c <= '9';
}

bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

struct Token {
    int symbol;
    Node payload;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        skip_trivia();
        start_ = pos_;
        if (pos_ == source_.size()) return token(0);
        char c = source_[pos_++];
        if (letter(c)) {
            while (pos_ < source_.size()) {
                char next = source_[pos_];
                if (!letter(next) && !digit(next) && next != '_') break;
                ++pos_;
            }
            auto result = token(ID);
            result.payload.op = Op::ID;
            result.payload.name = source_.substr(start_, pos_ - start_);
            return result;
        }
        if (digit(c)) {
            while (pos_ < source_.size() && digit(source_[pos_])) ++pos_;
            // The reference Windows scanner uses a signed 32-bit long and
            // saturates on overflow; the unary minus is parsed separately.
            std::int32_t n = 0;
            auto read = std::from_chars(source_.data() + start_, source_.data() + pos_, n);
            if (read.ec == std::errc::result_out_of_range) n = std::numeric_limits<std::int32_t>::max();
            auto result = token(NUMBER);
            result.payload.op = Op::INT;
            result.payload.number = n;
            return result;
        }
        switch (c) {
        case ':':
            if (consume('=')) return token(ASSIGN);
            if (consume('+')) return token(APPEND);
            break;
        case '>':
            if (consume('>')) return token(PRINT);
            return token(consume('=') ? GE : GT);
        case '<':
            if (consume('<')) return token(INPUT);
            return token(consume('=') ? LE : LT);
        case '!':
            if (consume('=')) return token(NE);
            break;
        case '=':
            if (consume('>')) return token(ARROW);
            break;
        }
        int symbol = static_cast<unsigned char>(c);
        if (!symbol || yytranslate[symbol] == 2) error("unexpected character");
        return token(symbol);
    }

    [[noreturn]] void error(std::string_view reason) const {
        auto prefix = source_.substr(0, start_);
        auto line = 1 + std::count(prefix.begin(), prefix.end(), '\n');
        auto line_start = prefix.rfind('\n');
        auto column = line_start == std::string_view::npos ? start_ + 1 : start_ - line_start;
        throw std::runtime_error(std::to_string(line) + ":" + std::to_string(column)
                                 + ": " + std::string(reason));
    }

private:
    Token token(int code) const {
        return {yytranslate[code], make_node(Op::SEQ)};
    }

    bool consume(char c) {
        if (pos_ < source_.size() && source_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void skip_trivia() {
        while (pos_ < source_.size()) {
            char c = source_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (source_.substr(pos_, 2) == "//") {
                pos_ = source_.find('\n', pos_ + 2);
                if (pos_ == std::string_view::npos) pos_ = source_.size();
            } else {
                break;
            }
        }
    }

    std::string_view source_;
    std::size_t pos_ = 0;
    std::size_t start_ = 0;
};

bool has_entry(int slot, int key) {
    return slot >= 0 && slot <= YYLAST && yycheck[slot] == key;
}

} // namespace

Node parse(std::string_view source) {
    Lexer lexer(source);
    Token next = lexer.next();
    std::vector<int> states{0};
    std::vector<Node> values;

    // LALR parsing preserves EPQ's context-sensitive choice between an OR
    // operator and a closing absolute-value bar, with one token of lookahead.
    for (;;) {
        int state = states.back();
        int base = yypact[state];
        int slot = base + next.symbol;
        int rule = yydefact[state];
        if (base != YYPACT_NINF && has_entry(slot, next.symbol)) {
            int action = yytable[slot];
            if (action == YYFINAL) return std::move(values.back());
            if (action > 0) {
                states.push_back(action);
                values.push_back(std::move(next.payload));
                next = lexer.next();
                continue;
            }
            rule = action == YYTABLE_NINF ? 0 : -action;
        }
        if (!rule) lexer.error("unexpected token");
        auto start = values.size() - static_cast<std::size_t>(yyr2[rule]);
        Node result = reduce(rule, values.empty() ? nullptr : values.data() + start);
        values.resize(start);
        states.resize(start + 1);
        int lhs = yyr1[rule] - YYNTOKENS;
        int transition = yypgoto[lhs] + states.back();
        int dest = has_entry(transition, states.back()) ? yytable[transition] : yydefgoto[lhs];
        states.push_back(dest);
        values.push_back(std::move(result));
    }
}

} // namespace epq
