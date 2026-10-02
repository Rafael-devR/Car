#pragma once
#include "Renderer.h"
#include <windows.h>
#include <vector>
#include <chrono>
#include <cstdint>

class Game {
public:
    void Init(HWND hwnd);
    void Tick();
    void OnMouseWheel(short delta);

private:
    enum class PedState : std::uint8_t { Wander, Flee, Aggressive, Police, Dead };

    struct Player {
        float x = 5.0f;
        float z = 5.0f;
        float yaw = 0.0f;
        float health = 100.0f;
        float armor = 25.0f;
        int money = 250;
        int ammo = 90;
        int wanted = 0;
        float crimeTimer = 0.0f;
        float shootCooldown = 0.0f;
        int car = -1;
    };

    struct Car {
        float x = 0;
        float z = 0;
        float yaw = 0;
        float speed = 0;
        float cruise = 7.0f;
        float hp = 100.0f;
        float turnCooldown = 0;
        std::uint32_t texture = 10;
        bool police = false;
        bool active = true;
    };

    struct Ped {
        float x = 0;
        float z = 0;
        float yaw = 0;
        float speed = 0;
        float health = 100;
        float decision = 0;
        float shootCooldown = 0;
        float deadTimer = 0;
        std::uint32_t texture = 7;
        std::uint32_t personality = 0;
        PedState state = PedState::Wander;
    };

    struct Bullet {
        float x = 0;
        float z = 0;
        float vx = 0;
        float vz = 0;
        float life = 0;
        bool hostile = false;
    };

    bool Key(int vk) const;
    bool CanMove(float x, float z, float radius) const;
    float DistanceSq(float ax, float az, float bx, float bz) const;
    std::uint32_t RandomU32();
    float Random01();

    void BuildCity();
    void SpawnPopulation();
    void Update(float dt);
    void UpdatePlayer(float dt);
    void UpdateTraffic(float dt);
    void UpdatePeds(float dt);
    void UpdateBullets(float dt);
    void UpdateWanted(float dt);
    void UpdateMission(float dt);
    void Shoot(bool hostile, float x, float z, float yaw);

    void BuildFrame();
    void AddCarDraw(const Car& car, bool playerControlled);
    void AddHudBar(float x, float y, float w, float h, float value,
                   const DirectX::XMFLOAT4& color);
    void AddDigit(int digit, float x, float y, float size, const DirectX::XMFLOAT4& color);
    void AddNumber(int value, float x, float y, float size, const DirectX::XMFLOAT4& color);

    HWND hwnd_ = nullptr;
    Renderer renderer_;
    Player player_;

    std::vector<RenderItem> staticWorld_;
    std::vector<RenderItem> frameWorld_;
    std::vector<RenderItem> hud_;
    std::vector<AABB2> colliders_;
    std::vector<Car> cars_;
    std::vector<Ped> peds_;
    std::vector<Bullet> bullets_;

    float cameraSize_ = 39.0f;
    bool ePrev_ = false;
    bool firePrev_ = false;
    std::uint32_t rng_ = 0x51A7C0DEu;

    bool missionActive_ = false;
    float missionX_ = -86.0f;
    float missionZ_ = 82.0f;
    float missionPulse_ = 0.0f;

    std::chrono::steady_clock::time_point last_{};
};
