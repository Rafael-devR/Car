#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static constexpr UINT kWidth = 1280;
static constexpr UINT kHeight = 720;
static constexpr UINT kFrameCount = 2;
static constexpr UINT kShadowSize = 2048;
static constexpr UINT kMaxDraws = 2048;
static constexpr UINT kTextureCount = 9;
static constexpr UINT kShadowSrvIndex = 12;

static void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("DirectX call failed.");
}

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

static D3D12_RESOURCE_DESC BufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Alignment = 0;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

static uint32_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

static uint32_t Hash2(int x, int y, uint32_t seed = 0x1234567u) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ seed;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return h;
}

struct Vertex {
    XMFLOAT3 pos;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
};

struct Mesh {
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    UINT indexCount = 0;
};

enum class MeshKind : uint8_t { Cube, Quad };

struct DrawItem {
    MeshKind mesh = MeshKind::Cube;
    XMFLOAT3 pos{0,0,0};
    XMFLOAT3 scale{1,1,1};
    float yaw = 0.0f;
    UINT texture = 0;
    bool castsShadow = true;
};

struct Collider {
    float minX, maxX, minZ, maxZ;
};

struct alignas(16) ObjectCB {
    XMFLOAT4X4 world;
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 lightViewProj;
    XMFLOAT4 lightDirAmbient;
};

static const char* kShader = R"HLSL(
cbuffer ObjectCB : register(b0)
{
    float4x4 World;
    float4x4 ViewProj;
    float4x4 LightViewProj;
    float4 LightDirAmbient;
};

Texture2D Albedo : register(t0);
Texture2D ShadowMap : register(t1);
SamplerState LinearWrap : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 shadowPos : TEXCOORD1;
};

VSOut VSMain(VSIn input)
{
    VSOut o;
    float4 wp = mul(float4(input.pos, 1.0), World);
    o.pos = mul(wp, ViewProj);
    o.normal = normalize(mul(float4(input.normal, 0.0), World).xyz);
    o.uv = input.uv;
    o.shadowPos = mul(wp, LightViewProj);
    return o;
}

float SampleShadow(float4 sp)
{
    float3 p = sp.xyz / max(sp.w, 0.0001);
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || p.z <= 0.0 || p.z >= 1.0)
        return 1.0;
    return ShadowMap.SampleCmpLevelZero(ShadowSampler, uv, p.z - 0.0015);
}

float4 PSMain(VSOut input) : SV_TARGET
{
    float4 albedo = Albedo.Sample(LinearWrap, input.uv);
    clip(albedo.a - 0.15);

    float3 n = normalize(input.normal);
    float ndl = saturate(dot(n, -normalize(LightDirAmbient.xyz)));
    float shadow = SampleShadow(input.shadowPos);
    float lighting = LightDirAmbient.w + ndl * shadow * (1.0 - LightDirAmbient.w);
    float3 color = albedo.rgb * lighting;

    // A little distance-independent lift keeps the colorful GTA-like readability.
    color = color + albedo.rgb * 0.055;
    return float4(saturate(color), albedo.a);
}

float4 VSShadow(VSIn input) : SV_POSITION
{
    float4 wp = mul(float4(input.pos, 1.0), World);
    return mul(wp, LightViewProj);
}
)HLSL";

