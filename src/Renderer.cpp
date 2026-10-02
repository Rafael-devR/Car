#include "Renderer.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("DirectX 12 call failed.");
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

std::uint32_t RGBA(int r, int g, int b, int a = 255) {
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    a = std::clamp(a, 0, 255);
    return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16) | (std::uint32_t(a) << 24);
}

std::uint32_t Hash2(int x, int y, std::uint32_t seed) {
    std::uint32_t h = std::uint32_t(x) * 0x8da6b343u ^ std::uint32_t(y) * 0xd8163841u ^ seed;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return h;
}

const char* ShaderSource = R"HLSL(
cbuffer ObjectCB : register(b0)
{
    float4x4 World;
    float4x4 ViewProj;
    float4x4 LightViewProj;
    float4 Tint;
    float4 LightDirAmbient;
    float4 Params;
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

float Visibility(float4 sp)
{
    float3 p = sp.xyz / max(sp.w, 0.00001);
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x <= 0 || uv.x >= 1 || uv.y <= 0 || uv.y >= 1 || p.z <= 0 || p.z >= 1)
        return 1.0;
    return ShadowMap.SampleCmpLevelZero(ShadowSampler, uv, p.z - 0.0013);
}

float4 PSMain(VSOut input) : SV_TARGET
{
    float4 tex = Albedo.Sample(LinearWrap, input.uv) * Tint;
    clip(tex.a - 0.08);

    if (Params.x > 0.5)
        return tex;

    float3 n = normalize(input.normal);
    float ndl = saturate(dot(n, -normalize(LightDirAmbient.xyz)));
    float vis = Visibility(input.shadowPos);
    float light = LightDirAmbient.w + ndl * vis * 0.72;
    float3 c = tex.rgb * light;
    c += tex.rgb * 0.10;
    c = saturate(c);
    return float4(c, tex.a);
}

float4 VSShadow(VSIn input) : SV_POSITION
{
    float4 wp = mul(float4(input.pos, 1.0), World);
    return mul(wp, LightViewProj);
}
)HLSL";
}

Renderer::~Renderer() {
    if (queue_ && fence_) {
        try { WaitGpu(); } catch (...) {}
    }
    if (constantBuffer_ && cbMapped_) constantBuffer_->Unmap(0, nullptr);
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

void Renderer::Init(HWND hwnd) {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
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
    sd.Width = Width;
    sd.Height = Height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FrameCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    ThrowIfFailed(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &swap));
    ThrowIfFailed(swap.As(&swapChain_));
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = FrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)));
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i) {
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
    srvDesc.NumDescriptors = 40;
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
    const UINT64 cbSize = UINT64(FrameCount) * MaxDraws * cbStride;
    auto upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = BufferDesc(cbSize);
    ThrowIfFailed(device_->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&constantBuffer_)));

    D3D12_RANGE noRead{0,0};
    ThrowIfFailed(constantBuffer_->Map(0, &noRead, reinterpret_cast<void**>(&cbMapped_)));

    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) throw std::runtime_error("Could not create GPU fence event.");

    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* lists[] = { commandList_.Get() };
    queue_->ExecuteCommandLists(1, lists);
    WaitGpu();
    textureUploads_.clear();
}

void Renderer::CreateDepthResources() {
    auto def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC depth{};
    depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth.Width = Width;
    depth.Height = Height;
    depth.DepthOrArraySize = 1;
    depth.MipLevels = 1;
    depth.Format = DXGI_FORMAT_D32_FLOAT;
    depth.SampleDesc.Count = 1;
    depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(device_->CreateCommittedResource(
        &def, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear, IID_PPV_ARGS(&depth_)));
    auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateDepthStencilView(depth_.Get(), nullptr, dsv);

    D3D12_RESOURCE_DESC shadow = depth;
    shadow.Width = ShadowSize;
    shadow.Height = ShadowSize;
    shadow.Format = DXGI_FORMAT_R32_TYPELESS;
    ThrowIfFailed(device_->CreateCommittedResource(
        &def, D3D12_HEAP_FLAG_NONE, &shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear, IID_PPV_ARGS(&shadowMap_)));

    D3D12_DEPTH_STENCIL_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_D32_FLOAT;
    sd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    auto shadowDsv = dsv;
    shadowDsv.ptr += dsvStride_;
    device_->CreateDepthStencilView(shadowMap_.Get(), &sd, shadowDsv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowMap_.Get(), &srv, CpuSrv(ShadowSrvIndex));
}

