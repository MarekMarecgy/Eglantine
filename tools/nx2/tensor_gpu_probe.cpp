// SPDX-License-Identifier: GPL-3.0-or-later
// GPU arithmetic check for the actual backend modules emitted by [tensor] tests.
// Windows x64: cl /std:c++20 /EHsc /O2 /arch:AVX2 /I<VulkanSDK>/Include
//   tensor_gpu_probe.cpp /link <VulkanSDK>/Lib/vulkan-1.lib
// Run with the EDEN_TENSOR_TEST_OUTPUT directory as the sole argument.
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

static void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Vulkan result " + std::to_string(result));
    }
}
static uint16_t Half(float value) {
    return static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(value), 0)));
}
static float Float(uint16_t value) {
    return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(value)));
}
static uint32_t Pack(float a, float b) { return Half(a) | (uint32_t{Half(b)} << 16); }

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: tensor_gpu_probe <SPIR-V directory>");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    Check(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> physicals(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, physicals.data()));
    VkPhysicalDevice physical{};
    for (auto candidate : physicals) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = candidate;
            std::cout << "GPU: " << properties.deviceName << '\n';
            break;
        }
    }
    if (!physical) throw std::runtime_error("No discrete Vulkan GPU");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family{};
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) throw std::runtime_error("No compute queue");
    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.subgroupSizeControl = VK_TRUE; features.computeFullSubgroups = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device{};
    Check(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
    constexpr size_t lanes = 64, words = lanes * 10, bytes = words * sizeof(uint32_t);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer{}; Check(vkCreateBuffer(device, &bci, nullptr, &buffer));
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t memory_type{};
    constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (memory_type < memory_properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1U << memory_type)) ||
            (memory_properties.memoryTypes[memory_type].propertyFlags & flags) != flags)) ++memory_type;
    if (memory_type == memory_properties.memoryTypeCount) throw std::runtime_error("No coherent host memory");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = requirements.size; mai.memoryTypeIndex = memory_type;
    VkDeviceMemory memory{}; Check(vkAllocateMemory(device, &mai, nullptr, &memory));
    Check(vkBindBufferMemory(device, buffer, memory, 0));
    uint32_t* mapped{}; Check(vkMapMemory(device, memory, 0, bytes, 0, reinterpret_cast<void**>(&mapped)));
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 1; slci.pBindings = &binding;
    VkDescriptorSetLayout set_layout{}; Check(vkCreateDescriptorSetLayout(device, &slci, nullptr, &set_layout));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout{}; Check(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
    VkDescriptorPool pool{}; Check(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set{}; Check(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo buffer_info{buffer, 0, bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info; vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family; cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool command_pool{}; Check(vkCreateCommandPool(device, &cpci, nullptr, &command_pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = command_pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer command{}; Check(vkAllocateCommandBuffers(device, &cbai, &command));
    std::mt19937 random{0x16816};
    std::uniform_real_distribution<float> value{-1, 1};
    uint64_t checked{};
    for (const char* name : {"tensor-shuffle-io-1", "tensor-shuffle-io-2", "tensor-shuffle-io-guarded"}) {
        const bool guarded = std::string_view{name}.ends_with("guarded");
        const int repeats = std::string_view{name}.ends_with("-2") ? 2 : 1;
        std::ifstream file{std::filesystem::path{argv[1]} / (std::string{name} + ".spv"), std::ios::binary | std::ios::ate};
        if (!file) throw std::runtime_error("Missing arithmetic test module");
        const size_t size = static_cast<size_t>(file.tellg());
        std::vector<uint32_t> code(size / 4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), size);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = size; smci.pCode = code.data();
        VkShaderModule module{}; Check(vkCreateShaderModule(device, &smci, nullptr, &module));
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        subgroup.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.pNext = &subgroup;
        pci.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = module; pci.stage.pName = "main";
        pci.layout = layout;
        VkPipeline pipeline{}; Check(vkCreateComputePipelines(device, {}, 1, &pci, nullptr, &pipeline));
        for (int trial = 0; trial < 128; ++trial) {
            std::array<uint32_t, words> expected;
            expected.fill(0xdeadbeef);
            std::fill(mapped, mapped + words, 0xdeadbeef);
            for (int warp = 0; warp < 2; ++warp) {
                float a[16][16], b[16][8], c[16][8];
                for (auto& row : a) for (auto& v : row) v = Float(Half(value(random)));
                for (auto& row : b) for (auto& v : row) v = Float(Half(value(random)));
                for (auto& row : c) for (auto& v : row) v = Float(Half(value(random)));
                for (int lane = 0; lane < 32; ++lane) {
                    const int g = lane / 4, i = 2 * (lane % 4), offset = (warp * 32 + lane) * 10;
                    const std::array<uint32_t, 8> input{
                        Pack(a[g][i], a[g][i+1]), Pack(a[g+8][i], a[g+8][i+1]),
                        Pack(a[g][i+8], a[g][i+9]), Pack(a[g+8][i+8], a[g+8][i+9]),
                        Pack(b[i][g], b[i+1][g]), Pack(b[i+8][g], b[i+9][g]),
                        Pack(c[g][i], c[g][i+1]), Pack(c[g+8][i], c[g+8][i+1])};
                    std::copy(input.begin(), input.end(), mapped + offset);
                }
                if (guarded && warp == 1) continue;
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    for (int row = 0; row < 16; ++row) for (int col = 0; col < 8; ++col) {
                        float sum = c[row][col];
                        for (int k = 0; k < 16; ++k) sum = std::fma(a[row][k], b[k][col], sum);
                        c[row][col] = Float(Half(sum));
                    }
                }
                for (int lane = 0; lane < 32; ++lane) {
                    const int g = lane / 4, i = 2 * (lane % 4), offset = (warp * 32 + lane) * 10;
                    expected[offset + 8] = Pack(c[g][i], c[g][i+1]);
                    expected[offset + 9] = Pack(c[g+8][i], c[g+8][i+1]);
                }
            }
            Check(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            Check(vkBeginCommandBuffer(command, &begin));
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command, 1, 1, 1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            Check(vkEndCommandBuffer(command));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            Check(vkQueueSubmit(queue, 1, &submit, {})); Check(vkQueueWaitIdle(queue));
            for (size_t lane = 0; lane < lanes; ++lane) for (int word = 8; word < 10; ++word) {
                const size_t offset = lane * 10 + word;
                if (mapped[offset] != expected[offset]) {
                    std::cerr << name << " trial " << trial << " lane " << lane << " word " << word
                              << " got " << std::hex << mapped[offset] << " expected " << expected[offset] << '\n';
                    throw std::runtime_error("GPU arithmetic mismatch");
                }
                checked += 2;
            }
        }
        std::cout << name << ": 128 randomized matrix cases passed (bit exact)\n";
        vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
    }
    std::cout << "PASS: " << checked << " output half values checked, including inactive subgroups\n";
    vkDestroyCommandPool(device, command_pool, nullptr); vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr); vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
