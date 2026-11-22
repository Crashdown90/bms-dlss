// Adaptivni FSR Frame Generation (AFG) - obalka forku FSR 3.1.4 frame interpolation + optical flow (src\helper\afg\ffx)
// s libovolnym casem mezisnimku. Rozhrani a konvence viz afg.h. Struktura volani odpovida providerovi FG v ffx-api
// SDK 1.1.4 (ffx_provider_framegeneration.cpp): sdileny backend (dilatovane MV / hloubka, vystupy optical flow) +
// backend pro optical flow a frame interpolation; rozdil: Dispatch je rozdeleny na par (AfgRecordPair) a mezisnimek
// (AfgRecordInterp), zdroj interpolace minuleho snimku drzi AFG ve svych obrazech (stridani dvou obrazu).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "afg.h"

#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

enum SharedId { kOfVector, kOfScd, kDilDepth, kDilMv, kRecPrevDepth, kSharedCount };

struct OwnImg {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool init = false;  // false = layout UNDEFINED (jeste nepouzity), true = SHADER_READ_ONLY_OPTIMAL
};

struct State {
    AfgDevice dev;
    AfgCreateInfo ci;
    // funkce Vulkanu (zarizeni + instance)
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    VkPhysicalDeviceMemoryProperties memProps = {};
    // FFX
    std::vector<uint8_t> scratchShared, scratchFi;
    FfxInterface beShared = {}, beFi = {};
    FfxUInt32 sharedCtx = 0;
    bool sharedCtxOk = false;
    FfxResourceInternal shared[kSharedCount] = {};
    bool sharedOk[kSharedCount] = {};
    FfxOpticalflowContext of = {};
    bool ofOk = false;
    FfxFrameInterpolationContext fi = {};
    bool fiOk = false;
    FfxUInt32 fiCtx = 0;
    // vlastni obrazy: zdroj interpolace (HUD-less, jinak backbuffer) stridave aktualni / minuly; backbuffer s HUD
    OwnImg src[2], pres;
    int cur = 0;
    bool havePair = false;  // aspon jeden par od vytvoreni (zdrojove obrazy maji platny layout / obsah)
    bool pairOk = false;    // posledni AfgRecordPair uspel (popis dd plati pro mezisnimky)
    bool pairHudless = false;
    uint32_t interpsInPair = 0;
    FfxFrameInterpolationDispatchDescription dd = {};  // popis posledniho paru pro mezisnimky
    size_t vram = 0;
    std::string err;
};

State* g = nullptr;

template <class F>
bool LoadDev(State& s, F* fn, const char* name) {
    *fn = (F)s.dev.getDeviceProcAddr(s.dev.device, name);
    return *fn != nullptr;
}

bool Fail(std::string* err, const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (g) g->err = b;
    if (err) *err = b;
    return false;
}

bool SetErr(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (g) g->err = b;
    return false;
}

