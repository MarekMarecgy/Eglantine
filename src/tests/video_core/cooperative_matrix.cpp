// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "video_core/vulkan_common/cooperative_matrix.h"

namespace {
using namespace Shader;

struct TensorProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    explicit TensorProgram(u32 count = 1, bool storage_io = false) {
        program.stage = Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.uses_tensor_mul_add = count != 0;
        program.info.uses_subgroup_invocation_id = count != 0;
        profile.support_cooperative_matrix = true;
        profile.supported_spirv = 0x00010500;
        profile.max_compute_shared_memory_size = 49152;
        profile.max_tensor_workgroup_size = 1024;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
        profile.max_subgroup_size = 32;
        IR::IREmitter ir{block};
        if (count == 0) {
            return;
        }
        const auto lane = ir.LaneId();
        IR::U32 byte_offset;
        if (storage_io) {
            program.info.uses_local_invocation_id = true;
            program.info.storage_buffers_descriptors.push_back({0, 0, 1, true});
            byte_offset = ir.IMul(ir.LocalInvocationIdX(), ir.Imm32(40U));
        }
        std::array<IR::U32, 8> inputs;
        for (u32 i = 0; i < inputs.size(); ++i) {
            inputs[i] = storage_io
                            ? IR::U32{ir.Inst(IR::Opcode::LoadStorage32, ir.Imm32(0U),
                                             ir.IAdd(byte_offset, ir.Imm32(i * 4U)))}
                            : IR::U32{ir.IAdd(ir.Imm32((i + 1) * 0x01000100U), lane)};
        }
        for (u32 i = 0; i < count; ++i) {
            const auto result = ir.Inst(IR::Opcode::TensorMulAdd, inputs[0], inputs[1],
                                       inputs[2], inputs[3], inputs[4], inputs[5],
                                       inputs[6], inputs[7]);
            inputs[6] = IR::U32{ir.CompositeExtract(result, 0)};
            inputs[7] = IR::U32{ir.CompositeExtract(result, 1)};
        }
        if (storage_io) {
            for (u32 i = 0; i < 2; ++i) {
                ir.Inst(IR::Opcode::WriteStorage32, ir.Imm32(0U),
                        ir.IAdd(byte_offset, ir.Imm32((i + 8) * 4U)), inputs[i + 6]);
            }
        }
    }

    std::vector<u32> Emit(std::string_view name = {}) {
        auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* directory = std::getenv("EDEN_TENSOR_TEST_OUTPUT");
            directory && !name.empty()) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
            REQUIRE(file.good());
        }
        return code;
    }
};

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == opcode) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + size);
        }
        offset += size;
    }
    return result;
}

bool HasCapability(const std::vector<u32>& code, u32 capability) {
    const auto capabilities = Instructions(code, 17);
    return std::ranges::any_of(capabilities, [=](const auto& words) {
        return words[0] == capability;
    });
}

struct TensorMode {
    std::string previous;
    bool existed{};

    explicit TensorMode(const char* mode) {
        if (const char* value = std::getenv("EDEN_NX2_TENSOR")) {
            previous = value;
            existed = true;
        }
        Set(mode);
    }

    static void Set(const char* value) {
#ifdef _WIN32
        _putenv_s("EDEN_NX2_TENSOR", value ? value : "");
#else
        if (value) {
            setenv("EDEN_NX2_TENSOR", value, 1);
        } else {
            unsetenv("EDEN_NX2_TENSOR");
        }
#endif
    }

    ~TensorMode() {
        Set(existed ? previous.c_str() : nullptr);
    }
};
}

TEST_CASE("Ordinary compute does not allocate cooperative resources", "[video_core][tensor]") {
    TensorProgram fixture{0};
    const auto code = fixture.Emit("ordinary");
    REQUIRE_FALSE(HasCapability(code, 5357));
    REQUIRE_FALSE(HasCapability(code, 9));
    const auto variables = Instructions(code, 59);
    REQUIRE(std::ranges::none_of(variables, [](const auto& words) { return words[2] == 4; }));
}

TEST_CASE("Tensor scratch barriers stay within each subgroup", "[video_core][tensor]") {
    for (u32 count : {1U, 8U, 32U}) {
        TensorProgram fixture{count};
        const auto code = fixture.Emit("tensor-" + std::to_string(count));
        std::map<u32, u32> constants;
        for (const auto& words : Instructions(code, 43)) {
            constants.emplace(words[1], words[2]);
        }
        const auto barriers = Instructions(code, 224);
        REQUIRE(barriers.size() == count * 3);
        for (const auto& barrier : barriers) {
            REQUIRE(constants.at(barrier[0]) == 3);
            REQUIRE(constants.at(barrier[1]) == 3);
            REQUIRE(constants.at(barrier[2]) == 0x108);
        }
        REQUIRE(Instructions(code, 5361).size() == count);
        REQUIRE(Instructions(code, 5359).size() == count * 3);
    }
}

