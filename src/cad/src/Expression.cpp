// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/Expression.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace aerocad::cad {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

class Parser {
public:
    Parser(std::string_view text, const ParameterLookup& lookup) : m_text(text), m_lookup(lookup) {}

    double parse()
    {
        const double v = expression();
        skipSpace();
        if (m_pos != m_text.size())
            fail("unexpected '" + std::string(1, m_text[m_pos]) + "'");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& message) const
    {
        throw ExpressionError("Invalid expression \"" + std::string(m_text) + "\": " + message);
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
    }

    bool accept(char c)
    {
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == c) {
            ++m_pos;
            return true;
        }
        return false;
    }

    double expression()
    {
        double v = term();
        for (;;) {
            if (accept('+'))
                v += term();
            else if (accept('-'))
                v -= term();
            else
                return v;
        }
    }

    double term()
    {
        double v = unary();
        for (;;) {
            if (accept('*')) {
                v *= unary();
            } else if (accept('/')) {
                const double d = unary();
                if (d == 0.0)
                    fail("division by zero");
                v /= d;
            } else {
                return v;
            }
        }
    }

    double unary()
    {
        if (accept('-'))
            return -unary();
        if (accept('+'))
            return unary();
        return power();
    }

    double power()
    {
        const double base = withUnit(primary());
        if (accept('^'))
            return std::pow(base, unary());
        return base;
    }

    /// Optional unit suffix right after a value ("10 mm", "2in", "45 deg").
    double withUnit(double v)
    {
        skipSpace();
        const std::size_t save = m_pos;
        const std::string word = identifier();
        if (word.empty())
            return v;
        if (word == "mm" || word == "deg")
            return v;
        if (word == "cm")
            return v * 10.0;
        if (word == "m")
            return v * 1000.0;
        if (word == "in")
            return v * 25.4;
        m_pos = save; // not a unit: e.g. "2 width" is a syntax error reported by the caller
        return v;
    }

    std::string identifier()
    {
        skipSpace();
        const std::size_t start = m_pos;
        while (m_pos < m_text.size() &&
               (std::isalnum(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '_')) {
            if (m_pos == start && std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
                break;
            ++m_pos;
        }
        return std::string(m_text.substr(start, m_pos - start));
    }

    double primary()
    {
        skipSpace();
        if (m_pos >= m_text.size())
            fail("unexpected end");
        const char c = m_text[m_pos];
        if (accept('(')) {
            const double v = expression();
            if (!accept(')'))
                fail("missing ')'");
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.')
            return number();
        const std::string name = identifier();
        if (name.empty())
            fail("unexpected '" + std::string(1, c) + "'");
        if (accept('('))
            return call(name);
        if (name == "pi")
            return std::numbers::pi;
        if (m_lookup) {
            if (auto v = m_lookup(name))
                return *v;
        }
        fail("unknown parameter '" + name + "'");
    }

    double number()
    {
        std::size_t end = m_pos;
        while (end < m_text.size() &&
               (std::isdigit(static_cast<unsigned char>(m_text[end])) || m_text[end] == '.' ||
                ((m_text[end] == 'e' || m_text[end] == 'E') && end + 1 < m_text.size() &&
                 (std::isdigit(static_cast<unsigned char>(m_text[end + 1])) || m_text[end + 1] == '-' ||
                  m_text[end + 1] == '+')))) {
            if ((m_text[end] == 'e' || m_text[end] == 'E') && (m_text[end + 1] == '-' || m_text[end + 1] == '+'))
                ++end;
            ++end;
        }
        // std::from_chars for double is not available everywhere (older libc++); use stod.
        const std::string token(m_text.substr(m_pos, end - m_pos));
        std::size_t used = 0;
        double v = 0.0;
        try {
            v = std::stod(token, &used);
        } catch (const std::exception&) {
            fail("invalid number '" + token + "'");
        }
        if (used != token.size())
            fail("invalid number '" + token + "'");
        m_pos = end;
        return v;
    }

    double call(const std::string& name)
    {
        std::vector<double> args;
        if (!accept(')')) {
            do {
                args.push_back(expression());
            } while (accept(','));
            if (!accept(')'))
                fail("missing ')' after arguments of " + name);
        }
        auto need = [&](std::size_t n) {
            if (args.size() != n)
                fail(name + " takes " + std::to_string(n) + " argument(s)");
        };
        if (name == "sqrt") {
            need(1);
            if (args[0] < 0)
                fail("sqrt of a negative number");
            return std::sqrt(args[0]);
        }
        if (name == "abs") {
            need(1);
            return std::abs(args[0]);
        }
        if (name == "sin") {
            need(1);
            return std::sin(args[0] * kDegToRad);
        }
        if (name == "cos") {
            need(1);
            return std::cos(args[0] * kDegToRad);
        }
        if (name == "tan") {
            need(1);
            return std::tan(args[0] * kDegToRad);
        }
        if (name == "min" || name == "max") {
            if (args.empty())
                fail(name + " needs arguments");
            double v = args[0];
            for (double a : args)
                v = name == "min" ? std::min(v, a) : std::max(v, a);
            return v;
        }
        fail("unknown function '" + name + "'");
    }

    std::string_view m_text;
    const ParameterLookup& m_lookup;
    std::size_t m_pos = 0;
};

} // namespace

double evaluateExpression(std::string_view expression, const ParameterLookup& lookup)
{
    const double v = Parser(expression, lookup).parse();
    if (!std::isfinite(v))
        throw ExpressionError("Expression \"" + std::string(expression) + "\" is not a finite number");
    return v;
}

} // namespace aerocad::cad