class Renderer {
public:
    void Init(HWND hwnd) {
#if defined(_DEBUG)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        }
#endif
        UINT factoryFlags = 0;
#if defined(_DEBUG)
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)));

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                adapter.Reset();
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) break;
            adapter.Reset();
        }
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_)));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = kWidth;
        sd.Height = kHeight;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFrameCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> tempSwap;
        ThrowIfFailed(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &tempSwap));
        ThrowIfFailed(tempSwap.As(&swapChain_));
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = kFrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)));
        rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])));
            device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtv);
            rtv.ptr += rtvStride_;
            ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])));
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.NumDescriptors = 2;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap_)));
        dsvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.NumDescriptors = 16;
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_)));
        srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CreateDepthResources();
        CreateRootSignatureAndPipelines();

        ThrowIfFailed(device_->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&commandList_)));

        CreateMeshes();
        CreateTextures();

        constexpr UINT cbStride = 256;
        const UINT64 cbSize = UINT64(kFrameCount) * kMaxDraws * cbStride;
        auto upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto cbDesc = BufferDesc(cbSize);
        ThrowIfFailed(device_->CreateCommittedResource(
            &upload, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&constantBuffer_)));
        D3D12_RANGE noRead{0,0};
        ThrowIfFailed(constantBuffer_->Map(0, &noRead, reinterpret_cast<void**>(&cbMapped_)));

        ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
        fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) throw std::runtime_error("CreateEvent failed.");

        ThrowIfFailed(commandList_->Close());
        ID3D12CommandList* lists[] = { commandList_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        WaitGpu();

        textureUploads_.clear();
    }

    ~Renderer() {
        if (queue_ && fence_) {
            try { WaitGpu(); } catch (...) {}
        }
        if (constantBuffer_ && cbMapped_) constantBuffer_->Unmap(0, nullptr);
        if (fenceEvent_) CloseHandle(fenceEvent_);
    }

    void Render(const std::vector<DrawItem>& items, const XMMATRIX& viewProj, const XMMATRIX& lightViewProj) {
        const UINT frame = swapChain_->GetCurrentBackBufferIndex();
        WaitForFrame(frame);

        ThrowIfFailed(allocators_[frame]->Reset());
        ThrowIfFailed(commandList_->Reset(allocators_[frame].Get(), nullptr));

        const UINT drawCount = std::min<UINT>(static_cast<UINT>(items.size()), kMaxDraws);
        constexpr UINT cbStride = 256;
        const UINT64 frameBase = UINT64(frame) * kMaxDraws * cbStride;

        for (UINT i = 0; i < drawCount; ++i) {
            const auto& d = items[i];
            XMMATRIX world = XMMatrixScaling(d.scale.x, d.scale.y, d.scale.z) *
                             XMMatrixRotationY(d.yaw) *
                             XMMatrixTranslation(d.pos.x, d.pos.y, d.pos.z);
            ObjectCB cb{};
            XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
            XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewProj));
            XMStoreFloat4x4(&cb.lightViewProj, XMMatrixTranspose(lightViewProj));
            cb.lightDirAmbient = XMFLOAT4(-0.48f, -0.82f, -0.31f, 0.28f);
            std::memcpy(cbMapped_ + frameBase + UINT64(i) * cbStride, &cb, sizeof(cb));
        }

        ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
        commandList_->SetDescriptorHeaps(1, heaps);
        commandList_->SetGraphicsRootSignature(rootSig_.Get());
        commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Shadow pass.
        auto shadowToDepth = Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList_->ResourceBarrier(1, &shadowToDepth);
        D3D12_VIEWPORT shadowVp{0,0,float(kShadowSize),float(kShadowSize),0,1};
        D3D12_RECT shadowRect{0,0,LONG(kShadowSize),LONG(kShadowSize)};
        commandList_->RSSetViewports(1, &shadowVp);
        commandList_->RSSetScissorRects(1, &shadowRect);
        auto shadowDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        shadowDsv.ptr += dsvStride_;
        commandList_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
        commandList_->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList_->SetPipelineState(shadowPso_.Get());

        for (UINT i = 0; i < drawCount; ++i) {
            if (!items[i].castsShadow) continue;
            BindMesh(items[i].mesh);
            commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress() + frameBase + UINT64(i) * cbStride);
            commandList_->DrawIndexedInstanced(CurrentMesh(items[i].mesh).indexCount, 1, 0, 0, 0);
        }

        auto shadowToSrv = Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &shadowToSrv);

        // Main color pass.
        auto toRT = Transition(backBuffers_[frame].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &toRT);

        D3D12_VIEWPORT vp{0,0,float(kWidth),float(kHeight),0,1};
        D3D12_RECT rect{0,0,LONG(kWidth),LONG(kHeight)};
        commandList_->RSSetViewports(1, &vp);
        commandList_->RSSetScissorRects(1, &rect);

        auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += UINT64(frame) * rtvStride_;
        auto mainDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        const float clear[] = {0.38f, 0.59f, 0.78f, 1.0f};
        commandList_->OMSetRenderTargets(1, &rtv, FALSE, &mainDsv);
        commandList_->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commandList_->ClearDepthStencilView(mainDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList_->SetPipelineState(mainPso_.Get());

        const auto shadowGpu = GpuSrv(kShadowSrvIndex);
        commandList_->SetGraphicsRootDescriptorTable(2, shadowGpu);

        for (UINT i = 0; i < drawCount; ++i) {
            BindMesh(items[i].mesh);
            commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress() + frameBase + UINT64(i) * cbStride);
            commandList_->SetGraphicsRootDescriptorTable(1, GpuSrv(std::min<UINT>(items[i].texture, kTextureCount - 1)));
            commandList_->DrawIndexedInstanced(CurrentMesh(items[i].mesh).indexCount, 1, 0, 0, 0);
        }

        auto toPresent = Transition(backBuffers_[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        commandList_->ResourceBarrier(1, &toPresent);

        ThrowIfFailed(commandList_->Close());
        ID3D12CommandList* lists[] = { commandList_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        ThrowIfFailed(swapChain_->Present(1, 0));

        const UINT64 signalValue = ++fenceValue_;
        ThrowIfFailed(queue_->Signal(fence_.Get(), signalValue));
        frameFence_[frame] = signalValue;
    }

private:
    void CreateDepthResources() {
        D3D12_RESOURCE_DESC depth{};
        depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth.Width = kWidth;
        depth.Height = kHeight;
        depth.DepthOrArraySize = 1;
        depth.MipLevels = 1;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.SampleDesc.Count = 1;
        depth.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        auto def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device_->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear, IID_PPV_ARGS(&depth_)));
        auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateDepthStencilView(depth_.Get(), nullptr, dsv);

        D3D12_RESOURCE_DESC shadow = depth;
        shadow.Width = kShadowSize;
        shadow.Height = kShadowSize;
        shadow.Format = DXGI_FORMAT_R32_TYPELESS;
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        ThrowIfFailed(device_->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear, IID_PPV_ARGS(&shadowMap_)));

        D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsvDesc{};
        shadowDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        auto shadowDsv = dsv;
        shadowDsv.ptr += dsvStride_;
        device_->CreateDepthStencilView(shadowMap_.Get(), &shadowDsvDesc, shadowDsv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(shadowMap_.Get(), &srv, CpuSrv(kShadowSrvIndex));
    }

    ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        ComPtr<ID3DBlob> shader, errors;
        HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "embedded.hlsl", nullptr, nullptr,
                                entry, target, flags, 0, &shader, &errors);
        if (FAILED(hr)) {
            if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
            ThrowIfFailed(hr);
        }
        return shader;
    }

    void CreateRootSignatureAndPipelines() {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].ShaderRegister = 0;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].ShaderRegister = 1;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 3;
        rs.pParameters = params;
        rs.NumStaticSamplers = 2;
        rs.pStaticSamplers = samplers;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig_)));

        auto vs = Compile("VSMain", "vs_5_1");
        auto ps = Compile("PSMain", "ps_5_1");
        auto shadowVs = Compile("VSShadow", "vs_5_1");

        D3D12_INPUT_ELEMENT_DESC input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex,pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex,normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex,uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };

        D3D12_RASTERIZER_DESC raster{};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_BACK;
        raster.FrontCounterClockwise = FALSE;
        raster.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        raster.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        raster.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC blend{};
        blend.AlphaToCoverageEnable = FALSE;
        blend.IndependentBlendEnable = FALSE;
        auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSig_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState = blend;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = raster;
        pso.DepthStencilState = depth;
        pso.InputLayout = {input, _countof(input)};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mainPso_)));

        pso.VS = {shadowVs->GetBufferPointer(), shadowVs->GetBufferSize()};
        pso.PS = {nullptr, 0};
        pso.NumRenderTargets = 0;
        pso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        pso.RasterizerState.DepthBias = 1200;
        pso.RasterizerState.SlopeScaledDepthBias = 2.0f;
        ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&shadowPso_)));
    }

    void CreateMeshes() {
        const std::vector<Vertex> cube = {
            {{-0.5f,-0.5f,-0.5f},{0,0,-1},{0,1}}, {{ 0.5f,-0.5f,-0.5f},{0,0,-1},{1,1}}, {{ 0.5f, 0.5f,-0.5f},{0,0,-1},{1,0}}, {{-0.5f, 0.5f,-0.5f},{0,0,-1},{0,0}},
            {{ 0.5f,-0.5f, 0.5f},{0,0,1},{0,1}},  {{-0.5f,-0.5f, 0.5f},{0,0,1},{1,1}},  {{-0.5f, 0.5f, 0.5f},{0,0,1},{1,0}},  {{ 0.5f, 0.5f, 0.5f},{0,0,1},{0,0}},
            {{-0.5f,-0.5f, 0.5f},{-1,0,0},{0,1}}, {{-0.5f,-0.5f,-0.5f},{-1,0,0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{-1,0,0},{1,0}}, {{-0.5f, 0.5f, 0.5f},{-1,0,0},{0,0}},
            {{ 0.5f,-0.5f,-0.5f},{1,0,0},{0,1}},  {{ 0.5f,-0.5f, 0.5f},{1,0,0},{1,1}},  {{ 0.5f, 0.5f, 0.5f},{1,0,0},{1,0}},  {{ 0.5f, 0.5f,-0.5f},{1,0,0},{0,0}},
            {{-0.5f, 0.5f,-0.5f},{0,1,0},{0,1}},  {{ 0.5f, 0.5f,-0.5f},{0,1,0},{1,1}},  {{ 0.5f, 0.5f, 0.5f},{0,1,0},{1,0}},  {{-0.5f, 0.5f, 0.5f},{0,1,0},{0,0}},
            {{-0.5f,-0.5f, 0.5f},{0,-1,0},{0,1}}, {{ 0.5f,-0.5f, 0.5f},{0,-1,0},{1,1}}, {{ 0.5f,-0.5f,-0.5f},{0,-1,0},{1,0}}, {{-0.5f,-0.5f,-0.5f},{0,-1,0},{0,0}}
        };
        std::vector<uint16_t> cubeIdx;
        for (uint16_t f = 0; f < 6; ++f) {
            const uint16_t b = f * 4;
            cubeIdx.insert(cubeIdx.end(), {uint16_t(b),uint16_t(b+1),uint16_t(b+2),uint16_t(b),uint16_t(b+2),uint16_t(b+3)});
        }
        cube_ = UploadMesh(cube, cubeIdx);

        const std::vector<Vertex> quad = {
            {{-0.5f,0,0},{0,0,-1},{0,1}},
            {{ 0.5f,0,0},{0,0,-1},{1,1}},
            {{ 0.5f,1,0},{0,0,-1},{1,0}},
            {{-0.5f,1,0},{0,0,-1},{0,0}}
        };
        const std::vector<uint16_t> qidx = {0,2,1,0,3,2};
        quad_ = UploadMesh(quad, qidx);
    }

    Mesh UploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint16_t>& indices) {
        Mesh m;
        const UINT vbSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        const UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint16_t));
        auto upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto vbDesc = BufferDesc(vbSize);
        auto ibDesc = BufferDesc(ibSize);
        ThrowIfFailed(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &vbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.vb)));
        ThrowIfFailed(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &ibDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.ib)));
        void* p = nullptr;
        D3D12_RANGE noRead{0,0};
        ThrowIfFailed(m.vb->Map(0, &noRead, &p));
        std::memcpy(p, vertices.data(), vbSize);
        m.vb->Unmap(0, nullptr);
        ThrowIfFailed(m.ib->Map(0, &noRead, &p));
        std::memcpy(p, indices.data(), ibSize);
        m.ib->Unmap(0, nullptr);
        m.vbv = {m.vb->GetGPUVirtualAddress(), vbSize, sizeof(Vertex)};
        m.ibv = {m.ib->GetGPUVirtualAddress(), ibSize, DXGI_FORMAT_R16_UINT};
        m.indexCount = static_cast<UINT>(indices.size());
        return m;
    }

    void CreateTextures() {
        constexpr int S = 64;
        for (UINT t = 0; t < kTextureCount; ++t) {
            std::vector<uint32_t> p(S*S);
            for (int y = 0; y < S; ++y) {
                for (int x = 0; x < S; ++x) {
                    const uint32_t h = Hash2(x,y, 99u + t*177u);
                    const int n = int(h & 15u) - 7;
                    uint32_t c = 0xffffffffu;
                    if (t == 0) { // grass
                        c = RGBA(uint8_t(53 + n), uint8_t(112 + n*2), uint8_t(48 + n), 255);
                    } else if (t == 1) { // asphalt
                        const int base = 54 + n;
                        c = RGBA(uint8_t(base), uint8_t(base+2), uint8_t(base+4), 255);
                    } else if (t == 2) { // concrete
                        const int base = 152 + n;
                        const bool seam = (x % 32 == 0 || y % 32 == 0);
                        c = seam ? RGBA(112,114,112) : RGBA(uint8_t(base),uint8_t(base),uint8_t(base-3));
                    } else if (t == 3) { // building wall + windows
                        const bool mortar = (x % 16 == 0 || y % 16 == 0);
                        const int lx = x % 16, ly = y % 16;
                        const bool window = lx >= 4 && lx <= 11 && ly >= 4 && ly <= 11;
                        if (mortar) c = RGBA(108,91,75);
                        else if (window) c = RGBA(35,56,68);
                        else c = RGBA(uint8_t(169+n),uint8_t(139+n),uint8_t(108+n));
                    } else if (t == 4) { // roof / metal
                        const int base = 107 + n;
                        const bool seam = (x % 8 == 0);
                        c = seam ? RGBA(72,76,79) : RGBA(uint8_t(base),uint8_t(base+4),uint8_t(base+5));
                    } else if (t == 5) { // car paint
                        const bool highlight = y < 16;
                        c = highlight ? RGBA(195,48,41) : RGBA(139,27,29);
                    } else if (t == 6) { // player sprite
                        c = RGBA(0,0,0,0);
                        const float dx = float(x - 32), dy = float(y - 13);
                        if (dx*dx + dy*dy < 72) c = RGBA(222,177,135,255);
                        if (x >= 23 && x <= 41 && y >= 21 && y <= 44) c = RGBA(44,83,145,255);
                        if (x >= 22 && x <= 29 && y >= 43 && y <= 62) c = RGBA(35,37,43,255);
                        if (x >= 35 && x <= 42 && y >= 43 && y <= 62) c = RGBA(35,37,43,255);
                    } else if (t == 7) { // tree sprite
                        c = RGBA(0,0,0,0);
                        if (x >= 28 && x <= 35 && y >= 34) c = RGBA(91,58,29,255);
                        const float dx = float(x - 32), dy = float(y - 24);
                        const float rr = dx*dx + dy*dy;
                        if (rr < 430 || ((x-20)*(x-20)+(y-29)*(y-29) < 190) || ((x-44)*(x-44)+(y-29)*(y-29) < 190))
                            c = RGBA(uint8_t(34+n/2), uint8_t(116+n*2), uint8_t(46+n), 255);
                    } else { // lane paint
                        c = RGBA(224,184,45,255);
                    }
                    p[y*S+x] = c;
                }
            }
            CreateTexture(t, S, S, p);
        }
    }

    void CreateTexture(UINT slot, UINT w, UINT h, const std::vector<uint32_t>& pixels) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        auto def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> tex;
        ThrowIfFailed(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowSize = 0, uploadSize = 0;
        device_->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &uploadSize);
        auto uploadProps = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufferDesc(uploadSize);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(device_->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead{0,0};
        ThrowIfFailed(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels.data());
        for (UINT y = 0; y < h; ++y) {
            std::memcpy(mapped + fp.Offset + UINT64(y) * fp.Footprint.RowPitch,
                        src + UINT64(y) * w * 4, UINT64(w) * 4);
        }
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = fp;
        commandList_->CopyTextureRegion(&dst, 0,0,0, &srcLoc, nullptr);
        auto barrier = Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(tex.Get(), &srv, CpuSrv(slot));

        textures_[slot] = tex;
        textureUploads_.push_back(upload);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(UINT index) const {
        auto h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(index) * srvStride_;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv(UINT index) const {
        auto h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(index) * srvStride_;
        return h;
    }

    Mesh& CurrentMesh(MeshKind kind) {
        return kind == MeshKind::Cube ? cube_ : quad_;
    }

    void BindMesh(MeshKind kind) {
        auto& m = CurrentMesh(kind);
        commandList_->IASetVertexBuffers(0, 1, &m.vbv);
        commandList_->IASetIndexBuffer(&m.ibv);
    }

    void WaitForFrame(UINT frame) {
        const UINT64 v = frameFence_[frame];
        if (v != 0 && fence_->GetCompletedValue() < v) {
            ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_));
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    void WaitGpu() {
        const UINT64 v = ++fenceValue_;
        ThrowIfFailed(queue_->Signal(fence_.Get(), v));
        if (fence_->GetCompletedValue() < v) {
            ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_));
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_, srvHeap_;
    UINT rtvStride_ = 0, dsvStride_ = 0, srvStride_ = 0;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> backBuffers_;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> allocators_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12Resource> depth_, shadowMap_;
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> mainPso_, shadowPso_;
    Mesh cube_, quad_;
    std::array<ComPtr<ID3D12Resource>, kTextureCount> textures_;
    std::vector<ComPtr<ID3D12Resource>> textureUploads_;
    ComPtr<ID3D12Resource> constantBuffer_;
    uint8_t* cbMapped_ = nullptr;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    std::array<UINT64, kFrameCount> frameFence_{};
};

class Game {
public:
    void Init(HWND hwnd) {
        hwnd_ = hwnd;
        renderer_.Init(hwnd);
        BuildCity();
        last_ = std::chrono::steady_clock::now();
    }

    void OnMouseWheel(short delta) {
        cameraDistance_ = std::clamp(cameraDistance_ - float(delta) / 120.0f * 4.0f, 18.0f, 72.0f);
    }

    void Tick() {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_).count();
        last_ = now;
        dt = std::min(dt, 0.05f);

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            PostMessage(hwnd_, WM_CLOSE, 0, 0);
            return;
        }

        Update(dt);
        Render();
    }