TEST_CASE("Tensor scratch coexists with both shared memory layouts", "[video_core][tensor]") {
    for (bool explicit_layout : {false, true}) {
        TensorProgram fixture;
        fixture.program.shared_memory_size = 4097;
        fixture.profile.support_explicit_workgroup_layout = explicit_layout;
        fixture.profile.support_workgroup_layout_16bit_access = true;
        const auto code = fixture.Emit(explicit_layout ? "shared-explicit" : "shared-plain");
        REQUIRE(HasCapability(code, 4428) == explicit_layout);
        REQUIRE(HasCapability(code, 4430) == explicit_layout);
        if (explicit_layout) {
            const auto decorations = Instructions(code, 72);
            REQUIRE(std::ranges::any_of(decorations, [](const auto& words) {
                return words.size() == 4 && words[2] == 35 && words[3] == 4112;
            }));
            std::vector<u32> aliased;
            for (const auto& words : Instructions(code, 71)) {
                if (words[1] == 20) {
                    aliased.push_back(words[0]);
                }
            }
            for (const auto& words : Instructions(code, 59)) {
                if (words[2] == 4) {
                    REQUIRE(std::ranges::find(aliased, words[1]) != aliased.end());
                }
            }
        }
    }
    TensorProgram fallback;
    fallback.program.shared_memory_size = 4096;
    fallback.profile.support_explicit_workgroup_layout = true;
    fallback.profile.support_workgroup_layout_16bit_access = false;
    REQUIRE_FALSE(HasCapability(fallback.Emit("shared-no16"), 4428));
}

TEST_CASE("Tensor admission also protects cached shaders", "[video_core][tensor]") {
    TensorProgram disabled;
    disabled.profile.support_cooperative_matrix = false;
    REQUIRE_THROWS_AS(disabled.Emit(), TensorRejected);
    TensorProgram stage;
    stage.program.stage = Stage::Fragment;
    REQUIRE_THROWS_AS(stage.Emit(), TensorRejected);
    TensorProgram version;
    version.profile.supported_spirv = 0x00010000;
    REQUIRE_THROWS_AS(version.Emit(), TensorRejected);
    for (auto size : {std::array{0U, 1U, 1U}, std::array{31U, 1U, 1U},
                      std::array{64U, 2U, 1U}, std::array{64U, 1U, 2U},
                      std::array{2048U, 1U, 1U}}) {
        TensorProgram fixture;
        fixture.program.workgroup_size = size;
        REQUIRE_THROWS_AS(fixture.Emit(), TensorRejected);
    }
    TensorProgram boundary;
    boundary.profile.max_tensor_workgroup_size = 32;
    REQUIRE_THROWS_AS(boundary.Emit(), TensorRejected);
    boundary.profile.max_tensor_workgroup_size = 1024;
    boundary.program.workgroup_size = {128, 1, 1};
    boundary.program.shared_memory_size = 44032;
    REQUIRE_NOTHROW(boundary.Emit("shared-boundary"));
    boundary.program.shared_memory_size = 44033;
    REQUIRE_THROWS_AS(boundary.Emit(), TensorRejected);
    for (auto type : {IR::AbstractSyntaxNode::Type::If, IR::AbstractSyntaxNode::Type::Loop,
                      IR::AbstractSyntaxNode::Type::Return, IR::AbstractSyntaxNode::Type::Break,
                      IR::AbstractSyntaxNode::Type::Unreachable}) {
        TensorProgram fixture;
        fixture.program.syntax_list.insert(fixture.program.syntax_list.begin(), {{}, type});
        REQUIRE_THROWS_AS(fixture.Emit(), TensorRejected);
    }
}

TEST_CASE("NV tensor shape requires the exact scope and all four half types", "[video_core][tensor]") {
    const VkCooperativeMatrixPropertiesNV exact{
        VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_NV, nullptr, 16, 8, 16,
        VK_COMPONENT_TYPE_FLOAT16_NV, VK_COMPONENT_TYPE_FLOAT16_NV,
        VK_COMPONENT_TYPE_FLOAT16_NV, VK_COMPONENT_TYPE_FLOAT16_NV, VK_SCOPE_SUBGROUP_NV};
    REQUIRE(Vulkan::IsTensorMatrixShape(exact));
    for (auto member : {&VkCooperativeMatrixPropertiesNV::MSize,
                        &VkCooperativeMatrixPropertiesNV::NSize,
                        &VkCooperativeMatrixPropertiesNV::KSize}) {
        auto wrong = exact;
        wrong.*member *= 2;
        REQUIRE_FALSE(Vulkan::IsTensorMatrixShape(wrong));
    }
    for (auto member : {&VkCooperativeMatrixPropertiesNV::AType,
                        &VkCooperativeMatrixPropertiesNV::BType,
                        &VkCooperativeMatrixPropertiesNV::CType,
                        &VkCooperativeMatrixPropertiesNV::DType}) {
        auto wrong = exact;
        wrong.*member = VK_COMPONENT_TYPE_FLOAT32_NV;
        REQUIRE_FALSE(Vulkan::IsTensorMatrixShape(wrong));
    }
    auto wrong = exact;
    wrong.scope = VK_SCOPE_WORKGROUP_NV;
    REQUIRE_FALSE(Vulkan::IsTensorMatrixShape(wrong));
}

