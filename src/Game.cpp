#include "Game.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <string>

using namespace DirectX;

namespace {
RenderItem Cube(float x,float y,float z,float sx,float sy,float sz,std::uint32_t tex,
                XMFLOAT4 tint={1,1,1,1},bool shadow=true,bool unlit=false) {
    RenderItem d;
    d.mesh=MeshKind::Cube;
    d.pos={x,y,z};
    d.scale={sx,sy,sz};
    d.texture=tex;
    d.tint=tint;
    d.castsShadow=shadow;
    d.unlit=unlit;
    return d;
}

RenderItem GroundQuad(float x,float y,float z,float sx,float sz,float yaw,std::uint32_t tex,
                      XMFLOAT4 tint={1,1,1,1},bool shadow=false,bool unlit=false) {
    RenderItem d;
    d.mesh=MeshKind::QuadXZ;
    d.pos={x,y,z};
    d.scale={sx,1,sz};
    d.yaw=yaw;
    d.texture=tex;
    d.tint=tint;
    d.castsShadow=shadow;
    d.unlit=unlit;
    return d;
}

RenderItem ScreenQuad(float x,float y,float sx,float sy,XMFLOAT4 tint,std::uint32_t tex=18) {
    RenderItem d;
    d.mesh=MeshKind::QuadXY;
    d.pos={x,y,0};
    d.scale={sx,sy,1};
    d.texture=tex;
    d.tint=tint;
    d.castsShadow=false;
    d.unlit=true;
    d.screenSpace=true;
    return d;
}

float WrapAngle(float a) {
    while(a>XM_PI) a-=XM_2PI;
    while(a<-XM_PI) a+=XM_2PI;
    return a;
}

float Approach(float v,float target,float delta) {
    if(v<target) return std::min(v+delta,target);
    return std::max(v-delta,target);
}
}

void Game::Init(HWND hwnd) {
    hwnd_=hwnd;
    renderer_.Init(hwnd);
    BuildCity();
    SpawnPopulation();
    last_=std::chrono::steady_clock::now();
}

void Game::OnMouseWheel(short delta) {
    cameraSize_=std::clamp(cameraSize_-float(delta)/120.0f*2.0f,24.0f,44.0f);
}

bool Game::Key(int vk) const {
    return (GetAsyncKeyState(vk)&0x8000)!=0;
}

float Game::DistanceSq(float ax,float az,float bx,float bz) const {
    const float dx=ax-bx, dz=az-bz;
    return dx*dx+dz*dz;
}

std::uint32_t Game::RandomU32() {
    rng_^=rng_<<13;
    rng_^=rng_>>17;
    rng_^=rng_<<5;
    return rng_;
}

float Game::Random01() {
    return float(RandomU32()&0x00ffffffu)/float(0x01000000u);
}

bool Game::CanMove(float x,float z,float radius) const {
    if(x<-124||x>124||z<-124||z>124) return false;
    for(const auto& c:colliders_) {
        float nx=std::clamp(x,c.minX,c.maxX);
        float nz=std::clamp(z,c.minZ,c.maxZ);
        float dx=x-nx,dz=z-nz;
        if(dx*dx+dz*dz<radius*radius) return false;
    }
    return true;
}

