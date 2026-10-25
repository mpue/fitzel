// The Vulkan device: instance, GPU, swapchain, frames and submission, the
// draw path (pipelines, descriptors, uniforms, vertex input, dynamic state),
// queries, fences and the window. Resources are in VkResources.cpp, shaders in
// VkShaders.cpp, the UI renderer in VkImGui.cpp.

#include "VulkanDevice.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include "VkImGui.hpp"

namespace fitzel::rhi::vk {

// --- The window -------------------------------------------------------------------

bool prepareWindowHints(int attempt) {
    if (attempt > 0) return false;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    return true;
}

std::unique_ptr<Device> createDevice(GLFWwindow* window, bool vsync) {
    return std::make_unique<VulkanDevice>(window, vsync);
}

namespace {

bool envOn(const char* name) {
    const char* e = std::getenv(name);
    return e && e[0] && e[0] != '0';
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[Fitzel] Vulkan: %s\n", data->pMessage);
    return VK_FALSE;
}

VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

std::uint64_t fnv(const void* data, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

VkCompareOp compareOf(CompareOp c) {
    switch (c) {
        case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:         return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

VkStencilOp stencilOf(StencilOp o) {
    switch (o) {
        case StencilOp::Keep:          return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero:          return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace:       return VK_STENCIL_OP_REPLACE;
        case StencilOp::Increment:     return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOp::Decrement:     return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOp::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        case StencilOp::Invert:        return VK_STENCIL_OP_INVERT;
    }
    return VK_STENCIL_OP_KEEP;
}

VkBlendFactor blendOf(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColor:    return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::SrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp blendOpOf(BlendOp o) {
    switch (o) {
        case BlendOp::Add:             return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:             return VK_BLEND_OP_MIN;
        case BlendOp::Max:             return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkPrimitiveTopology topologyOf(Primitive p) {
    switch (p) {
        case Primitive::Points:        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case Primitive::Lines:         return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case Primitive::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case Primitive::Triangles:     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case Primitive::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkFormat vertexFormatOf(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float:      return VK_FORMAT_R32_SFLOAT;
        case VertexFormat::Float2:     return VK_FORMAT_R32G32_SFLOAT;
        case VertexFormat::Float3:     return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::Float4:     return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexFormat::UByte4Norm: return VK_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::Int:        return VK_FORMAT_R32_SINT;
        case VertexFormat::UInt:       return VK_FORMAT_R32_UINT;
    }
    return VK_FORMAT_R32G32B32_SFLOAT;
}

std::uint32_t vertexSizeOf(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float:      return 4;
        case VertexFormat::Float2:     return 8;
        case VertexFormat::Float3:     return 12;
        case VertexFormat::Float4:     return 16;
        case VertexFormat::UByte4Norm: return 4;
        case VertexFormat::Int:        return 4;
        case VertexFormat::UInt:       return 4;
    }
    return 12;
}

} // namespace

// --- Set-up -----------------------------------------------------------------------

VulkanDevice::VulkanDevice(GLFWwindow* window, bool vsync) : m_window(window), m_vsync(vsync) {
    if (volkInitialize() != VK_SUCCESS)
        throw std::runtime_error("Vulkan: no Vulkan loader on this system");
    createInstance();
    pickPhysicalDevice();
    createLogicalDevice();
    createFrames();
    beginCommands();
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    createSwapchain();
    createBackbuffers(std::max(w, 1), std::max(h, 1));
    createDummies();
    m_viewport = Rect{0, 0, w, h};
    m_scissor  = Rect{0, 0, w, h};
}

void VulkanDevice::createInstance() {
    std::uint32_t glfwCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwCount);
    if (!glfwExts) throw std::runtime_error("Vulkan: GLFW cannot make a Vulkan surface here");
    std::vector<const char*> exts(glfwExts, glfwExts + glfwCount);
    std::vector<const char*> layers;

    // FITZEL_VK_VALIDATION=1: the Khronos validation layer, where it is installed
    // (or found through VK_LAYER_PATH), its warnings and errors on stderr.
    const bool validate = envOn("FITZEL_VK_VALIDATION");
    if (validate) {
        std::uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (const auto& p : props)
            if (std::string_view(p.layerName) == "VK_LAYER_KHRONOS_validation")
                layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) std::fprintf(stderr, "[Fitzel] Vulkan: no validation layer found\n");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Fitzel";
    app.pEngineName = "Fitzel";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    FZ_VK_INIT(vkCreateInstance(&ci, nullptr, &m_instance));
    volkLoadInstance(m_instance);

    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = debugMessage;
        vkCreateDebugUtilsMessengerEXT(m_instance, &mi, nullptr, &g_messenger);
    }
    FZ_VK_INIT(glfwCreateWindowSurface(m_instance, m_window, nullptr, &m_surface));
}

void VulkanDevice::pickPhysicalDevice() {
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(m_instance, &n, gpus.data());
    // FITZEL_VK_DEVICE=<part of the name> picks one; otherwise a discrete GPU
    // before an integrated one before the rest.
    const char* want = std::getenv("FITZEL_VK_DEVICE");
    int best = -1;
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        std::uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        int family = -1;
        for (std::uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, m_surface, &present);
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((qs[i].queueFlags & need) == need && present) { family = static_cast<int>(i); break; }
        }
        if (family < 0) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want && std::string_view(p.deviceName).find(want) != std::string_view::npos) score += 10;
        if (score > best) {
            best = score;
            m_gpu = g;
            m_queueFamily = static_cast<std::uint32_t>(family);
            m_props = p;
        }
    }
    if (!m_gpu) throw std::runtime_error("Vulkan: no GPU with Vulkan 1.3 that can show this window");
}

void VulkanDevice::createLogicalDevice() {
    std::uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &n, avail.data());
    auto has = [&](const char* e) {
        for (const auto& a : avail) if (std::string_view(a.extensionName) == e) return true;
        return false;
    };
    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    m_memoryBudget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (m_memoryBudget) exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(m_gpu, &have);
    if (!have.shaderClipDistance)
        throw std::runtime_error("Vulkan: the GPU has no clip distances");
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.samplerAnisotropy = have.samplerAnisotropy;
    f2.features.fillModeNonSolid = have.fillModeNonSolid;
    f2.features.shaderClipDistance = VK_TRUE;
    f2.features.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
    f2.features.shaderStorageImageReadWithoutFormat = have.shaderStorageImageReadWithoutFormat;
    f2.features.largePoints = have.largePoints;
    m_wireframe = have.fillModeNonSolid;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;   // `discard` in SPIR-V 1.6
    f2.pNext = &f12;
    f12.pNext = &f13;

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    FZ_VK_INIT(vkCreateDevice(m_gpu, &ci, nullptr, &m_device));
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VmaVulkanFunctions fn{};
    fn.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fn.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = m_gpu;
    ai.device = m_device;
    ai.instance = m_instance;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.pVulkanFunctions = &fn;
    if (m_memoryBudget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    FZ_VK_INIT(vmaCreateAllocator(&ai, &m_vma));

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(m_gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    m_formats.hasD32S8 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(m_device, &pc, nullptr, &m_pipelineCache);

    VkSemaphoreTypeCreateInfo tl{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tl.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tl;
    FZ_VK_INIT(vkCreateSemaphore(m_device, &sci, nullptr, &m_timeline));

    VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = 4096;
    FZ_VK_INIT(vkCreateQueryPool(m_device, &qp, nullptr, &m_timestampPool));
    qp.queryType = VK_QUERY_TYPE_OCCLUSION;
    qp.queryCount = 1024;
    FZ_VK_INIT(vkCreateQueryPool(m_device, &qp, nullptr, &m_occlusionPool));
    for (std::uint32_t i = 4096; i-- > 0;) m_freeTimestamps.push_back(i);
    for (std::uint32_t i = 1024; i-- > 0;) m_freeOcclusion.push_back(i);

    // What the device can do, in the engine's words.
    const VkPhysicalDeviceLimits& lim = m_props.limits;
    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &qn, qs.data());
    m_caps.backend = Backend::Vulkan;
    m_caps.compute = true;
    m_caps.timestampBits = static_cast<int>(qs[m_queueFamily].timestampValidBits);
    m_caps.timerQueries = m_caps.timestampBits > 0 && lim.timestampComputeAndGraphics;
    m_caps.maxTextureSize = static_cast<int>(lim.maxImageDimension2D);
    m_maxAniso = have.samplerAnisotropy ? lim.maxSamplerAnisotropy : 1.0f;
    m_caps.maxAnisotropy = m_maxAniso;
    m_caps.maxStorageBlockSize = lim.maxStorageBufferRange;
    m_caps.apiMajor = static_cast<int>(VK_API_VERSION_MAJOR(m_props.apiVersion));
    m_caps.apiMinor = static_cast<int>(VK_API_VERSION_MINOR(m_props.apiVersion));
    m_apiVersion = std::to_string(m_caps.apiMajor) + "." + std::to_string(m_caps.apiMinor) + "." +
                   std::to_string(VK_API_VERSION_PATCH(m_props.apiVersion));
    m_apiName  = "Vulkan " + m_apiVersion;
    m_renderer = m_props.deviceName;
    m_vendor   = std::to_string(m_props.vendorID);
    m_caps.apiName = m_apiName.c_str();
    m_caps.apiVersion = m_apiVersion.c_str();
    m_caps.renderer = m_renderer.c_str();
    m_caps.vendor = m_vendor.c_str();
}

void VulkanDevice::createFrames() {
    for (Frame& f : m_frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = m_queueFamily;
        FZ_VK_INIT(vkCreateCommandPool(m_device, &pi, nullptr, &f.pool));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        FZ_VK_INIT(vkCreateSemaphore(m_device, &si, nullptr, &f.acquire));
    }
}

void VulkanDevice::createSwapchain() {
    VkSurfaceCapabilitiesKHR sc;
    FZ_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &sc));
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    VkExtent2D ext = sc.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width  = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::max(w, 1)),
                                               sc.minImageExtent.width, sc.maxImageExtent.width);
        ext.height = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::max(h, 1)),
                                               sc.minImageExtent.height, sc.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) return;   // minimised: nothing to show into

    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &n, formats.data());
    // Not sRGB: the engine writes display values itself, as it does into
    // OpenGL's default framebuffer.
    VkSurfaceFormatKHR pick = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = f; break; }

    vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (m_vsync && m_adaptive)
        for (VkPresentModeKHR m : modes) if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) mode = m;
    if (!m_vsync) {
        for (VkPresentModeKHR m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        for (VkPresentModeKHR m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = m_surface;
    ci.minImageCount = std::max(sc.minImageCount, 2u);
    if (sc.maxImageCount) ci.minImageCount = std::min(ci.minImageCount + 1, sc.maxImageCount);
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = sc.currentTransform;
    ci.compositeAlpha = (sc.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : static_cast<VkCompositeAlphaFlagBitsKHR>(sc.supportedCompositeAlpha &
                                                                       -sc.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = m_swapchain;
    VkSwapchainKHR next = VK_NULL_HANDLE;
    FZ_VK_INIT(vkCreateSwapchainKHR(m_device, &ci, nullptr, &next));
    destroySwapchain();
    m_swapchain = next;
    m_swapFormat = pick.format;
    m_swapExtent = ext;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr);
    m_swapImages.resize(n);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, m_swapImages.data());
    for (std::uint32_t i = 0; i < n; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        FZ_VK_INIT(vkCreateSemaphore(m_device, &si, nullptr, &s));
        m_renderDone.push_back(s);
    }
    m_swapDirty = false;
}

void VulkanDevice::destroySwapchain() {
    if (!m_swapchain) return;
    vkDeviceWaitIdle(m_device);
    for (VkSemaphore s : m_renderDone) vkDestroySemaphore(m_device, s, nullptr);
    m_renderDone.clear();
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_swapImages.clear();
}

void VulkanDevice::createBackbuffers(int width, int height) {
    for (TextureHandle& t : m_back) if (t) destroy(t);
    if (m_backDepth) destroy(m_backDepth);
    TextureDesc c;
    c.format = Format::RGBA8;
    c.width = width;
    c.height = height;
    c.sampler = SamplerDesc::nearestClamp();
    m_back[0] = createTexture(c, nullptr, Format::Unknown);
    m_back[1] = createTexture(c, nullptr, Format::Unknown);
    TextureDesc d;
    d.format = Format::Depth24Stencil8;
    d.width = width;
    d.height = height;
    d.usage = UsageRenderTarget;
    m_backDepth = createTexture(d, nullptr, Format::Unknown);
    m_backIndex = 0;
    m_windowDesc = FramebufferDesc{};
    m_windowDesc.color[0].texture = m_back[0];
    m_windowDesc.depth.texture = m_backDepth;
    m_backW = width;
    m_backH = height;
}

void VulkanDevice::createDummies() {
    // (0,0,0,1): what OpenGL samples from an incomplete texture.
    const unsigned char black[4 * 6] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
                                        0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    const TextureType types[4] = {TextureType::Tex2D, TextureType::Tex2DArray, TextureType::Tex3D,
                                  TextureType::Cube};
    for (int k = 0; k < 4; ++k) {
        TextureDesc d;
        d.type = types[k];
        d.format = Format::RGBA8;
        d.usage = UsageSampled;
        m_dummy[static_cast<std::size_t>(k)] = createTexture(d, black, Format::RGBA8);
    }
    const float attrib[8] = {0, 0, 0, 1, 0, 0, 0, 0};
    std::int32_t ints[4] = {0, 0, 0, 1};
    unsigned char bytes[32];
    std::memcpy(bytes, attrib, 16);
    std::memcpy(bytes + 16, ints, 16);
    m_defaultAttrib = createBuffer(BufferKind::Vertex, BufferUsage::Static, sizeof(bytes), bytes);
}

VulkanDevice::~VulkanDevice() {
    if (m_device) vkDeviceWaitIdle(m_device);
    m_imgui.reset();
    if (m_cmd) vkEndCommandBuffer(m_cmd);
    // Whatever the engine still holds goes with the device.
    m_programs.forEach([this](std::uint32_t, VkProg& p) { freeProgram(p); });
    m_textures.forEach([this](std::uint32_t, VkTex& t) { freeTexture(t); });
    m_buffers.forEach([this](std::uint32_t, VkBuf& b) { freeBuffer(b); });
    while (!m_deferred.empty()) { m_deferred.front().second(); m_deferred.pop_front(); }
    for (auto& [k, p] : m_pipelines) vkDestroyPipeline(m_device, p, nullptr);
    for (auto& [k, s] : m_samplers) vkDestroySampler(m_device, s, nullptr);
    for (Frame& f : m_frames) {
        for (Chunk& c : f.staging) vmaDestroyBuffer(m_vma, c.buffer, c.alloc);
        for (Chunk& c : f.uniforms) vmaDestroyBuffer(m_vma, c.buffer, c.alloc);
        for (VkDescriptorPool p : f.descPools) vkDestroyDescriptorPool(m_device, p, nullptr);
        vkDestroyCommandPool(m_device, f.pool, nullptr);
        vkDestroySemaphore(m_device, f.acquire, nullptr);
    }
    destroySwapchain();
    vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
    vkDestroyQueryPool(m_device, m_occlusionPool, nullptr);
    vkDestroySemaphore(m_device, m_timeline, nullptr);
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    vmaDestroyAllocator(m_vma);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (g_messenger) vkDestroyDebugUtilsMessengerEXT(m_instance, g_messenger, nullptr);
    g_messenger = VK_NULL_HANDLE;
    vkDestroyInstance(m_instance, nullptr);
}

// --- Submission -------------------------------------------------------------------

VkCommandBuffer VulkanDevice::cmd() { return m_cmd; }

void VulkanDevice::beginCommands() {
    Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
    if (f.cmdUsed == f.cmds.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer c;
        FZ_VK_INIT(vkAllocateCommandBuffers(m_device, &ai, &c));
        f.cmds.push_back(c);
    }
    m_cmd = f.cmds[f.cmdUsed++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    FZ_VK(vkBeginCommandBuffer(m_cmd, &bi));
    m_segment = Segment::None;
    m_rendering = false;
    m_boundPipeline = VK_NULL_HANDLE;
    m_dyn.valid = false;
}

std::uint64_t VulkanDevice::submit(VkSemaphore wait, VkSemaphore signal) {
    endRendering();
    FZ_VK(vkEndCommandBuffer(m_cmd));
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = m_cmd;
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = wait;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo sig[2]{};
    sig[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig[0].semaphore = m_timeline;
    sig[0].value = ++m_submitted;
    sig[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sig[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig[1].semaphore = signal;
    sig[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = wait ? 1u : 0u;
    si.pWaitSemaphoreInfos = &waitInfo;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = signal ? 2u : 1u;
    si.pSignalSemaphoreInfos = sig;
    FZ_VK(vkQueueSubmit2(m_queue, 1, &si, VK_NULL_HANDLE));
    m_frames[static_cast<std::size_t>(m_frameIndex)].lastValue = m_submitted;
    m_cmd = VK_NULL_HANDLE;
    return m_submitted;
}

std::uint64_t VulkanDevice::completedValue() const {
    std::uint64_t v = 0;
    vkGetSemaphoreCounterValue(m_device, m_timeline, &v);
    return v;
}

void VulkanDevice::waitValue(std::uint64_t value) {
    if (value == 0) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &value;
    FZ_VK(vkWaitSemaphores(m_device, &wi, UINT64_MAX));
    runDeferred();
}

void VulkanDevice::waitFor(std::uint64_t value) {
    if (value > m_submitted) {
        submit();
        beginCommands();
    }
    waitValue(value);
}

void VulkanDevice::nextFrame() {
    m_frameIndex ^= 1;
    Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
    waitValue(f.lastValue);
    vkResetCommandPool(m_device, f.pool, 0);
    f.cmdUsed = 0;
    for (VkDescriptorPool p : f.descPools) vkResetDescriptorPool(m_device, p, 0);
    f.descPoolUsed = 0;
    f.sets.clear();
    for (Chunk& c : f.staging) c.used = 0;
    for (Chunk& c : f.uniforms) c.used = 0;
    ++m_frameCounter;
    runDeferred();
    beginCommands();
}

void VulkanDevice::deferDelete(std::function<void()> f) {
    m_deferred.emplace_back(pendingValue(), std::move(f));
}

void VulkanDevice::runDeferred() {
    const std::uint64_t done = completedValue();
    while (!m_deferred.empty() && m_deferred.front().first <= done) {
        m_deferred.front().second();
        m_deferred.pop_front();
    }
}

void VulkanDevice::flush() {
    submit();
    beginCommands();
}

void VulkanDevice::finish() {
    const std::uint64_t v = submit();
    beginCommands();
    waitValue(v);
}

void VulkanDevice::resyncState() { m_dyn.valid = false; }

bool VulkanDevice::videoMemory(std::int64_t& usedKB, std::int64_t& totalKB) {
    if (!m_memoryBudget) return false;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(m_vma, budgets);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &mp);
    std::uint64_t used = 0, total = 0;
    for (std::uint32_t i = 0; i < mp.memoryHeapCount; ++i)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            used += budgets[i].usage;
            total += mp.memoryHeaps[i].size;
        }
    if (!total) return false;
    usedKB = static_cast<std::int64_t>(used / 1024);
    totalKB = static_cast<std::int64_t>(total / 1024);
    return true;
}

// --- Segments ---------------------------------------------------------------------

void VulkanDevice::fullBarrier() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(m_cmd, &di);
}

void VulkanDevice::enterSegment(Segment s) {
    if (s != Segment::Render) endRendering();
    // A run of transfers stays one segment; transferTouch puts a barrier
    // between two that touch the same resource.
    if (s == Segment::Transfer && m_segment == Segment::Transfer) return;
    fullBarrier();
    m_segment = s;
    m_transferBuffers.clear();
    m_transferImages.clear();
}

void VulkanDevice::transferTouch(VkBuffer b) {
    if (std::find(m_transferBuffers.begin(), m_transferBuffers.end(), b) != m_transferBuffers.end()) {
        fullBarrier();
        m_transferBuffers.clear();
        m_transferImages.clear();
    }
    m_transferBuffers.push_back(b);
}

void VulkanDevice::transferTouch(VkImage i) {
    if (std::find(m_transferImages.begin(), m_transferImages.end(), i) != m_transferImages.end()) {
        fullBarrier();
        m_transferBuffers.clear();
        m_transferImages.clear();
    }
    m_transferImages.push_back(i);
}

void VulkanDevice::endRendering() {
    if (!m_rendering) return;
    // An occlusion query cannot outlive the rendering it counts in: it ends
    // here, with what it has counted so far.
    if (VkQuery* q = m_queries.find(m_activeOcclusion.id)) {
        vkCmdEndQuery(m_cmd, m_occlusionPool, q->first);
        q->active = false;
        m_activeOcclusion = QueryHandle{};
    }
    vkCmdEndRendering(m_cmd);
    m_rendering = false;
}

const FramebufferDesc& VulkanDevice::currentDesc() const {
    if (const VkFb* f = m_framebuffers.find(m_fb.id)) return f->desc;
    return m_windowDesc;
}

void VulkanDevice::ensureRendering() {
    if (m_rendering) return;
    const FramebufferDesc& d = currentDesc();
    VkRenderingAttachmentInfo colors[kMaxColorAttachments]{};
    std::uint32_t count = 0;
    std::uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (int i = 0; i < kMaxColorAttachments; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        VkTex* t = m_textures.find(d.color[i].texture.id);
        if (!t) continue;
        colors[i].imageView = attachView(*t, d.color[i].level, d.color[i].layer);
        colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        count = static_cast<std::uint32_t>(i) + 1;
        w = std::min<std::uint32_t>(w, std::max(1, t->desc.width >> d.color[i].level));
        h = std::min<std::uint32_t>(h, std::max(1, t->desc.height >> d.color[i].level));
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkTex* dt = m_textures.find(d.depth.texture.id);
    if (dt) {
        depth.imageView = attachView(*dt, d.depth.level, d.depth.layer);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (dt->fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = depth;
        w = std::min<std::uint32_t>(w, std::max(1, dt->desc.width >> d.depth.level));
        h = std::min<std::uint32_t>(h, std::max(1, dt->desc.height >> d.depth.level));
    }
    if (count == 0 && !dt) return;   // nothing to draw into

    enterSegment(Segment::Render);
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = count;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = dt ? &depth : nullptr;
    ri.pStencilAttachment = (dt && (dt->fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) ? &stencil : nullptr;
    vkCmdBeginRendering(m_cmd, &ri);
    m_rendering = true;
    m_renderExtent = {w, h};
    m_dyn.valid = false;
    for (int i = 0; i < kMaxColorAttachments; ++i)
        if (VkTex* t = m_textures.find(d.color[i].texture.id)) t->lastWrite = pendingValue();
    if (dt) dt->lastWrite = pendingValue();
}

void VulkanDevice::bindFramebuffer(FramebufferHandle h) {
    if (!m_framebuffers.find(h.id)) h = FramebufferHandle{};
    if (h == m_fb) return;
    endRendering();
    m_fb = h;
}

void VulkanDevice::windowFramebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(m_window, &width, &height);
}

// --- Clears -----------------------------------------------------------------------

namespace {
VkClearRect clearRectOf(const RenderState& s, const Rect& scissor, VkExtent2D ext) {
    int x0 = 0, y0 = 0, x1 = static_cast<int>(ext.width), y1 = static_cast<int>(ext.height);
    if (s.raster.scissor) {
        x0 = std::max(x0, scissor.x);
        y0 = std::max(y0, scissor.y);
        x1 = std::min(x1, scissor.x + scissor.w);
        y1 = std::min(y1, scissor.y + scissor.h);
    }
    VkClearRect r{};
    r.rect.offset = {x0, y0};
    r.rect.extent = {static_cast<std::uint32_t>(std::max(0, x1 - x0)),
                     static_cast<std::uint32_t>(std::max(0, y1 - y0))};
    r.layerCount = 1;
    return r;
}
} // namespace

void VulkanDevice::clear(std::uint8_t mask) {
    ensureRendering();
    if (!m_rendering) return;
    const FramebufferDesc& d = currentDesc();
    VkClearAttachment atts[kMaxColorAttachments + 1];
    std::uint32_t n = 0;
    // As a draw would: the colour only where the mask lets it (all or
    // nothing here), the depth only while depth writes are on.
    const ColorMask& cm = m_state.colorMask;
    if ((mask & ClearColor) && (cm.r || cm.g || cm.b || cm.a)) {
        for (int i = 0; i < kMaxColorAttachments; ++i) {
            if (!m_textures.find(d.color[i].texture.id)) continue;
            VkClearAttachment& a = atts[n++];
            a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            a.colorAttachment = static_cast<std::uint32_t>(i);
            a.clearValue.color = {{m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a}};
        }
    }
    if (const VkTex* dt = m_textures.find(d.depth.texture.id)) {
        VkImageAspectFlags aspect = 0;
        if ((mask & ClearDepth) && m_state.depth.write) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((mask & ClearStencil) && (dt->fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
            m_state.stencil.writeMask)
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (aspect) {
            VkClearAttachment& a = atts[n++];
            a.aspectMask = aspect;
            a.clearValue.depthStencil = {m_clearDepth, static_cast<std::uint32_t>(m_clearStencil)};
        }
    }
    const VkClearRect r = clearRectOf(m_state, m_scissor, m_renderExtent);
    if (n && r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(m_cmd, n, atts, 1, &r);
}

void VulkanDevice::clearColorAttachment(int index, const glm::vec4& v) {
    ensureRendering();
    if (!m_rendering || index < 0 || index >= kMaxColorAttachments) return;
    if (!m_textures.find(currentDesc().color[index].texture.id)) return;
    VkClearAttachment a{};
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = static_cast<std::uint32_t>(index);
    a.clearValue.color = {{v.r, v.g, v.b, v.a}};
    const VkClearRect r = clearRectOf(m_state, m_scissor, m_renderExtent);
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(m_cmd, 1, &a, 1, &r);
}

// --- Draws ------------------------------------------------------------------------

struct VulkanDevice::PipelineKey {
    std::uint32_t program, topology, polygon, blend, colorMask;
    std::uint32_t srcC, dstC, srcA, dstA, opC, opA;
    std::uint32_t colorCount;
    VkFormat      colors[kMaxColorAttachments];
    VkFormat      depth, stencil;
    std::uint32_t inputCount;
    struct In { std::uint32_t location, format, stride, rate; } inputs[16];
};

namespace {
// The vertex input of the draw being prepared: per shader input, the buffer
// and how it is read.
struct Feed {
    VkBuffer      buffer;
    VkDeviceSize  offset;
    std::uint32_t location, stride;
    VkFormat      format;
    VkVertexInputRate rate;
};
thread_local std::vector<Feed> t_feeds;
} // namespace

VkPipeline VulkanDevice::graphicsPipeline(VkProg& p, Primitive prim) {
    PipelineKey k;
    std::memset(&k, 0, sizeof(k));
    k.program = m_program.id;
    k.topology = topologyOf(prim);
    k.polygon = (m_state.raster.wireframe && m_wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    const BlendState& b = m_state.blend;
    k.blend = b.enabled;
    if (b.enabled) {
        k.srcC = blendOf(b.srcColor); k.dstC = blendOf(b.dstColor);
        k.srcA = blendOf(b.srcAlpha); k.dstA = blendOf(b.dstAlpha);
        k.opC = blendOpOf(b.colorOp); k.opA = blendOpOf(b.alphaOp);
    }
    const ColorMask& m = m_state.colorMask;
    k.colorMask = (m.r ? std::uint32_t{VK_COLOR_COMPONENT_R_BIT} : 0u) |
                  (m.g ? std::uint32_t{VK_COLOR_COMPONENT_G_BIT} : 0u) |
                  (m.b ? std::uint32_t{VK_COLOR_COMPONENT_B_BIT} : 0u) |
                  (m.a ? std::uint32_t{VK_COLOR_COMPONENT_A_BIT} : 0u);
    const FramebufferDesc& d = currentDesc();
    for (int i = 0; i < kMaxColorAttachments; ++i)
        if (const VkTex* t = m_textures.find(d.color[i].texture.id)) {
            k.colors[i] = t->fmt.format;
            k.colorCount = static_cast<std::uint32_t>(i) + 1;
        }
    if (const VkTex* dt = m_textures.find(d.depth.texture.id)) {
        k.depth = dt->fmt.format;
        if (dt->fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) k.stencil = dt->fmt.format;
    }
    k.inputCount = static_cast<std::uint32_t>(std::min<std::size_t>(t_feeds.size(), 16));
    for (std::uint32_t i = 0; i < k.inputCount; ++i)
        k.inputs[i] = {t_feeds[i].location, static_cast<std::uint32_t>(t_feeds[i].format),
                       t_feeds[i].stride, static_cast<std::uint32_t>(t_feeds[i].rate)};

    const std::uint64_t hash = fnv(&k, sizeof(k));
    if (auto it = m_pipelines.find(hash); it != m_pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p.vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p.fs;

    VkVertexInputBindingDescription bind[16];
    VkVertexInputAttributeDescription attr[16];
    for (std::uint32_t i = 0; i < k.inputCount; ++i) {
        bind[i] = {i, k.inputs[i].stride, static_cast<VkVertexInputRate>(k.inputs[i].rate)};
        attr[i] = {k.inputs[i].location, i, static_cast<VkFormat>(k.inputs[i].format), 0};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.inputCount;
    vi.pVertexBindingDescriptions = bind;
    vi.vertexAttributeDescriptionCount = k.inputCount;
    vi.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = static_cast<VkPrimitiveTopology>(k.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = static_cast<VkPolygonMode>(k.polygon);
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[kMaxColorAttachments]{};
    for (std::uint32_t i = 0; i < k.colorCount; ++i) {
        cb[i].blendEnable = k.blend;
        cb[i].srcColorBlendFactor = static_cast<VkBlendFactor>(k.srcC);
        cb[i].dstColorBlendFactor = static_cast<VkBlendFactor>(k.dstC);
        cb[i].colorBlendOp = static_cast<VkBlendOp>(k.opC);
        cb[i].srcAlphaBlendFactor = static_cast<VkBlendFactor>(k.srcA);
        cb[i].dstAlphaBlendFactor = static_cast<VkBlendFactor>(k.dstA);
        cb[i].alphaBlendOp = static_cast<VkBlendOp>(k.opA);
        cb[i].colorWriteMask = k.colorMask;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.colorCount;
    cbs.pAttachments = cb;
    const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<std::uint32_t>(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = k.colorCount;
    rci.pColorAttachmentFormats = k.colors;
    rci.depthAttachmentFormat = k.depth;
    rci.stencilAttachmentFormat = k.stencil;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy;
    ci.layout = p.layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    FZ_VK(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &pipe));
    m_pipelines[hash] = pipe;
    m_pipelineOwner[hash] = m_program.id;
    return pipe;
}

void VulkanDevice::setDynamicState() {
    const RenderState& s = m_state;
    const bool all = !m_dyn.valid;
    const RenderState& o = m_dyn.state;

    VkViewport vpt{};
    vpt.x = static_cast<float>(m_viewport.x);
    vpt.y = static_cast<float>(m_viewport.y);
    vpt.width = static_cast<float>(std::max(m_viewport.w, 1));
    vpt.height = static_cast<float>(std::max(m_viewport.h, 1));
    vpt.minDepth = 0.0f;
    vpt.maxDepth = 1.0f;
    if (all || std::memcmp(&vpt, &m_dyn.viewport, sizeof(vpt)) != 0) {
        vkCmdSetViewport(m_cmd, 0, 1, &vpt);
        m_dyn.viewport = vpt;
    }
    VkRect2D sc{};
    if (s.raster.scissor) {
        const int x0 = std::max(0, m_scissor.x), y0 = std::max(0, m_scissor.y);
        const int x1 = std::max(x0, m_scissor.x + m_scissor.w);
        const int y1 = std::max(y0, m_scissor.y + m_scissor.h);
        sc = {{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    } else {
        sc = {{0, 0}, m_renderExtent};
    }
    if (all || std::memcmp(&sc, &m_dyn.scissor, sizeof(sc)) != 0) {
        vkCmdSetScissor(m_cmd, 0, 1, &sc);
        m_dyn.scissor = sc;
    }
    if (all || s.raster.cull != o.raster.cull || s.raster.cullFace != o.raster.cullFace) {
        VkCullModeFlags cm = VK_CULL_MODE_NONE;
        if (s.raster.cull)
            cm = s.raster.cullFace == CullMode::Back  ? VkCullModeFlags(VK_CULL_MODE_BACK_BIT)
               : s.raster.cullFace == CullMode::Front ? VkCullModeFlags(VK_CULL_MODE_FRONT_BIT)
                                                      : VkCullModeFlags(VK_CULL_MODE_FRONT_AND_BACK);
        vkCmdSetCullMode(m_cmd, cm);
    }
    if (all) {
        // OpenGL's default front face is counter-clockwise with y up the
        // window. The picture's rows are stored the same way round here, but
        // Vulkan reckons the winding with y running down them -- so the same
        // triangle turns the other way: clockwise, in Vulkan's words.
        vkCmdSetFrontFace(m_cmd, VK_FRONT_FACE_CLOCKWISE);
        const float blend[4] = {0, 0, 0, 0};
        vkCmdSetBlendConstants(m_cmd, blend);
    }
    if (all || s.depth.test != o.depth.test) vkCmdSetDepthTestEnable(m_cmd, s.depth.test);
    if (all || s.depth.write != o.depth.write) vkCmdSetDepthWriteEnable(m_cmd, s.depth.write);
    if (all || s.depth.func != o.depth.func) vkCmdSetDepthCompareOp(m_cmd, compareOf(s.depth.func));
    if (all || s.stencil.test != o.stencil.test) vkCmdSetStencilTestEnable(m_cmd, s.stencil.test);
    if (all || s.stencil.fail != o.stencil.fail || s.stencil.depthFail != o.stencil.depthFail ||
        s.stencil.pass != o.stencil.pass || s.stencil.func != o.stencil.func)
        vkCmdSetStencilOp(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilOf(s.stencil.fail),
                          stencilOf(s.stencil.pass), stencilOf(s.stencil.depthFail),
                          compareOf(s.stencil.func));
    if (all || s.stencil.readMask != o.stencil.readMask)
        vkCmdSetStencilCompareMask(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil.readMask);
    if (all || s.stencil.writeMask != o.stencil.writeMask)
        vkCmdSetStencilWriteMask(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil.writeMask);
    if (all || s.stencil.ref != o.stencil.ref)
        vkCmdSetStencilReference(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                 static_cast<std::uint32_t>(s.stencil.ref));
    if (all || s.raster.polygonOffset != o.raster.polygonOffset)
        vkCmdSetDepthBiasEnable(m_cmd, s.raster.polygonOffset);
    if (all || s.raster.offsetFactor != o.raster.offsetFactor ||
        s.raster.offsetUnits != o.raster.offsetUnits)
        vkCmdSetDepthBias(m_cmd, s.raster.offsetUnits, 0.0f, s.raster.offsetFactor);
    m_dyn.state = s;
    m_dyn.valid = true;
}

VkTex* VulkanDevice::textureForUnit(int unit, int kind) {
    if (unit < 0 || unit >= static_cast<int>(m_units.size()))
        return m_textures.find(m_dummy[static_cast<std::size_t>(kind)].id);
    if (VkTex* t = m_textures.find(m_units[static_cast<std::size_t>(unit)].byKind[static_cast<std::size_t>(kind)].id))
        return t;
    return m_textures.find(m_dummy[static_cast<std::size_t>(kind)].id);
}

VkDescriptorPool VulkanDevice::descriptorPool() {
    Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
    if (f.descPoolUsed < f.descPools.size()) return f.descPools[f.descPoolUsed];
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2048},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
    };
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 2048;
    ci.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
    ci.pPoolSizes = sizes;
    VkDescriptorPool pool;
    FZ_VK(vkCreateDescriptorPool(m_device, &ci, nullptr, &pool));
    f.descPools.push_back(pool);
    return pool;
}

void VulkanDevice::uploadUniforms(VkProg& p) {
    if (p.uboBinding == UINT32_MAX) return;
    if (p.clipMask >= 0) {
        const std::int32_t mask = m_state.raster.clipDistances;
        std::uint8_t* at = p.uniforms.data() + p.locs[static_cast<std::size_t>(p.clipMask)].offset;
        if (std::memcmp(at, &mask, 4) != 0) {
            std::memcpy(at, &mask, 4);
            p.dirty = true;
        }
    }
    if (!p.dirty && p.uboFrame == m_frameCounter && p.uboBuffer) return;
    const Staging s = uniformRing(p.uniforms.size());
    std::memcpy(s.ptr, p.uniforms.data(), p.uniforms.size());
    p.uboBuffer = s.buffer;
    p.uboOffset = static_cast<std::uint32_t>(s.offset);
    p.uboFrame = m_frameCounter;
    p.dirty = false;
}

void VulkanDevice::bindDescriptors(VkProg& p, VkPipelineBindPoint point) {
    // The set is cached per frame by everything it holds: a run of draws with
    // the same textures (the common case -- only the uniforms change, and they
    // ride on the dynamic offset) binds one set.
    thread_local std::vector<std::uint64_t> key;
    key.clear();
    key.push_back(m_program.id);
    key.push_back(reinterpret_cast<std::uint64_t>(p.uboBuffer));
    struct Img { VkTex* tex; VkSampler sampler; };
    thread_local std::vector<Img> imgs;
    imgs.clear();
    for (const SamplerSlot& s : p.samplers)
        for (std::uint32_t e = 0; e < s.count; ++e) {
            const int unit = s.units[e];
            VkTex* t = textureForUnit(unit, s.kind);
            const VkSampler smp = (unit == 0 && m_unit0Sampler) ? sampler(*m_unit0Sampler)
                                                                : sampler(t->desc.sampler);
            imgs.push_back({t, smp});
            key.push_back(reinterpret_cast<std::uint64_t>(t->view));
            key.push_back(reinterpret_cast<std::uint64_t>(smp));
        }
    for (const ResourceSlot& r : p.resources) {
        if (r.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const ImageUnit& u = m_imageUnits[std::min<std::size_t>(r.binding, m_imageUnits.size() - 1)];
            VkTex* t = m_textures.find(u.tex.id);
            key.push_back(t ? reinterpret_cast<std::uint64_t>(storageView(*t, u.level)) : 0);
        } else {
            const auto& table = r.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? m_storageBuffers
                                                                            : m_uniformBuffers;
            const VkBuf* b = m_buffers.find(table[std::min<std::size_t>(r.binding, table.size() - 1)].id);
            key.push_back(b ? reinterpret_cast<std::uint64_t>(b->buffer) : 0);
        }
    }
    Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
    const std::uint64_t hash = fnv(key.data(), key.size() * sizeof(std::uint64_t));
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (auto it = f.sets.find(hash); it != f.sets.end()) {
        set = it->second;
    } else {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptorPool();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p.setLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) {
            ++f.descPoolUsed;   // full: the next pool
            ai.descriptorPool = descriptorPool();
            FZ_VK(vkAllocateDescriptorSets(m_device, &ai, &set));
        }
        thread_local std::vector<VkWriteDescriptorSet> writes;
        thread_local std::vector<VkDescriptorImageInfo> images;
        thread_local std::vector<VkDescriptorBufferInfo> buffers;
        writes.clear();
        images.clear();
        buffers.clear();
        images.reserve(imgs.size() + p.resources.size());
        buffers.reserve(p.resources.size() + 1);
        if (p.uboBinding != UINT32_MAX) {
            buffers.push_back({p.uboBuffer, 0, p.uniforms.size()});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = p.uboBinding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w.pBufferInfo = &buffers.back();
            writes.push_back(w);
        }
        std::size_t i = 0;
        for (const SamplerSlot& s : p.samplers) {
            const std::size_t first = images.size();
            for (std::uint32_t e = 0; e < s.count; ++e, ++i)
                images.push_back({imgs[i].sampler, imgs[i].tex->view, VK_IMAGE_LAYOUT_GENERAL});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = s.binding;
            w.descriptorCount = s.count;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[first];
            writes.push_back(w);
        }
        for (const ResourceSlot& r : p.resources) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = r.binding;
            w.descriptorCount = 1;
            w.descriptorType = r.type;
            if (r.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                const ImageUnit& u = m_imageUnits[std::min<std::size_t>(r.binding, m_imageUnits.size() - 1)];
                VkTex* t = m_textures.find(u.tex.id);
                if (!t) continue;
                images.push_back({VK_NULL_HANDLE, storageView(*t, u.level), VK_IMAGE_LAYOUT_GENERAL});
                w.pImageInfo = &images.back();
            } else {
                const auto& table = r.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? m_storageBuffers
                                                                                : m_uniformBuffers;
                const VkBuf* b = m_buffers.find(table[std::min<std::size_t>(r.binding, table.size() - 1)].id);
                if (!b) b = m_buffers.find(m_defaultAttrib.id);
                buffers.push_back({b->buffer, 0, VK_WHOLE_SIZE});
                w.pBufferInfo = &buffers.back();
            }
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
        f.sets[hash] = set;
    }
    const std::uint32_t dynamicOffset = p.uboOffset;
    vkCmdBindDescriptorSets(m_cmd, point, p.layout, 0, 1, &set,
                            p.uboBinding != UINT32_MAX ? 1u : 0u, &dynamicOffset);
}

void VulkanDevice::bindVertexInput(VkProg& p) {
    // Per input the shader has: the attribute that feeds it, the array's
    // constant for it, or (as OpenGL would read a disabled attribute) the
    // default (0,0,0,1).
    t_feeds.clear();
    const VkVao* va = m_vertexArrays.find(m_vao.id);
    const VkBuf* def = m_buffers.find(m_defaultAttrib.id);
    for (const VertexInput& in : p.inputs) {
        Feed f{};
        f.location = in.location;
        bool fed = false;
        if (va) {
            for (const VertexAttribute& a : va->attrs) {
                if (a.location != in.location) continue;
                const VkBuf* b = m_buffers.find(a.buffer.id);
                if (!b) break;
                f.buffer = b->buffer;
                f.offset = a.offset;
                f.format = vertexFormatOf(a.format);
                f.stride = a.stride ? a.stride : vertexSizeOf(a.format);
                f.rate = a.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
                fed = true;
                break;
            }
            if (!fed)
                for (std::size_t c = 0; c < va->consts.size(); ++c) {
                    if (va->consts[c].location != in.location) continue;
                    const VkBuf* b = m_buffers.find(va->constBuffer.id);
                    if (!b) break;
                    f.buffer = b->buffer;
                    f.offset = c * 16;
                    f.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                    f.stride = 0;
                    f.rate = VK_VERTEX_INPUT_RATE_VERTEX;
                    fed = true;
                    break;
                }
        }
        if (!fed) {
            f.buffer = def->buffer;
            f.offset = in.base == 0 ? 0 : 16;
            f.format = in.base == 0 ? VK_FORMAT_R32G32B32A32_SFLOAT
                     : in.base == 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
            f.stride = 0;
            f.rate = VK_VERTEX_INPUT_RATE_VERTEX;
        }
        t_feeds.push_back(f);
    }
}

void VulkanDevice::prepareDraw(VkProg& p, Primitive prim, VkPipelineBindPoint point) {
    (void)point;
    bindVertexInput(p);
    const VkPipeline pipe = graphicsPipeline(p, prim);
    if (pipe != m_boundPipeline) {
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        m_boundPipeline = pipe;
    }
    setDynamicState();
    if (!t_feeds.empty()) {
        VkBuffer bufs[16];
        VkDeviceSize offs[16];
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(t_feeds.size(), 16));
        for (std::uint32_t i = 0; i < n; ++i) { bufs[i] = t_feeds[i].buffer; offs[i] = t_feeds[i].offset; }
        vkCmdBindVertexBuffers(m_cmd, 0, n, bufs, offs);
    }
    uploadUniforms(p);
    bindDescriptors(p, VK_PIPELINE_BIND_POINT_GRAPHICS);
}

void VulkanDevice::draw(Primitive prim, int first, int count, int instances) {
    if (count <= 0 || instances <= 0 || m_viewport.w <= 0 || m_viewport.h <= 0) return;
    VkProg* p = m_programs.find(m_program.id);
    if (!p || p->compute) return;
    ensureRendering();
    if (!m_rendering) return;
    prepareDraw(*p, prim, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdDraw(m_cmd, static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(instances),
              static_cast<std::uint32_t>(first), 0);
}

void VulkanDevice::drawIndexed(Primitive prim, int count, IndexType type, std::size_t byteOffset,
                               int instances, int baseVertex) {
    if (count <= 0 || instances <= 0 || m_viewport.w <= 0 || m_viewport.h <= 0) return;
    VkProg* p = m_programs.find(m_program.id);
    const VkVao* va = m_vertexArrays.find(m_vao.id);
    const VkBuf* ib = va ? m_buffers.find(va->index.id) : nullptr;
    if (!p || p->compute || !ib) return;
    ensureRendering();
    if (!m_rendering) return;
    prepareDraw(*p, prim, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdBindIndexBuffer(m_cmd, ib->buffer, byteOffset,
                         type == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(m_cmd, static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(instances),
                     0, baseVertex, 0);
}

void VulkanDevice::dispatch(int x, int y, int z) {
    VkProg* p = m_programs.find(m_program.id);
    if (!p || !p->compute) return;
    endRendering();
    m_segment = Segment::None;   // every dispatch gets its barrier
    enterSegment(Segment::Compute);
    if (!p->computePipeline) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = p->cs;
        ci.stage.pName = "main";
        ci.layout = p->layout;
        FZ_VK(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &p->computePipeline));
    }
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->computePipeline);
    m_boundPipeline = VK_NULL_HANDLE;
    uploadUniforms(*p);
    bindDescriptors(*p, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdDispatch(m_cmd, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(z));
    for (const ImageUnit& u : m_imageUnits)
        if (VkTex* t = m_textures.find(u.tex.id)) t->lastWrite = pendingValue();
}

void VulkanDevice::memoryBarrier(std::uint32_t) {
    // A barrier needs to be outside a rendering scope; inside one, the next
    // scope's own barrier does the job.
    if (!m_rendering) fullBarrier();
}

// --- Bindings ---------------------------------------------------------------------

namespace {
int kindOf(TextureType t) {
    switch (t) {
        case TextureType::Tex2DArray: return 1;
        case TextureType::Tex3D:      return 2;
        case TextureType::Cube:       return 3;
        case TextureType::Tex2D:      break;
    }
    return 0;
}
} // namespace

void VulkanDevice::bindTexture(int unit, TextureHandle h) {
    if (unit < 0 || unit >= static_cast<int>(m_units.size())) return;
    Unit& u = m_units[static_cast<std::size_t>(unit)];
    const VkTex* t = m_textures.find(h.id);
    if (!t) {
        u = Unit{};
        return;
    }
    u.last = h;
    u.byKind[static_cast<std::size_t>(kindOf(t->desc.type))] = h;
}

TextureHandle VulkanDevice::boundTexture(int unit) const {
    if (unit < 0 || unit >= static_cast<int>(m_units.size())) return {};
    return m_units[static_cast<std::size_t>(unit)].last;
}

void VulkanDevice::bindImage(int unit, TextureHandle h, int level, Access, Format) {
    if (unit < 0 || unit >= static_cast<int>(m_imageUnits.size())) return;
    m_imageUnits[static_cast<std::size_t>(unit)] = ImageUnit{h, level};
}

void VulkanDevice::bindBufferBase(BufferKind kind, int index, BufferHandle h) {
    if (index < 0 || index >= 16) return;
    if (kind == BufferKind::Storage) m_storageBuffers[static_cast<std::size_t>(index)] = h;
    else if (kind == BufferKind::Uniform) m_uniformBuffers[static_cast<std::size_t>(index)] = h;
}

// --- Frame ------------------------------------------------------------------------

void VulkanDevice::setVsync(bool on, bool adaptive) {
    if (on == m_vsync && adaptive == m_adaptive) return;
    m_vsync = on;
    m_adaptive = adaptive;
    m_swapDirty = true;
}

void VulkanDevice::present() {
    endRendering();
    // The frame's work first, without waiting for anything: the GPU starts on
    // it while the swapchain is still being asked for an image.
    submit();
    beginCommands();

    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    if (w > 0 && h > 0) {
        if (m_swapDirty || !m_swapchain || m_swapExtent.width != static_cast<std::uint32_t>(w) ||
            m_swapExtent.height != static_cast<std::uint32_t>(h))
            createSwapchain();
        Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
        std::uint32_t index = 0;
        VkResult r = m_swapchain ? vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, f.acquire,
                                                         VK_NULL_HANDLE, &index)
                                 : VK_ERROR_OUT_OF_DATE_KHR;
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            const VkImage dst = m_swapImages[index];
            VkTex* back = m_textures.find(m_back[m_backIndex].id);
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.srcAccessMask = 0;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.image = dst;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(m_cmd, &di);
            // Flipped on the way: row 0 of the picture is its bottom, as in
            // OpenGL, and row 0 of the swapchain image is the top of the screen.
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {m_backW, m_backH, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {0, static_cast<std::int32_t>(m_swapExtent.height), 0};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(m_swapExtent.width), 0, 1};
            const bool same = m_backW == static_cast<int>(m_swapExtent.width) &&
                              m_backH == static_cast<int>(m_swapExtent.height);
            vkCmdBlitImage(m_cmd, back->image, VK_IMAGE_LAYOUT_GENERAL, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.dstAccessMask = 0;
            ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            di.memoryBarrierCount = 0;
            vkCmdPipelineBarrier2(m_cmd, &di);
            submit(f.acquire, m_renderDone[index]);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &m_renderDone[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &m_swapchain;
            pi.pImageIndices = &index;
            const VkResult pr = vkQueuePresentKHR(m_queue, &pi);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapDirty = true;
        } else {
            m_swapDirty = true;
            submit();
        }
    } else {
        submit();
    }

    // The two pictures take turns: what was just shown is the front buffer.
    m_backIndex ^= 1;
    if (w > 0 && h > 0 && (w != m_backW || h != m_backH)) {
        createBackbuffers(w, h);
    } else {
        m_windowDesc.color[0].texture = m_back[m_backIndex];
    }
    nextFrame();
}

// --- Queries and fences -----------------------------------------------------------

QueryHandle VulkanDevice::createQuery(QueryType type) {
    VkQuery q;
    q.type = type;
    std::vector<std::uint32_t>& free = type == QueryType::AnySamplesPassed ? m_freeOcclusion
                                                                          : m_freeTimestamps;
    const std::uint32_t need = type == QueryType::TimeElapsed ? 2u : 1u;
    if (free.size() < need) return {};
    if (need == 2) {
        // Two neighbours, so one call reads both.
        std::sort(free.begin(), free.end(), std::greater<>());
        for (std::size_t i = free.size(); i-- > 1;)
            if (free[i] + 1 == free[i - 1]) {
                q.first = free[i];
                free.erase(free.begin() + static_cast<std::ptrdiff_t>(i - 1), free.begin() + static_cast<std::ptrdiff_t>(i + 1));
                break;
            }
    } else {
        q.first = free.back();
        free.pop_back();
    }
    vkResetQueryPool(m_device, type == QueryType::AnySamplesPassed ? m_occlusionPool : m_timestampPool,
                     q.first, need);
    return QueryHandle{m_queries.add(q)};
}

void VulkanDevice::destroy(QueryHandle h) {
    VkQuery* q = m_queries.find(h.id);
    if (!q) return;
    const VkQuery copy = *q;
    m_queries.remove(h.id);
    if (m_activeOcclusion == h) m_activeOcclusion = QueryHandle{};
    deferDelete([this, copy] {
        if (copy.type == QueryType::AnySamplesPassed) {
            m_freeOcclusion.push_back(copy.first);
        } else {
            m_freeTimestamps.push_back(copy.first);
            if (copy.type == QueryType::TimeElapsed) m_freeTimestamps.push_back(copy.first + 1);
        }
    });
}

namespace {
// Before a query is written again, its last results must be in (a pool slot is
// reset on the host, which must not happen while a submission still uses it).
} // namespace

void VulkanDevice::beginQuery(QueryHandle h) {
    VkQuery* q = m_queries.find(h.id);
    if (!q) return;
    if (q->serial && q->serial > completedValue()) waitFor(q->serial);
    if (q->type == QueryType::AnySamplesPassed) {
        vkResetQueryPool(m_device, m_occlusionPool, q->first, 1);
        ensureRendering();
        if (!m_rendering) return;
        vkCmdBeginQuery(m_cmd, m_occlusionPool, q->first, 0);
        m_activeOcclusion = h;
    } else {
        vkResetQueryPool(m_device, m_timestampPool, q->first, 2);
        vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, q->first);
    }
    q->active = true;
    q->serial = pendingValue();
}

void VulkanDevice::endQuery(QueryHandle h) {
    VkQuery* q = m_queries.find(h.id);
    if (!q || !q->active) return;
    if (q->type == QueryType::AnySamplesPassed) {
        if (m_rendering && m_activeOcclusion == h) vkCmdEndQuery(m_cmd, m_occlusionPool, q->first);
        if (m_activeOcclusion == h) m_activeOcclusion = QueryHandle{};
    } else {
        vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, q->first + 1);
    }
    q->active = false;
}

void VulkanDevice::timestamp(QueryHandle h) {
    VkQuery* q = m_queries.find(h.id);
    if (!q) return;
    if (q->serial && q->serial > completedValue()) waitFor(q->serial);
    vkResetQueryPool(m_device, m_timestampPool, q->first, 1);
    vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, q->first);
    q->serial = pendingValue();
}

bool VulkanDevice::queryReady(QueryHandle h) {
    VkQuery* q = m_queries.find(h.id);
    if (!q || !q->serial || q->serial > m_submitted) return false;
    return q->serial <= completedValue();
}

std::uint64_t VulkanDevice::queryResult(QueryHandle h) {
    VkQuery* q = m_queries.find(h.id);
    if (!q || !q->serial) return 0;
    waitFor(q->serial);
    const bool occ = q->type == QueryType::AnySamplesPassed;
    const std::uint32_t n = q->type == QueryType::TimeElapsed ? 2u : 1u;
    std::uint64_t v[2] = {0, 0};
    vkGetQueryPoolResults(m_device, occ ? m_occlusionPool : m_timestampPool, q->first, n, sizeof(v), v,
                          sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (occ) return v[0] ? 1u : 0u;
    const double period = m_props.limits.timestampPeriod;
    const std::uint64_t mask = m_caps.timestampBits >= 64 ? ~0ull
                                                          : ((1ull << m_caps.timestampBits) - 1ull);
    if (n == 2) return static_cast<std::uint64_t>(static_cast<double>((v[1] - v[0]) & mask) * period);
    return static_cast<std::uint64_t>(static_cast<double>(v[0] & mask) * period);
}

FenceHandle VulkanDevice::createFence() {
    return FenceHandle{m_fences.add(VkFenceH{pendingValue()})};
}

void VulkanDevice::destroy(FenceHandle h) { m_fences.remove(h.id); }

bool VulkanDevice::fenceSignaled(FenceHandle h, std::uint64_t timeoutNs) {
    const VkFenceH* f = m_fences.find(h.id);
    if (!f) return true;
    if (f->value > m_submitted) {
        if (timeoutNs == 0) return false;
        flush();
    }
    if (timeoutNs == 0) return completedValue() >= f->value;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &f->value;
    return vkWaitSemaphores(m_device, &wi, timeoutNs) == VK_SUCCESS;
}

// --- Dear ImGui -------------------------------------------------------------------

void VulkanDevice::imguiInit(GLFWwindow* window) {
    ImGui_ImplGlfw_InitForVulkan(window, true);
    m_imgui = std::make_unique<ImGuiRenderer>(*this);
}

void VulkanDevice::imguiShutdown() {
    m_imgui.reset();
    ImGui_ImplGlfw_Shutdown();
}

void VulkanDevice::imguiNewFrame() {}

void VulkanDevice::imguiRender(ImDrawData* data) {
    if (m_imgui) m_imgui->render(data);
}

Device::ImGuiCallback VulkanDevice::imguiTextureSamplerCallback() const {
    return ImGuiRenderer::useTextureSampling;
}

} // namespace fitzel::rhi::vk
