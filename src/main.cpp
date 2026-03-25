#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "RootTool.h"
#include "resources.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <d3d11.h>
#include <tchar.h>
#include <dwmapi.h>


static ID3D11Device*           g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*         g_pSwapChain            = nullptr;
static bool                    g_SwapChainOccluded     = false;
static UINT                    g_ResizeWidth           = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView  = nullptr;
HWND                           g_hWnd                  = nullptr;


bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

bool LoadTextureFromResource(int resId, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(resId), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return false;
    DWORD size = SizeofResource(nullptr, hRes);
    const void* data = LockResource(hData);
    if (!data || size == 0) return false;

    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load_from_memory((const stbi_uc*)data, size, &image_width, &image_height, NULL, 4);
    if (image_data == NULL) return false;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D *pTexture = NULL;
    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = image_data;
    subResource.SysMemPitch = desc.Width * 4;
    subResource.SysMemSlicePitch = 0;
    g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
    if (!pTexture) { stbi_image_free(image_data); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;
    g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;
    stbi_image_free(image_data);
    return true;
}



int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                       ::GetModuleHandle(nullptr), ::LoadIcon(::GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1)), nullptr,
                       nullptr, nullptr, L"BstkRooterWnd", ::LoadIcon(::GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1)) };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"BSTK Rooter",
        WS_POPUP, 100, 100,
        (int)(820 * dpi), (int)(600 * dpi),
        nullptr, nullptr, wc.hInstance, nullptr);
    g_hWnd = hwnd;


    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DWORD backdropType = 2;
    DwmSetWindowAttribute(hwnd, 38, &backdropType, sizeof(backdropType));
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;


    ImGui::StyleColorsDark();
    RootTool::SetupTheme();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;
    io.ConfigDpiScaleFonts    = true;
    io.ConfigDpiScaleViewports = true;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);


    const float fontSize = 16.0f;
    if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", fontSize * dpi) == nullptr)
        io.Fonts->AddFontDefault();

    constexpr float BG[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    RootTool app;

    ID3D11ShaderResourceView* logo_srv = nullptr;
    int logo_w = 0, logo_h = 0;
    if (LoadTextureFromResource(IDR_LOGO_PNG, &logo_srv, &logo_w, &logo_h)) {
        app.SetLogo(logo_srv, logo_w, logo_h);
    }

    bool done = false;

    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
            { ::Sleep(10); continue; }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth && g_ResizeHeight) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.RenderUI();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, BG);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}



bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 2;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hWnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fla[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, fla, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            0, fla, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    IDXGIFactory* fac;
    if (SUCCEEDED(g_pSwapChain->GetParent(IID_PPV_ARGS(&fac))))
        { fac->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER); fac->Release(); }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release();  g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();         g_pd3dDevice        = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* bb;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRenderTargetView);
    bb->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}



LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_ResizeWidth  = LOWORD(lParam);
            g_ResizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rc;
        GetClientRect(hWnd, &rc);

        if (pt.y < 40 && pt.x < (rc.right - 120))
            return HTCAPTION;
        break;
    }
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