void Game::BuildCity() {
    staticWorld_.clear();
    colliders_.clear();

    staticWorld_.push_back(Cube(0,-0.25f,0,270,0.45f,270,2));

    constexpr float roadStep=36.0f;
    constexpr float roadWidth=10.0f;
    constexpr float extent=252.0f;

    for(int r=-3;r<=3;++r) {
        float p=float(r)*roadStep;
        staticWorld_.push_back(Cube(p,0.01f,0,roadWidth,0.12f,extent,0));
        staticWorld_.push_back(Cube(0,0.015f,p,extent,0.12f,roadWidth,0));

        for(int k=-11;k<=11;++k) {
            if((k&1)==0) continue;
            float q=float(k)*10.0f;
            staticWorld_.push_back(Cube(p,0.10f,q,0.16f,0.025f,4.8f,16,{1,1,1,1},false,true));
            staticWorld_.push_back(Cube(q,0.105f,p,4.8f,0.025f,0.16f,16,{1,1,1,1},false,true));
        }
    }

    for(int bz=-3;bz<3;++bz) {
        for(int bx=-3;bx<3;++bx) {
            float cx=float(bx)*roadStep+roadStep*0.5f;
            float cz=float(bz)*roadStep+roadStep*0.5f;
            std::uint32_t h=std::uint32_t((bx+9)*92821u)^std::uint32_t((bz+11)*68917u)^0xA51C9E2Du;

            staticWorld_.push_back(Cube(cx,0.16f,cz,26.0f,0.28f,26.0f,1));

            bool park=(h%9u)==0u || (bx==1&&bz==-2);
            if(park) {
                staticWorld_.push_back(Cube(cx,0.34f,cz,21.8f,0.18f,21.8f,2));
                for(int t=0;t<5;++t) {
                    float a=float(t)*1.2566f+float((h>>8)&7u)*0.13f;
                    float rr=5.0f+float((h>>(t+3))&3u);
                    float tx=cx+std::cos(a)*rr;
                    float tz=cz+std::sin(a)*rr;
                    staticWorld_.push_back(Cube(tx,1.05f,tz,0.75f,2.0f,0.75f,15));
                    staticWorld_.push_back(Cube(tx,3.05f,tz,3.1f,3.6f,3.1f,14));
                }
                continue;
            }

            int layout=int((h>>3)&3u);
            int buildings=(layout==0)?1:((layout==1)?2:3);
            for(int b=0;b<buildings;++b) {
                float ox=0,oz=0,sx=0,sz=0;
                if(buildings==1) {
                    sx=15.0f+float((h>>9)&3u);
                    sz=15.0f+float((h>>11)&3u);
                } else if(buildings==2) {
                    bool splitX=((h>>13)&1u)!=0;
                    if(splitX) {
                        ox=(b==0?-6.2f:6.2f);
                        sx=9.2f; sz=18.0f;
                    } else {
                        oz=(b==0?-6.2f:6.2f);
                        sx=18.0f; sz=9.2f;
                    }
                } else {
                    static const float px[3]={-6.1f,6.1f,0};
                    static const float pz[3]={-5.8f,-5.8f,6.0f};
                    ox=px[b]; oz=pz[b];
                    sx=(b==2)?14.0f:9.3f;
                    sz=(b==2)?8.8f:9.6f;
                }

                float sy=5.5f+float((h>>(15+b*3))&7u)*1.7f;
                float x=cx+ox;
                float z=cz+oz;
                std::uint32_t facade=3u+((h>>(20+b))&1u);
                if(((h>>(24+b))&1u)!=0) facade=24;
                if(((h>>(27+b))&1u)!=0) facade=25;

                staticWorld_.push_back(Cube(x,0.34f+sy*0.5f,z,sx,sy,sz,facade));
                staticWorld_.push_back(Cube(x,0.43f+sy,z,sx*0.94f,0.32f,sz*0.94f,5));
                if(sy>11.0f) {
                    staticWorld_.push_back(Cube(x+sx*0.22f,0.85f+sy,z+sz*0.18f,
                                                std::max(1.8f,sx*0.16f),1.1f,std::max(1.8f,sz*0.18f),5));
                }
                colliders_.push_back({x-sx*0.5f-0.20f,x+sx*0.5f+0.20f,z-sz*0.5f-0.20f,z+sz*0.5f+0.20f});
            }

            if((h&1u)!=0) {
                float tx=cx-11.0f,tz=cz+9.5f;
                staticWorld_.push_back(Cube(tx,0.95f,tz,0.55f,1.8f,0.55f,15));
                staticWorld_.push_back(Cube(tx,2.55f,tz,2.5f,2.6f,2.5f,14));
            }

            float lampX=cx+11.2f;
            float lampZ=cz-10.8f;
            staticWorld_.push_back(Cube(lampX,1.7f,lampZ,0.18f,3.2f,0.18f,5));
            staticWorld_.push_back(Cube(lampX,3.25f,lampZ,0.75f,0.16f,0.75f,18,{0.95f,0.88f,0.58f,1},false,true));
        }
    }

    // Small central plaza island gives the player an immediate readable landmark.
    staticWorld_.push_back(Cube(0,0.22f,0,5.4f,0.36f,5.4f,25));
    staticWorld_.push_back(Cube(0,0.85f,0,2.0f,1.2f,2.0f,4));
}