ComPtr<ID3DBlob> Renderer::Compile(const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> shader, errors;
    HRESULT hr = D3DCompile(ShaderSource, std::strlen(ShaderSource), "CarShaders.hlsl",
                            nullptr, nullptr, entry, target, flags, 0, &shader, &errors);
    if (FAILED(hr)) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    return shader;
}

void Renderer::CreateRootSignatureAndPipelines() {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 1;

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
    ThrowIfFailed(device_->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig_)));

    auto vs = Compile("VSMain", "vs_5_1");
    auto ps = Compile("PSMain", "ps_5_1");
    auto shadowVs = Compile("VSShadow", "vs_5_1");

    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,pos),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,normal),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(Vertex,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;

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
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&worldPso_)));

    auto shadowPso = pso;
    shadowPso.VS = {shadowVs->GetBufferPointer(), shadowVs->GetBufferSize()};
    shadowPso.PS = {nullptr,0};
    shadowPso.NumRenderTargets = 0;
    shadowPso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    shadowPso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    shadowPso.RasterizerState.DepthBias = 900;
    shadowPso.RasterizerState.SlopeScaledDepthBias = 1.7f;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&shadowPso, IID_PPV_ARGS(&shadowPso_)));

    auto hudPso = pso;
    hudPso.DepthStencilState.DepthEnable = FALSE;
    hudPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&hudPso, IID_PPV_ARGS(&hudPso_)));
}

void Renderer::CreateMeshes() {
    const std::vector<Vertex> cube = {
        {{-0.5f,-0.5f,-0.5f},{0,0,-1},{0,1}}, {{0.5f,-0.5f,-0.5f},{0,0,-1},{1,1}}, {{0.5f,0.5f,-0.5f},{0,0,-1},{1,0}}, {{-0.5f,0.5f,-0.5f},{0,0,-1},{0,0}},
        {{-0.5f,-0.5f,0.5f},{0,0,1},{0,1}}, {{0.5f,-0.5f,0.5f},{0,0,1},{1,1}}, {{0.5f,0.5f,0.5f},{0,0,1},{1,0}}, {{-0.5f,0.5f,0.5f},{0,0,1},{0,0}},
        {{-0.5f,-0.5f,-0.5f},{-1,0,0},{0,1}}, {{-0.5f,-0.5f,0.5f},{-1,0,0},{1,1}}, {{-0.5f,0.5f,0.5f},{-1,0,0},{1,0}}, {{-0.5f,0.5f,-0.5f},{-1,0,0},{0,0}},
        {{0.5f,-0.5f,0.5f},{1,0,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{1,0,0},{1,1}}, {{0.5f,0.5f,-0.5f},{1,0,0},{1,0}}, {{0.5f,0.5f,0.5f},{1,0,0},{0,0}},
        {{-0.5f,0.5f,0.5f},{0,1,0},{0,1}}, {{0.5f,0.5f,0.5f},{0,1,0},{1,1}}, {{0.5f,0.5f,-0.5f},{0,1,0},{1,0}}, {{-0.5f,0.5f,-0.5f},{0,1,0},{0,0}},
        {{-0.5f,-0.5f,-0.5f},{0,-1,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{0,-1,0},{1,1}}, {{0.5f,-0.5f,0.5f},{0,-1,0},{1,0}}, {{-0.5f,-0.5f,0.5f},{0,-1,0},{0,0}}
    };
    std::vector<std::uint16_t> ci;
    for (std::uint16_t f = 0; f < 6; ++f) {
        std::uint16_t b = f * 4;
        ci.insert(ci.end(), {b,std::uint16_t(b+1),std::uint16_t(b+2),b,std::uint16_t(b+2),std::uint16_t(b+3)});
    }
    cube_ = UploadMesh(cube, ci);

    const std::vector<Vertex> xz = {
        {{-0.5f,0,-0.5f},{0,1,0},{0,1}}, {{0.5f,0,-0.5f},{0,1,0},{1,1}},
        {{0.5f,0,0.5f},{0,1,0},{1,0}}, {{-0.5f,0,0.5f},{0,1,0},{0,0}}
    };
    quadXZ_ = UploadMesh(xz, {0,1,2,0,2,3});

    const std::vector<Vertex> xy = {
        {{-0.5f,-0.5f,0},{0,0,-1},{0,1}}, {{0.5f,-0.5f,0},{0,0,-1},{1,1}},
        {{0.5f,0.5f,0},{0,0,-1},{1,0}}, {{-0.5f,0.5f,0},{0,0,-1},{0,0}}
    };
    quadXY_ = UploadMesh(xy, {0,1,2,0,2,3});
}

Renderer::Mesh Renderer::UploadMesh(const std::vector<Vertex>& v, const std::vector<std::uint16_t>& i) {
    Mesh m;
    const UINT vbSize = UINT(v.size() * sizeof(Vertex));
    const UINT ibSize = UINT(i.size() * sizeof(std::uint16_t));
    auto upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto vb = BufferDesc(vbSize);
    auto ib = BufferDesc(ibSize);

    ThrowIfFailed(device_->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&vb,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m.vb)));
    ThrowIfFailed(device_->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&ib,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m.ib)));

    D3D12_RANGE noRead{0,0};
    void* p = nullptr;
    ThrowIfFailed(m.vb->Map(0,&noRead,&p));
    std::memcpy(p,v.data(),vbSize);
    m.vb->Unmap(0,nullptr);
    ThrowIfFailed(m.ib->Map(0,&noRead,&p));
    std::memcpy(p,i.data(),ibSize);
    m.ib->Unmap(0,nullptr);

    m.vbv = {m.vb->GetGPUVirtualAddress(),vbSize,sizeof(Vertex)};
    m.ibv = {m.ib->GetGPUVirtualAddress(),ibSize,DXGI_FORMAT_R16_UINT};
    m.indexCount = UINT(i.size());
    return m;
}