TEST_CASE("Shuffle tensor lowering needs neither half math nor shared scratch", "[video_core][tensor]") {
    TensorProgram fixture;
    fixture.profile.support_cooperative_matrix = false;
    fixture.profile.support_tensor_shuffle = true;
    fixture.program.shared_memory_size = fixture.profile.max_compute_shared_memory_size;
    const auto code = fixture.Emit("tensor-shuffle");
    REQUIRE_FALSE(HasCapability(code, 5357));
    REQUIRE_FALSE(HasCapability(code, 9)); // Float16
    REQUIRE(HasCapability(code, 65)); // GroupNonUniformShuffle
    REQUIRE(Instructions(code, 224).empty()); // OpControlBarrier
    REQUIRE(Instructions(code, 345).size() == 32); // OpGroupNonUniformShuffle
}

TEST_CASE("Shuffle tensor lowering accepts a structured selection before the multiply",
          "[video_core][tensor]") {
    TensorProgram fixture;
    IR::Block entry{fixture.pool}, body{fixture.pool};
    IR::IREmitter ir{entry};
    const auto group = ir.WorkgroupIdX();
    const auto cond = ir.IEqual(group, ir.Imm32(0U));
    fixture.program.info.uses_workgroup_id = true;
    fixture.program.blocks = {&entry, &body, &fixture.block};
    fixture.program.post_order_blocks = {&fixture.block, &body, &entry};
    fixture.program.syntax_list = {
        {{.block = &entry}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {cond, &body, &fixture.block}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &body}, IR::AbstractSyntaxNode::Type::Block},
        {{.end_if = {&fixture.block}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &fixture.block}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
    };
    REQUIRE_THROWS_AS(fixture.Emit(), TensorRejected);
    fixture.profile.support_tensor_shuffle = true;
    const auto code = fixture.Emit("tensor-shuffle-selection");
    REQUIRE_FALSE(HasCapability(code, 5357));
    REQUIRE(Instructions(code, 224).empty());
    REQUIRE(Instructions(code, 345).size() == 32);
}

// With EDEN_TENSOR_TEST_OUTPUT these modules can also be dispatched by a Vulkan
// harness to compare all four outputs per lane against a CPU matrix reference.
TEST_CASE("Shuffle tensor arithmetic modules retain observable storage outputs", "[video_core][tensor]") {
    for (u32 count : {1U, 2U}) {
        TensorProgram fixture{count, true};
        fixture.profile.support_cooperative_matrix = false;
        fixture.profile.support_tensor_shuffle = true;
        const auto code = fixture.Emit("tensor-shuffle-io-" + std::to_string(count));
        REQUIRE(Instructions(code, 345).size() == 32 * count);
        REQUIRE_FALSE(HasCapability(code, 9));
    }
    TensorProgram fixture{1, true};
    fixture.profile.support_tensor_shuffle = true;
    IR::Block entry{fixture.pool}, merge{fixture.pool};
    IR::IREmitter ir{entry};
    const auto cond = ir.ILessThan(ir.LocalInvocationIdX(), ir.Imm32(32U), false);
    fixture.program.blocks = {&entry, &fixture.block, &merge};
    fixture.program.post_order_blocks = {&merge, &fixture.block, &entry};
    fixture.program.syntax_list = {
        {{.block = &entry}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {cond, &fixture.block, &merge}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &fixture.block}, IR::AbstractSyntaxNode::Type::Block},
        {{.end_if = {&merge}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &merge}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
    };
    const auto code = fixture.Emit("tensor-shuffle-io-guarded");
    REQUIRE_FALSE(HasCapability(code, 5357));
    REQUIRE(Instructions(code, 224).empty());
}

TEST_CASE("Native tensor lowering preserves explicit disable and fallback errors", "[video_core][tensor]") {
    TensorMode mode{nullptr};
    std::array<Ampere::Instruction, 2> code{{
        {{0x0810723c, 12, 0x8ff, 0}}, {{0x0000794d, 0, 0x03800000, 0}}}};
    REQUIRE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    TensorMode::Set("off");
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    TensorMode::Set("scalar");
    REQUIRE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    TensorMode::Set("fast");
    const auto unsupported = Ampere::LowerToMaxwell(code, false, false);
    REQUIRE_FALSE(static_cast<bool>(unsupported));
    REQUIRE(unsupported.error.find("matrix multiply") != std::string_view::npos);
    const auto supported = Ampere::LowerToMaxwell(code, false, true);
    REQUIRE(static_cast<bool>(supported));
    REQUIRE(supported.instructions[0] >> 52 == 0x5d3);
    REQUIRE(Maxwell::Decode(supported.instructions[0]) == Maxwell::Opcode::TENSOR_MUL_ADD);
    code[0].words[0] &= ~0x7000U;
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    code[0].words[0] |= 0x7000;
    code[0].words[3] = 1;
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    code[0].words[3] = 0;
    code[0].words[0] += 1U << 24;
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
}