void Game::SpawnPopulation() {
    cars_.clear();
    peds_.clear();
    bullets_.clear();

    constexpr float roads[7]={-108,-72,-36,0,36,72,108};
    for(int i=0;i<28;++i) {
        Car c;
        bool horizontal=(i&1)==0;
        int ri=int(RandomU32()%7u);
        bool positive=(RandomU32()&1u)!=0;
        float lane=positive?2.1f:-2.1f;
        float along=-112.0f+Random01()*224.0f;
        if(horizontal) {
            c.x=along;
            c.z=roads[ri]+lane;
            c.yaw=positive?XM_PIDIV2:-XM_PIDIV2;
        } else {
            c.x=roads[ri]+(positive?-2.1f:2.1f);
            c.z=along;
            c.yaw=positive?0.0f:XM_PI;
        }
        c.speed=4.0f+Random01()*4.0f;
        c.cruise=6.0f+Random01()*3.0f;
        std::uint32_t variants[6]={10,11,12,21,22,23};
        c.texture=variants[RandomU32()%6u];
        cars_.push_back(c);
    }

    for(int i=0;i<38;++i) {
        Ped p;
        for(int tries=0;tries<50;++tries) {
            int ri=int(RandomU32()%7u);
            float road=roads[ri];
            float side=(RandomU32()&1u)?7.0f:-7.0f;
            bool vertical=(RandomU32()&1u)!=0;
            float along=-112.0f+Random01()*224.0f;
            p.x=vertical?road+side:along;
            p.z=vertical?along:road+side;
            if(CanMove(p.x,p.z,0.45f)) break;
        }
        p.yaw=Random01()*XM_2PI-XM_PI;
        p.decision=Random01()*2.5f;
        p.personality=RandomU32()%100u;
        p.texture=(i%3==0)?8:7;
        peds_.push_back(p);
    }
}

void Game::Tick() {
    auto now=std::chrono::steady_clock::now();
    float dt=std::chrono::duration<float>(now-last_).count();
    last_=now;
    dt=std::clamp(dt,0.0f,0.05f);

    if(Key(VK_ESCAPE)) {
        PostMessage(hwnd_,WM_CLOSE,0,0);
        return;
    }

    Update(dt);
    BuildFrame();
}

void Game::Update(float dt) {
    UpdatePlayer(dt);
    UpdateTraffic(dt);
    UpdatePeds(dt);
    UpdateBullets(dt);
    UpdateWanted(dt);
    UpdateMission(dt);

    if(player_.health<=0.0f) {
        if(player_.car>=0 && player_.car<int(cars_.size())) player_.car=-1;
        player_.x=7.0f;
        player_.z=7.0f;
        player_.health=100.0f;
        player_.armor=0.0f;
        player_.wanted=0;
        player_.crimeTimer=0;
        player_.money=std::max(0,player_.money-100);
        bullets_.clear();
    }
}

