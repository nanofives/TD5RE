/* Pre-flight #1: MinGW-w64 d3d12.h C-macro coverage smoke test.
 * Compile-only + link check via bundled toolchain. Exercises the exact COM
 * shapes the port needs: device, direct command queue, flip-discard swapchain,
 * fence, descriptor heap, root signature serialize, committed resource, DRED. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

int main(void)
{
    ID3D12Device *dev = NULL;
    IDXGIFactory4 *factory = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12Fence *fence = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12Resource *res = NULL;
    ID3D12RootSignature *rootsig = NULL;
    ID3D12Debug *debug = NULL;
    ID3D12DeviceRemovedExtendedDataSettings *dred = NULL;
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;

    /* debug layer + DRED (Phase 1 forensics) */
    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)))
        ID3D12Debug_EnableDebugLayer(debug);
    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&dred))) {
        ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
    }

    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    printf("CreateDXGIFactory2: 0x%08lX\n", (unsigned long)hr);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    printf("D3D12CreateDevice(FL11_0): 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !dev) { printf("NO DEVICE (expected on headless/no-GPU host; compile+link is what matters)\n"); }

    if (dev) {
        D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
        hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        printf("CreateCommandQueue: 0x%08lX\n", (unsigned long)hr);

        hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        printf("CreateCommandAllocator: 0x%08lX\n", (unsigned long)hr);

        hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
        printf("CreateCommandList: 0x%08lX\n", (unsigned long)hr);

        hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
        printf("CreateFence: 0x%08lX\n", (unsigned long)hr);

        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        hr = ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap);
        printf("CreateDescriptorHeap: 0x%08lX\n", (unsigned long)hr);

        /* committed upload buffer */
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
        D3D12_RESOURCE_DESC rd; ZeroMemory(&rd, sizeof rd);
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 1<<20; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&res);
        printf("CreateCommittedResource(upload): 0x%08lX\n", (unsigned long)hr);

        /* root signature serialize (static sampler shape) */
        D3D12_STATIC_SAMPLER_DESC ss; ZeroMemory(&ss, sizeof ss);
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs; ZeroMemory(&rs, sizeof rs);
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &ss; rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        printf("D3D12SerializeRootSignature: 0x%08lX\n", (unsigned long)hr);
        if (SUCCEEDED(hr) && blob) {
            hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rootsig);
            printf("CreateRootSignature: 0x%08lX\n", (unsigned long)hr);
        }

        /* swapchain shape (no real hwnd -> expect failure, but the CALL must compile/link) */
        DXGI_SWAP_CHAIN_DESC1 scd; ZeroMemory(&scd, sizeof scd);
        scd.BufferCount = 2; scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1; scd.Width = 640; scd.Height = 480;
        IDXGISwapChain1 *sc = NULL;
        hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, GetDesktopWindow(), &scd, NULL, NULL, &sc);
        printf("CreateSwapChainForHwnd(shape): 0x%08lX\n", (unsigned long)hr);
        if (sc) IDXGISwapChain1_Release(sc);
    }

    printf("SMOKE-COMPILE-LINK-OK\n");
    return 0;
}