void Renderer::CreateTextures() {
    constexpr int S = 128;

    auto make = [&](UINT slot, auto painter) {
        std::vector<std::uint32_t> p(S*S, RGBA(255,255,255,255));
        painter(p);
        CreateTexture(slot,S,S,p);
    };
    auto set = [&](std::vector<std::uint32_t>& p, int x, int y, std::uint32_t c) {
        if (x >= 0 && x < S && y >= 0 && y < S) p[y*S+x] = c;
    };
    auto rect = [&](std::vector<std::uint32_t>& p, int x0,int y0,int x1,int y1,std::uint32_t c) {
        x0=std::max(0,x0); y0=std::max(0,y0); x1=std::min(S-1,x1); y1=std::min(S-1,y1);
        for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) p[y*S+x]=c;
    };
    auto circle = [&](std::vector<std::uint32_t>& p, int cx,int cy,int r,std::uint32_t c) {
        for(int y=cy-r;y<=cy+r;++y) for(int x=cx-r;x<=cx+r;++x)
            if((x-cx)*(x-cx)+(y-cy)*(y-cy)<=r*r) set(p,x,y,c);
    };

    make(0,[&](auto& p){ // asphalt
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,11)&31)-15;
            p[y*S+x]=RGBA(61+n/3,69+n/3,72+n/3);
            if((Hash2(x,y,91)&255)<5) p[y*S+x]=RGBA(92,96,95);
        }
    });
    make(1,[&](auto& p){ // sidewalk
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,17)&15)-7;
            bool seam=(x%32==0)||(y%32==0);
            p[y*S+x]=seam?RGBA(116,120,116):RGBA(177+n,179+n,168+n);
        }
    });
    make(2,[&](auto& p){ // grass
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,31)&31)-15;
            p[y*S+x]=RGBA(58+n/3,122+n,58+n/4);
        }
    });
    make(3,[&](auto& p){ // warm brick facade
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,41)&15)-7;
            bool mortar=(y%24<2)||(x%32<2 && (y/24)%2==0)||(((x+16)%32)<2 && (y/24)%2==1);
            bool win=(x%32>=7&&x%32<=24&&y%24>=5&&y%24<=17);
            if(mortar) p[y*S+x]=RGBA(104,91,77);
            else if(win) p[y*S+x]=RGBA(29,51,67);
            else p[y*S+x]=RGBA(177+n,139+n/2,105+n/2);
        }
    });
    make(4,[&](auto& p){ // cool facade
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,51)&15)-7;
            bool band=(y%28<3);
            bool win=(x%32>=6&&x%32<=25&&y%28>=7&&y%28<=20);
            if(band) p[y*S+x]=RGBA(111,119,122);
            else if(win) p[y*S+x]=RGBA(38,67,80);
            else p[y*S+x]=RGBA(154+n,165+n,164+n);
        }
    });
    make(5,[&](auto& p){ // roof
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,61)&15)-7;
            bool seam=(x%16<2);
            p[y*S+x]=seam?RGBA(69,73,75):RGBA(99+n,104+n,105+n);
        }
    });

    auto person = [&](UINT slot, std::uint32_t shirt, std::uint32_t pants, std::uint32_t skin) {
        make(slot,[&](auto& p){
            std::fill(p.begin(),p.end(),RGBA(0,0,0,0));
            circle(p,64,33,18,skin);
            rect(p,47,48,81,87,shirt);
            rect(p,45,57,52,92,skin);
            rect(p,76,57,83,92,skin);
            rect(p,49,86,62,119,pants);
            rect(p,67,86,80,119,pants);
            rect(p,46,112,62,123,RGBA(35,35,38));
            rect(p,67,112,83,123,RGBA(35,35,38));
        });
    };
    person(6,RGBA(36,113,205),RGBA(38,42,50),RGBA(224,177,133));
    person(7,RGBA(217,168,43),RGBA(61,64,73),RGBA(192,137,96));
    person(8,RGBA(45,162,100),RGBA(61,47,45),RGBA(235,193,155));
    person(9,RGBA(27,65,139),RGBA(23,30,49),RGBA(214,164,126));

    auto car = [&](UINT slot, std::uint32_t body, bool police, bool taxi) {
        make(slot,[&](auto& p){
            std::fill(p.begin(),p.end(),RGBA(0,0,0,0));
            rect(p,31,10,96,117,RGBA(20,22,24,220));
            rect(p,26,26,101,103,body);
            rect(p,31,19,96,108,body);
            rect(p,38,32,89,56,RGBA(43,67,79));
            rect(p,38,72,89,97,RGBA(40,61,70));
            rect(p,31,58,96,69,body);
            rect(p,28,27,35,48,RGBA(30,30,33));
            rect(p,92,27,99,48,RGBA(30,30,33));
            rect(p,28,80,35,101,RGBA(30,30,33));
            rect(p,92,80,99,101,RGBA(30,30,33));
            rect(p,39,11,54,17,RGBA(246,241,196));
            rect(p,74,11,89,17,RGBA(246,241,196));
            rect(p,39,110,54,117,RGBA(197,37,32));
            rect(p,74,110,89,117,RGBA(197,37,32));
            if(police) {
                rect(p,27,55,100,72,RGBA(236,239,241));
                rect(p,48,58,62,68,RGBA(220,30,36));
                rect(p,65,58,79,68,RGBA(31,98,213));
            }
            if(taxi) {
                rect(p,47,57,80,70,RGBA(31,31,31));
                rect(p,54,59,73,67,RGBA(238,190,40));
            }
        });
    };
    car(10,RGBA(190,45,42),false,false);
    car(11,RGBA(42,86,176),false,false);
    car(12,RGBA(225,170,34),false,true);
    car(13,RGBA(38,70,132),true,false);
    car(21,RGBA(44,142,124),false,false);
    car(22,RGBA(115,72,154),false,false);
    car(23,RGBA(196,96,37),false,false);

    make(14,[&](auto& p){ // leaves
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,71)&31)-15;
            p[y*S+x]=RGBA(48+n/3,128+n,55+n/3);
        }
    });
    make(15,[&](auto& p){ // trunk
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,81)&15)-7;
            p[y*S+x]=RGBA(101+n,69+n/2,37);
        }
    });
    make(16,[&](auto& p){ std::fill(p.begin(),p.end(),RGBA(232,193,53)); });
    make(17,[&](auto& p){ std::fill(p.begin(),p.end(),RGBA(255,201,45)); });
    make(18,[&](auto& p){ std::fill(p.begin(),p.end(),RGBA(255,255,255)); });
    make(19,[&](auto& p){ std::fill(p.begin(),p.end(),RGBA(196,42,45)); });
    make(20,[&](auto& p){ std::fill(p.begin(),p.end(),RGBA(48,126,207)); });

    make(24,[&](auto& p){ // dark glass / shop
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            bool frame=(x%32<3)||(y%32<3);
            p[y*S+x]=frame?RGBA(71,74,70):RGBA(29,55,61);
        }
    });
    make(25,[&](auto& p){ // pale stone
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int n=int(Hash2(x,y,101)&15)-7;
            bool seam=(x%32<2)||(y%24<2);
            p[y*S+x]=seam?RGBA(145,137,122):RGBA(198+n,189+n,168+n);
        }
    });
    make(26,[&](auto& p){ // dark road curb
        for(int y=0;y<S;++y) for(int x=0;x<S;++x) {
            int v=((x/16+y/16)&1)?128:154;
            p[y*S+x]=RGBA(v,v,v);
        }
    });
}