void Game::UpdatePlayer(float dt) {
    player_.shootCooldown=std::max(0.0f,player_.shootCooldown-dt);

    const bool e=Key('E');
    if(e&&!ePrev_) {
        if(player_.car>=0 && player_.car<int(cars_.size())) {
            Car& c=cars_[player_.car];
            float sx=std::cos(c.yaw)*2.2f;
            float sz=-std::sin(c.yaw)*2.2f;
            float ex=c.x+sx, ez=c.z+sz;
            if(CanMove(ex,ez,0.45f)) {
                player_.x=ex; player_.z=ez; player_.car=-1;
            }
        } else {
            int best=-1;
            float bestD=9.0f;
            for(int i=0;i<int(cars_.size());++i) {
                if(!cars_[i].active || cars_[i].police) continue;
                float d=DistanceSq(player_.x,player_.z,cars_[i].x,cars_[i].z);
                if(d<bestD && std::abs(cars_[i].speed)<5.5f) { bestD=d; best=i; }
            }
            if(best>=0) player_.car=best;
        }
    }
    ePrev_=e;

    if(player_.car>=0 && player_.car<int(cars_.size())) {
        Car& c=cars_[player_.car];
        float throttle=(Key('W')?1.0f:0.0f)-(Key('S')?1.0f:0.0f);
        float steer=(Key('D')?1.0f:0.0f)-(Key('A')?1.0f:0.0f);
        c.speed+=throttle*17.0f*dt;
        if(std::abs(throttle)<0.1f) c.speed=Approach(c.speed,0.0f,7.0f*dt);
        c.speed=std::clamp(c.speed,-7.0f,20.0f);
        if(std::abs(c.speed)>0.35f) c.yaw=WrapAngle(c.yaw+steer*1.7f*dt*(c.speed>=0?1.0f:-1.0f));

        float dx=std::sin(c.yaw), dz=std::cos(c.yaw);
        float nx=c.x+dx*c.speed*dt;
        float nz=c.z+dz*c.speed*dt;
        if(CanMove(nx,nz,1.25f)) {
            c.x=nx; c.z=nz;
        } else {
            c.speed*=-0.22f;
        }
        player_.x=c.x;
        player_.z=c.z;
        player_.yaw=c.yaw;
        return;
    }

    float mx=(Key('D')?1.0f:0.0f)-(Key('A')?1.0f:0.0f);
    float mz=(Key('S')?1.0f:0.0f)-(Key('W')?1.0f:0.0f);
    float len=std::sqrt(mx*mx+mz*mz);
    if(len>0.01f) {
        mx/=len; mz/=len;
        float speed=Key(VK_SHIFT)?7.8f:4.7f;
        float nx=player_.x+mx*speed*dt;
        float nz=player_.z+mz*speed*dt;
        if(CanMove(nx,player_.z,0.42f)) player_.x=nx;
        if(CanMove(player_.x,nz,0.42f)) player_.z=nz;
        player_.yaw=std::atan2(mx,mz);
    }

    POINT pt{};
    if(GetCursorPos(&pt) && ScreenToClient(hwnd_,&pt)) {
        float dx=float(pt.x)-float(Renderer::Width)*0.5f;
        float dy=float(pt.y)-float(Renderer::Height)*0.5f;
        if(dx*dx+dy*dy>400.0f) player_.yaw=std::atan2(dx,dy);
    }

    bool fire=Key(VK_LBUTTON)||Key(VK_SPACE);
    if(fire && player_.shootCooldown<=0.0f && player_.ammo>0) {
        Shoot(false,player_.x,player_.z,player_.yaw);
        player_.shootCooldown=0.18f;
        --player_.ammo;

        for(auto& p:peds_) {
            if(p.state==PedState::Dead||p.state==PedState::Police) continue;
            if(DistanceSq(player_.x,player_.z,p.x,p.z)<110.0f) {
                p.state=(p.personality<18)?PedState::Aggressive:PedState::Flee;
                p.decision=3.0f;
            }
        }
    }
    firePrev_=fire;
}

void Game::UpdateTraffic(float dt) {
    constexpr float step=36.0f;
    for(int i=0;i<int(cars_.size());++i) {
        if(i==player_.car) continue;
        Car& c=cars_[i];
        if(!c.active) continue;
        c.turnCooldown=std::max(0.0f,c.turnCooldown-dt);

        float fx=std::sin(c.yaw), fz=std::cos(c.yaw);
        float desired=c.cruise;
        for(int j=0;j<int(cars_.size());++j) {
            if(i==j||!cars_[j].active) continue;
            float rx=cars_[j].x-c.x, rz=cars_[j].z-c.z;
            float ahead=rx*fx+rz*fz;
            float side=std::abs(rx*fz-rz*fx);
            if(ahead>0&&ahead<7.0f&&side<2.0f) {
                desired=std::min(desired,std::max(0.0f,cars_[j].speed-1.5f));
            }
        }
        c.speed=Approach(c.speed,desired,5.0f*dt);

        float oldX=c.x,oldZ=c.z;
        c.x+=fx*c.speed*dt;
        c.z+=fz*c.speed*dt;

        if(c.x>120)c.x=-120;
        if(c.x<-120)c.x=120;
        if(c.z>120)c.z=-120;
        if(c.z<-120)c.z=120;

        bool horizontal=std::abs(fx)>0.7f;
        float along=horizontal?c.x:c.z;
        float oldAlong=horizontal?oldX:oldZ;
        float nearest=std::round(along/step)*step;
        bool crossed=(oldAlong-nearest)*(along-nearest)<=0.0f && std::abs(along-nearest)<1.2f;

        if(crossed && c.turnCooldown<=0.0f && std::abs(nearest)<=108.1f) {
            std::uint32_t roll=RandomU32()%100u;
            if(roll<28u) {
                bool left=(roll<14u);
                c.yaw=WrapAngle(c.yaw+(left?-XM_PIDIV2:XM_PIDIV2));
                float roadX=std::round(c.x/step)*step;
                float roadZ=std::round(c.z/step)*step;
                float nfx=std::sin(c.yaw), nfz=std::cos(c.yaw);
                if(std::abs(nfx)>0.7f) {
                    c.x=roadX;
                    c.z=roadZ+(nfx>0?2.1f:-2.1f);
                } else {
                    c.z=roadZ;
                    c.x=roadX+(nfz>0?-2.1f:2.1f);
                }
                c.turnCooldown=1.0f;
            }
        }
    }
}

