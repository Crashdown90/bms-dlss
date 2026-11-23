// bms-dlss, spike S2 (FG varianta 2): prezentacni instance helperu - "bms_dlss_helper.exe <pid hry> present".
//
// Proxy ([present] HelperPresent=1, src/present.cpp) presmeruje DXVK do skryteho okna a kazdy finalni snimek
// (backbuffer v Present) zkopiruje do sdileneho obrazu (BmsdPresentShared, bmsd_shared.h). Tento proces ma Vulkan
// swapchain na HWND hry: ceka na semReady >= N, obraz do swapchainu zkopiruje blitem a prezentuje. Kazdy prevzaty
// snimek potvrdi signalem semDone = N (i kdyz ho nezobrazi: minimalizace, okno obsazene) a helperAck = N.
//
// Swapchain se uvolnuje na kazde ceste ven (konec hry, zadost proxy, chyba, vyjimka - SEH): po tvrdem ukonceni
// procesu zustava HWND pro Vulkan obsazene (spike S1) a DXVK by do okna hry uz nemohl. Chyba ovladace: pri
// VK_ERROR_NATIVE_WINDOW_IN_USE_KHR vraci vkCreateSwapchainKHR nenulovy handle, jehoz zniceni spadne -> zahodit.
// Log: bms_dlss_present.log (predchozi .prev) vedle helperu.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../bmsd_shared.h"

namespace {

FILE* g_plog;
CRITICAL_SECTION g_logCs;

void PLog(const char* fmt, ...) {
    if (!g_plog) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_plog, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_plog, fmt, ap);
    va_end(ap);
    fputc('\n', g_plog);
    fflush(g_plog);
    LeaveCriticalSection(&g_logCs);
}

#define S_INST_FUNCS(X)                                                                                           \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2)                         \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)          \
    X(vkGetDeviceProcAddr) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                          \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties)
#define S_DEV_FUNCS(X)                                                                                            \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueWaitIdle) X(vkCreateImage) X(vkDestroyImage)                 \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateSemaphore) \
    X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool)       \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)            \
    X(vkCmdPipelineBarrier) X(vkCmdBlitImage) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence)                \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)                         \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkWaitSemaphores)

struct {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkWaitForPresentKHR vkWaitForPresentKHR;  // jen s VK_KHR_present_wait (mereni zobrazeni)
#define DECL(n) PFN_##n n;
    S_INST_FUNCS(DECL)
    S_DEV_FUNCS(DECL)
#undef DECL
} vk;

BmsdPresentShared* sh;
HWND g_wnd;
VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
VkQueue g_queue;
uint32_t g_qf = UINT32_MAX;
VkSemaphore g_semReady, g_semDone;

const int K = BMSD_PRESENT_IMAGES;
const int kSlots = 3;
VkImage g_img[K];
VkDeviceMemory g_mem[K];
uint32_t g_iw, g_ih;
int32_t g_gen;

VkCommandPool g_pool;
VkCommandBuffer g_cmd[kSlots];
VkFence g_fence[kSlots];
bool g_fenceUsed[kSlots];
VkSemaphore g_acq[kSlots];

VkSurfaceKHR g_surf;
VkSwapchainKHR g_sc;
VkImage g_scImg[16];
VkSemaphore g_scDone[16];  // render hotov -> present (na obraz swapchainu)
uint32_t g_scCount;
VkExtent2D g_scExt;
VkFormat g_scFmt;
VkPresentModeKHR g_scMode;
uint32_t g_scInterval = 2;  // presentInterval, pro ktery swapchain vznikl
bool g_needRecreate;
// Stav okna hry pri vytvoreni swapchainu. Ve hre se prvni swapchain (vytvoreny brzy po startu) nezobrazoval (DWM
// ukazoval stary obsah okna), dokud se swapchain nevytvoril znovu - proto zmena stavu okna = novy swapchain a navic
// jednorazove nove vytvoreni kResyncMs po prvnim presentu.
struct WndState {
    LONG style, ex;
    RECT rc;
    BOOL vis;
} g_scWnd;
ULONGLONG g_firstPresentTick;
bool g_resyncDone;
ULONGLONG g_lastFrameTick, g_frameGap;  // g_frameGap: mezera pred timto snimkem (ms), kdyz > 1 s
const ULONGLONG kResyncMs = 1500;

WndState GetWndState() {
    WndState s = {};
    s.style = GetWindowLongW(g_wnd, GWL_STYLE);
    s.ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    GetWindowRect(g_wnd, &s.rc);
    s.vis = IsWindowVisible(g_wnd);
    return s;
}
ULONGLONG g_inUseRetry;     // NATIVE_WINDOW_IN_USE: dalsi pokus
ULONGLONG g_inUseSince;
bool g_inUseLogged;
volatile bool g_fatal;
LARGE_INTEGER g_qpf;

