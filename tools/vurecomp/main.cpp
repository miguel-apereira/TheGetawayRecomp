// SPDX-License-Identifier: MIT
//
// tgr-vurecomp: compiles the game's VU microprograms to C++ ahead of time.
//
//   tgr-vurecomp <SCUS_971.33> <programs.csv> <out-dir>
//
// programs.csv lists one microprogram per line, as the game uploads it with
// a VIF MPG command:
//
//   unit,elf_address,size,load_address
//   0,0x0034A120,0x400,0x0
//
// unit is 0 or 1, elf_address is where the code sits in the ELF, size is in
// bytes and load_address is where the game puts it in VU code memory. Lines
// starting with # are comments.
//
// For every program it writes <out-dir>/vu<unit>_<elf_address>.cpp with one
// function that runs the program from any of its instructions on the model
// in src/hw/vu/VuCore.h, plus <out-dir>/vu_programs.cpp with the table the
// runtime uses to find a program by what is in VU code memory. The output is
// built from the user's own ELF and is never committed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct ProgramEntry
{
    int unit = 0;
    std::uint32_t elfAddress = 0;
    std::uint32_t size = 0;
    std::uint32_t loadAddress = 0;
};

// ---- ELF ------------------------------------------------------------------

struct Segment
{
    std::uint32_t address;
    std::uint32_t offset;
    std::uint32_t fileSize;
};

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    if (offset + 4 <= bytes.size())
        std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

std::uint16_t read16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint16_t value = 0;
    if (offset + 2 <= bytes.size())
        std::memcpy(&value, bytes.data() + offset, 2);
    return value;
}

std::vector<Segment> loadSegments(const std::vector<std::uint8_t>& elf)
{
    std::vector<Segment> segments;
    if (elf.size() < 0x34 || std::memcmp(elf.data(), "\x7F" "ELF", 4) != 0)
        return segments;
    const std::uint32_t phoff = read32(elf, 0x1C);
    const std::uint16_t phentsize = read16(elf, 0x2A);
    const std::uint16_t phnum = read16(elf, 0x2C);
    for (std::uint16_t i = 0; i < phnum; ++i)
    {
        const std::size_t ph = phoff + static_cast<std::size_t>(i) * phentsize;
        if (read32(elf, ph) != 1) // PT_LOAD
            continue;
        segments.push_back({read32(elf, ph + 8), read32(elf, ph + 4), read32(elf, ph + 16)});
    }
    return segments;
}

std::optional<std::vector<std::uint8_t>> readProgram(const std::vector<std::uint8_t>& elf,
                                                     const std::vector<Segment>& segments, std::uint32_t address,
                                                     std::uint32_t size)
{
    for (const Segment& segment : segments)
    {
        if (address >= segment.address && address + size <= segment.address + segment.fileSize)
        {
            const std::size_t offset = segment.offset + (address - segment.address);
            if (offset + size > elf.size())
                return std::nullopt;
            return std::vector<std::uint8_t>(elf.begin() + offset, elf.begin() + offset + size);
        }
    }
    return std::nullopt;
}

// FNV-1a, 64-bit: matches tgr::hw::vu::programHash.
std::uint64_t programHash(const std::uint8_t* bytes, std::size_t size)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

// ---- Decoding ---------------------------------------------------------------

const char* kField[4] = {"x", "y", "z", "w"};

int fieldsOf(std::uint32_t word)
{
    return static_cast<int>((word >> 21) & 0xF);
}
int ftOf(std::uint32_t word)
{
    return static_cast<int>((word >> 16) & 0x1F);
}
int fsOf(std::uint32_t word)
{
    return static_cast<int>((word >> 11) & 0x1F);
}
int fdOf(std::uint32_t word)
{
    return static_cast<int>((word >> 6) & 0x1F);
}
int imm11(std::uint32_t word)
{
    return static_cast<int>(static_cast<std::int32_t>(word << 21) >> 21);
}

std::string fieldText(int fields)
{
    std::string text;
    for (int c = 0; c < 4; ++c)
    {
        if (fields & (8 >> c))
            text += kField[c];
    }
    return text;
}

// One field selected by a two-bit index, as the fields mask for stalls.
int fieldBit(int index)
{
    return 8 >> index;
}

struct Translator
{
    int unit = 0;
    std::uint32_t codeMask = 0; // bytes
    std::ostringstream out;
    std::vector<std::string> warnings;

    void warn(std::uint32_t pc, const std::string& text)
    {
        warnings.push_back(std::format("0x{:04X}: {}", pc, text));
    }

    // ---- Upper ----------------------------------------------------------------