private:
    bool Key(int vk) const { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    bool CanMove(float x, float z, float radius) const {
        if (x < -112 || x > 112 || z < -112 || z > 112) return false;
        for (const auto& c : colliders_) {
            const float nx = std::clamp(x, c.minX, c.maxX);
            const float nz = std::clamp(z, c.minZ, c.maxZ);
            const float dx = x - nx, dz = z - nz;
            if (dx*dx + dz*dz < radius*radius) return false;
        }
        return true;
    }

    void Update(float dt) {
        if (Key('Q')) cameraYaw_ -= 1.25f * dt;
        if (Key('R')) cameraYaw_ += 1.25f * dt;

        const bool e = Key('E');
        if (e && !ePrev_) {
            if (inCar_) {
                inCar_ = false;
                const float sideX = std::cos(carYaw_) * 2.1f;
                const float sideZ = -std::sin(carYaw_) * 2.1f;
                playerX_ = carX_ + sideX;
                playerZ_ = carZ_ + sideZ;
            } else {
                const float dx = playerX_ - carX_;
                const float dz = playerZ_ - carZ_;
                if (dx*dx + dz*dz < 12.0f) inCar_ = true;
            }
        }
        ePrev_ = e;

        if (inCar_) {
            const float throttle = (Key('W') ? 1.0f : 0.0f) - (Key('S') ? 1.0f : 0.0f);
            const float steer = (Key('D') ? 1.0f : 0.0f) - (Key('A') ? 1.0f : 0.0f);
            carSpeed_ += throttle * 19.0f * dt;
            carSpeed_ *= std::pow(0.26f, dt);
            carSpeed_ = std::clamp(carSpeed_, -8.0f, 22.0f);
            if (std::abs(carSpeed_) > 0.3f) carYaw_ += steer * 1.8f * dt * (carSpeed_ >= 0 ? 1.0f : -1.0f);

            const float nx = carX_ + std::sin(carYaw_) * carSpeed_ * dt;
            const float nz = carZ_ + std::cos(carYaw_) * carSpeed_ * dt;
            if (CanMove(nx, nz, 1.25f)) {
                carX_ = nx; carZ_ = nz;
            } else {
                carSpeed_ *= -0.18f;
            }
        } else {
            const float f = (Key('W') ? 1.0f : 0.0f) - (Key('S') ? 1.0f : 0.0f);
            const float r = (Key('D') ? 1.0f : 0.0f) - (Key('A') ? 1.0f : 0.0f);
            float dx = -std::sin(cameraYaw_) * f + std::cos(cameraYaw_) * r;
            float dz = -std::cos(cameraYaw_) * f - std::sin(cameraYaw_) * r;
            const float len = std::sqrt(dx*dx + dz*dz);
            if (len > 0.001f) {
                dx /= len; dz /= len;
                const float speed = Key(VK_SHIFT) ? 9.0f : 5.4f;
                const float nx = playerX_ + dx * speed * dt;
                const float nz = playerZ_ + dz * speed * dt;
                if (CanMove(nx, playerZ_, 0.45f)) playerX_ = nx;
                if (CanMove(playerX_, nz, 0.45f)) playerZ_ = nz;
            }
        }
    }

    void Render() {
        draws_ = staticDraws_;

        // Trees are crossed sprite planes: visually 2D art living in 3D space.
        for (const auto& t : treeBases_) {
            DrawItem a{MeshKind::Quad, t, {4.4f,6.3f,1.0f}, 0.0f, 7, false};
            DrawItem b = a;
            b.yaw = XM_PIDIV2;
            draws_.push_back(a);
            draws_.push_back(b);
        }

        if (!inCar_) {
            DrawItem player{MeshKind::Quad, {playerX_,0.18f,playerZ_}, {1.35f,2.35f,1.0f}, cameraYaw_ + XM_PI, 6, false};
            draws_.push_back(player);
        }

        AddCarPart({carX_,0.55f,carZ_}, {1.9f,0.65f,3.9f}, 0.0f, 5);
        AddCarLocal(0.0f,0.95f,-0.15f, {1.55f,0.65f,1.9f}, 4);
        AddCarLocal(-0.92f,0.30f, 1.15f, {0.38f,0.52f,0.72f}, 1);
        AddCarLocal( 0.92f,0.30f, 1.15f, {0.38f,0.52f,0.72f}, 1);
        AddCarLocal(-0.92f,0.30f,-1.15f, {0.38f,0.52f,0.72f}, 1);
        AddCarLocal( 0.92f,0.30f,-1.15f, {0.38f,0.52f,0.72f}, 1);

        const float tx = inCar_ ? carX_ : playerX_;
        const float tz = inCar_ ? carZ_ : playerZ_;
        const float camX = tx + std::sin(cameraYaw_) * cameraDistance_;
        const float camZ = tz + std::cos(cameraYaw_) * cameraDistance_;
        const float camY = 28.0f + cameraDistance_ * 0.34f;

        XMVECTOR eye = XMVectorSet(camX, camY, camZ, 1);
        XMVECTOR at = XMVectorSet(tx, 0.9f, tz, 1);
        XMMATRIX view = XMMatrixLookAtLH(eye, at, XMVectorSet(0,1,0,0));
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(52.0f), float(kWidth)/float(kHeight), 0.1f, 420.0f);
        XMMATRIX viewProj = view * proj;

        const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(-0.48f,-0.82f,-0.31f,0));
        const XMVECTOR lightPos = XMVectorSubtract(XMVectorSet(tx,0,tz,1), XMVectorScale(lightDir, 115.0f));
        XMMATRIX lightView = XMMatrixLookAtLH(lightPos, XMVectorSet(tx,0,tz,1), XMVectorSet(0,1,0,0));
        XMMATRIX lightProj = XMMatrixOrthographicLH(190.0f,190.0f,1.0f,300.0f);
        renderer_.Render(draws_, viewProj, lightView * lightProj);
    }

    void AddCarPart(XMFLOAT3 pos, XMFLOAT3 scale, float yawOffset, UINT tex) {
        DrawItem d;
        d.mesh = MeshKind::Cube;
        d.pos = pos;
        d.scale = scale;
        d.yaw = carYaw_ + yawOffset;
        d.texture = tex;
        d.castsShadow = true;
        draws_.push_back(d);
    }

    void AddCarLocal(float lx, float ly, float lz, XMFLOAT3 scale, UINT tex) {
        const float s = std::sin(carYaw_), c = std::cos(carYaw_);
        XMFLOAT3 p{
            carX_ + lx*c + lz*s,
            ly,
            carZ_ - lx*s + lz*c
        };
        AddCarPart(p, scale, 0.0f, tex);
    }

    void BuildCity() {
        staticDraws_.clear();
        colliders_.clear();
        treeBases_.clear();

        // Grass base.
        staticDraws_.push_back({MeshKind::Cube,{0,-0.18f,0},{240,0.30f,240},0,0,true});

        // Grid roads.
        for (int k = -3; k <= 3; ++k) {
            const float p = float(k * 32);
            staticDraws_.push_back({MeshKind::Cube,{p,0.02f,0},{8.0f,0.12f,224.0f},0,1,true});
            staticDraws_.push_back({MeshKind::Cube,{0,0.025f,p},{224.0f,0.13f,8.0f},0,1,true});

            // Dashed center markings, real 3D geometry just above asphalt.
            for (int j = -10; j <= 10; ++j) {
                if ((j & 1) == 0) continue;
                const float q = float(j * 10);
                staticDraws_.push_back({MeshKind::Cube,{p,0.105f,q},{0.16f,0.035f,4.2f},0,8,false});
                staticDraws_.push_back({MeshKind::Cube,{q,0.11f,p},{4.2f,0.035f,0.16f},0,8,false});
            }
        }

        // Six by six blocks; each block gets four independent buildings.
        for (int bz = -3; bz < 3; ++bz) {
            for (int bx = -3; bx < 3; ++bx) {
                const float cx = float(bx * 32 + 16);
                const float cz = float(bz * 32 + 16);
                staticDraws_.push_back({MeshKind::Cube,{cx,0.14f,cz},{23.0f,0.25f,23.0f},0,2,true});

                for (int iz = 0; iz < 2; ++iz) {
                    for (int ix = 0; ix < 2; ++ix) {
                        const uint32_t h = Hash2(bx*7+ix,bz*11+iz,0xa314u);
                        const float px = cx + (ix ? 5.8f : -5.8f);
                        const float pz = cz + (iz ? 5.8f : -5.8f);
                        const float sx = 8.0f + float((h >> 1) & 3u);
                        const float sz = 8.0f + float((h >> 4) & 3u);
                        const float sy = 8.0f + float((h >> 7) % 19u);
                        staticDraws_.push_back({MeshKind::Cube,{px,0.28f + sy*0.5f,pz},{sx,sy,sz},0,3,true});
                        staticDraws_.push_back({MeshKind::Cube,{px,0.30f + sy,pz},{sx*0.94f,0.30f,sz*0.94f},0,4,true});
                        colliders_.push_back({px-sx*0.5f-0.15f,px+sx*0.5f+0.15f,pz-sz*0.5f-0.15f,pz+sz*0.5f+0.15f});
                    }
                }

                treeBases_.push_back({cx-10.2f,0.22f,cz});
                treeBases_.push_back({cx+10.2f,0.22f,cz});
                treeBases_.push_back({cx,0.22f,cz-10.2f});
                treeBases_.push_back({cx,0.22f,cz+10.2f});
            }
        }

        // A central plaza instead of buildings at the origin-adjacent corner.
        // Decorative fountain/pedestal on a free road island.
        staticDraws_.push_back({MeshKind::Cube,{0,0.36f,0},{3.0f,0.7f,3.0f},0,2,true});
    }

    HWND hwnd_ = nullptr;
    Renderer renderer_;
    std::vector<DrawItem> staticDraws_;
    std::vector<DrawItem> draws_;
    std::vector<Collider> colliders_;
    std::vector<XMFLOAT3> treeBases_;

    float playerX_ = -2.0f, playerZ_ = -1.0f;
    float carX_ = 4.5f, carZ_ = 0.0f;
    float carYaw_ = XM_PIDIV2;
    float carSpeed_ = 0.0f;
    bool inCar_ = false;
    bool ePrev_ = false;

    float cameraYaw_ = 0.72f;
    float cameraDistance_ = 42.0f;
    std::chrono::steady_clock::time_point last_;
};

static Game* gGame = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEWHEEL:
            if (gGame) gGame->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"CarDX12Window";
        RegisterClassExW(&wc);

        RECT r{0,0,LONG(kWidth),LONG(kHeight)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowExW(
            0, wc.lpszClassName,
            L"Car — DirectX 12 Open City | WASD mover/dirigir | E entrar/sair | Q/R câmera | roda zoom",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
            nullptr, nullptr, instance, nullptr);
        if (!hwnd) throw std::runtime_error("Could not create window.");

        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);

        Game game;
        gGame = &game;
        game.Init(hwnd);

        MSG msg{};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                game.Tick();
            }
        }
        gGame = nullptr;
        return 0;
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Car DX12 - Fatal Error", MB_OK | MB_ICONERROR);
        return -1;
    }
}