// statistika
struct Acc {
    double sum, max;
    unsigned n;
    void add(double v) {
        sum += v;
        if (v > max) max = v;
        ++n;
    }
    float avg() const { return n ? (float)(sum / n) : 0.0f; }
};
Acc g_stWake, g_stDone, g_stPres, g_stInt, g_stDisp;
// Mereni (vlastni vlakno, [present] MeasureDisplay): cas, kdy GPU dokoncila kopii proxy (semReady = konec snimku
// hry + kopie) a blit helperu (semDone); hlavni vlakno pak ceka na zobrazeni (present_wait).
CRITICAL_SECTION g_measCs;
HANDLE g_measEv;
uint64_t g_measN, g_measDoneN;
LONGLONG g_measPost, g_measDoneQpc;
Acc g_stReady, g_stBlit, g_stDispAfterDone;  // post -> kopie hotova, kopie -> blit hotov, blit hotov -> zobrazeno
unsigned g_stFrames, g_stPresented, g_stRecreate;
LONGLONG g_statQpc, g_lastPresQpc;

LONGLONG Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double Ms(LONGLONG a, LONGLONG b) { return (double)(b - a) * 1000.0 / (double)g_qpf.QuadPart; }

const char* VkStr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    default: return "jina chyba";
    }
}

const char* ModeStr(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "?";
    }
}

// Trvala chyba: proxy prejde na zalohu (helper pak swapchain uvolni a skonci)
void Fatal(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    PLog("CHYBA: %s", buf);
    if (sh) {
        strncpy(sh->helperMsg, buf, sizeof sh->helperMsg - 1);
        MemoryBarrier();
        sh->helperState = BMSD_PST_FAILED;
    }
    g_fatal = true;
}

void SetState(int32_t s) {
    if (sh->helperState != s) {
        static const char* names[] = {"start", "prezentuje", "okno minimalizovane / 0x0", "okno obsazene"};
        PLog("stav: %s", s >= 0 && s <= 3 ? names[s] : "chyba");
        sh->helperState = s;
    }
}

bool InitVk() {
    HMODULE dll = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);  // jen ze System32
    if (!dll) return Fatal("vulkan-1.dll (64-bit) not found in System32"), false;
    vk.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
    vk.vkCreateInstance = (PFN_vkCreateInstance)vk.vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (!vk.vkCreateInstance) return Fatal("vulkan-1.dll bez vkCreateInstance"), false;
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "bms_dlss_present", 1, "bms-dlss", 1,
                             VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, iext};
    VkResult r = vk.vkCreateInstance(&ici, nullptr, &g_inst);
    if (r != VK_SUCCESS) return Fatal("vkCreateInstance %s", VkStr(r)), false;
#define LOADI(n)                                              \
    vk.n = (PFN_##n)vk.vkGetInstanceProcAddr(g_inst, #n);     \
    if (!vk.n) return Fatal("chybi funkce %s", #n), false;
    S_INST_FUNCS(LOADI)
#undef LOADI
    uint32_t count = 16;
    VkPhysicalDevice pds[16];
    vk.vkEnumeratePhysicalDevices(g_inst, &count, pds);
    for (uint32_t i = 0; i < count && !g_pd; ++i) {
        VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
        vk.vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (!memcmp(idp.deviceUUID, sh->deviceUUID, 16)) {
            g_pd = pds[i];
            PLog("GPU: %s (shoda UUID s DXVK), ovladac 0x%X", p2.properties.deviceName, p2.properties.driverVersion);
        }
    }
    if (!g_pd) return Fatal("GPU se shodnym UUID jako DXVK nenalezena"), false;
    VkQueueFamilyProperties qfp[32];
    uint32_t qfCount = 32;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qfCount, qfp);
    for (uint32_t i = 0; i < qfCount && g_qf == UINT32_MAX; ++i)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g_qf = i;
    if (g_qf == UINT32_MAX) return Fatal("zadna graficka fronta"), false;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vk.vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    if (!f12.timelineSemaphore) return Fatal("GPU neumi timeline semafory"), false;
    // present_id + present_wait (S1: dostupne) jen na mereni "snimek zobrazen"
    bool hasPid = false, hasPw = false;
    {
        VkExtensionProperties ep[512];
        uint32_t ne = 512;
        vk.vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &ne, ep);
        for (uint32_t i = 0; i < ne; ++i) {
            hasPid = hasPid || !strcmp(ep[i].extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME);
            hasPw = hasPw || !strcmp(ep[i].extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
    }
    VkPhysicalDevicePresentWaitFeaturesKHR pwf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR pidf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &pwf};
    if (hasPid && hasPw) {
        VkPhysicalDeviceFeatures2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pidf};
        vk.vkGetPhysicalDeviceFeatures2(g_pd, &q2);
    }
    const bool pw = hasPid && hasPw && pidf.presentId && pwf.presentWait;
    VkPhysicalDeviceVulkan12Features en12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    en12.timelineSemaphore = VK_TRUE;
    VkPhysicalDevicePresentWaitFeaturesKHR enPw = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, nullptr,
                                                   VK_TRUE};
    VkPhysicalDevicePresentIdFeaturesKHR enPid = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &enPw,
                                                  VK_TRUE};
    if (pw) en12.pNext = &enPid;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                          VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME,
                          VK_KHR_PRESENT_WAIT_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, g_qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = pw ? 5 : 3;
    dci.ppEnabledExtensionNames = dext;
    r = vk.vkCreateDevice(g_pd, &dci, nullptr, &g_dev);
    if (r != VK_SUCCESS) return Fatal("vkCreateDevice %s", VkStr(r)), false;
