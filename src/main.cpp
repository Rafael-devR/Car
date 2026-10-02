#include "Game.h"
#include <windows.h>
#include <stdexcept>

static Game* gGame = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
        case WM_MOUSEWHEEL:
            if(gGame) gGame->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_KEYDOWN:
            if(wp==VK_ESCAPE) {
                PostMessage(hwnd,WM_CLOSE,0,0);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    try {
        SetProcessDPIAware();

        WNDCLASSEXW wc{};
        wc.cbSize=sizeof(wc);
        wc.style=CS_HREDRAW|CS_VREDRAW;
        wc.lpfnWndProc=WndProc;
        wc.hInstance=instance;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName=L"CarTopDownDX12";
        if(!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassExW failed.");

        RECT r{0,0,LONG(Renderer::Width),LONG(Renderer::Height)};
        DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        AdjustWindowRect(&r,style,FALSE);

        HWND hwnd=CreateWindowExW(
            0,wc.lpszClassName,
            L"Car — Top-Down Crime City | WASD mover | mouse mirar | clique/Space atirar | E entrar/sair",
            style,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
            nullptr,nullptr,instance,nullptr);
        if(!hwnd) throw std::runtime_error("Could not create game window.");

        ShowWindow(hwnd,show);
        UpdateWindow(hwnd);

        Game game;
        gGame=&game;
        game.Init(hwnd);

        MSG msg{};
        while(msg.message!=WM_QUIT) {
            if(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                game.Tick();
            }
        }

        gGame=nullptr;
        return 0;
    } catch(const std::exception& e) {
        MessageBoxA(nullptr,e.what(),"Car DX12 - Fatal Error",MB_OK|MB_ICONERROR);
        return -1;
    }
}
