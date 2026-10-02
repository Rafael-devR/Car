#pragma once
#include "GameTypes.h"
#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <array>
#include <vector>
#include <cstdint>

class Renderer {
public:
    static constexpr UINT Width = 1280;
    static constexpr UINT Height = 720;

    Renderer() = default;
    ~Renderer();

    void Init(HWND hwnd);
    void Render(
        const std::vector<RenderItem>& world,
        const std::vector<RenderItem>& hud,
        const DirectX::XMMATRIX& viewProj,
        const DirectX::XMMATRIX& lightViewProj);

private:
    struct Vertex {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
    };

    struct Mesh {
        Microsoft::WRL::ComPtr<ID3D12Resource> vb;
        Microsoft::WRL::ComPtr<ID3D12Resource> ib;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW ibv{};
        UINT indexCount = 0;
    };

    struct alignas(16) ObjectCB {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 lightViewProj;
        DirectX::XMFLOAT4 tint;
        DirectX::XMFLOAT4 lightDirAmbient;
        DirectX::XMFLOAT4 params;
    };

    static constexpr UINT FrameCount = 2;
    static constexpr UINT ShadowSize = 2048;
    static constexpr UINT MaxDraws = 4096;
    static constexpr UINT TextureCount = 27;
    static constexpr UINT ShadowSrvIndex = 31;

    void CreateDepthResources();
    void CreateRootSignatureAndPipelines();
    void CreateMeshes();
    void CreateTextures();

    Mesh UploadMesh(const std::vector<Vertex>& vertices, const std::vector<std::uint16_t>& indices);
    void CreateTexture(UINT slot, UINT w, UINT h, const std::vector<std::uint32_t>& pixels);

    Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* target);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv(UINT index) const;
    Mesh& CurrentMesh(MeshKind kind);
    void BindMesh(MeshKind kind);
    void WaitForFrame(UINT frame);
    void WaitGpu();

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    UINT rtvStride_ = 0;
    UINT dsvStride_ = 0;
    UINT srvStride_ = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> backBuffers_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, FrameCount> allocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> shadowMap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> worldPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> hudPso_;

    Mesh cube_;
    Mesh quadXZ_;
    Mesh quadXY_;

    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, TextureCount> textures_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textureUploads_;

    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    std::uint8_t* cbMapped_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    std::array<UINT64, FrameCount> frameFence_{};
};