#define LOADD(n)                                              \
    vk.n = (PFN_##n)vk.vkGetDeviceProcAddr(g_dev, #n);        \
    if (!vk.n) return Fatal("chybi funkce %s", #n), false;
    S_DEV_FUNCS(LOADD)
#undef LOADD
    if (pw) vk.vkWaitForPresentKHR = (PFN_vkWaitForPresentKHR)vk.vkGetDeviceProcAddr(g_dev, "vkWaitForPresentKHR");
    sh->presentWaitOk = vk.vkWaitForPresentKHR ? 1 : 0;
    PLog("present_wait (mereni zobrazeni): %s", vk.vkWaitForPresentKHR ? "ano" : "ne");
    vk.vkGetDeviceQueue(g_dev, g_qf, 0, &g_queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
    if (vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) return Fatal("command pool"), false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, kSlots};
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_cmd) != VK_SUCCESS) return Fatal("command buffery"), false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < kSlots; ++i) {
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_fence[i]) != VK_SUCCESS) return Fatal("fence"), false;
        if (vk.vkCreateSemaphore(g_dev, &bsi, nullptr, &g_acq[i]) != VK_SUCCESS) return Fatal("semafor"), false;
    }
    return true;
}

bool ImportSem(uint64_t handle, VkSemaphore* out) {
    VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE,
                                     0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    if (vk.vkCreateSemaphore(g_dev, &sci, nullptr, out) != VK_SUCCESS) return false;
    VkImportSemaphoreWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    ii.semaphore = *out;
    ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    ii.handle = (HANDLE)(uintptr_t)handle;
    const VkResult r = vk.vkImportSemaphoreWin32HandleKHR(g_dev, &ii);
    CloseHandle(ii.handle);
    return r == VK_SUCCESS;
}

bool WaitSlots(uint64_t ns) {
    for (int i = 0; i < kSlots; ++i)
        if (g_fenceUsed[i]) {
            if (vk.vkWaitForFences(g_dev, 1, &g_fence[i], VK_TRUE, ns) != VK_SUCCESS) return false;
            vk.vkResetFences(g_dev, 1, &g_fence[i]);
            g_fenceUsed[i] = false;
        }
    return true;
}

bool Import() {
    if (!WaitSlots(2000000000ull)) return Fatal("GPU nedobehla pred importem"), false;
    for (int i = 0; i < K; ++i) {
        if (g_img[i]) vk.vkDestroyImage(g_dev, g_img[i], nullptr);
        if (g_mem[i]) vk.vkFreeMemory(g_dev, g_mem[i], nullptr);
        g_img[i] = VK_NULL_HANDLE;
        g_mem[i] = VK_NULL_HANDLE;
    }
    if (!g_semReady) {
        if (!ImportSem(sh->semReadyHandle, &g_semReady) || !ImportSem(sh->semDoneHandle, &g_semDone))
            return Fatal("import semaforu selhal"), false;
    }
    VkPhysicalDeviceMemoryProperties mp = {};
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    for (int i = 0; i < K; ++i) {
        const BmsdImage& bi = sh->img[i];
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, ht};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = (VkFormat)bi.format;
        ci.extent = {bi.width, bi.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = bi.usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vk.vkCreateImage(g_dev, &ci, nullptr, &g_img[i]);
        if (r != VK_SUCCESS) return Fatal("vkCreateImage %d: %s", i, VkStr(r)), false;
        VkMemoryRequirements req = {};
        vk.vkGetImageMemoryRequirements(g_dev, g_img[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, g_img[i]};
        VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded, ht,
                                                (HANDLE)(uintptr_t)bi.handle};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, bi.allocSize, type};
        r = vk.vkAllocateMemory(g_dev, &ai, nullptr, &g_mem[i]);
        CloseHandle((HANDLE)(uintptr_t)bi.handle);
        if (r != VK_SUCCESS) return Fatal("import pameti obrazu %d: %s", i, VkStr(r)), false;
        if (vk.vkBindImageMemory(g_dev, g_img[i], g_mem[i], 0) != VK_SUCCESS) return Fatal("bind obrazu %d", i), false;
    }
    g_iw = sh->width;
    g_ih = sh->height;
    g_gen = sh->generation;
    PLog("obrazy importovany: %ux%u x%d, VkFormat %d (generace %d)", g_iw, g_ih, K, sh->img[0].format, g_gen);
    MemoryBarrier();
    sh->helperGeneration = g_gen;
    return true;
}