void Game::UpdatePeds(float dt) {
    float px=player_.x,pz=player_.z;

    for(auto& p:peds_) {
        if(p.state==PedState::Dead) {
            p.deadTimer-=dt;
            if(p.deadTimer<=0.0f) {
                p.health=100;
                p.state=PedState::Wander;
                p.texture=(p.personality&1u)?7:8;
                p.x=-110.0f+Random01()*220.0f;
                p.z=-110.0f+Random01()*220.0f;
                if(!CanMove(p.x,p.z,0.45f)) { p.x=7; p.z=-7; }
            }
            continue;
        }

        p.decision-=dt;
        p.shootCooldown=std::max(0.0f,p.shootCooldown-dt);

        float targetSpeed=1.2f;
        if(p.state==PedState::Wander) {
            if(p.decision<=0.0f) {
                p.yaw=Random01()*XM_2PI-XM_PI;
                p.decision=1.2f+Random01()*2.6f;
            }
        } else if(p.state==PedState::Flee) {
            p.yaw=std::atan2(p.x-px,p.z-pz);
            targetSpeed=2.7f;
            if(DistanceSq(p.x,p.z,px,pz)>650.0f && p.decision<=0) p.state=PedState::Wander;
        } else if(p.state==PedState::Aggressive || p.state==PedState::Police) {
            p.yaw=std::atan2(px-p.x,pz-p.z);
            float d=DistanceSq(p.x,p.z,px,pz);
            targetSpeed=(d>55.0f)?2.25f:0.7f;
            if(d<180.0f && p.shootCooldown<=0.0f) {
                Shoot(true,p.x,p.z,p.yaw);
                p.shootCooldown=(p.state==PedState::Police)?0.75f:1.15f;
            }
        }

        p.speed=Approach(p.speed,targetSpeed,4.0f*dt);
        float dx=std::sin(p.yaw),dz=std::cos(p.yaw);
        float nx=p.x+dx*p.speed*dt;
        float nz=p.z+dz*p.speed*dt;
        if(CanMove(nx,nz,0.40f)) {
            p.x=nx;p.z=nz;
        } else {
            p.yaw=WrapAngle(p.yaw+XM_PIDIV2+Random01()*0.8f);
            p.decision=0.6f;
        }
    }
}

void Game::Shoot(bool hostile,float x,float z,float yaw) {
    Bullet b;
    float dx=std::sin(yaw),dz=std::cos(yaw);
    b.x=x+dx*0.75f;
    b.z=z+dz*0.75f;
    b.vx=dx*28.0f;
    b.vz=dz*28.0f;
    b.life=1.2f;
    b.hostile=hostile;
    bullets_.push_back(b);
}

void Game::UpdateBullets(float dt) {
    for(auto& b:bullets_) {
        if(b.life<=0) continue;
        b.x+=b.vx*dt;
        b.z+=b.vz*dt;
        b.life-=dt;

        if(!CanMove(b.x,b.z,0.08f)) {
            b.life=0;
            continue;
        }

        if(b.hostile) {
            if(DistanceSq(b.x,b.z,player_.x,player_.z)<0.65f) {
                float dmg=9.0f;
                if(player_.armor>0) {
                    float used=std::min(player_.armor,dmg);
                    player_.armor-=used;
                    dmg-=used;
                }
                player_.health-=dmg;
                b.life=0;
            }
        } else {
            for(auto& p:peds_) {
                if(p.state==PedState::Dead) continue;
                if(DistanceSq(b.x,b.z,p.x,p.z)<0.62f) {
                    p.health-=40.0f;
                    b.life=0;
                    if(p.state!=PedState::Police) {
                        player_.wanted=std::min(5,std::max(1,player_.wanted+1));
                        player_.crimeTimer=0;
                        if(p.personality<28) p.state=PedState::Aggressive;
                        else p.state=PedState::Flee;
                    } else {
                        player_.wanted=std::min(5,std::max(2,player_.wanted+1));
                        player_.crimeTimer=0;
                    }
                    if(p.health<=0) {
                        p.state=PedState::Dead;
                        p.deadTimer=8.0f+Random01()*6.0f;
                        p.speed=0;
                        player_.money+=15;
                    }
                    break;
                }
            }
        }
    }
    bullets_.erase(std::remove_if(bullets_.begin(),bullets_.end(),
        [](const Bullet& b){return b.life<=0;}),bullets_.end());
}

