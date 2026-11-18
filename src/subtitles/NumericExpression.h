#pragma once

#include <array>
#include <cmath>
#include <cwchar>
#include <limits>

namespace AssNumeric
{
enum Variable { Start, Mid, End, Duration, Width, Height, Left, Right, Center, Top, Bottom, Middle, VariableCount };
using Context = std::array<double, VariableCount>;

struct Operand
{
    double value = 0;
    bool relative = false;

    double Resolve(double current, double direction = 1) const
    {
        return relative ? current + direction * value : value;
    }
};

class Program
{
    enum Code { Literal, Percentage, VariableValue, Add, Subtract, Multiply, Divide, Negate };
    struct Step { Code code; double value; };
    std::array<Step, 64> steps{};
    size_t count = 0;
    bool valid = false;

    class Parser
    {
        Program& output;
        const wchar_t* cursor;
        bool percentages;

        void Space()
        {
            while (*cursor == L' ' || *cursor == L'\t') ++cursor;
        }

        bool Emit(Code code, double value = 0)
        {
            if (output.count == output.steps.size()) return false;
            output.steps[output.count++] = {code, value};
            return true;
        }

        bool Atom(unsigned depth)
        {
            if (depth > 16) return false;
            Space();
            const wchar_t c = *cursor;
            if (c == L'+' || c == L'-')
            {
                ++cursor;
                return Atom(depth + 1) && (c != L'-' || Emit(Negate));
            }
            if (c == L'(')
            {
                ++cursor;
                if (!Expression(depth + 1) || *cursor != L')') return false;
                ++cursor;
                return true;
            }
            if (c == L'$')
            {
                const wchar_t* first = ++cursor;
                while ((*cursor >= L'a' && *cursor <= L'z') || (*cursor >= L'A' && *cursor <= L'Z'))
                    ++cursor;
                const size_t length = cursor - first;
                static constexpr const wchar_t* names[] = {
                    L"start", L"mid", L"end", L"dur", L"width", L"height",
                    L"left", L"right", L"center", L"top", L"bottom", L"middle"
                };
                for (int i = 0; i < VariableCount; ++i)
                    if (std::wcslen(names[i]) == length && !std::wcsncmp(first, names[i], length))
                        return Emit(VariableValue, i);
                return false;
            }

            double value = 0;
            bool digits = false;
            while (*cursor >= L'0' && *cursor <= L'9')
            {
                digits = true;
                value = value * 10 + (*cursor++ - L'0');
            }
            if (*cursor == L'.')
            {
                ++cursor;
                double place = 0.1;
                while (*cursor >= L'0' && *cursor <= L'9')
                {
                    digits = true;
                    value += (*cursor++ - L'0') * place;
                    place *= 0.1;
                }
            }
            if (!digits || !std::isfinite(value)) return false;
            if (percentages && *cursor == L'%')
            {
                ++cursor;
                return Emit(Percentage, value / 100);
            }
            return Emit(Literal, value);
        }

        bool Product(unsigned depth)
        {
            if (!Atom(depth)) return false;
            Space();
            while (*cursor == L'*' || *cursor == L'/')
            {
                const auto op = *cursor++ == L'*' ? Multiply : Divide;
                if (!Atom(depth) || !Emit(op)) return false;
                Space();
            }
            return true;
        }

        bool Expression(unsigned depth)
        {
            if (!Product(depth)) return false;
            while (*cursor == L'+' || *cursor == L'-')
            {
                const auto op = *cursor++ == L'+' ? Add : Subtract;
                if (!Product(depth) || !Emit(op)) return false;
            }
            return true;
        }

    public:
        Parser(Program& program, const wchar_t* input, bool allowPercent)
            : output(program), cursor(input), percentages(allowPercent) {}

        bool Parse()
        {
            // A standalone negative percentage is measured back from the end.
            if (percentages && *cursor == L'-' &&
                ((cursor[1] >= L'0' && cursor[1] <= L'9') || cursor[1] == L'.'))
            {
                const auto* first = cursor++;
                if (Atom(0) && output.count == 1 && output.steps[0].code == Percentage)
                {
                    Space();
                    if (!*cursor)
                    {
                        output.steps[0].value = 1 - output.steps[0].value;
                        return true;
                    }
                }
                output.count = 0;
                cursor = first;
            }
            return Expression(0) && !*cursor;
        }
    };

public:
    static Program Compile(const wchar_t* input, bool allowPercent)
    {
        Program program;
        if (std::wcslen(input) <= 256)
            program.valid = Parser(program, input, allowPercent).Parse();
        return program;
    }

    bool Evaluate(const Context& context, double& result) const
    {
        if (!valid) return false;
        std::array<double, 64> stack;
        size_t top = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const auto& step = steps[i];
            switch (step.code)
            {
            case Literal: stack[top++] = step.value; break;
            case VariableValue: stack[top++] = context[size_t(step.value)]; break;
            case Percentage: stack[top++] = context[Duration] * step.value; break;
            case Negate: stack[top - 1] = -stack[top - 1]; break;
            default:
                const auto rhs = stack[--top];
                auto& lhs = stack[top - 1];
                if (step.code == Add) lhs += rhs;
                else if (step.code == Subtract) lhs -= rhs;
                else if (step.code == Multiply) lhs *= rhs;
                else
                {
                    if (!rhs) return false;
                    lhs /= rhs;
                }
                break;
            }
            if (!std::isfinite(stack[top - 1])) return false;
        }
        result = stack[0];
        return true;
    }
};
}