    // Emits the stalls for the upper instruction's reads and the expression
    // that computes it into `u`. Returns false for a reserved encoding.
    bool upper(std::uint32_t word, std::string& stalls, std::string& compute, std::string& text)
    {
        const int op = static_cast<int>(word & 0x3F);
        const int dest = fieldsOf(word);
        const int ft = ftOf(word);
        const int fs = fsOf(word);
        const int fd = fdOf(word);
        const std::string d = std::to_string(dest);

        auto stallVf = [&](int reg, int fields) {
            if (reg != 0 && fields != 0)
                stalls += std::format("vu::stallVf(s, {}, {}); ", reg, fields);
        };
        auto stallAcc = [&](int fields) { stalls += std::format("vu::stallAcc(s, {}); ", fields); };
        auto arith = [&](const char* opName, bool toAcc, const std::string& source, int target) {
            compute = std::format("vu::arithmetic(s, vu::Op::{}, {}, {}, {}, {}, {})", opName, toAcc ? "true" : "false",
                                  target, fs, source, d);
        };

        static const char* kArith[] = {"Add", "Sub", "Madd", "Msub", "Max", "Mini", nullptr, "Mul"};
        static const char* kArithName[] = {"ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", nullptr, "MUL"};

        if (op < 0x1C) // xxxbc
        {
            const int group = op >> 2;
            const int bc = op & 3;
            const char* name = kArith[group == 6 ? 7 : group];
            stallVf(fs, dest);
            stallVf(ft, fieldBit(bc));
            if (group == 2 || group == 3)
                stallAcc(dest);
            arith(name, false, std::format("vu::sourceBroadcast(s, {}, {})", ft, bc), fd);
            text = std::format("{}{}.{} vf{}, vf{}, vf{}{}", kArithName[group == 6 ? 7 : group], kField[bc],
                               fieldText(dest), fd, fs, ft, kField[bc]);
            return true;
        }
        if (op < 0x30)
        {
            struct Form
            {
                const char* op;
                const char* name;
                int source; // 0 Q, 1 I, 2 VF
            };
            static const Form kForms[] = {
                {"Mul", "MULq", 0},  {"Max", "MAXi", 1},  {"Mul", "MULi", 1},  {"Mini", "MINIi", 1},
                {"Add", "ADDq", 0},  {"Madd", "MADDq", 0}, {"Add", "ADDi", 1}, {"Madd", "MADDi", 1},
                {"Sub", "SUBq", 0},  {"Msub", "MSUBq", 0}, {"Sub", "SUBi", 1}, {"Msub", "MSUBi", 1},
                {"Add", "ADD", 2},   {"Madd", "MADD", 2},  {"Mul", "MUL", 2},  {"Max", "MAX", 2},
                {"Sub", "SUB", 2},   {"Msub", "MSUB", 2},  {nullptr, "OPMSUB", 2}, {"Mini", "MINI", 2},
            };
            const Form& form = kForms[op - 0x1C];
            stallVf(fs, dest);
            if (form.source == 2)
                stallVf(ft, op == 0x2E ? 0xE : dest);
            const std::string name(form.name);
            if (name.find("MADD") == 0 || name.find("MSUB") == 0 || name == "OPMSUB")
                stallAcc(dest);
            if (op == 0x2E)
            {
                compute = std::format("vu::outerProduct(s, true, {}, {}, {}, {})", fd, fs, ft, d);
                text = std::format("OPMSUB.{} vf{}, vf{}, vf{}", fieldText(dest), fd, fs, ft);
                return true;
            }
            const std::string source = form.source == 0   ? "vu::sourceScalar(s.q)"
                                       : form.source == 1 ? "vu::sourceScalar(s.i)"
                                                          : std::format("vu::sourceVf(s, {})", ft);
            arith(form.op, false, source, fd);
            text = std::format("{}.{} vf{}, vf{}{}", form.name, fieldText(dest), fd, fs,
                               form.source == 2 ? std::format(", vf{}", ft) : std::string(form.source ? ", I" : ", Q"));
            return true;
        }
        if (op < 0x3C)
            return false;

        const int special = static_cast<int>((word & 0x3) | ((word >> 4) & 0x7C));
        if (special < 0x10) // xxxAbc
        {
            static const char* kOps[] = {"Add", "Sub", "Madd", "Msub"};
            static const char* kNames[] = {"ADDA", "SUBA", "MADDA", "MSUBA"};
            const int group = special >> 2;
            const int bc = special & 3;
            stallVf(fs, dest);
            stallVf(ft, fieldBit(bc));
            if (group >= 2)
                stallAcc(dest);
            arith(kOps[group], true, std::format("vu::sourceBroadcast(s, {}, {})", ft, bc), 0);
            text = std::format("{}{}.{} ACC, vf{}, vf{}{}", kNames[group], kField[bc], fieldText(dest), fs, ft, kField[bc]);
            return true;
        }
        if (special < 0x18) // ITOF / FTOI
        {
            static const char* kScale[] = {"1.0", "16.0", "4096.0", "32768.0"};
            static const char* kSuffix[] = {"0", "4", "12", "15"};
            const bool toFloat = special < 0x14;
            stallVf(fs, dest);
            compute = std::format("vu::{}(s, {}, {}, {}, {})", toFloat ? "toFloat" : "toInteger", ft, fs, d,
                                  kScale[special & 3]);
            text = std::format("{}{}.{} vf{}, vf{}", toFloat ? "ITOF" : "FTOI", kSuffix[special & 3], fieldText(dest), ft,
                               fs);
            return true;
        }
        if (special < 0x1C) // MULAbc
        {
            const int bc = special & 3;
            stallVf(fs, dest);
            stallVf(ft, fieldBit(bc));
            arith("Mul", true, std::format("vu::sourceBroadcast(s, {}, {})", ft, bc), 0);
            text = std::format("MULA{}.{} ACC, vf{}, vf{}{}", kField[bc], fieldText(dest), fs, ft, kField[bc]);
            return true;
        }
        switch (special)
        {
        case 0x1C:
            stallVf(fs, dest);
            arith("Mul", true, "vu::sourceScalar(s.q)", 0);
            text = std::format("MULAq.{} ACC, vf{}, Q", fieldText(dest), fs);
            return true;
        case 0x1D:
            stallVf(fs, dest);
            compute = std::format("vu::absolute(s, {}, {}, {})", ft, fs, d);
            text = std::format("ABS.{} vf{}, vf{}", fieldText(dest), ft, fs);
            return true;
        case 0x1E:
            stallVf(fs, dest);
            arith("Mul", true, "vu::sourceScalar(s.i)", 0);
            text = std::format("MULAi.{} ACC, vf{}, I", fieldText(dest), fs);
            return true;
        case 0x1F:
            stallVf(fs, 0xE);
            stallVf(ft, 0x1);
            compute = std::format("vu::clipJudge(s, {}, {})", fs, ft);
            text = std::format("CLIP vf{}xyz, vf{}w", fs, ft);
            return true;
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x27:
        {
            static const char* kOps[] = {"Add", "Madd", "Add", "Madd", "Sub", "Msub", "Sub", "Msub"};
            static const char* kNames[] = {"ADDAq", "MADDAq", "ADDAi", "MADDAi", "SUBAq", "MSUBAq", "SUBAi", "MSUBAi"};
            const int index = special - 0x20;
            const bool useI = (index & 2) != 0;
            stallVf(fs, dest);
            if (index & 1)
                stallAcc(dest);
            arith(kOps[index], true, useI ? "vu::sourceScalar(s.i)" : "vu::sourceScalar(s.q)", 0);
            text = std::format("{}.{} ACC, vf{}, {}", kNames[index], fieldText(dest), fs, useI ? "I" : "Q");
            return true;
        }
        case 0x28:
        case 0x29:
        case 0x2A:
        case 0x2C:
        case 0x2D:
        {
            const char* opName = special == 0x28   ? "Add"
                                 : special == 0x29 ? "Madd"
                                 : special == 0x2A ? "Mul"
                                 : special == 0x2C ? "Sub"
                                                   : "Msub";
            const char* name = special == 0x28   ? "ADDA"
                               : special == 0x29 ? "MADDA"
                               : special == 0x2A ? "MULA"
                               : special == 0x2C ? "SUBA"
                                                 : "MSUBA";
            stallVf(fs, dest);
            stallVf(ft, dest);
            if (special == 0x29 || special == 0x2D)
                stallAcc(dest);
            arith(opName, true, std::format("vu::sourceVf(s, {})", ft), 0);
            text = std::format("{}.{} ACC, vf{}, vf{}", name, fieldText(dest), fs, ft);
            return true;
        }
        case 0x2E:
            stallVf(fs, 0xE);
            stallVf(ft, 0xE);
            compute = std::format("vu::outerProduct(s, false, 0, {}, {}, {})", fs, ft, d);
            text = std::format("OPMULA.{} ACC, vf{}, vf{}", fieldText(dest), fs, ft);
            return true;
        case 0x2F:
        case 0x30:
        case 0x2B: // reserved slot that some assemblers emit as NOP
            compute.clear();
            text = "NOP";
            return special != 0x2B;
        default:
            return false;
        }
    }