void Game::UpdateWanted(float dt) {
    if(player_.wanted<=0) return;
    player_.crimeTimer+=dt;

    int policeCount=0;
    for(const auto& p:peds_) if(p.state==PedState::Police) ++policeCount;
    int desired=player_.wanted*2;

    while(policeCount<desired && int(peds_.size())<64) {
        Ped p;
        float a=Random01()*XM_2PI;
        float r=14.0f+Random01()*8.0f;
        p.x=player_.x+std::cos(a)*r;
        p.z=player_.z+std::sin(a)*r;
        if(!CanMove(p.x,p.z,0.45f)) {
            p.x=player_.x+((RandomU32()&1u)?10.0f:-10.0f);
            p.z=player_.z;
        }
        p.state=PedState::Police;
        p.texture=9;
        p.personality=99;
        p.decision=5.0f;
        peds_.push_back(p);
        ++policeCount;
    }

    float loseTime=18.0f+float(player_.wanted)*5.0f;
    if(player_.crimeTimer>loseTime) {
        --player_.wanted;
        player_.crimeTimer=0;
        if(player_.wanted==0) {
            for(auto& p:peds_) if(p.state==PedState::Police) {
                p.state=PedState::Wander;
                p.texture=7;
                p.decision=0.2f;
            }
        }
    }
}

void Game::UpdateMission(float dt) {
    missionPulse_+=dt;
    if(DistanceSq(player_.x,player_.z,missionX_,missionZ_)<12.0f) {
        if(!missionActive_) {
            missionActive_=true;
            missionX_=86.0f;
            missionZ_=-82.0f;
        } else {
            missionActive_=false;
            player_.money+=500;
            player_.ammo+=30;
            player_.armor=std::min(100.0f,player_.armor+25.0f);
            missionX_=-86.0f;
            missionZ_=82.0f;
        }
    }
}

void Game::AddCarDraw(const Car& c,bool controlled) {
    XMFLOAT4 tint=controlled?XMFLOAT4(1.12f,1.12f,1.12f,1):XMFLOAT4(1,1,1,1);
    auto chassis=Cube(c.x,0.31f,c.z,1.75f,0.48f,3.55f,c.texture,tint,true,false);
    chassis.yaw=c.yaw;
    frameWorld_.push_back(chassis);
    auto top=GroundQuad(c.x,0.565f,c.z,2.10f,4.15f,c.yaw,c.texture,{1,1,1,1},false,true);
    frameWorld_.push_back(top);
}

void Game::AddHudBar(float x,float y,float w,float h,float value,const XMFLOAT4& color) {
    value=std::clamp(value,0.0f,1.0f);
    hud_.push_back(ScreenQuad(x,y,w,h,{0.05f,0.06f,0.07f,0.92f}));
    float fw=w*value;
    float fx=x-w*0.5f+fw*0.5f;
    hud_.push_back(ScreenQuad(fx,y,fw,h*0.66f,color));
}

void Game::AddDigit(int digit,float x,float y,float size,const XMFLOAT4& color) {
    static const unsigned char masks[10]={
        0b1111110,0b0110000,0b1101101,0b1111001,0b0110011,
        0b1011011,0b1011111,0b1110000,0b1111111,0b1111011
    };
    digit=std::clamp(digit,0,9);
    unsigned char m=masks[digit];
    float t=size*0.18f;
    float l=size*0.70f;
    auto seg=[&](int bit,float sx,float sy,float px,float py){
        if(m&(1<<bit)) hud_.push_back(ScreenQuad(x+px*size,y+py*size,sx,sy,color));
    };
    seg(6,l,t,0, 0.82f);
    seg(5,t,l,-0.39f, 0.42f);
    seg(4,t,l,-0.39f,-0.42f);
    seg(3,l,t,0,-0.82f);
    seg(2,t,l, 0.39f,-0.42f);
    seg(1,t,l, 0.39f, 0.42f);
    seg(0,l,t,0,0);
}