// Uvolnit swapchain (a s nim obsazeni HWND). Volat jen kdyz nic neceka na GPU (nebo po limitu).
void DestroySwapchain(const char* why) {
    if (!g_sc) return;
    if (WaitSlots(2000000000ull)) vk.vkQueueWaitIdle(g_queue);
    vk.vkDestroySwapchainKHR(g_dev, g_sc, nullptr);
    g_sc = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < g_scCount; ++i)
        if (g_scDone[i]) vk.vkDestroySemaphore(g_dev, g_scDone[i], nullptr);
    memset(g_scDone, 0, sizeof g_scDone);
    g_scCount = 0;
    MemoryBarrier();
    sh->swapchainAlive = 0;
    PLog("swapchain uvolnen (%s)", why);
}

VkPresentModeKHR PickMode(uint32_t interval) {
    VkPresentModeKHR modes[8];
    uint32_t n = 8;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, g_surf, &n, modes);
    auto has = [&](VkPresentModeKHR m) {
        for (uint32_t i = 0; i < n; ++i)
            if (modes[i] == m) return true;
        return false;
    };
    if (interval == 0) {  // hra bez VSync: jako DXVK (IMMEDIATE, jinak MAILBOX)
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Swapchain na okne hry podle jeho aktualni velikosti. false = ted neprezentovat (okno 0x0, obsazene, chyba).
bool EnsureSwapchain() {
    if (!IsWindow(g_wnd)) return Fatal("okno hry %p uz neexistuje", (void*)g_wnd), false;
    if (!g_surf) {
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = g_wnd;
        VkResult r = vk.vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &g_surf);
        if (r != VK_SUCCESS) return Fatal("vkCreateWin32SurfaceKHR %s", VkStr(r)), false;
        VkBool32 sup = VK_FALSE;
        vk.vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_surf, &sup);
        if (!sup) return Fatal("fronta %u neumi prezentovat do okna hry", g_qf), false;
    }
    RECT rc = {};
    GetClientRect(g_wnd, &rc);
    // Cil je dcerine okno pres klientskou plochu hry: minimalizace je videt jen na korenovem okne
    const bool zero = IsIconic(g_wnd) || IsIconic(GetAncestor(g_wnd, GA_ROOT)) || rc.right <= 0 || rc.bottom <= 0;
    if (zero) {
        DestroySwapchain("okno minimalizovane / 0x0");
        SetState(BMSD_PST_NOWINDOW);
        return false;
    }
    const uint32_t interval = sh->presentInterval;
    if (g_sc && (g_needRecreate || (uint32_t)rc.right != g_scExt.width || (uint32_t)rc.bottom != g_scExt.height ||
                 interval != g_scInterval)) {
        DestroySwapchain(g_needRecreate ? "OUT_OF_DATE / SUBOPTIMAL" : "zmena velikosti okna nebo VSync");
        ++g_stRecreate;
    }
    if (g_sc) {
        const WndState ws = GetWndState();
        if (ws.style != g_scWnd.style || ws.ex != g_scWnd.ex || ws.vis != g_scWnd.vis ||
            memcmp(&ws.rc, &g_scWnd.rc, sizeof ws.rc)) {
            PLog("okno hry se zmenilo (styl %08lX -> %08lX, ex %08lX -> %08lX, %ld,%ld-%ld,%ld -> %ld,%ld-%ld,%ld, "
                 "viditelne %d -> %d) - swapchain znovu",
                 g_scWnd.style, ws.style, g_scWnd.ex, ws.ex, g_scWnd.rc.left, g_scWnd.rc.top, g_scWnd.rc.right,
                 g_scWnd.rc.bottom, ws.rc.left, ws.rc.top, ws.rc.right, ws.rc.bottom, g_scWnd.vis, ws.vis);
            DestroySwapchain("zmena okna hry");
            ++g_stRecreate;
        } else if (g_frameGap) {  // hra dele nekreslila (nacitani mapy): swapchain znovu, kdyby byl "mrtvy"
            PLog("snimky %.1f s nechodily (nacitani?) - swapchain znovu", g_frameGap / 1000.0);
            DestroySwapchain("po pauze ve snimcich");
            ++g_stRecreate;
        } else if (!g_resyncDone && g_firstPresentTick && GetTickCount64() - g_firstPresentTick > kResyncMs) {
            g_resyncDone = true;
            PLog("pojistka: swapchain znovu %llu ms po prvnim presentu (prvni swapchain po startu nebyl videt)",
                 kResyncMs);
            DestroySwapchain("pojistka po startu");
            ++g_stRecreate;
        }
    }
    g_needRecreate = false;
    if (g_sc) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g_inUseRetry) return false;

    VkSurfaceCapabilitiesKHR caps = {};
    VkResult r = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &caps);
    if (r != VK_SUCCESS) return Fatal("surface caps %s", VkStr(r)), false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext = {(uint32_t)rc.right, (uint32_t)rc.bottom};
    if (!ext.width || !ext.height) {
        SetState(BMSD_PST_NOWINDOW);
        return false;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return Fatal("swapchain neumi TRANSFER_DST"), false;
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fmts);
    VkSurfaceFormatKHR sf = nf ? fmts[0] : VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (uint32_t i = 0; i < nf; ++i)  // hodnoty backbufferu jsou uz v sRGB: format UNORM (blit bez prevodu)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sf = fmts[i];
            break;
        }
    const VkPresentModeKHR mode = PickMode(interval);
    uint32_t minImg = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && minImg > caps.maxImageCount) minImg = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surf;
    ci.minImageCount = minImg;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = vk.vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc);
    if (r != VK_SUCCESS) {
        // Chyba ovladace: pri chybe muze byt sc nenulovy - nesmi se znicit (access violation), jen zahodit.
        if (sc) PLog("vkCreateSwapchainKHR %s vratil nenulovy handle %p (chyba ovladace) - zahazuji", VkStr(r),
                     (void*)sc);
        if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            if (!g_inUseLogged) {
                g_inUseLogged = true;
                g_inUseSince = now;
                PLog("okno hry je pro Vulkan obsazene (%s) - zkousim kazdych 250 ms", VkStr(r));
            }
            g_inUseRetry = now + 250;
            SetState(BMSD_PST_INUSE);
            return false;
        }
        return Fatal("vkCreateSwapchainKHR %s", VkStr(r)), false;
    }
    if (g_inUseLogged) PLog("okno hry volne po %.1f s", (now - g_inUseSince) / 1000.0);
    g_inUseLogged = false;
    g_sc = sc;
    g_scWnd = GetWndState();
    MemoryBarrier();
    sh->swapchainAlive = 1;
    g_scCount = 16;
    vk.vkGetSwapchainImagesKHR(g_dev, g_sc, &g_scCount, g_scImg);
    VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_scCount; ++i)
        if (vk.vkCreateSemaphore(g_dev, &bsi, nullptr, &g_scDone[i]) != VK_SUCCESS) return Fatal("semafor"), false;
    g_scExt = ext;
    g_scFmt = sf.format;
    g_scMode = mode;
    g_scInterval = interval;
    sh->presentMode = (uint32_t)mode;
    sh->swapW = ext.width;
    sh->swapH = ext.height;
    PLog("swapchain: %ux%u, %s, VkFormat %d, %u obrazku (min %u), okno hry %p", ext.width, ext.height, ModeStr(mode),
         (int)sf.format, g_scCount, caps.minImageCount, (void*)g_wnd);
    return true;
}