    // ---- Lower ----------------------------------------------------------------

    enum class Flow
    {
        None,
        Branch,     // static target, maybe conditional
        Jump,       // JR/JALR: target in a VI register
    };

    struct Lower
    {
        std::string preStalls;  // before the upper instruction computes (waits)
        std::string stalls;     // read stalls
        std::string code;       // the instruction
        std::string text;
        Flow flow = Flow::None;
        std::string condition;  // for branches; empty when unconditional
        std::uint32_t target = 0;
        std::string jumpExpression;
        bool reserved = false;
    };

    Lower lower(std::uint32_t pc, std::uint32_t word)
    {
        Lower l;
        if (word == 0x8000033C || word == 0)
        {
            l.text = "NOP";
            return l;
        }
        const int opHi = static_cast<int>((word >> 25) & 0x7F);
        const int fields = fieldsOf(word);
        const int it = ftOf(word); // also ft
        const int is = fsOf(word); // also fs
        const int id = fdOf(word);
        const int imm = imm11(word);
        auto stallVf = [&](int reg, int f) {
            if (reg != 0 && f != 0)
                l.stalls += std::format("vu::stallVf(s, {}, {}); ", reg, f);
        };
        auto stallVi = [&](int reg) {
            if (reg != 0)
                l.stalls += std::format("vu::stallVi(s, {}); ", reg);
        };
        const std::uint32_t branchTarget = (pc + 8 + static_cast<std::uint32_t>(imm * 8)) & codeMask;
        auto branchVi = [&](int reg) { return std::format("vu::branchVi(s, prevReg, prevValue, {})", reg); };

        switch (opHi)
        {
        case 0x00: // LQ
            stallVi(is);
            l.code = std::format("vu::loadQuadword(s, {}, {}, static_cast<std::uint16_t>(s.vi[{}] + {}));", it, fields,
                                 is, imm);
            l.text = std::format("LQ.{} vf{}, {}(vi{})", fieldText(fields), it, imm, is);
            return l;
        case 0x01: // SQ
            stallVi(it);
            stallVf(is, fields);
            l.code = std::format("vu::storeQuadword(s, {}, {}, static_cast<std::uint16_t>(s.vi[{}] + {}));", is, fields,
                                 it, imm);
            l.text = std::format("SQ.{} vf{}, {}(vi{})", fieldText(fields), is, imm, it);
            return l;
        case 0x04: // ILW
            stallVi(is);
            l.code = std::format("vu::writeVi(s, {}, vu::loadInteger(s, {}, static_cast<std::uint16_t>(s.vi[{}] + {})), 4, "
                                 "false);",
                                 it, fields, is, imm);
            l.text = std::format("ILW.{} vi{}, {}(vi{})", fieldText(fields), it, imm, is);
            return l;
        case 0x05: // ISW
            stallVi(is);
            stallVi(it);
            l.code = std::format("vu::storeInteger(s, {}, static_cast<std::uint16_t>(s.vi[{}] + {}), s.vi[{}]);", fields,
                                 is, imm, it);
            l.text = std::format("ISW.{} vi{}, {}(vi{})", fieldText(fields), it, imm, is);
            return l;
        case 0x08: // IADDIU
        case 0x09: // ISUBIU
        {
            const std::uint32_t imm15 = (word & 0x7FF) | ((word >> 10) & 0x7800);
            stallVi(is);
            l.code = std::format("vu::writeVi(s, {}, s.vi[{}] {} {}u, 1, true);", it, is, opHi == 0x08 ? '+' : '-', imm15);
            l.text = std::format("{} vi{}, vi{}, {}", opHi == 0x08 ? "IADDIU" : "ISUBIU", it, is, imm15);
            return l;
        }
        case 0x10: // FCEQ
            l.code = std::format("vu::writeVi(s, 1, (s.clip & 0xFFFFFF) == 0x{:06X}u ? 1 : 0, 1, false);", word & 0xFFFFFF);
            l.text = std::format("FCEQ vi1, 0x{:06X}", word & 0xFFFFFF);
            return l;
        case 0x11: // FCSET
            l.code = std::format("vu::fcset(s, 0x{:06X}u);", word & 0xFFFFFF);
            l.text = std::format("FCSET 0x{:06X}", word & 0xFFFFFF);
            return l;
        case 0x12: // FCAND
            l.code = std::format("vu::writeVi(s, 1, (s.clip & 0x{:06X}u) != 0 ? 1 : 0, 1, false);", word & 0xFFFFFF);
            l.text = std::format("FCAND vi1, 0x{:06X}", word & 0xFFFFFF);
            return l;
        case 0x13: // FCOR
            l.code = std::format("vu::writeVi(s, 1, ((s.clip | 0x{:06X}u) & 0xFFFFFF) == 0xFFFFFF ? 1 : 0, 1, false);",
                                 word & 0xFFFFFF);
            l.text = std::format("FCOR vi1, 0x{:06X}", word & 0xFFFFFF);
            return l;
        case 0x14: // FSEQ
        case 0x15: // FSSET
        case 0x16: // FSAND
        case 0x17: // FSOR
        {
            const std::uint32_t imm12 = (((word >> 21) & 1) << 11) | (word & 0x7FF);
            switch (opHi)
            {
            case 0x14:
                l.code = std::format("vu::writeVi(s, {}, (s.status & 0xFFF) == 0x{:03X}u ? 1 : 0, 1, false);", it, imm12);
                l.text = std::format("FSEQ vi{}, 0x{:03X}", it, imm12);
                break;
            case 0x15:
                l.code = std::format("vu::fsset(s, 0x{:03X}u);", imm12);
                l.text = std::format("FSSET 0x{:03X}", imm12);
                break;
            case 0x16:
                l.code = std::format("vu::writeVi(s, {}, s.status & 0x{:03X}u, 1, false);", it, imm12);
                l.text = std::format("FSAND vi{}, 0x{:03X}", it, imm12);
                break;
            default:
                l.code = std::format("vu::writeVi(s, {}, (s.status & 0xFFF) | 0x{:03X}u, 1, false);", it, imm12);
                l.text = std::format("FSOR vi{}, 0x{:03X}", it, imm12);
                break;
            }
            return l;
        }
        case 0x18: // FMEQ
        case 0x1A: // FMAND
        case 0x1B: // FMOR
            stallVi(is);
            if (opHi == 0x18)
                l.code = std::format("vu::writeVi(s, {}, (s.mac & 0xFFFF) == s.vi[{}] ? 1 : 0, 1, false);", it, is);
            else
                l.code = std::format("vu::writeVi(s, {}, s.mac {} s.vi[{}], 1, false);", it, opHi == 0x1A ? '&' : '|', is);
            l.text = std::format("{} vi{}, vi{}", opHi == 0x18 ? "FMEQ" : opHi == 0x1A ? "FMAND" : "FMOR", it, is);
            return l;
        case 0x1C: // FCGET
            l.code = std::format("vu::writeVi(s, {}, s.clip & 0xFFF, 1, false);", it);
            l.text = std::format("FCGET vi{}", it);
            return l;
        case 0x20: // B
        case 0x21: // BAL
            l.flow = Flow::Branch;
            l.target = branchTarget;
            if (opHi == 0x21)
                l.code = std::format("vu::writeVi(s, {}, 0x{:X}u, 1, false);", it, (pc + 16) / 8);
            l.text = std::format("{} 0x{:04X}", opHi == 0x20 ? "B" : std::format("BAL vi{},", it), branchTarget);
            return l;
        case 0x24: // JR
        case 0x25: // JALR
            stallVi(is);
            l.flow = Flow::Jump;
            l.jumpExpression = std::format("((static_cast<std::uint32_t>({}) * 8u) & 0x{:X}u)", branchVi(is), codeMask);
            if (opHi == 0x25)
                l.code = std::format("vu::writeVi(s, {}, 0x{:X}u, 1, false);", it, (pc + 16) / 8);
            l.text = opHi == 0x24 ? std::format("JR vi{}", is) : std::format("JALR vi{}, vi{}", it, is);
            return l;
        case 0x28: // IBEQ
        case 0x29: // IBNE
            stallVi(is);
            stallVi(it);
            l.flow = Flow::Branch;
            l.target = branchTarget;
            l.condition = std::format("{} {} {}", branchVi(is), opHi == 0x28 ? "==" : "!=", branchVi(it));
            l.text = std::format("{} vi{}, vi{}, 0x{:04X}", opHi == 0x28 ? "IBEQ" : "IBNE", it, is, branchTarget);
            return l;
        case 0x2C: // IBLTZ
        case 0x2D: // IBGTZ
        case 0x2E: // IBLEZ
        case 0x2F: // IBGEZ
        {
            static const char* kCompare[] = {"<", ">", "<=", ">="};
            static const char* kNames[] = {"IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"};
            stallVi(is);
            l.flow = Flow::Branch;
            l.target = branchTarget;
            l.condition = std::format("static_cast<std::int16_t>({}) {} 0", branchVi(is), kCompare[opHi - 0x2C]);
            l.text = std::format("{} vi{}, 0x{:04X}", kNames[opHi - 0x2C], is, branchTarget);
            return l;
        }
        case 0x40:
            break;
        default:
            l.reserved = true;
            return l;
        }

        // Lower 1.
        const int funct = static_cast<int>(word & 0x3F);
        switch (funct)
        {
        case 0x30: // IADD
        case 0x31: // ISUB
        case 0x34: // IAND
        case 0x35: // IOR
        {
            static const char* kOps[] = {"+", "-", nullptr, nullptr, "&", "|"};
            static const char* kNames[] = {"IADD", "ISUB", nullptr, nullptr, "IAND", "IOR"};
            stallVi(is);
            stallVi(it);
            l.code = std::format("vu::writeVi(s, {}, s.vi[{}] {} s.vi[{}], 1, true);", id, is, kOps[funct - 0x30], it);
            l.text = std::format("{} vi{}, vi{}, vi{}", kNames[funct - 0x30], id, is, it);
            return l;
        }
        case 0x32: // IADDI
        {
            const int imm5 = static_cast<int>(static_cast<std::int32_t>(((word >> 6) & 0x1F) << 27) >> 27);
            stallVi(is);
            l.code = std::format("vu::writeVi(s, {}, s.vi[{}] + ({}), 1, true);", it, is, imm5);
            l.text = std::format("IADDI vi{}, vi{}, {}", it, is, imm5);
            return l;
        }
        default:
            break;
        }
        if (funct < 0x3C)
        {
            l.reserved = true;
            return l;
        }

        const int special = static_cast<int>((word & 0x3) | ((word >> 4) & 0x7C));
        const int fsf = static_cast<int>((word >> 21) & 0x3);
        const int ftf = static_cast<int>((word >> 23) & 0x3);
        switch (special)
        {
        case 0x30: // MOVE
            stallVf(is, fields);
            l.code = std::format("vu::writeVfFields(s, {}, {}, s.vf[{}], 4);", it, fields, is);
            l.text = std::format("MOVE.{} vf{}, vf{}", fieldText(fields), it, is);
            return l;
        case 0x31: // MR32
            stallVf(is, 0xF);
            l.code = std::format("{{ const std::uint32_t v[4] = {{s.vf[{0}][1], s.vf[{0}][2], s.vf[{0}][3], s.vf[{0}][0]}}; "
                                 "vu::writeVfFields(s, {1}, {2}, v, 4); }}",
                                 is, it, fields);
            l.text = std::format("MR32.{} vf{}, vf{}", fieldText(fields), it, is);
            return l;
        case 0x34: // LQI
            stallVi(is);
            l.code = std::format("{{ const std::uint16_t a = s.vi[{0}]; vu::loadQuadword(s, {1}, {2}, a); "
                                 "vu::writeVi(s, {0}, a + 1, 1, true); }}",
                                 is, it, fields);
            l.text = std::format("LQI.{} vf{}, (vi{}++)", fieldText(fields), it, is);
            return l;
        case 0x35: // SQI
            stallVi(it);
            stallVf(is, fields);
            l.code = std::format("{{ const std::uint16_t a = s.vi[{0}]; vu::storeQuadword(s, {1}, {2}, a); "
                                 "vu::writeVi(s, {0}, a + 1, 1, true); }}",
                                 it, is, fields);
            l.text = std::format("SQI.{} vf{}, (vi{}++)", fieldText(fields), is, it);
            return l;
        case 0x36: // LQD
            stallVi(is);
            l.code = std::format("{{ const std::uint16_t a = static_cast<std::uint16_t>(s.vi[{0}] - 1); "
                                 "vu::writeVi(s, {0}, a, 1, true); vu::loadQuadword(s, {1}, {2}, a); }}",
                                 is, it, fields);
            l.text = std::format("LQD.{} vf{}, (--vi{})", fieldText(fields), it, is);
            return l;
        case 0x37: // SQD
            stallVi(it);
            stallVf(is, fields);
            l.code = std::format("{{ const std::uint16_t a = static_cast<std::uint16_t>(s.vi[{0}] - 1); "
                                 "vu::writeVi(s, {0}, a, 1, true); vu::storeQuadword(s, {1}, {2}, a); }}",
                                 it, is, fields);
            l.text = std::format("SQD.{} vf{}, (--vi{})", fieldText(fields), is, it);
            return l;
        case 0x38: // DIV
        case 0x39: // SQRT
        case 0x3A: // RSQRT
        {
            const int kind = special - 0x38;
            if (kind != 1)
                stallVf(is, fieldBit(fsf));
            stallVf(it, fieldBit(ftf));
            l.preStalls = "vu::waitQ(s); ";
            l.code = std::format("vu::divide(s, {}, s.vf[{}][{}], {});", kind == 1 ? std::string("0") : std::format("s.vf[{}][{}]", is, fsf),
                                 it, ftf, kind);
            l.text = kind == 1 ? std::format("SQRT Q, vf{}{}", it, kField[ftf])
                               : std::format("{} Q, vf{}{}, vf{}{}", kind == 0 ? "DIV" : "RSQRT", is, kField[fsf], it,
                                             kField[ftf]);
            return l;
        }
        case 0x3B: // WAITQ
            l.preStalls = "vu::waitQ(s); ";
            l.text = "WAITQ";
            return l;
        case 0x3C: // MTIR
            stallVf(is, fieldBit(fsf));
            l.code = std::format("vu::writeVi(s, {}, s.vf[{}][{}], 1, true);", it, is, fsf);
            l.text = std::format("MTIR vi{}, vf{}{}", it, is, kField[fsf]);
            return l;
        case 0x3D: // MFIR
            stallVi(is);
            l.code = std::format("{{ const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int16_t>(s.vi[{0}])); "
                                 "const std::uint32_t w[4] = {{v, v, v, v}}; vu::writeVfFields(s, {1}, {2}, w, 4); }}",
                                 is, it, fields);
            l.text = std::format("MFIR.{} vf{}, vi{}", fieldText(fields), it, is);
            return l;
        case 0x3E: // ILWR
            stallVi(is);
            l.code = std::format("vu::writeVi(s, {}, vu::loadInteger(s, {}, s.vi[{}]), 4, false);", it, fields, is);
            l.text = std::format("ILWR.{} vi{}, (vi{})", fieldText(fields), it, is);
            return l;
        case 0x3F: // ISWR
            stallVi(is);
            stallVi(it);
            l.code = std::format("vu::storeInteger(s, {}, s.vi[{}], s.vi[{}]);", fields, is, it);
            l.text = std::format("ISWR.{} vi{}, (vi{})", fieldText(fields), it, is);
            return l;
        case 0x40: // RNEXT
            l.code = std::format("{{ const std::uint32_t x = (s.r >> 4) & 1u, y = (s.r >> 22) & 1u; "
                                 "s.r = 0x3F800000u | (((s.r << 1) ^ x ^ y) & 0x007FFFFFu); "
                                 "const std::uint32_t w[4] = {{s.r, s.r, s.r, s.r}}; vu::writeVfFields(s, {}, {}, w, 4); }}",
                                 it, fields);
            l.text = std::format("RNEXT.{} vf{}, R", fieldText(fields), it);
            return l;
        case 0x41: // RGET
            l.code = std::format("{{ const std::uint32_t w[4] = {{s.r, s.r, s.r, s.r}}; vu::writeVfFields(s, {}, {}, w, 4); }}",
                                 it, fields);
            l.text = std::format("RGET.{} vf{}, R", fieldText(fields), it);
            return l;
        case 0x42: // RINIT
            stallVf(is, fieldBit(fsf));
            l.code = std::format("s.r = 0x3F800000u | (s.vf[{}][{}] & 0x007FFFFFu);", is, fsf);
            l.text = std::format("RINIT R, vf{}{}", is, kField[fsf]);
            return l;
        case 0x43: // RXOR
            stallVf(is, fieldBit(fsf));
            l.code = std::format("s.r = 0x3F800000u | ((s.r ^ s.vf[{}][{}]) & 0x007FFFFFu);", is, fsf);
            l.text = std::format("RXOR R, vf{}{}", is, kField[fsf]);
            return l;
        case 0x64: // MFP
            l.code = std::format("{{ const std::uint32_t w[4] = {{s.p, s.p, s.p, s.p}}; vu::writeVfFields(s, {}, {}, w, 4); }}",
                                 it, fields);
            l.text = std::format("MFP.{} vf{}, P", fieldText(fields), it);
            return l;
        case 0x68: // XTOP
            l.code = std::format("vu::writeVi(s, {}, s.top & 0x3FF, 1, false);", it);
            l.text = std::format("XTOP vi{}", it);
            return l;
        case 0x69: // XITOP
            l.code = std::format("vu::writeVi(s, {}, s.itop & 0x3FF, 1, false);", it);
            l.text = std::format("XITOP vi{}", it);
            return l;
        case 0x6C: // XGKICK
            stallVi(is);
            l.code = std::format("vu::xgkick(s, s.vi[{}]);", is);
            l.text = std::format("XGKICK vi{}", is);
            if (unit == 0)
                l.reserved = true;
            return l;
        case 0x7B: // WAITP
            l.preStalls = "vu::waitP(s); ";
            l.text = "WAITP";
            return l;
        default:
            break;
        }
        if (special >= 0x70 && special <= 0x7D && special != 0x7B)
        {
            if (unit == 0)
            {
                l.reserved = true; // VU0 has no EFU
                return l;
            }
            l.preStalls = "vu::waitP(s); ";
            const std::string x = std::format("vu::toDouble(s.vf[{}][0])", is);
            const std::string y = std::format("vu::toDouble(s.vf[{}][1])", is);
            const std::string z = std::format("vu::toDouble(s.vf[{}][2])", is);
            const std::string f = std::format("vu::toDouble(s.vf[{}][{}])", is, fsf);
            const std::string sq = std::format("({0} * {0} + {1} * {1} + {2} * {2})", x, y, z);
            std::string value;
            int latency = 0;
            const char* name = "";
            switch (special)
            {
            case 0x70: value = sq; latency = 11; name = "ESADD"; break;
            case 0x71: value = std::format("vu::efuReciprocal({})", sq); latency = 18; name = "ERSADD"; break;
            case 0x72: value = std::format("std::sqrt({})", sq); latency = 18; name = "ELENG"; break;
            case 0x73: value = std::format("vu::efuReciprocal(std::sqrt({}))", sq); latency = 24; name = "ERLENG"; break;
            case 0x74: value = std::format("vu::efuAtanRatio({}, {})", y, x); latency = 54; name = "EATANxy"; break;
            case 0x75: value = std::format("vu::efuAtanRatio({}, {})", z, x); latency = 54; name = "EATANxz"; break;
            case 0x76: value = std::format("({} + {} + {} + vu::toDouble(s.vf[{}][3]))", x, y, z, is); latency = 12; name = "ESUM"; break;
            case 0x77: value = std::format("vu::efuReciprocal(std::sqrt(std::fabs({})))", f); latency = 18; name = "ERSQRT"; break;
            case 0x78: value = std::format("std::sqrt(std::fabs({}))", f); latency = 12; name = "ESQRT"; break;
            case 0x79: value = std::format("vu::efuSin({})", f); latency = 29; name = "ESIN"; break;
            case 0x7A: value = std::format("vu::efuReciprocal({})", f); latency = 12; name = "ERCPR"; break;
            case 0x7C: value = std::format("vu::efuAtan({})", f); latency = 54; name = "EATAN"; break;
            case 0x7D: value = std::format("vu::efuExp({})", f); latency = 44; name = "EEXP"; break;
            default: l.reserved = true; return l;
            }
            stallVf(is, special <= 0x76 ? 0xF : fieldBit(fsf));
            l.code = std::format("vu::efu(s, {}, {});", value, latency);
            l.text = std::format("{} P, vf{}", name, is);
            return l;
        }
        l.reserved = true;
        return l;
    }

