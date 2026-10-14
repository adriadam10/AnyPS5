#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

alignas(256) constexpr std::array<std::uint32_t, 7> LineKernel{
    0xd7460000u, 0x04010c04u, 0x380200ffu, 0xabcd0000u, 0xe0102000u, 0x80000100u, 0xbf810000u
};
alignas(256) constexpr std::array<std::uint32_t, 13> GridKernel{
    0xd7460000u, 0x04010804u, 0xd7460001u, 0x04050405u, 0xd76f0002u, 0x04011101u, 0x380404ffu, 0xab000000u,
    0x16060294u, 0x4a060700u, 0xe0102000u, 0x80000203u, 0xbf810000u
};
constexpr std::uint32_t GridRowStride = 20;
constexpr std::uint32_t Sentinel = 0xcafebabeu;
alignas(256) std::array<std::uint32_t, 512> Output{};

struct Kernel {
    std::span<const std::uint32_t> code;
    std::array<std::uint32_t, 3> group;
    std::uint32_t resources;
};

constexpr Kernel Line{LineKernel, {64, 1, 1}, (4u << 1u) | (1u << 7u)};
constexpr Kernel Grid{GridKernel, {16, 4, 1}, (4u << 1u) | (1u << 7u) | (1u << 8u) | (1u << 11u)};

struct Case {
    const Kernel& kernel;
    std::array<std::uint32_t, 3> threads;
    bool packedGroups;
};

std::array<std::uint32_t, 4> Descriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(Output.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (4u << 16u), static_cast<std::uint32_t>(Output.size()), 0x14004u};
}

std::vector<std::uint32_t> Commands(const Case& item, std::uint32_t wave) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    std::array<std::uint32_t, 3> shape = item.kernel.group;
    std::array<std::uint32_t, 3> dimensions = item.threads;
    auto initiator = 0x41u | (wave == 32 ? 0x8000u : 0u);
    if (item.packedGroups) {
        initiator |= 0x2u;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto group = item.kernel.group[axis];
            const auto remainder = item.threads[axis] % group;
            shape[axis] = group | ((remainder == 0 ? group : remainder) << 16u);
            dimensions[axis] = (item.threads[axis] + group - 1) / group;
        }
    } else {
        initiator |= 0x20u;
    }
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(item.kernel.code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resources{item.kernel.resources};
    registers(0x213, resources);
    registers(0x240, Descriptor());
    words.insert(words.end(), {0xc0031500u, dimensions[0], dimensions[1], dimensions[2], initiator});
    return words;
}

std::uint32_t Expected(const Case& item, std::size_t index) {
    if (&item.kernel == &Line) return index < item.threads[0] ? 0xabcd0000u | static_cast<std::uint32_t>(index) : Sentinel;
    const auto x = static_cast<std::uint32_t>(index % GridRowStride);
    const auto y = static_cast<std::uint32_t>(index / GridRowStride);
    return x < item.threads[0] && y < item.threads[1] ? 0xab000000u | (y << 8u) | x : Sentinel;
}

void Run(const Case& item, std::uint32_t wave) {
    Output.fill(Sentinel);
    const auto words = Commands(item, wave);
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
    for (std::size_t index = 0; index < Output.size(); ++index) {
        const auto expected = Expected(item, index);
        Require(Output[index] == expected, (&item.kernel == &Line ? "line" : "grid") + std::string(item.packedGroups ? " packed" : " threads") + " dispatch of " + std::to_string(item.threads[0]) + "x" + std::to_string(item.threads[1]) + " wave " + std::to_string(wave) + " mismatch at " + std::to_string(index) + ": got " + std::to_string(Output[index]) + " expected " + std::to_string(expected));
    }
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        constexpr std::array<Case, 9> cases{{
            {Line, {128, 1, 1}, false},
            {Line, {80, 1, 1}, false},
            {Line, {80, 1, 1}, true},
            {Line, {1, 1, 1}, false},
            {Line, {129, 1, 1}, true},
            {Grid, {16, 4, 1}, false},
            {Grid, {20, 6, 1}, false},
            {Grid, {20, 6, 1}, true},
            {Grid, {17, 5, 1}, true}
        }};
        for (const auto wave : {32u, 64u}) {
            for (const auto& item : cases) Run(item, wave);
        }
        AgcDriverShutdown_nid_postfix();
        std::puts("dispatch thread limit tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
