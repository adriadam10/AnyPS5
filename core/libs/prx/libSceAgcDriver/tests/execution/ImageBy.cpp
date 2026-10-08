#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Width = 256;
constexpr std::size_t Bytes = 16384;
alignas(65536) std::array<std::uint8_t, Bytes> Texels{};
alignas(256) std::array<std::uint32_t, Threads * 4> Output{};
constexpr std::array<std::uint32_t, 8> Encodings{0x42u, 0x43u, 0x4au, 0x4bu, 0x52u, 0x53u, 0x5au, 0x5bu};
constexpr std::array<std::uint32_t, 4> Values{101u, 202u, 303u, 404u};

std::vector<std::uint32_t> Code(std::uint32_t opcode) {
    const bool store = (opcode & 0x10u) != 0u;
    std::vector<std::uint32_t> code{0x34060084u, 0x343c0081u + (opcode & 1u), 0x7e3e0280u, 0x7e400281u};
    if (store) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            code.push_back(0x7e0002ffu | ((10u + index) << 17u));
            code.push_back(Values[index]);
        }
    }
    code.push_back(0xf0000f08u | (opcode << 18u));
    code.push_back(0x00010a1eu);
    code.push_back(0xbf8c3f70u);
    if (!store) {
        code.push_back(0xe0781000u);
        code.push_back(0x80000a03u);
    }
    code.push_back(0xbf810000u);
    return code;
}

std::array<std::uint32_t, 8> Descriptor(std::uint32_t format, std::uint32_t swizzle = 0xfacu) {
    const auto address = reinterpret_cast<std::uintptr_t>(Texels.data());
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        (Width - 1u) >> 2u, swizzle | (1u << 16u) | (9u << 28u), 0u, 1u << 4u, 0u, 0u};
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t opcode, std::uint32_t format, std::uint32_t swizzle = 0xfacu) {
    const auto code = Code(opcode);
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    std::vector<std::uint32_t> userData(16, 0u);
    userData[0] = static_cast<std::uint32_t>(output);
    userData[1] = static_cast<std::uint32_t>(output >> 32u);
    userData[2] = static_cast<std::uint32_t>(Output.size() * sizeof(std::uint32_t));
    userData[3] = 0x01016facu;
    const auto descriptor = Descriptor(format, swizzle);
    std::copy(descriptor.begin(), descriptor.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), words, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    if ((opcode & 0x10u) != 0u) {
        AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(Texels.data()), Bytes, nullptr, "image BY test");
        device.WaitIdle();
    }
}

void Check(AgcDriver::VulkanDevice& device, std::uint32_t opcode) {
    const auto elements = (opcode & 1u) != 0u ? 4u : 2u;
    const auto channels = 4u / elements;
    const auto format = elements == 2u ? 62u : 20u;
    const auto mips = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, format, Width, 1u, 2u);
    Require(AgcDriver::Graphics::ComputeSurfaceSize(mips, 1u) <= Texels.size(), "image BY texture size");
    Texels.fill(0xeeu);
    for (std::uint32_t level = 0; level < mips.size(); ++level) {
        for (std::uint32_t x = 0; x < mips[level].width; ++x) {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const auto value = level * 100000u + x * 100u + channel + 7u;
                std::memcpy(Texels.data() + mips[level].tiledOffset + (x * channels + channel) * 4u, &value, 4u);
            }
        }
    }
    const auto initial = Texels;
    Output.fill(0xdeadbeefu);
    Run(device, opcode, format);
    const auto level = (opcode & 8u) != 0u ? 1u : 0u;
    if ((opcode & 0x10u) != 0u) {
        auto expected = initial;
        for (std::uint32_t thread = 0; thread < Threads; ++thread) {
            std::memcpy(expected.data() + mips[level].tiledOffset + thread * 16u, Values.data(), 16u);
        }
        Require(Texels == expected, "image BY store changes adjacent texels, preserving the other mip and untouched bytes");
    } else {
        for (std::uint32_t thread = 0; thread < Threads; ++thread) {
            for (std::uint32_t index = 0; index < 4u; ++index) {
                const auto expected = level * 100000u + (thread * elements + index / channels) * 100u + index % channels + 7u;
                Require(Output[thread * 4u + index] == expected, "image BY load texel/channel order");
            }
        }
        Require(Texels == initial, "image BY load leaves the source unchanged");
    }
}

void Refused(AgcDriver::VulkanDevice& device, std::uint32_t opcode, std::uint32_t format, std::uint32_t swizzle = 0xfacu) {
    bool refused = false;
    try { Run(device, opcode, format, swizzle); }
    catch (const std::exception& error) { refused = std::string(error.what()).find("RG32_UINT/R32_UINT") != std::string::npos; }
    Require(refused, "image BY unsupported format/swizzle rejection");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestAllocations::Mutation().Add(Texels.data(), Bytes, true, true);
        for (auto opcode : Encodings) {
            Check(*device, opcode);
            Refused(*device, opcode, (opcode & 1u) != 0u ? 62u : 20u);
            Refused(*device, opcode, (opcode & 1u) != 0u ? 20u : 62u, 0xf2eu);
        }
        GuestAllocations::Mutation().Remove(Texels.data());
        std::cout << "image BY2/BY4 execution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
