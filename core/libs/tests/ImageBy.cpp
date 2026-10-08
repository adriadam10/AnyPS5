#include "Translation/TranslationContext.hpp"
#include "RdnaDecoder/RdnaImageOpDecoder.hpp"
#include <array>
#include <stdexcept>
#include <source_location>
#include <string>
#include <vector>

using namespace ShaderRecompiler;

namespace {

void Require(bool value, std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("image BY2/BY4 regression at line " + std::to_string(location.line()));
}

constexpr std::array<std::uint32_t, 8> Encodings{0x42u, 0x43u, 0x4au, 0x4bu, 0x52u, 0x53u, 0x5au, 0x5bu};
constexpr std::array<RdnaOpcode, 8> Opcodes{RdnaOpcode::ImageLoadBy2, RdnaOpcode::ImageLoadBy4, RdnaOpcode::ImageLoadMipBy2, RdnaOpcode::ImageLoadMipBy4, RdnaOpcode::ImageStoreBy2, RdnaOpcode::ImageStoreBy4, RdnaOpcode::ImageStoreMipBy2, RdnaOpcode::ImageStoreMipBy4};

RdnaInstruction Decode(std::uint32_t encoding, std::uint32_t controls = 0u, std::uint32_t word1 = 0x00101e1eu, std::uint32_t mask = 15u, std::uint32_t dimension = 1u) {
    const std::array<std::uint32_t, 3> code{0xf0000000u | (dimension << 3u) | (encoding << 18u) | (mask << 8u) | controls, word1, 0x0000201fu};
    return DecodeRdnaMimg(0u, code, 0u);
}

void Check(std::uint32_t index, bool nsa) {
    const auto encoding = Encodings[index];
    const auto inst = Decode(encoding, nsa ? 2u : 0u);
    const auto elements = (encoding & 1u) != 0u ? 4u : 2u;
    const auto channels = 4u / elements;
    const bool store = (encoding & 0x10u) != 0u;
    Require(inst.op == Opcodes[index] && IsImageOpcode(inst.op));
    Require(inst.dataDwordCount == 4u && inst.dataComponents == 4u);
    Require(inst.imageAddressComponents == ((encoding & 8u) != 0u ? 3u : 2u));
    Require(inst.wordCount == (nsa ? 3u : 2u));
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    TranslationContext context(program, block, 256);
    context.TranslateInstruction(inst);
    std::vector<IrValue*> operations;
    bool wroteRegister = false;
    std::uint32_t writes = 0;
    for (auto* value : block.Instructions()) {
        if (value->Opcode() == IrOpcode::SetVectorRegister) {
            wroteRegister = true;
            ++writes;
        }
        if (value->Opcode() == (store ? IrOpcode::ImageWrite : IrOpcode::ImageRead)) {
            Require(!wroteRegister);
            operations.push_back(value);
        }
    }
    Require(operations.size() == elements && writes == (store ? 0u : 4u));
    IrValue* first = operations[0]->Argument(1)->Resolve();
    for (std::uint32_t texel = 0; texel < elements; ++texel) {
        const auto& memory = program.Resources().memoryInfo[operations[texel]->Flags<MemoryFlags>().index];
        Require(memory.imageByElements == elements && memory.dmask == (1u << channels) - 1u);
        Require(memory.dataDwords == channels && memory.componentCount == channels);
        Require(memory.imageHasMip == ((encoding & 8u) != 0u));
        IrValue* address = operations[texel]->Argument(1)->Resolve();
        Require(address->Opcode() == IrOpcode::MakeImageAddress);
        for (std::uint32_t component = 1; component < 13u; ++component) Require(address->Argument(component) == first->Argument(component));
        if (texel != 0u) {
            auto* x = address->Argument(0)->Resolve();
            Require(x->Opcode() == IrOpcode::IAdd32 && x->Argument(0) == first->Argument(0));
            Require(x->Argument(1)->ImmediateU32() == texel);
        }
    }
}

void CheckOrdinary(std::uint32_t encoding) {
    const auto inst = Decode(encoding);
    const bool store = (encoding & 8u) != 0u;
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    TranslationContext context(program, block, 256);
    context.TranslateInstruction(inst);
    std::uint32_t operations = 0;
    for (auto* value : block.Instructions()) {
        if (value->Opcode() == (store ? IrOpcode::ImageWrite : IrOpcode::ImageRead)) {
            const auto& memory = program.Resources().memoryInfo[value->Flags<MemoryFlags>().index];
            Require(memory.imageByElements == 0u && memory.dmask == 15u && memory.dataDwords == 4u);
            ++operations;
        }
    }
    Require(operations == 1u);
}

void Refused(std::uint32_t encoding, std::uint32_t control, std::uint32_t word1 = 0x00101e1eu, std::uint32_t mask = 15u, std::uint32_t dimension = 1u) {
    bool refused = false;
    try { (void)Decode(encoding, control, word1, mask, dimension); }
    catch (const std::runtime_error&) { refused = true; }
    Require(refused);
}

}

int main() {
    for (auto encoding : {0u, 1u, 8u, 9u}) CheckOrdinary(encoding);
    for (std::uint32_t index = 0; index < Encodings.size(); ++index) {
        Check(index, false);
        Check(index, true);
        for (std::uint32_t mask = 0u; mask < 15u; ++mask) Refused(Encodings[index], 0u, 0x00101e1eu, mask);
        for (auto bits : {0x8000u, 0x10000u, 0x20000u}) Refused(Encodings[index], bits);
        for (auto bits : {0x40000000u, 0x80000000u}) Refused(Encodings[index], 0u, 0x00101e1eu | bits);
        Refused(Encodings[index], 0u, 0x0010fd1eu);
        Refused(Encodings[index], 0u, 0x00101effu);
        for (std::uint32_t dimension = 0; dimension < 8u; ++dimension) {
            if (dimension != 1u) Refused(Encodings[index], 0u, 0x00101e1eu, 15u, dimension);
        }
    }
}