void Renderer::CreateTexture(UINT slot, UINT w, UINT h, const std::vector<std::uint32_t>& pixels) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;

    auto def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    ThrowIfFailed(device_->CreateCommittedResource(
        &def,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, uploadSize = 0;
    device_->GetCopyableFootprints(&td,0,1,0,&fp,&rows,&rowSize,&uploadSize);

    auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = BufferDesc(uploadSize);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device_->CreateCommittedResource(
        &up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));

    std::uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{0,0};
    ThrowIfFailed(upload->Map(0,&noRead,reinterpret_cast<void**>(&mapped)));
    const auto* src = reinterpret_cast<const std::uint8_t*>(pixels.data());
    for(UINT y=0;y<h;++y)
        std::memcpy(mapped+fp.Offset+UINT64(y)*fp.Footprint.RowPitch,src+UINT64(y)*w*4,UINT64(w)*4);
    upload->Unmap(0,nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource=tex.Get();
    dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource=upload.Get();
    srcLoc.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint=fp;
    commandList_->CopyTextureRegion(&dst,0,0,0,&srcLoc,nullptr);

    auto b=Transition(tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1,&b);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(tex.Get(),&srv,CpuSrv(slot));

    textures_[slot]=tex;
    textureUploads_.push_back(upload);
}

void Renderer::Render(
    const std::vector<RenderItem>& world,
    const std::vector<RenderItem>& hud,
    const XMMATRIX& viewProj,
    const XMMATRIX& lightViewProj)
{
    const UINT frame=swapChain_->GetCurrentBackBufferIndex();
    WaitForFrame(frame);
    ThrowIfFailed(allocators_[frame]->Reset());
    ThrowIfFailed(commandList_->Reset(allocators_[frame].Get(),nullptr));

    const UINT worldCount=std::min<UINT>(UINT(world.size()),MaxDraws);
    const UINT hudCount=std::min<UINT>(UINT(hud.size()),MaxDraws-worldCount);
    constexpr UINT cbStride=256;
    const UINT64 frameBase=UINT64(frame)*MaxDraws*cbStride;

    auto writeCB=[&](UINT index,const RenderItem& d,bool screen) {
        XMMATRIX worldM;
        XMMATRIX vp;
        if(screen) {
            worldM=XMMatrixScaling(d.scale.x,d.scale.y,1.0f)*XMMatrixTranslation(d.pos.x,d.pos.y,0.0f);
            vp=XMMatrixIdentity();
        } else {
            worldM=XMMatrixScaling(d.scale.x,d.scale.y,d.scale.z)*XMMatrixRotationY(d.yaw)*XMMatrixTranslation(d.pos.x,d.pos.y,d.pos.z);
            vp=viewProj;
        }
        ObjectCB cb{};
        XMStoreFloat4x4(&cb.world,XMMatrixTranspose(worldM));
        XMStoreFloat4x4(&cb.viewProj,XMMatrixTranspose(vp));
        XMStoreFloat4x4(&cb.lightViewProj,XMMatrixTranspose(lightViewProj));
        cb.tint=d.tint;
        cb.lightDirAmbient=XMFLOAT4(-0.52f,-0.81f,-0.27f,0.58f);
        cb.params=XMFLOAT4((d.unlit||screen)?1.0f:0.0f,0,0,0);
        std::memcpy(cbMapped_+frameBase+UINT64(index)*cbStride,&cb,sizeof(cb));
    };
    for(UINT i=0;i<worldCount;++i) writeCB(i,world[i],false);
    for(UINT i=0;i<hudCount;++i) writeCB(worldCount+i,hud[i],true);

    ID3D12DescriptorHeap* heaps[]={srvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1,heaps);
    commandList_->SetGraphicsRootSignature(rootSig_.Get());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto toDepth=Transition(shadowMap_.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList_->ResourceBarrier(1,&toDepth);
    D3D12_VIEWPORT svp{0,0,float(ShadowSize),float(ShadowSize),0,1};
    D3D12_RECT srect{0,0,LONG(ShadowSize),LONG(ShadowSize)};
    commandList_->RSSetViewports(1,&svp);
    commandList_->RSSetScissorRects(1,&srect);
    auto shadowDsv=dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    shadowDsv.ptr+=dsvStride_;
    commandList_->OMSetRenderTargets(0,nullptr,FALSE,&shadowDsv);
    commandList_->ClearDepthStencilView(shadowDsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    commandList_->SetPipelineState(shadowPso_.Get());
    for(UINT i=0;i<worldCount;++i) {
        if(!world[i].castsShadow) continue;
        BindMesh(world[i].mesh);
        commandList_->SetGraphicsRootConstantBufferView(0,constantBuffer_->GetGPUVirtualAddress()+frameBase+UINT64(i)*cbStride);
        commandList_->DrawIndexedInstanced(CurrentMesh(world[i].mesh).indexCount,1,0,0,0);
    }
    auto toSrv=Transition(shadowMap_.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1,&toSrv);

    auto toRT=Transition(backBuffers_[frame].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1,&toRT);

    D3D12_VIEWPORT vp{0,0,float(Width),float(Height),0,1};
    D3D12_RECT rect{0,0,LONG(Width),LONG(Height)};
    commandList_->RSSetViewports(1,&vp);
    commandList_->RSSetScissorRects(1,&rect);

    auto rtv=rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr+=UINT64(frame)*rtvStride_;
    auto dsv=dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    const float clear[]={0.29f,0.45f,0.57f,1.0f};
    commandList_->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
    commandList_->ClearRenderTargetView(rtv,clear,0,nullptr);
    commandList_->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);

    commandList_->SetPipelineState(worldPso_.Get());
    commandList_->SetGraphicsRootDescriptorTable(2,GpuSrv(ShadowSrvIndex));
    for(UINT i=0;i<worldCount;++i) {
        BindMesh(world[i].mesh);
        commandList_->SetGraphicsRootConstantBufferView(0,constantBuffer_->GetGPUVirtualAddress()+frameBase+UINT64(i)*cbStride);
        commandList_->SetGraphicsRootDescriptorTable(1,GpuSrv(std::min<UINT>(world[i].texture,TextureCount-1)));
        commandList_->DrawIndexedInstanced(CurrentMesh(world[i].mesh).indexCount,1,0,0,0);
    }

    commandList_->SetPipelineState(hudPso_.Get());
    for(UINT i=0;i<hudCount;++i) {
        UINT idx=worldCount+i;
        BindMesh(hud[i].mesh);
        commandList_->SetGraphicsRootConstantBufferView(0,constantBuffer_->GetGPUVirtualAddress()+frameBase+UINT64(idx)*cbStride);
        commandList_->SetGraphicsRootDescriptorTable(1,GpuSrv(std::min<UINT>(hud[i].texture,TextureCount-1)));
        commandList_->DrawIndexedInstanced(CurrentMesh(hud[i].mesh).indexCount,1,0,0,0);
    }

    auto toPresent=Transition(backBuffers_[frame].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    commandList_->ResourceBarrier(1,&toPresent);
    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* lists[]={commandList_.Get()};
    queue_->ExecuteCommandLists(1,lists);
    ThrowIfFailed(swapChain_->Present(1,0));

    const UINT64 v=++fenceValue_;
    ThrowIfFailed(queue_->Signal(fence_.Get(),v));
    frameFence_[frame]=v;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::CpuSrv(UINT i) const {
    auto h=srvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr+=UINT64(i)*srvStride_;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::GpuSrv(UINT i) const {
    auto h=srvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr+=UINT64(i)*srvStride_;
    return h;
}
Renderer::Mesh& Renderer::CurrentMesh(MeshKind k) {
    if(k==MeshKind::QuadXZ) return quadXZ_;
    if(k==MeshKind::QuadXY) return quadXY_;
    return cube_;
}
void Renderer::BindMesh(MeshKind k) {
    auto& m=CurrentMesh(k);
    commandList_->IASetVertexBuffers(0,1,&m.vbv);
    commandList_->IASetIndexBuffer(&m.ibv);
}
void Renderer::WaitForFrame(UINT frame) {
    UINT64 v=frameFence_[frame];
    if(v && fence_->GetCompletedValue()<v) {
        ThrowIfFailed(fence_->SetEventOnCompletion(v,fenceEvent_));
        WaitForSingleObject(fenceEvent_,INFINITE);
    }
}
void Renderer::WaitGpu() {
    UINT64 v=++fenceValue_;
    ThrowIfFailed(queue_->Signal(fence_.Get(),v));
    if(fence_->GetCompletedValue()<v) {
        ThrowIfFailed(fence_->SetEventOnCompletion(v,fenceEvent_));
        WaitForSingleObject(fenceEvent_,INFINITE);
    }
}
