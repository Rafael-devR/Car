#pragma once
#include <DirectXMath.h>
#include <cstdint>

enum class MeshKind : std::uint8_t {
    Cube,
    Cylinder,
    Wedge,
    QuadXZ,
    QuadXY
};

struct RenderItem {
    MeshKind mesh = MeshKind::Cube;
    DirectX::XMFLOAT3 pos{0,0,0};
    DirectX::XMFLOAT3 scale{1,1,1};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    std::uint32_t texture = 0;
    DirectX::XMFLOAT4 tint{1,1,1,1};
    bool castsShadow = true;
    bool unlit = false;
    bool screenSpace = false;
};

struct AABB2 {
    float minX = 0;
    float maxX = 0;
    float minZ = 0;
    float maxZ = 0;
};