uint32_t MemType(const State& s, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < s.memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (s.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool MakeImg(State& s, OwnImg& o, VkFormat fmt, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (s.CreateImage(s.dev.device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr = {};
    s.GetImageMemoryRequirements(s.dev.device, o.img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = MemType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = MemType(s, mr.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || s.AllocateMemory(s.dev.device, &ai, nullptr, &o.mem) != VK_SUCCESS)
        return false;
    if (s.BindImageMemory(s.dev.device, o.img, o.mem, 0) != VK_SUCCESS) return false;
    o.size = mr.size;
    o.init = false;
    return true;
}

void FreeImg(State& s, OwnImg& o) {
    if (o.img) s.DestroyImage(s.dev.device, o.img, nullptr);
    if (o.mem) s.FreeMemory(s.dev.device, o.mem, nullptr);
    o = OwnImg();
}

// Popis obrazu pro FFX. state = stav (layout), ve kterem obraz pri volani je a do ktereho ho FFX vrati:
// COMPUTE_READ = SHADER_READ_ONLY_OPTIMAL, UNORDERED_ACCESS = GENERAL.
FfxResource Res(VkImage img, VkFormat fmt, uint32_t w, uint32_t h, uint32_t usage, FfxResourceStates state) {
    FfxResource r = {};
    r.resource = (void*)img;
    r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxGetSurfaceFormatVK(fmt);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_RESOURCE_FLAGS_NONE;
    r.description.usage = (FfxResourceUsage)usage;
    r.state = state;
    return r;
}

bool IsDepthFmt(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
bool HasStencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

VkImageMemoryBarrier Ib(VkImage img, VkAccessFlags sa, VkAccessFlags da, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void ReleaseAll(State& s) {
    if (s.fiOk) ffxFrameInterpolationContextDestroy(&s.fi);
    s.fiOk = false;
    if (s.ofOk) ffxOpticalflowContextDestroy(&s.of);
    s.ofOk = false;
    for (int i = 0; i < kSharedCount; ++i)
        if (s.sharedOk[i]) s.beShared.fpDestroyResource(&s.beShared, s.shared[i], s.sharedCtx);
    memset(s.sharedOk, 0, sizeof s.sharedOk);
    if (s.sharedCtxOk) s.beShared.fpDestroyBackendContext(&s.beShared, s.sharedCtx);
    s.sharedCtxOk = false;
    if (s.CreateImage) {
        FreeImg(s, s.src[0]);
        FreeImg(s, s.src[1]);
        FreeImg(s, s.pres);
    }
}

FfxResource SharedRes(State& s, SharedId id) { return s.beShared.fpGetResource(&s.beShared, s.shared[id]); }

}  // namespace

bool AfgCreate(const AfgDevice& dev, const AfgCreateInfo& ci, std::string* err) {
    if (g) return Fail(err, "AFG uz je vytvoreny");
    if (!dev.instance || !dev.physicalDevice || !dev.device || !dev.getInstanceProcAddr || !dev.getDeviceProcAddr)
        return Fail(err, "neuplne zarizeni Vulkanu");
    if (!ci.displayW || !ci.displayH || !ci.maxRenderW || !ci.maxRenderH) return Fail(err, "nulova velikost");
    if (ci.colorFormat != VK_FORMAT_B8G8R8A8_UNORM && ci.colorFormat != VK_FORMAT_R8G8B8A8_UNORM)
        return Fail(err, "format backbufferu %d neni B8G8R8A8_UNORM / R8G8B8A8_UNORM", (int)ci.colorFormat);
    g = new State();
    State& s = *g;
    s.dev = dev;
    s.ci = ci;
    auto bail = [&](const char* fmt, auto... a) {
        char b[512];
        snprintf(b, sizeof b, fmt, a...);
        std::string msg = b;
        ReleaseAll(s);
        delete g;
        g = nullptr;
        if (err) *err = msg;
        return false;
    };
    bool ok = LoadDev(s, &s.CreateImage, "vkCreateImage") && LoadDev(s, &s.DestroyImage, "vkDestroyImage") &&
              LoadDev(s, &s.GetImageMemoryRequirements, "vkGetImageMemoryRequirements") &&
              LoadDev(s, &s.AllocateMemory, "vkAllocateMemory") && LoadDev(s, &s.FreeMemory, "vkFreeMemory") &&
              LoadDev(s, &s.BindImageMemory, "vkBindImageMemory") &&
              LoadDev(s, &s.CmdPipelineBarrier, "vkCmdPipelineBarrier") &&
              LoadDev(s, &s.CmdCopyImage, "vkCmdCopyImage");
    s.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)dev.getInstanceProcAddr(
        dev.instance, "vkGetPhysicalDeviceMemoryProperties");
    s.GetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)dev.getInstanceProcAddr(
        dev.instance, "vkGetPhysicalDeviceFormatProperties");
    if (!ok || !s.GetPhysicalDeviceMemoryProperties || !s.GetPhysicalDeviceFormatProperties)
        return bail("chybi funkce Vulkanu");
    s.GetPhysicalDeviceMemoryProperties(dev.physicalDevice, &s.memProps);
    {
        VkFormatProperties fp = {};
        s.GetPhysicalDeviceFormatProperties(dev.physicalDevice, ci.colorFormat, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need)
            return bail("format backbufferu %d neumi SAMPLED + STORAGE + TRANSFER", (int)ci.colorFormat);
    }
    if (!ffxVkAfgSetInstanceProcAddr(dev.instance, dev.getInstanceProcAddr))
        return bail("chybi funkce instance Vulkanu 1.1 (vkGetPhysicalDeviceProperties2 / Features2)");

    VkDeviceContext vdc = {dev.device, dev.physicalDevice, dev.getDeviceProcAddr};
    const FfxDevice fdev = ffxGetDeviceVK(&vdc);
    // sdileny backend (1 kontext: dilatovane MV / hloubka, optical flow) a backend efektu (optical flow + FI)
    const size_t szShared = ffxGetScratchMemorySizeVK(dev.physicalDevice, 1);
    const size_t szFi = ffxGetScratchMemorySizeVK(dev.physicalDevice, 2);
    s.scratchShared.assign(szShared, 0);
    s.scratchFi.assign(szFi, 0);
    FfxErrorCode e = ffxGetInterfaceVK(&s.beShared, fdev, s.scratchShared.data(), szShared, 1);
    if (e != FFX_OK) return bail("ffxGetInterfaceVK (sdileny) kod 0x%X", (unsigned)e);
    e = ffxGetInterfaceVK(&s.beFi, fdev, s.scratchFi.data(), szFi, 2);
    if (e != FFX_OK) return bail("ffxGetInterfaceVK kod 0x%X", (unsigned)e);
    e = s.beShared.fpCreateBackendContext(&s.beShared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &s.sharedCtx);
    if (e != FFX_OK) return bail("sdileny kontext backendu kod 0x%X", (unsigned)e);
    s.sharedCtxOk = true;

    FfxOpticalflowContextDescription od = {};
    od.backendInterface = s.beFi;
    od.flags = 0;
    od.resolution = {ci.displayW, ci.displayH};
    e = ffxOpticalflowContextCreate(&s.of, &od);
    if (e != FFX_OK) return bail("ffxOpticalflowContextCreate kod 0x%X", (unsigned)e);
    s.ofOk = true;

    FfxFrameInterpolationContextDescription fd = {};
    fd.backendInterface = s.beFi;
    fd.flags = (ci.depthInverted ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0u) |
               (ci.depthInfinite ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE : 0u);
    fd.maxRenderSize = {ci.maxRenderW, ci.maxRenderH};
    fd.displaySize = {ci.displayW, ci.displayH};
    fd.backBufferFormat = ffxGetSurfaceFormatVK(ci.colorFormat);
    fd.previousInterpolationSourceFormat = fd.backBufferFormat;
    e = ffxFrameInterpolationContextCreate(&s.fi, &fd);
    if (e != FFX_OK) return bail("ffxFrameInterpolationContextCreate kod 0x%X", (unsigned)e);
    s.fiOk = true;
    s.fiCtx = ffxFrameInterpolationAfgEffectContextId(&s.fi);
    // vice mezisnimku na par: dynamicke pohledy FI ziji do 4. dalsiho paru (AfgRecordPair posouva snimek)
    ffxVkAfgSetManualFrames(&s.beFi, s.fiCtx, true);

    FfxOpticalflowSharedResourceDescriptions ofr = {};
    e = ffxOpticalflowGetSharedResourceDescriptions(&s.of, &ofr);
    if (e != FFX_OK) return bail("sdilene obrazy optical flow kod 0x%X", (unsigned)e);
    FfxFrameInterpolationSharedResourceDescriptions fir = {};
    e = ffxFrameInterpolationGetSharedResourceDescriptions(&s.fi, &fir);
    if (e != FFX_OK) return bail("sdilene obrazy FI kod 0x%X", (unsigned)e);
    const FfxCreateResourceDescription* descs[kSharedCount] = {&ofr.opticalFlowVector, &ofr.opticalFlowSCD,
                                                               &fir.dilatedDepth, &fir.dilatedMotionVectors,
                                                               &fir.reconstructedPrevNearestDepth};
    for (int i = 0; i < kSharedCount; ++i) {
        e = s.beShared.fpCreateResource(&s.beShared, descs[i], s.sharedCtx, &s.shared[i]);
        if (e != FFX_OK) return bail("sdileny obraz %d kod 0x%X", i, (unsigned)e);
        s.sharedOk[i] = true;
    }

    if (!MakeImg(s, s.src[0], ci.colorFormat, ci.displayW, ci.displayH) ||
        !MakeImg(s, s.src[1], ci.colorFormat, ci.displayW, ci.displayH) ||
        (ci.hudless && !MakeImg(s, s.pres, ci.colorFormat, ci.displayW, ci.displayH)))
        return bail("vlastni obrazy %ux%u nejdou vytvorit", ci.displayW, ci.displayH);

    FfxEffectMemoryUsage mf = {}, mo = {}, ms = {};
    ffxFrameInterpolationContextGetGpuMemoryUsage(&s.fi, &mf);
    ffxOpticalflowContextGetGpuMemoryUsage(&s.of, &mo);
    ffxSharedContextGetGpuMemoryUsage(&s.beShared, &ms);
    s.vram = (size_t)(mf.totalUsageInBytes + mo.totalUsageInBytes + ms.totalUsageInBytes) + (size_t)s.src[0].size +
             (size_t)s.src[1].size + (size_t)s.pres.size;
    s.cur = 0;
    s.havePair = false;
    return true;
}

void AfgDestroy() {
    if (!g) return;
    ReleaseAll(*g);
    delete g;
    g = nullptr;
}

bool AfgReady() { return g != nullptr; }
size_t AfgVramBytes() { return g ? g->vram : 0; }
const char* AfgVersionName() { return "FSR 3.1.4 AFG"; }
const char* AfgLastError() { return g ? g->err.c_str() : "AFG neni vytvoreny"; }

void AfgReleaseViews() {
    if (!g) return;
    for (int i = 0; i < FFX_MAX_QUEUED_FRAMES; ++i) ffxVkAfgAdvanceFrame(&g->beFi, g->fiCtx);
}

bool AfgRecordPair(VkCommandBuffer cmd, const AfgPairInfo& p) {
    if (!g) return false;
    State& s = *g;
    const AfgCreateInfo& ci = s.ci;
    if (!cmd || !p.color || !p.motionVectors || !p.depth) return SetErr("AfgRecordPair: chybi vstup");
    if (p.inputW > ci.maxRenderW || p.inputH > ci.maxRenderH || !p.renderW || !p.renderH || p.renderW > p.inputW ||
        p.renderH > p.inputH)
        return SetErr("AfgRecordPair: render %ux%u / obrazy %ux%u mimo max. %ux%u", p.renderW, p.renderH, p.inputW,
                      p.inputH, ci.maxRenderW, ci.maxRenderH);
    if (!IsDepthFmt(p.depthFormat)) return SetErr("AfgRecordPair: format hloubky %d", (int)p.depthFormat);
    const bool hl = ci.hudless && p.hudless;
    s.pairOk = false;

    // novy snimek do druheho zdrojoveho obrazu; minuly zustava v s.src[s.cur]
    const int nw = s.havePair ? 1 - s.cur : 0;
    OwnImg& dst = s.src[nw];
    OwnImg& other = s.src[1 - nw];

    // pohledy FFX zaregistrovane pred 4 pary se ted ruseji (viz afg.h)
    ffxVkAfgAdvanceFrame(&s.beFi, s.fiCtx);

    // kopie: zdroj interpolace (HUD-less, jinak backbuffer) -> dst; backbuffer s HUD -> pres
    {
        const VkImageLayout L = p.colorLayout, SRO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            TS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        const bool keepL = L == VK_IMAGE_LAYOUT_GENERAL || L == TS;  // kopirovat primo z layoutu volajiciho
        const VkImageLayout srcL = keepL ? L : TS;
        VkImageMemoryBarrier pre[5];
        uint32_t n = 0;
        pre[n++] = Ib(hl ? p.hudless : p.color, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, L, srcL);
        pre[n++] = Ib(dst.img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, dst.init ? SRO : VK_IMAGE_LAYOUT_UNDEFINED, TD);
        if (hl) {
            pre[n++] = Ib(p.color, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, L, srcL);
            pre[n++] = Ib(s.pres.img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, s.pres.init ? SRO : VK_IMAGE_LAYOUT_UNDEFINED, TD);
        }
        if (!other.init)  // prvni snimek: druhy obraz jen do platneho layoutu (obsah se pri resetu necte)
            pre[n++] = Ib(other.img, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, SRO);
        s.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, n, pre);
        VkImageCopy c = {};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {ci.displayW, ci.displayH, 1};
        s.CmdCopyImage(cmd, hl ? p.hudless : p.color, srcL, dst.img, TD, 1, &c);
        if (hl) s.CmdCopyImage(cmd, p.color, srcL, s.pres.img, TD, 1, &c);
        VkImageMemoryBarrier post[4];
        n = 0;
        post[n++] = Ib(hl ? p.hudless : p.color, VK_ACCESS_TRANSFER_READ_BIT, 0, srcL, L);
        post[n++] = Ib(dst.img, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TD, SRO);
        if (hl) {
            post[n++] = Ib(p.color, VK_ACCESS_TRANSFER_READ_BIT, 0, srcL, L);
            post[n++] = Ib(s.pres.img, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TD, SRO);
        }
        s.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, n, post);
        dst.init = other.init = true;
        if (hl) s.pres.init = true;
    }

    // prvni par po vytvoreni = reset (FI i optical flow); mezisnimky pak = kopie aktualniho
    const bool reset = p.reset || !s.havePair;
    const FfxResourceStates READ = FFX_RESOURCE_STATE_COMPUTE_READ;
    const uint32_t depthUsage = FFX_RESOURCE_USAGE_DEPTHTARGET | (HasStencil(p.depthFormat) ? FFX_RESOURCE_USAGE_STENCILTARGET : 0u);
    const FfxResource curSrc = Res(dst.img, ci.colorFormat, ci.displayW, ci.displayH, FFX_RESOURCE_USAGE_READ_ONLY, READ);

    // 1) dilatace MV / hloubky + hloubka minuleho snimku (ffxFrameInterpolationPrepare)
    FfxFrameInterpolationPrepareDescription pd = {};
    pd.flags = 0;
    pd.commandList = ffxGetCommandListVK(cmd);
    pd.renderSize = {p.renderW, p.renderH};
    pd.jitterOffset = {p.jitterX, p.jitterY};
    pd.motionVectorScale = {p.mvScaleX, p.mvScaleY};
    pd.frameTimeDelta = p.frameTimeDeltaMs;
    pd.cameraNear = p.cameraNear;
    pd.cameraFar = p.cameraFar;
    pd.viewSpaceToMetersFactor = p.metersPerUnit;
    pd.cameraFovAngleVertical = p.cameraFovY;
    pd.depth = Res(p.depth, p.depthFormat, p.inputW, p.inputH, depthUsage, READ);
    pd.motionVectors = Res(p.motionVectors, p.mvFormat, p.inputW, p.inputH, FFX_RESOURCE_USAGE_READ_ONLY, READ);
    pd.frameID = p.frameId;
    pd.dilatedDepth = SharedRes(s, kDilDepth);
    pd.dilatedMotionVectors = SharedRes(s, kDilMv);
    pd.reconstructedPrevDepth = SharedRes(s, kRecPrevDepth);
    pd.cameraUp[1] = 1.0f;
    pd.cameraRight[0] = 1.0f;
    pd.cameraForward[2] = 1.0f;
    FfxErrorCode e = ffxFrameInterpolationPrepare(&s.fi, &pd);
    if (e != FFX_OK) return SetErr("ffxFrameInterpolationPrepare kod 0x%X", (unsigned)e);

    // 2) optical flow aktualniho zdroje interpolace (minuly si OF drzi sam)
    FfxOpticalflowDispatchDescription od = {};
    od.commandList = ffxGetCommandListVK(cmd);
    od.color = curSrc;
    od.opticalFlowVector = SharedRes(s, kOfVector);
    od.opticalFlowSCD = SharedRes(s, kOfScd);
    od.reset = reset;
    od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    od.minMaxLuminance = {0.0f, 0.0f};
    e = ffxOpticalflowContextDispatch(&s.of, &od);
    if (e != FFX_OK) return SetErr("ffxOpticalflowContextDispatch kod 0x%X", (unsigned)e);

    // 3) par pro FI: reset / frameID, konstanty, citac snimku; popis se pouzije pro vsechny mezisnimky
    FfxFrameInterpolationDispatchDescription& d = s.dd;
    d = {};
    d.flags = 0;
    d.commandList = ffxGetCommandListVK(cmd);
    d.displaySize = {ci.displayW, ci.displayH};
    d.renderSize = {p.renderW, p.renderH};
    if (hl) {
        d.currentBackBuffer = Res(s.pres.img, ci.colorFormat, ci.displayW, ci.displayH, FFX_RESOURCE_USAGE_READ_ONLY, READ);
        d.currentBackBuffer_HUDLess = curSrc;
    } else {
        d.currentBackBuffer = curSrc;
    }
    d.previousInterpolationSource =
        Res(other.img, ci.colorFormat, ci.displayW, ci.displayH, FFX_RESOURCE_USAGE_READ_ONLY, READ);
    d.interpolationRect = {0, 0, (int32_t)ci.displayW, (int32_t)ci.displayH};
    d.opticalFlowVector = SharedRes(s, kOfVector);
    d.opticalFlowSceneChangeDetection = SharedRes(s, kOfScd);
    d.opticalFlowBlockSize = 8;
    d.opticalFlowScale = {1.0f / ci.displayW, 1.0f / ci.displayH};
    d.cameraNear = p.cameraNear;
    d.cameraFar = p.cameraFar;
    d.cameraFovAngleVertical = p.cameraFovY;
    d.viewSpaceToMetersFactor = p.metersPerUnit;
    d.frameTimeDelta = p.frameTimeDeltaMs;
    d.reset = reset;
    d.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    d.minMaxLuminance[0] = d.minMaxLuminance[1] = 0.0f;
    d.frameID = p.frameId;
    d.dilatedDepth = SharedRes(s, kDilDepth);
    d.dilatedMotionVectors = SharedRes(s, kDilMv);
    d.reconstructedPrevDepth = SharedRes(s, kRecPrevDepth);
    e = ffxFrameInterpolationAfgPair(&s.fi, &d);
    if (e != FFX_OK) return SetErr("ffxFrameInterpolationAfgPair kod 0x%X", (unsigned)e);

    s.cur = nw;
    s.havePair = true;
    s.pairOk = true;
    s.pairHudless = hl;
    s.interpsInPair = 0;
    return true;
}

bool AfgRecordInterp(VkCommandBuffer cmd, float t, VkImage out, VkImageView outView) {
    (void)outView;
    if (!g) return false;
    State& s = *g;
    if (!s.pairOk) return SetErr("AfgRecordInterp: zadny platny par (nejdriv uspesny AfgRecordPair)");
    if (!cmd || !out) return SetErr("AfgRecordInterp: chybi command buffer / vystup");
    if (s.interpsInPair >= AFG_MAX_INTERP_PER_PAIR)
        return SetErr("AfgRecordInterp: vic nez %d mezisnimku na par", AFG_MAX_INTERP_PER_PAIR);
    if (ffxVkAfgFreeDynamicViews(&s.beFi, s.fiCtx) < 64)
        return SetErr("AfgRecordInterp: dosly dynamicke pohledy FFX v tomto paru");
    if (!(t >= 0.0f)) t = 0.0f;  // i NaN
    if (t > 1.0f) t = 1.0f;

    VkImageMemoryBarrier b = Ib(out, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                VK_IMAGE_LAYOUT_GENERAL);
    s.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);

    FfxFrameInterpolationDispatchDescription d = s.dd;
    d.commandList = ffxGetCommandListVK(cmd);
    d.output = Res(out, s.ci.colorFormat, s.ci.displayW, s.ci.displayH, FFX_RESOURCE_USAGE_UAV,
                   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxErrorCode e = ffxFrameInterpolationAfgInterp(&s.fi, &d, t);
    if (e != FFX_OK) return SetErr("ffxFrameInterpolationAfgInterp kod 0x%X", (unsigned)e);

    b = Ib(out, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    s.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    ++s.interpsInPair;
    return true;
}
