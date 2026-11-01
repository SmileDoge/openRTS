#include <openrts/formats/signal_script.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>
#include <stdexcept>

#include <openrts/formats/signal_config.hpp>

// A port of the RTS signal script compiler (rts1.exe 0x496DB0, 0x497ED8,
// 0x497E14, 0x497334, 0x45911C and helpers; docs/reverse/signals.md). It
// keeps RTS's character classes, bounds and quirks so that the text assembly
// matches what RTS writes to sigscr.dat.asm.

namespace openrts::formats {
namespace {

// The file as RTS sees it after MultiByteToWideChar: ASCII stays, bytes
// >= 0x80 become code units above 'z' (their exact values never matter to the
// compiler's character classes, which only test ASCII ranges).
using Text = std::u16string;

struct CompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[nodiscard]] char16_t at(const Text& text, const std::size_t index) {
    return index < text.size() ? text[index] : u'\0';
}

[[nodiscard]] std::string narrow(const Text& text, const std::size_t begin, const std::size_t length) {
    std::string result;
    for (std::size_t i = 0; i < length && begin + i < text.size(); ++i) {
        const auto c = text[begin + i];
        result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return result;
}

// cp1251 (the ANSI code page RTS reads the file with on Russian systems)
// back to UTF-8, for names; the compiler's own text uses the code units.
[[nodiscard]] std::string to_utf8(const Text& text, const std::size_t begin, const std::size_t length) {
    std::string result;
    for (std::size_t i = 0; i < length && begin + i < text.size(); ++i) {
        const auto c = text[begin + i];
        if (c < 0x80) {
            result.push_back(static_cast<char>(c));
            continue;
        }
        const auto byte = static_cast<unsigned>(c - 0x400);
        unsigned code = 0xFFFD;
        if (byte >= 0xC0) code = 0x410 + (byte - 0xC0);
        else if (byte == 0xA8) code = 0x401;
        else if (byte == 0xB8) code = 0x451;
        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    return result;
}

[[nodiscard]] std::string reg(const int index) {
    return std::string{static_cast<char>('A' + index), 'X'};
}

// 0x459D3C: comments are blanked; `/*RTS#` keeps the body. Both scans start
// at index 1 and stop before the last two characters.
void blank_comments(Text& text) {
    const auto n = text.size();
    bool in_block = false;
    bool rts_body = false;
    for (std::size_t i = 1; n >= 2 && i < n - 2; ++i) {
        if (!in_block && text[i] == u'/' && text[i + 1] == u'*') {
            in_block = true;
            text[i + 1] = u' ';
            if (n - i > 5 && text.compare(i + 2, 4, u"RTS#") == 0) {
                text[i] = u' ';
                auto j = i + 2;
                text[j++] = u' ';
                text[j++] = u' ';
                text[j] = u' ';
                text[j + 1] = u' ';
                i = j + 2;
                rts_body = true;
            }
        }
        if (in_block) {
            if (text[i] == u'*' && i + 1 < n && text[i + 1] == u'/') {
                in_block = false;
                rts_body = false;
                text[i] = u' ';
                text[++i] = u' ';
            } else if (!rts_body) {
                text[i] = u' ';
            }
        }
    }
    bool in_line = false;
    for (std::size_t j = 1; n >= 2 && j < n - 2; ++j) {
        if (!in_line && text[j] == u'/') in_line = text[j + 1] == u'/';
        if (in_line) {
            if (text[j] == u'\r' && text[j + 1] == u'\n') {
                in_line = false;
                ++j;
            } else {
                text[j] = u' ';
            }
        }
    }
}

// 0x4598E8: replace every occurrence (substring, case-sensitive) in place.
void substitute(Text& text, const std::size_t begin, const std::size_t length, std::u16string_view name,
                std::u16string_view replacement) {
    const auto n = name.size();
    if (n > length) return;
    for (std::size_t v = 0; v <= length - n;) {
        if (text.compare(begin + v, n, name) == 0) {
            for (std::size_t j = 0; j < n; ++j) text[begin + v++] = replacement[j];
        } else {
            ++v;
        }
    }
}

constexpr std::pair<std::u16string_view, std::u16string_view> constants[] = {
    {u"SIGASP_STOP_AND_PROCEED", u"1                      "},
    {u"SIGASP_STOP", u"0          "},
    {u"SIGASP_RESTRICTING", u"2                 "},
    {u"SIGASP_APPROACH_1", u"3                "},
    {u"SIGASP_APPROACH_2", u"4                "},
    {u"SIGASP_APPROACH_3", u"5                "},
    {u"SIGASP_CLEAR_1", u"6             "},
    {u"SIGASP_CLEAR_2", u"7             "},
    {u"SIGFN_NORMAL", u"1           "},
    {u"SIGFN_DISTANCE", u"3             "},
    {u"SIGFN_SHUNTING", u"2             "},
    {u"SIGFN_INFO", u"5         "},
    {u"SIGFN_REPEATER", u"4             "},
    {u"BLOCK_CLEAR", u"0          "},
    {u"BLOCK_OCCUPIED", u"1             "},
    {u"BLOCK_JN_OBSTRUCTED", u"2                  "},
    {u"SIGFEAT_USER", u"            "},
    {u"SIGFEAT_NUMBER_PLATE", u"32                  "},
    {u"SIGFEAT_GRADIENT_PLATE", u"33                    "},
    {u"SIGMODE_NORMAL", u"0             "},
    {u"SIGMODE_SHUNT", u"1            "},
    {u"SIGMODE_FAILED", u"2             "},
    {u"SIGMODE_EXCL", u"3           "},
};

constexpr std::pair<std::string_view, int> builtin_ids[] = {
    {"block_state", 1},     {"route_set", 2},         {"next_sig_lr", 3},  {"next_sig_mr", 4},
    {"def_draw_state", 5},  {"sig_feature", 6},       {"state", 7},        {"draw_state", 8},
    {"enabled", 9},         {"dist_multi_sig_mr", 10}, {"this_sig_mr", 11}, {"this_sig_lr", 12},
    {"signal_mode", 13},
};

struct Variable {
    std::string name;
    bool external = false;
    int builtin = 0; // id for externs
};

// One compile of a script body.
class Compiler {
public:
    Compiler(Text& text, std::string& out, std::vector<Variable>& variables)
        : text_(text), out_(out), variables_(variables) {}

    // 0x497ED8 on [begin, begin + length).
    void statements(const std::size_t begin, const std::size_t length, const int depth) {
        for (const auto& [name, value] : constants) substitute(text_, begin, length, name, value);
        std::size_t pos = 0;
        std::size_t word = 0;
        bool external = false;
        const auto a = [&](std::size_t i) { return at(text_, begin + i); };
        while (next_word(begin, pos, length, word)) {
            const auto token = narrow(text_, begin + pos, word);
            if (token == "extern") {
                external = true;
                variables_.push_back({.external = true});
            } else if (token == "float") {
                if (!external) variables_.push_back({});
                pos += word;
                if (!next_word(begin, pos, length, word)) throw CompileError("declaration without a name");
                auto& variable = variables_.back();
                variable.name = narrow(text_, begin + pos, word);
                if (variable.external) {
                    for (const auto& [name, id] : builtin_ids) {
                        if (variable.name == name) variable.builtin = id;
                    }
                }
                pos += word;
                std::size_t end = length;
                statement_end(begin, pos, end, false);
                std::size_t run = end - pos;
                if (operator_run(begin, pos, end, run)) {
                    if (a(pos) == u'=') {
                        // RTS bounds the initialiser by the operator run's
                        // length here, so in practice nothing is compiled.
                        std::size_t limit = run;
                        statement_end(begin, pos + 1, limit, false);
                        expression(begin + pos + 1, limit, 0);
                        out_ += "MOV " + variable.name + ",AX\r\n";
                    }
                }
                word = run;
                external = false;
            } else if (token == "if") {
                pos += 2;
                next_word(begin, pos, length, word);
                ++pos;
                word -= 2;
                expression(begin + pos, word, 0);
                out_ += "JZ if" + std::to_string(depth) + "\r\n";
                pos += word + 1;
                word = 0;
                skip_spaces(begin, pos);
                std::size_t end = length;
                if (!statement_end(begin, pos, end, false)) throw CompileError("if without a statement");
                std::size_t body = end - pos + 1;
                statements(begin + pos, body, depth + 1);
                out_ += "JMP else" + std::to_string(depth) + "\r\n";
                out_ += ":if" + std::to_string(depth) + "\r\n";
                pos += body;
                skip_spaces(begin, pos);
                if (pos < length) {
                    if (a(pos) == u'e' && a(pos + 1) == u'l' && a(pos + 2) == u's' && a(pos + 3) == u'e') {
                        pos += 4;
                        word = 0;
                        skip_spaces(begin, pos);
                        std::size_t else_end = length;
                        if (!statement_end(begin, pos, else_end, true)) throw CompileError("else without a statement");
                        word = else_end - pos + 1;
                        statements(begin + pos, word, depth + 1);
                    } else {
                        word = 0;
                    }
                }
                out_ += ":else" + std::to_string(depth) + "\r\n";
            } else if (token == "return") {
                out_ += "RET\r\n";
            } else if (word > 1 && a(pos + 1) >= u'A') {
                std::size_t i = pos;
                while (at(text_, begin + i) != u';') {
                    if (begin + i >= text_.size()) throw CompileError("statement without ';'");
                    ++i;
                }
                expression(begin + pos, i - pos + 1, 0);
                pos = i;
                word = 0;
            }
            pos += word;
        }
    }

private:
    struct Token {
        int kind = 0; // 1 number, 2 call, 3 identifier, 4 group, 5 operator
        std::size_t start{}, end{}, length{};
        int precedence{};
        int op{};
        std::size_t args_begin{}, args_end{};
        bool binary = true;
    };

    // 0x458D98: the next word or bracketed group within [pos, limit].
    bool next_word(const std::size_t base, std::size_t& pos, const std::size_t limit, std::size_t& length) {
        auto v = pos;
        if (v > limit) return false;
        while (v <= limit) {
            const auto c = at(text_, base + v);
            if ((c >= 0x2D && c <= 0x39) || c >= 0x41 || c == u'(') break;
            ++v;
        }
        if (v >= limit) return false;
        pos = v;
        length = 0;
        if (at(text_, base + v) == u'(') {
            ++v;
            if (!match_bracket(base, limit, v)) return false;
            length = v - pos;
        } else {
            while (length + v <= limit) {
                const auto c = at(text_, base + v + length);
                if (!((c >= 0x2D && c <= 0x39) || c >= 0x41)) break;
                ++length;
            }
        }
        return length + pos <= limit;
    }

    // 0x459ACC: past the bracket matching the one before `pos`.
    bool match_bracket(const std::size_t base, const std::size_t limit, std::size_t& pos) {
        int depth = 1;
        while (pos < limit && depth) {
            const auto c = at(text_, base + pos);
            if (c == u'(') ++depth;
            if (c == u')') --depth;
            ++pos;
        }
        return depth == 0;
    }

    // 0x458190 with no parameters to skip: past spaces (no bound in RTS).
    void skip_spaces(const std::size_t base, std::size_t& pos) {
        while (base + pos < text_.size() && text_[base + pos] <= 0x20) ++pos;
    }

    // 0x458FC8: the `;` at brace depth 0, or the closing `}`; with `chain`,
    // a following `else` statement is included.
    bool statement_end(const std::size_t base, std::size_t from, std::size_t& end, const bool chain) {
        const auto bound = end;
        int depth = 0;
        if (from > bound) return false;
        while (true) {
            const auto c = at(text_, base + from);
            bool done = depth == 0 && c == u';';
            if (c == u'{') {
                ++depth;
            } else if (c == u'}') {
                if (depth <= 1) done = true;
                --depth;
            }
            if (done) break;
            if (++from > bound) return false;
        }
        if (chain) {
            auto next = from + 1;
            skip_spaces(base, next);
            if (at(text_, base + next) == u'e' && at(text_, base + next + 1) == u'l' && at(text_, base + next + 2) == u's'
                && at(text_, base + next + 3) == u'e') {
                auto chained = bound;
                if (statement_end(base, next, chained, true)) from = chained;
            }
        }
        end = from;
        return true;
    }

    // 0x458EAC: the next run of operator characters before `limit`.
    bool operator_run(const std::size_t base, std::size_t& pos, const std::size_t limit, std::size_t& run) {
        const auto is_op = [](const char16_t c) {
            return (c >= 0x21 && c < 0x30) || (c >= 0x3C && c < 0x3F) || c == u'|' || c == u'^';
        };
        auto i = pos;
        while (i <= limit && !is_op(at(text_, base + i))) ++i;
        if (i > limit) return false;
        pos = i;
        run = 0;
        while (i + run <= limit && is_op(at(text_, base + i + run))) ++run;
        return i + run <= limit;
    }

    // 0x45911C: the next expression token within [pos, limit) of `base`.
    bool next_token(const std::size_t base, std::size_t& pos, const std::size_t limit, Token& token) {
        if (pos >= limit) return false;
        skip_spaces(base, pos);
        const auto c = at(text_, base + pos);
        auto v = pos;
        token = {};
        token.start = pos;
        if ((c >= u'0' && c <= u'9') || c == u'.') {
            while (v <= limit && ((at(text_, base + v) >= u'0' && at(text_, base + v) <= u'9') || at(text_, base + v) == u'.')) ++v;
            token.kind = 1;
            token.end = v;
        } else if (c >= 0x41 && c <= 0x7A) {
            while (v <= limit) {
                const auto d = at(text_, base + v);
                if (!((d >= u'0' && d <= u'9') || (d >= 0x41 && d <= 0x7A) || d == u'_')) break;
                ++v;
            }
            auto after = v;
            if (at(text_, base + after) <= 0x20) skip_spaces(base, after);
            if (at(text_, base + after) == u'(') {
                token.kind = 2;
                token.end = v;
                token.length = v - pos;
                token.args_begin = after + 1;
                auto close = token.args_begin;
                if (!match_bracket(base, limit, close)) throw CompileError("unbalanced call brackets");
                token.args_end = close;
                token.end = close;
                pos = close;
                return true;
            }
            token.kind = 3;
            token.end = v;
        } else if (c == u'(') {
            ++v;
            if (!match_bracket(base, limit, v)) throw CompileError("unbalanced brackets");
            token.kind = 4;
            token.end = v;
        } else {
            if (c != u'-' && c != u'=' && c != u'!' && c != u'>' && c != u'<' && c != u'&' && c != u'|' && c != u'#') return false;
            token.kind = 5;
            const auto three = std::u16string{at(text_, base + pos), at(text_, base + pos + 1), at(text_, base + pos + 2)};
            const auto two = three.substr(0, 2);
            std::size_t length = 3;
            if (three == u"==#") token.op = 1;
            else if (three == u">=#") token.op = 5;
            else if (three == u"<=#") token.op = 3;
            else if (three == u"!=#") token.op = 6;
            else {
                length = 2;
                if (two == u">#") token.op = 4;
                else if (two == u"<#") token.op = 2;
                else if (two == u"==") token.op = 13;
                else if (two == u">=") token.op = 17;
                else if (two == u"<=") token.op = 15;
                else if (two == u"!=") token.op = 18;
                else if (two == u"&&") token.op = 7;
                else if (two == u"||") token.op = 8;
                else {
                    length = 1;
                    switch (c) {
                    case u'>': token.op = 16; break;
                    case u'<': token.op = 14; break;
                    case u'=': token.op = 10; break;
                    case u'!': token.op = 12; token.binary = false; break;
                    case u'-': token.op = 11; token.binary = false; break;
                    case u'#': token.op = 19; token.binary = false; break;
                    default: throw CompileError("unknown operator");
                    }
                }
            }
            token.precedence = token.op == 10 ? 0 : (token.op == 7 || token.op == 8) ? 1 : !token.binary ? 3 : 2;
            token.end = pos + length;
        }
        if (token.kind != 5) token.length = token.end - token.start;
        else token.length = token.end - token.start;
        pos = token.end;
        return true;
    }

    // 0x497E14: compiles [begin, begin + length) at register r. False when
    // there was no token.
    bool expression(const std::size_t begin, const std::size_t length, const int r) {
        if (length == 0) return false;
        std::vector<Token> tokens;
        std::size_t pos = 0;
        Token token;
        while (next_token(begin, pos, length, token)) {
            token.start += begin;
            token.end += begin;
            token.args_begin += begin;
            token.args_end += begin;
            tokens.push_back(token);
        }
        if (tokens.empty()) return false;
        generate(tokens, 0, tokens.size() - 1, r);
        return true;
    }

    [[nodiscard]] std::string token_text(const Token& token) const { return narrow(text_, token.start, token.length); }

    // 0x497334 on tokens [first, last].
    void generate(const std::vector<Token>& tokens, const std::size_t first, const std::size_t last, const int r) {
        if (r > 7) throw CompileError("expression needs more than 8 registers");
        int lowest = 10;
        int operators = 0;
        for (std::size_t i = first; i <= last; ++i) {
            if (tokens[i].kind == 5) {
                lowest = (std::min)(lowest, tokens[i].precedence);
                ++operators;
            }
        }
        const auto count = last - first + 1;
        if (operators == 0) {
            if (count > 2) throw CompileError("operands without an operator");
            const auto& token = tokens[first];
            if (token.kind == 1 || token.kind == 3) {
                out_ += "MOV " + reg(r) + "," + token_text(token) + "\r\n";
            } else if (token.kind == 2) {
                auto from = token.args_begin;
                auto i = token.args_begin;
                for (; token.args_end > i; ++i) {
                    if (at(text_, i) == u',') {
                        expression(from, i - from, r);
                        out_ += "PUSH " + reg(r) + "\r\n";
                        from = i + 1;
                    }
                }
                if (i > from && expression(from, i - from, r)) out_ += "PUSH " + reg(r) + "\r\n";
                out_ += "CALL " + token_text(token) + "," + reg(r) + "\r\n";
            } else if (token.kind == 4) {
                expression(token.start + 1, token.length, r);
            }
            return;
        }
        if (count == 1) throw CompileError("an operator alone");
        std::optional<std::size_t> left = first;
        for (std::size_t i = first; i <= last; ++i) {
            const auto& op = tokens[i];
            if (op.kind == 5 && op.precedence == lowest) {
                if (i == last) throw CompileError("operator without a right operand");
                auto right_end = last;
                for (auto j = i + 1; j <= last; ++j) {
                    if (tokens[j].kind == 5 && tokens[j].precedence == lowest) {
                        right_end = j - 1;
                        break;
                    }
                }
                if (left && op.op != 10 && op.binary) {
                    // RTS: with nothing before the operator the left part is empty.
                    if (i > *left) generate(tokens, *left, i - 1, r);
                    left.reset();
                }
                if (right_end < i + 1) throw CompileError("operator without a right operand");
                generate(tokens, i + 1, right_end, r + 1);
                const auto pair = " " + reg(r) + "," + reg(r + 1) + "\r\n";
                switch (op.op) {
                case 1: out_ += "CMPE" + pair; break;
                case 2: out_ += "CMPL" + pair; break;
                case 3: out_ += "CMPLE" + pair; break;
                case 4: out_ += "CMPG" + pair; break;
                case 5: out_ += "CMPGE" + pair; break;
                case 6: out_ += "CMPNE" + pair; break;
                case 7: out_ += "AND" + pair; break;
                case 8: out_ += "OR" + pair; break;
                case 9: out_ += "XOR" + pair; break;
                case 10:
                    if (i == first || tokens[i - 1].kind != 3) throw CompileError("assignment to a non-variable");
                    out_ += "MOV " + token_text(tokens[i - 1]) + "," + reg(r + 1) + "\r\n";
                    break;
                case 11: out_ += "NEG " + reg(r + 1) + "\r\nMOV" + pair; break;
                case 12: out_ += "NOT " + reg(r + 1) + "\r\nMOV" + pair; break;
                case 13: out_ += "CMPEF" + pair; break;
                case 14: out_ += "CMPLF" + pair; break;
                case 15: out_ += "CMPLEF" + pair; break;
                case 16: out_ += "CMPGF" + pair; break;
                case 17: out_ += "CMPGEF" + pair; break;
                case 18: out_ += "CMPNEF" + pair; break;
                case 19: out_ += "ROUND " + reg(r + 1) + "\r\nMOV" + pair; break;
                default: break;
                }
            }
        }
    }

    Text& text_;
    std::string& out_;
    std::vector<Variable>& variables_;
};

constexpr std::pair<std::string_view, SignalOp> mnemonics[] = {
    {"MOV", SignalOp::mov},       {"CALL", SignalOp::call},     {"PUSH", SignalOp::push},
    {"JMP", SignalOp::jmp},       {"JZ", SignalOp::jz},         {"CMPE", SignalOp::cmpe},
    {"CMPNE", SignalOp::cmpne},   {"CMPG", SignalOp::cmpg},     {"CMPGE", SignalOp::cmpge},
    {"CMPL", SignalOp::cmpl},     {"CMPLE", SignalOp::cmple},   {"CMPEF", SignalOp::cmpef},
    {"CMPNEF", SignalOp::cmpnef}, {"CMPGF", SignalOp::cmpgf},   {"CMPGEF", SignalOp::cmpgef},
    {"CMPLF", SignalOp::cmplf},   {"CMPLEF", SignalOp::cmplef}, {"AND", SignalOp::op_and},
    {"OR", SignalOp::op_or},      {"XOR", SignalOp::op_xor},    {"NOT", SignalOp::op_not},
    {"NEG", SignalOp::neg},       {"ROUND", SignalOp::round},   {"RET", SignalOp::ret},
};

// The assembler (0x498964): text assembly to instructions. Jumps take the
// first not yet taken label of their name, in order.
void assemble(SignalScript& script, const std::vector<Variable>& variables) {
    struct Label {
        std::string name;
        std::uint32_t index{};
        bool taken = false;
    };
    std::vector<Label> labels;
    std::vector<std::pair<std::size_t, std::string>> jumps;
    for (const auto& variable : variables) {
        if (!variable.external && !variable.name.empty()
            && std::find(script.locals.begin(), script.locals.end(), variable.name) == script.locals.end()) {
            script.locals.push_back(variable.name);
        }
    }
    const auto operand = [&](std::string_view text) -> SignalOperand {
        if (text.size() == 2 && text[1] == 'X' && text[0] >= 'A' && text[0] <= 'H') {
            return {.kind = SignalOperandKind::reg, .index = static_cast<std::uint32_t>(text[0] - 'A')};
        }
        if (!text.empty() && ((text[0] >= '0' && text[0] <= '9') || text[0] == '.')) {
            return {.kind = SignalOperandKind::immediate, .value = static_cast<float>(std::strtod(std::string(text).c_str(), nullptr))};
        }
        for (const auto& variable : variables) {
            if (variable.name != text) continue;
            if (!variable.external) {
                const auto it = std::find(script.locals.begin(), script.locals.end(), variable.name);
                return {.kind = SignalOperandKind::local, .index = static_cast<std::uint32_t>(it - script.locals.begin())};
            }
            if (variable.builtin == 7 || variable.builtin == 8 || variable.builtin == 9 || variable.builtin == 13) {
                return {.kind = SignalOperandKind::head, .index = static_cast<std::uint32_t>(variable.builtin)};
            }
        }
        throw CompileError("unknown name '" + std::string(text) + "'");
    };
    std::size_t start = 0;
    const auto& text = script.assembly;
    while (start < text.size()) {
        auto end = text.find("\r\n", start);
        if (end == std::string::npos) end = text.size();
        const std::string_view line(text.data() + start, end - start);
        start = end + 2;
        if (line.empty()) continue;
        if (line[0] == ':') {
            labels.push_back({.name = std::string(line.substr(1)), .index = static_cast<std::uint32_t>(script.code.size())});
            continue;
        }
        const auto space = line.find(' ');
        const auto mnemonic = line.substr(0, space);
        const auto rest = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
        SignalInstruction instruction;
        bool known = false;
        for (const auto& [name, op] : mnemonics) {
            if (mnemonic == name) {
                instruction.op = op;
                known = true;
            }
        }
        if (!known) throw CompileError("unknown instruction " + std::string(mnemonic));
        const auto comma = rest.find(',');
        const auto first = rest.substr(0, comma);
        const auto second = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        switch (instruction.op) {
        case SignalOp::jmp:
        case SignalOp::jz:
            jumps.emplace_back(script.code.size(), std::string(first));
            break;
        case SignalOp::ret:
            break;
        case SignalOp::call: {
            int id = 0;
            for (const auto& variable : variables) {
                if (variable.name == first && variable.external) id = variable.builtin;
            }
            if (id == 0 || id == 7 || id == 8 || id == 9 || id == 13) throw CompileError("call of unknown function " + std::string(first));
            instruction.builtin = static_cast<SignalBuiltin>(id);
            instruction.a = operand(second);
            if (std::find(script.builtins_used.begin(), script.builtins_used.end(), instruction.builtin) == script.builtins_used.end()) {
                script.builtins_used.push_back(instruction.builtin);
            }
            break;
        }
        case SignalOp::push:
        case SignalOp::op_not:
        case SignalOp::neg:
        case SignalOp::round:
            instruction.a = operand(first);
            break;
        default:
            instruction.a = operand(first);
            instruction.b = operand(second);
            break;
        }
        script.code.push_back(instruction);
    }
    for (const auto& [index, name] : jumps) {
        auto it = std::find_if(labels.begin(), labels.end(), [&](const Label& label) { return !label.taken && label.name == name; });
        if (it == labels.end()) throw CompileError("jump to a missing label " + name);
        it->taken = true;
        script.code[index].target = it->index;
    }
}

// Whether a local can be read before any write on some path (the code only
// jumps forward, so one pass over the paths with "written" sets suffices).
bool reads_stale_local(const SignalScript& script) {
    const auto n = script.code.size();
    if (script.locals.empty()) return false;
    // written[i]: locals surely written on every path reaching i.
    std::vector<std::optional<std::set<std::uint32_t>>> written(n + 1);
    written[0] = std::set<std::uint32_t>{};
    const auto merge = [&](std::size_t target, const std::set<std::uint32_t>& state) {
        if (target > n) return;
        if (!written[target]) {
            written[target] = state;
        } else {
            std::set<std::uint32_t> both;
            for (auto v : *written[target]) {
                if (state.contains(v)) both.insert(v);
            }
            written[target] = both;
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (!written[i]) continue; // unreachable
        auto state = *written[i];
        const auto& in = script.code[i];
        const auto reads = [&](const SignalOperand& operand) {
            return operand.kind == SignalOperandKind::local && !state.contains(operand.index);
        };
        switch (in.op) {
        case SignalOp::mov:
            if (reads(in.b)) return true;
            if (in.a.kind == SignalOperandKind::local) state.insert(in.a.index);
            break;
        case SignalOp::jmp:
            merge(in.target, state);
            continue;
        case SignalOp::jz:
            merge(in.target, state);
            break;
        case SignalOp::ret:
            continue;
        case SignalOp::call:
            if (in.a.kind == SignalOperandKind::local) state.insert(in.a.index);
            break;
        default:
            if (reads(in.a) || reads(in.b)) return true;
            break;
        }
        merge(i + 1, state);
    }
    return false;
}

} // namespace

const char* to_string(const SignalOp op) noexcept {
    for (const auto& [name, value] : mnemonics) {
        if (value == op) return name.data();
    }
    return "?";
}

const SignalScript* SignalScriptFile::find(const std::string_view type) const {
    const auto lower = signal_name_key(type);
    for (const auto& script : scripts) {
        if (script.name == lower) return &script;
    }
    return nullptr;
}

std::string SignalScriptFile::assembly() const {
    std::string result;
    for (const auto& script : scripts) result += "SCRIPT " + script.name + "\r\n\r\n" + script.assembly + "\r\n\r\n";
    return result;
}

SignalScriptFile compile_signal_scripts(const std::string_view bytes, const SignalConfig* config) {
    SignalScriptFile file;
    Text text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        const auto c = static_cast<unsigned char>(byte);
        text.push_back(c < 0x80 ? static_cast<char16_t>(c) : static_cast<char16_t>(0x400 + c));
    }
    blank_comments(text);

    // Scripts start at "SCRIPT" (searched from index 32, as RTS does) and run
    // to the next "SCRIPT" (a plain, case-sensitive substring search).
    const auto is_space = [](char16_t c) { return c <= 0x20; };
    std::size_t search = 32;
    std::set<std::string> seen;
    while (true) {
        std::size_t start = std::u16string::npos;
        for (auto i = search; i + 6 <= text.size(); ++i) {
            if (text.compare(i, 6, u"SCRIPT") == 0 && (i == 0 || is_space(text[i - 1]))
                && (i + 6 >= text.size() || is_space(text[i + 6]))) {
                start = i;
                break;
            }
        }
        if (start == std::u16string::npos) break;
        auto end = text.find(u"SCRIPT", start + 6);
        if (end == std::u16string::npos) end = text.size();
        search = end;

        auto name_begin = start + 6;
        while (name_begin < end && is_space(text[name_begin])) ++name_begin;
        auto line_end = name_begin;
        while (line_end < end && text[line_end] != u'\n' && text[line_end] != u'\r') ++line_end;
        auto name_end = name_begin;
        while (name_end < line_end && !is_space(text[name_end]) && text[name_end] != u'(' && text[name_end] != u')') ++name_end;
        const std::string name = signal_name_key(to_utf8(text, name_begin, name_end - name_begin));

        if (config && !config->find_type(name)) {
            file.messages.push_back("script " + name + ": no signal type of that name");
            continue;
        }
        if (seen.contains(name)) {
            file.messages.push_back("script " + name + ": a second script for this signal type (skipped)");
            continue;
        }
        seen.insert(name);

        SignalScript script{.name = name};
        std::vector<Variable> variables;
        const auto body = line_end + 2;
        try {
            if (body < end) {
                Compiler compiler(text, script.assembly, variables);
                compiler.statements(body, end - body, 0);
            }
            assemble(script, variables);
            script.reads_stale_local = reads_stale_local(script);
        } catch (const CompileError& error) {
            script.error = error.what();
            file.messages.push_back("script " + name + ": " + error.what());
        }
        file.scripts.push_back(std::move(script));
    }
    return file;
}

SignalScriptFile load_signal_scripts(const core::Vfs& vfs, const core::VirtualPath& route, const SignalConfig& config) {
    SignalScriptFile all;
    for (const auto& name : config.script_files) {
        const auto path = route.resolve(name);
        std::string error;
        const auto bytes = path ? vfs.read(*path, &error) : std::nullopt;
        if (!bytes) {
            all.messages.push_back("script file " + name + ": " + (error.empty() ? "not found" : error));
            continue;
        }
        auto file = compile_signal_scripts(*bytes, &config);
        for (auto& message : file.messages) all.messages.push_back(name + ": " + message);
        for (auto& script : file.scripts) {
            if (all.find(script.name)) {
                all.messages.push_back(name + ": script " + script.name + " defined again in another file (skipped)");
                continue;
            }
            all.scripts.push_back(std::move(script));
        }
    }
    return all;
}

} // namespace openrts::formats