    // ---- Pairs -----------------------------------------------------------------

    // The body of one pair, without the control flow that follows it.
    // decision: for a branch, the statement that sets `taken` and `target`
    // from the registers as the pair reads them.
    void emitPair(std::uint32_t pc, std::uint32_t lowerWord, std::uint32_t upperWord, const Lower& l, bool iBit,
                  const std::string& indent, const std::string& decision = {})
    {
        std::string stalls;
        std::string compute;
        std::string upperText;
        if (!upper(upperWord, stalls, compute, upperText))
        {
            warn(pc, std::format("reserved upper instruction 0x{:08X}", upperWord));
            upperText = std::format("reserved 0x{:08X}", upperWord);
            compute.clear();
        }
        out << indent << "// " << upperText << " | "
            << (iBit ? std::format("LOI 0x{:08X}", lowerWord) : l.text) << "\n";
        out << indent << "prevReg = s.branchBackupReg; prevValue = s.branchBackupValue; s.branchBackupReg = 0;\n";
        if (!iBit)
            stalls += l.stalls;
        if (!stalls.empty())
            out << indent << stalls << "\n";
        if (!iBit && !l.preStalls.empty())
            out << indent << l.preStalls << "\n";
        out << indent << "vu::beginPair(s);\n";
        if (!decision.empty())
            out << indent << decision << "\n";
        if (!compute.empty())
            out << indent << "{ const vu::Upper u = " << compute << ";\n";
        else
            out << indent << "{\n";
        if (!iBit && !l.reserved && !l.code.empty())
            out << indent << "  " << l.code << "\n";
        if (!compute.empty())
            out << indent << "  vu::commit(s, u);\n";
        out << indent << "}\n";
        if (iBit)
            out << indent << std::format("s.i = 0x{:08X}u;\n", lowerWord);
        out << indent << "vu::endPair(s);\n";
    }

