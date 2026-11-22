// Rozhrani interop z DXVK v3.1.1 (src/d3d9/d3d9_interfaces.h), poradi metod musi sedet.
// Pouziva most (interop.cpp) i prezentacni kanal spiku S2 (present.cpp).
#pragma once
#include <d3d9.h>
#include <vulkan/vulkan.h>

struct __declspec(uuid("d56344f5-8d35-46fd-806d-94c351b472c1")) ID3D9VkInteropTexture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout,
                                                         VkImageCreateInfo* pInfo) = 0;
};

struct D3D9VkExtImageDesc;

struct __declspec(uuid("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")) ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev,
                                                    VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex,
                                                      uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture,
                                                           const VkImageSubresourceRange* pSubresources,
                                                           VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateImage(const D3D9VkExtImageDesc* desc,
                                                  IDirect3DResource9** ppResult) = 0;
};
