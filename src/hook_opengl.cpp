#include "global.h"
#include <windows.h>
#include "MinHook.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

typedef BOOL (WINAPI *wglSwapBuffers_t)(HDC);
wglSwapBuffers_t g_owglSwapBuffers = nullptr;

BOOL WINAPI hook_wglSwapBuffers(HDC hDc) {
    static bool init = false;
    if (!init) {
        // We need an HWND. WindowFromDC(hDc) works.
        g_hWnd = WindowFromDC(hDc);
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplOpenGL3_Init(); // Default to auto-detection (GL 3.0+)
        
        g_RendererName = "OpenGL";
        
        // Try getting version string
        // const GLubyte* renderer = glGetString(GL_RENDERER);
        // if(renderer) g_RendererName = std::string((const char*)renderer);
        
        init = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    CheckLimiter();
    RenderOverlay(); // Handles the UI

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return g_owglSwapBuffers(hDc);
}

void InitOpenGLHook() {
    HMODULE hMod = GetModuleHandleA("opengl32.dll");
    if (hMod) {
        void* proc = (void*)GetProcAddress(hMod, "wglSwapBuffers");
        if (proc) {
            MH_CreateHook(proc, (LPVOID)hook_wglSwapBuffers, (LPVOID*)&g_owglSwapBuffers);
            MH_EnableHook(proc);
            Log("OpenGL Hook Installed.");
        }
    }
}