    void translate(const ProgramEntry& entry, const std::vector<std::uint8_t>& code, const std::string& name)
    {
        const std::uint32_t pairs = static_cast<std::uint32_t>(code.size() / 8);
        auto word = [&](std::uint32_t index, int half) {
            std::uint32_t value;
            std::memcpy(&value, code.data() + index * 8 + half * 4, 4);
            return value;
        };
        auto address = [&](std::uint32_t index) { return (entry.loadAddress + index * 8) & codeMask; };
        auto label = [](std::uint32_t pc) { return std::format("L_{:04X}", pc); };
        auto inProgram = [&](std::uint32_t pc) {
            const std::uint32_t offset = (pc - entry.loadAddress) & codeMask;
            return offset < pairs * 8;
        };

        out << std::format("// VU{} microprogram from ELF 0x{:08X}, {} instructions, loaded at 0x{:04X}.\n", entry.unit,
                           entry.elfAddress, pairs, entry.loadAddress);
        out << std::format("int {}(vu::State& s, std::uint32_t entry)\n{{\n", name);
        out << "    std::uint32_t prevReg = 0;\n    std::uint16_t prevValue = 0;\n";
        out << "    bool taken = false;\n    std::uint32_t target = 0;\n";
        out << "    (void)prevReg; (void)prevValue; (void)taken; (void)target;\n";
        out << "dispatch:\n    switch (entry)\n    {\n";
        for (std::uint32_t n = 0; n < pairs; ++n)
            out << std::format("    case 0x{:04X}: goto {};\n", address(n), label(address(n)));
        out << "    default: s.pc = entry; return 1;\n    }\n";

        for (std::uint32_t n = 0; n < pairs; ++n)
        {
            const std::uint32_t pc = address(n);
            const std::uint32_t lowerWord = word(n, 0);
            const std::uint32_t upperWord = word(n, 1);
            const bool iBit = (upperWord & 0x80000000u) != 0;
            const bool eBit = (upperWord & 0x40000000u) != 0;
            const Lower l = iBit ? Lower{} : lower(pc, lowerWord);
            if (l.reserved)
                warn(pc, std::format("reserved lower instruction 0x{:08X}", lowerWord));

            const bool hasDelaySlot = eBit || l.flow != Flow::None;
            std::string decision;
            if (l.flow == Flow::Branch)
                decision = "taken = " + (l.condition.empty() ? std::string("true") : l.condition) + ";";
            else if (l.flow == Flow::Jump)
                decision = "taken = true; target = " + l.jumpExpression + ";";

            out << label(pc) << ":\n";
            out << std::format("    if (s.budget == 0) {{ s.pc = 0x{:04X}; return 2; }} --s.budget;\n", pc);
            emitPair(pc, lowerWord, upperWord, l, iBit, "    ", decision);

            if (!hasDelaySlot)
            {
                if (n + 1 == pairs)
                    out << std::format("    s.pc = 0x{:04X}; return 1;\n", (pc + 8) & codeMask);
                continue;
            }

            // The decision was made in the pair; the delay slot runs before it takes effect.
            out << "    {\n";

            if (n + 1 < pairs)
            {
                const std::uint32_t slotPc = address(n + 1);
                const std::uint32_t slotLower = word(n + 1, 0);
                const std::uint32_t slotUpper = word(n + 1, 1);
                const bool slotI = (slotUpper & 0x80000000u) != 0;
                const Lower slot = slotI ? Lower{} : lower(slotPc, slotLower);
                if (slot.flow != Flow::None)
                    warn(slotPc, "branch in a delay slot is ignored");
                if (slotUpper & 0x40000000u)
                    warn(slotPc, "E bit in a delay slot is ignored");
                emitPair(slotPc, slotLower, slotUpper, slot, slotI, "      ");
            }
            else
            {
                warn(pc, "the delay slot is outside the program");
            }

            const std::uint32_t after = (pc + 16) & codeMask;
            if (eBit)
            {
                if (l.flow != Flow::None)
                    warn(pc, "branch together with the E bit is ignored");
                out << std::format("      vu::finish(s, 0x{:04X}); return 0;\n", after);
            }
            else if (l.flow == Flow::Branch)
            {
                if (inProgram(l.target))
                    out << std::format("      if (taken) goto {};\n", label(l.target));
                else
                    out << std::format("      if (taken) {{ s.pc = 0x{:04X}; return 1; }}\n", l.target);
            }
            else
            {
                out << "      entry = target; goto dispatch;\n";
            }
            if (!eBit && l.flow != Flow::Jump)
            {
                if (n + 2 < pairs)
                    out << "      goto " << label(after) << ";\n";
                else
                    out << std::format("      s.pc = 0x{:04X}; return 1;\n", after);
            }
            out << "    }\n";
        }
        out << "}\n";
    }
};