VkImageMemoryBarrier IB(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldL, VkImageLayout newL,
                        uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED, uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = srcQ;
    b.dstQueueFamilyIndex = dstQ;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void PublishStats(LONGLONG now) {
    if (!g_statQpc) g_statQpc = now;
    if (Ms(g_statQpc, now) < 5000.0) return;
    EnterCriticalSection(&g_measCs);
    sh->statFrames = g_stFrames;
    sh->statPresented = g_stPresented;
    sh->statRecreate = g_stRecreate;
    sh->statWakeMs = g_stWake.avg();
    sh->statWakeMax = (float)g_stWake.max;
    sh->statDoneMs = g_stDone.avg();
    sh->statDoneMax = (float)g_stDone.max;
    sh->statPresentMs = g_stPres.avg();
    sh->statPresentMax = (float)g_stPres.max;
    sh->statIntervalMs = g_stInt.avg();
    sh->statIntervalMax = (float)g_stInt.max;
    sh->statDispMs = g_stDisp.avg();
    sh->statDispMax = (float)g_stDisp.max;
    MemoryBarrier();
    ++sh->statSeq;
    PLog("vykon %.1f s: prevzato %u, zobrazeno %u, %s %ux%u, znovu %u | probuzeni %.3f / %.2f ms, post -> blit hotov "
         "%.3f / %.2f ms, post -> zobrazeno %.3f / %.2f ms (%u), vkQueuePresent %.3f / %.2f ms, interval presentu "
         "%.2f / %.2f ms",
         Ms(g_statQpc, now) / 1000.0, g_stFrames, g_stPresented, ModeStr(g_scMode), g_scExt.width, g_scExt.height,
         g_stRecreate, g_stWake.avg(), g_stWake.max, g_stDone.avg(), g_stDone.max, g_stDisp.avg(), g_stDisp.max,
         g_stDisp.n, g_stPres.avg(), g_stPres.max, g_stInt.avg(), g_stInt.max);
    if (g_stReady.n)
        PLog("  rozpad: post -> kopie proxy hotova (~konec snimku hry na GPU) %.3f / %.2f ms, kopie -> blit helperu "
             "hotov %.3f / %.2f ms, blit hotov -> zobrazeno %.3f / %.2f ms (%u)",
             g_stReady.avg(), g_stReady.max, g_stBlit.avg(), g_stBlit.max, g_stDispAfterDone.avg(),
             g_stDispAfterDone.max, g_stDispAfterDone.n);
    g_stWake = g_stDone = g_stPres = g_stInt = g_stDisp = g_stReady = g_stBlit = g_stDispAfterDone = Acc();
    LeaveCriticalSection(&g_measCs);
    g_stFrames = g_stPresented = g_stRecreate = 0;
    g_statQpc = now;
}

// Dokonceni slotu: fence + mereni post -> blit hotov
bool FinishSlot(int s, uint64_t ns) {
    if (!g_fenceUsed[s]) return true;
    if (vk.vkWaitForFences(g_dev, 1, &g_fence[s], VK_TRUE, ns) != VK_SUCCESS) return false;
    vk.vkResetFences(g_dev, 1, &g_fence[s]);
    g_fenceUsed[s] = false;
    return true;
}

int g_slot;
HANDLE g_evFrame, g_evDone, g_parent;

void ProcessFrame(uint64_t N, LONGLONG tWake) {
    const int s = g_slot;
    g_slot = (g_slot + 1) % kSlots;
    if (!FinishSlot(s, 2000000000ull)) return Fatal("GPU nedobehla (slot)");
    const LONGLONG posted = (LONGLONG)sh->postQpc;
    if (posted) g_stWake.add(Ms(posted, tWake));
    const ULONGLONG nowTick = GetTickCount64();
    g_frameGap = g_lastFrameTick && nowTick - g_lastFrameTick > 1000 ? nowTick - g_lastFrameTick : 0;
    g_lastFrameTick = nowTick;
    ++g_stFrames;
    const int k = (int)(N % K);
    bool acquired = false;
    uint32_t idx = 0;
    if (EnsureSwapchain()) {
        const VkResult r = vk.vkAcquireNextImageKHR(g_dev, g_sc, 100000000ull, g_acq[s], VK_NULL_HANDLE, &idx);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            acquired = true;
            if (r == VK_SUBOPTIMAL_KHR) g_needRecreate = true;
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            g_needRecreate = true;
        } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
            DestroySwapchain("SURFACE_LOST");
            vk.vkDestroySurfaceKHR(g_inst, g_surf, nullptr);
            g_surf = VK_NULL_HANDLE;
        } else if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            return Fatal("vkAcquireNextImageKHR %s", VkStr(r));
        }
    }
    if (g_fatal) return;
    VkCommandBuffer cmd = g_cmd[s];
    if (acquired) {
        SetState(BMSD_PST_PRESENTING);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vk.vkResetCommandBuffer(cmd, 0);
        vk.vkBeginCommandBuffer(cmd, &bi);
        const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL;
        const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
        VkImageMemoryBarrier pre[2] = {
            IB(g_img[k], 0, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN, EXT, g_qf),  // prevzeti od proxy
            IB(g_scImg[idx], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 2, pre);
        VkImageBlit b = {};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)g_iw, (int32_t)g_ih, 1};
        b.dstOffsets[1] = {(int32_t)g_scExt.width, (int32_t)g_scExt.height, 1};
        const bool same = g_iw == g_scExt.width && g_ih == g_scExt.height;
        vk.vkCmdBlitImage(cmd, g_img[k], GEN, g_scImg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                          same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        VkImageMemoryBarrier post[2] = {
            IB(g_img[k], VK_ACCESS_TRANSFER_READ_BIT, 0, GEN, GEN, g_qf, EXT),  // zpet proxy
            IB(g_scImg[idx], VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)};
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                nullptr, 0, nullptr, 2, post);
        vk.vkEndCommandBuffer(cmd);
    }
    // Odeslat vzdy: i bez obrazu ceka na semReady = N a signalizuje semDone = N (proxy pak obraz znovu pouzije)
    VkSemaphore waitS[2] = {g_semReady, g_acq[s]};
    uint64_t waitV[2] = {N, 0};
    VkPipelineStageFlags waitStage[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore sigS[2] = {g_semDone, acquired ? g_scDone[idx] : VK_NULL_HANDLE};
    uint64_t sigV[2] = {N, 0};
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = acquired ? 2 : 1;
    ti.pWaitSemaphoreValues = waitV;
    ti.signalSemaphoreValueCount = acquired ? 2 : 1;
    ti.pSignalSemaphoreValues = sigV;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.waitSemaphoreCount = acquired ? 2 : 1;
    si.pWaitSemaphores = waitS;
    si.pWaitDstStageMask = waitStage;
    si.commandBufferCount = acquired ? 1 : 0;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = acquired ? 2 : 1;
    si.pSignalSemaphores = sigS;
    VkResult r = vk.vkQueueSubmit(g_queue, 1, &si, g_fence[s]);
    if (r != VK_SUCCESS) return Fatal("vkQueueSubmit %s", VkStr(r));
    g_fenceUsed[s] = true;
    if (sh->measureDisplay && posted && acquired) {  // mereni: vlakno pocka na kopii proxy a na blit (semafory)
        EnterCriticalSection(&g_measCs);
        g_measN = N;
        g_measPost = posted;
        LeaveCriticalSection(&g_measCs);
        SetEvent(g_measEv);
    }
    MemoryBarrier();
    sh->helperAck = N;
    SetEvent(g_evDone);  // proxy muze obraz znovu pouzit (na GPU ceka na semDone)
    if (!acquired) return;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_scDone[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_sc;
    pi.pImageIndices = &idx;
    VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr, 1, &N};
    if (vk.vkWaitForPresentKHR) pi.pNext = &pid;
    const LONGLONG p0 = Qpc();
    r = vk.vkQueuePresentKHR(g_queue, &pi);
    const LONGLONG p1 = Qpc();
    g_stPres.add(Ms(p0, p1));
    if (g_lastPresQpc) g_stInt.add(Ms(g_lastPresQpc, p1));
    g_lastPresQpc = p1;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        ++g_stPresented;
        if (!g_firstPresentTick) g_firstPresentTick = GetTickCount64();
        if (r == VK_SUBOPTIMAL_KHR) g_needRecreate = true;
        // Mereni (measureDisplay): post -> blit na GPU hotov, post -> snimek zobrazen. Helper tu ceka (max 20 ms);
        // dalsi snimek prijde nejdriv za interval snimku hry.
        if (sh->measureDisplay && posted && vk.vkWaitForPresentKHR &&
            vk.vkWaitForPresentKHR(g_dev, g_sc, N, 20000000ull) == VK_SUCCESS) {
            const LONGLONG t = Qpc();
            EnterCriticalSection(&g_measCs);
            g_stDisp.add(Ms(posted, t));
            if (g_measDoneN == N) g_stDispAfterDone.add(Ms(g_measDoneQpc, t));
            LeaveCriticalSection(&g_measCs);
        }
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_needRecreate = true;
    } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
        DestroySwapchain("SURFACE_LOST");
        vk.vkDestroySurfaceKHR(g_inst, g_surf, nullptr);
        g_surf = VK_NULL_HANDLE;
    } else {
        Fatal("vkQueuePresentKHR %s", VkStr(r));
    }
}

