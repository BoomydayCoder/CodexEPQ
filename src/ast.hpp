#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace epq {

enum class Op {
    ADD, SUB, MUL, DIV, NEG, INT, SEQ, ID, IND, SET, EXP, PRINT, BLK, INP,
    EQ, NOT, IF, AND, OR, GT, LT, GE, LE, NE, WHL, FOR, RNG, LST, APP,
    CAL, DEF, RET, ABS, CLS, GET, SGET
};

struct Node {
    Op op;
    std::string name;
    std::int32_t number = 0;
    std::vector<Node> children;
};

Node parse(std::string_view source);

} // namespace epq