std::vector<ProgramEntry> readPrograms(const fs::path& path, std::string& error)
{
    std::vector<ProgramEntry> programs;
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot open " + path.string();
        return programs;
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line))
    {
        ++lineNumber;
        if (const auto hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);
        if (line.find_first_not_of(" \t\r") == std::string::npos || line.rfind("unit", 0) == 0)
            continue;
        ProgramEntry entry;
        char* cursor = line.data();
        entry.unit = static_cast<int>(std::strtoul(cursor, &cursor, 0));
        std::uint32_t* fields[3] = {&entry.elfAddress, &entry.size, &entry.loadAddress};
        bool ok = true;
        for (std::uint32_t* field : fields)
        {
            if (*cursor != ',')
            {
                ok = false;
                break;
            }
            *field = static_cast<std::uint32_t>(std::strtoul(cursor + 1, &cursor, 0));
        }
        if (!ok || (entry.unit != 0 && entry.unit != 1) || entry.size == 0 || entry.size % 8 != 0)
        {
            error = std::format("{}:{}: expected unit,elf_address,size,load_address", path.string(), lineNumber);
            return {};
        }
        programs.push_back(entry);
    }
    return programs;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fputs("usage: tgr-vurecomp <SCUS_971.33> <programs.csv> <out-dir>\n", stderr);
        return 2;
    }
    std::ifstream elfFile(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> elf((std::istreambuf_iterator<char>(elfFile)), std::istreambuf_iterator<char>());
    const std::vector<Segment> segments = loadSegments(elf);
    if (segments.empty())
    {
        std::fprintf(stderr, "%s is not an ELF with loadable segments\n", argv[1]);
        return 1;
    }
    std::string error;
    const std::vector<ProgramEntry> programs = readPrograms(argv[2], error);
    if (!error.empty())
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    const fs::path outDir = argv[3];
    std::error_code ec;
    fs::create_directories(outDir, ec);

    std::ostringstream table;
    std::ostringstream declarations;
    int warnings = 0;
    for (const ProgramEntry& entry : programs)
    {
        const auto code = readProgram(elf, segments, entry.elfAddress, entry.size);
        if (!code)
        {
            std::fprintf(stderr, "VU%d program at 0x%08X (%u bytes) is not inside the ELF\n", entry.unit,
                         entry.elfAddress, entry.size);
            return 1;
        }
        Translator translator;
        translator.unit = entry.unit;
        translator.codeMask = entry.unit == 0 ? 0xFFF : 0x3FFF;
        const std::string name = std::format("vu{}_{:08X}", entry.unit, entry.elfAddress);
        translator.out << "// Generated by tgr-vurecomp from the user's own SCUS_971.33. Do not commit.\n"
                       << "#include \"hw/vu/VuCore.h\"\n#include \"hw/vu/VuEfu.h\"\n\n"
                       << "namespace tgr::hw\n{\n\n";
        translator.translate(entry, *code, name);
        translator.out << "\n} // namespace tgr::hw\n";
        for (const std::string& warning : translator.warnings)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), warning.c_str());
            ++warnings;
        }

        std::ofstream file(outDir / (name + ".cpp"), std::ios::binary);
        file << translator.out.str();

        declarations << std::format("int {}(vu::State& s, std::uint32_t entry);\n", name);
        table << std::format("    {{{}, 0x{:04X}, 0x{:X}, 0x{:016X}ull, 0x{:08X}, &{}}},\n", entry.unit, entry.loadAddress,
                             entry.size, programHash(code->data(), code->size()), entry.elfAddress, name);
    }

    std::ofstream registry(outDir / "vu_programs.cpp", std::ios::binary);
    registry << "// Generated by tgr-vurecomp from the user's own SCUS_971.33. Do not commit.\n"
             << "#include \"hw/vu/VuPrograms.h\"\n\nnamespace tgr::hw\n{\n\n"
             << declarations.str() << "\nnamespace vu\n{\n\n"
             << "const ProgramInfo kPrograms[] = {\n"
             << table.str() << "    {0, 0, 0, 0, 0, nullptr},\n};\n"
             << std::format("const std::size_t kProgramCount = {};\n", programs.size())
             << "\n} // namespace vu\n} // namespace tgr::hw\n";

    std::printf("tgr-vurecomp: %zu programs, %d warnings\n", programs.size(), warnings);
    return 0;
}