volatile bool g_measQuit;

DWORD WINAPI MeasureProc(void*) {
    for (;;) {
        const DWORD w = WaitForSingleObject(g_measEv, 500);
        if (g_measQuit) return 0;
        if (w != WAIT_OBJECT_0 || !g_semReady) continue;
        EnterCriticalSection(&g_measCs);
        uint64_t N = g_measN;
        const LONGLONG post = g_measPost;
        LeaveCriticalSection(&g_measCs);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_semReady, &N};
        if (vk.vkWaitSemaphores(g_dev, &wi, 50000000ull) != VK_SUCCESS) continue;
        const LONGLONG tR = Qpc();
        wi.pSemaphores = &g_semDone;
        if (vk.vkWaitSemaphores(g_dev, &wi, 50000000ull) != VK_SUCCESS) continue;
        const LONGLONG tD = Qpc();
        EnterCriticalSection(&g_measCs);
        g_stReady.add(Ms(post, tR));
        g_stBlit.add(Ms(tR, tD));
        g_stDone.add(Ms(post, tD));
        g_measDoneN = N;
        g_measDoneQpc = tD;
        LeaveCriticalSection(&g_measCs);
    }
}

// Hlavni smycka bez C++ objektu s destruktory (kvuli __try v PresentGuarded)
int PresentLoop() {
    if (!InitVk()) return 1;
    InitializeCriticalSection(&g_measCs);
    g_measEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (sh->measureDisplay && g_measEv) {
        HANDLE th = CreateThread(nullptr, 0, MeasureProc, nullptr, 0, nullptr);
        if (th) {
            SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);
            CloseHandle(th);
        }
    }
    MemoryBarrier();
    sh->helperReady = 1;
    PLog("Vulkan pripraven, okno hry %p", (void*)g_wnd);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint64_t lastN = 0;
    HANDLE waits[2] = {g_evFrame, g_parent};
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 250);
        const LONGLONG tWake = Qpc();
        if (w == WAIT_OBJECT_0 + 1) {
            PLog("hra skoncila");
            break;
        }
        MemoryBarrier();
        if (sh->stop) {
            PLog("proxy zada konec");
            break;
        }
        if (sh->generation != g_gen && !Import()) break;
        const uint64_t N = sh->frameValue;
        if (N && N != lastN) {
            lastN = N;
            ProcessFrame(N, tWake);
            SetEvent(g_evDone);
        }
        if (g_fatal) break;
        PublishStats(Qpc());
    }
    g_measQuit = true;
    if (g_measEv) SetEvent(g_measEv);
    return g_fatal ? 2 : 0;
}