void Game::AddNumber(int value,float x,float y,float size,const XMFLOAT4& color) {
    std::string s=std::to_string(std::max(0,value));
    float spacing=size*1.05f;
    for(std::size_t i=0;i<s.size();++i)
        AddDigit(s[i]-'0',x+float(i)*spacing,y,size,color);
}

void Game::BuildFrame() {
    frameWorld_=staticWorld_;
    hud_.clear();

    for(int i=0;i<int(cars_.size());++i)
        if(cars_[i].active) AddCarDraw(cars_[i],i==player_.car);

    for(const auto& p:peds_) {
        if(p.state==PedState::Dead) {
            frameWorld_.push_back(GroundQuad(p.x,0.19f,p.z,0.72f,1.35f,p.yaw,p.texture,{0.42f,0.42f,0.42f,0.85f},false,true));
        } else {
            frameWorld_.push_back(GroundQuad(p.x,0.20f,p.z,0.82f,1.45f,p.yaw,p.texture,{1,1,1,1},false,true));
        }
    }

    if(player_.car<0)
        frameWorld_.push_back(GroundQuad(player_.x,0.22f,player_.z,0.90f,1.55f,player_.yaw,6,{1,1,1,1},false,true));

    for(const auto& b:bullets_)
        frameWorld_.push_back(GroundQuad(b.x,0.30f,b.z,0.22f,0.46f,0,17,{1.2f,0.95f,0.42f,1},false,true));

    float pulse=1.0f+std::sin(missionPulse_*4.0f)*0.12f;
    XMFLOAT4 markerColor=missionActive_?XMFLOAT4(0.24f,0.92f,0.42f,0.72f):XMFLOAT4(0.95f,0.73f,0.16f,0.72f);
    frameWorld_.push_back(GroundQuad(missionX_,0.18f,missionZ_,4.2f*pulse,4.2f*pulse,0,18,markerColor,false,true));

    AddHudBar(-0.73f,0.88f,0.38f,0.052f,player_.health/100.0f,{0.88f,0.16f,0.18f,1});
    AddHudBar(-0.73f,0.80f,0.38f,0.042f,player_.armor/100.0f,{0.18f,0.48f,0.92f,1});
    AddHudBar(-0.73f,0.73f,0.38f,0.036f,float(player_.ammo%101)/100.0f,{0.94f,0.75f,0.18f,1});

    for(int i=0;i<5;++i) {
        XMFLOAT4 c=(i<player_.wanted)?XMFLOAT4(1.0f,0.78f,0.16f,1):XMFLOAT4(0.13f,0.14f,0.15f,0.86f);
        hud_.push_back(ScreenQuad(0.61f+float(i)*0.07f,0.88f,0.050f,0.050f,c));
    }
    AddNumber(player_.money,0.60f,0.73f,0.055f,{0.96f,0.83f,0.35f,1});
    AddNumber(player_.ammo,0.75f,-0.84f,0.050f,{0.95f,0.95f,0.92f,1});

    float tx=player_.x,tz=player_.z;
    if(player_.car>=0 && player_.car<int(cars_.size())) {
        tx=cars_[player_.car].x;
        tz=cars_[player_.car].z;
    }

    // Near-overhead orthographic camera: player/car remains exactly at the center.
    XMVECTOR eye=XMVectorSet(tx,46.0f,tz+7.5f,1);
    XMVECTOR at=XMVectorSet(tx,0.0f,tz,1);
    XMMATRIX view=XMMatrixLookAtLH(eye,at,XMVectorSet(0,0,-1,0));
    float aspect=float(Renderer::Width)/float(Renderer::Height);
    XMMATRIX proj=XMMatrixOrthographicLH(cameraSize_*aspect,cameraSize_,0.1f,180.0f);
    XMMATRIX viewProj=view*proj;

    XMVECTOR lightDir=XMVector3Normalize(XMVectorSet(-0.55f,-0.80f,-0.25f,0));
    XMVECTOR lightTarget=XMVectorSet(tx,0,tz,1);
    XMVECTOR lightPos=XMVectorSubtract(lightTarget,XMVectorScale(lightDir,90.0f));
    XMMATRIX lightView=XMMatrixLookAtLH(lightPos,lightTarget,XMVectorSet(0,1,0,0));
    XMMATRIX lightProj=XMMatrixOrthographicLH(92.0f,92.0f,1.0f,220.0f);

    renderer_.Render(frameWorld_,hud_,viewProj,lightView*lightProj);
}