// Uvolneni swapchainu na kazde ceste ven (i po vyjimce)
void Cleanup(const char* why) {
    if (g_dev) DestroySwapchain(why);
    if (g_surf) vk.vkDestroySurfaceKHR(g_inst, g_surf, nullptr);
    g_surf = VK_NULL_HANDLE;
}

int PresentGuarded() {
    __try {
        return PresentLoop();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PLog("VYJIMKA 0x%08lX - uvolnuji swapchain a koncim", GetExceptionCode());
        if (sh) {
            strncpy(sh->helperMsg, "vyjimka v prezentacnim helperu", sizeof sh->helperMsg - 1);
            sh->helperState = BMSD_PST_FAILED;
        }
        __try {
            Cleanup("vyjimka");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLog("uvolneni swapchainu po vyjimce selhalo");
        }
        return 3;
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    // Vyjimka mimo PresentGuarded (jine vlakno ovladace): aspon zkusit swapchain uvolnit
    PLog("NEOSETRENA VYJIMKA 0x%08lX - zkousim uvolnit swapchain", ep ? ep->ExceptionRecord->ExceptionCode : 0);
    if (g_dev && g_sc) vk.vkDestroySwapchainKHR(g_dev, g_sc, nullptr);
    if (sh) sh->swapchainAlive = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int PresentMain(unsigned long pid, const wchar_t* dir) {
    InitializeCriticalSection(&g_logCs);
    wchar_t path[MAX_PATH + 64], prev[MAX_PATH + 64];
    swprintf(path, MAX_PATH + 64, L"%ls\\bms_dlss_present.log", dir);
    swprintf(prev, MAX_PATH + 64, L"%ls\\bms_dlss_present.log.prev", dir);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_plog = _wfopen(path, L"w");
    QueryPerformanceFrequency(&g_qpf);
    PLog("bms_dlss_helper (prezentace, spike S2) start, hra pid %lu", pid);
    wchar_t name[128];
    swprintf(name, 128, L"Local\\bms_dlss_%lu_present", pid);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) return PLog("nelze otevrit sdilenou pamet"), 1;
    sh = (BmsdPresentShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BmsdPresentShared));
    if (!sh) return PLog("sdilenou pamet nelze namapovat"), 1;
    if (sh->magic != BMSD_PRESENT_MAGIC || sh->version != BMSD_PRESENT_VERSION) {
        PLog("CHYBA: jina verze prezentacniho kanalu (proxy %u, helper %u)", sh->version, BMSD_PRESENT_VERSION);
        sh = nullptr;
        return 1;
    }
    g_wnd = (HWND)(uintptr_t)sh->gameHwnd;
    swprintf(name, 128, L"Local\\bms_dlss_%lu_pframe", pid);
    g_evFrame = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    swprintf(name, 128, L"Local\\bms_dlss_%lu_pdone", pid);
    g_evDone = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    g_parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!g_evFrame || !g_evDone || !g_parent) return Fatal("nelze otevrit eventy nebo proces hry"), 1;
    SetUnhandledExceptionFilter(CrashFilter);
    const int rc = PresentGuarded();
    Cleanup(rc == 0 ? "konec" : "chyba");
    PLog("konec (kod %d)", rc);
    if (g_plog) fclose(g_plog);
    return rc;
}
