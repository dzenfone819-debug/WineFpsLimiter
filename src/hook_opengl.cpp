#include "global.h"
#include <windows.h>
#include <GL/gl.h>
#include "MinHook.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

// --- Globals ---
static GLuint g_OverlayFBO = 0;
static GLuint g_OverlayTexture = 0;
static GLuint g_ComposeProgram = 0;
static GLuint g_ComposeVAO = 0;
static int g_OverlayWidth = 0;
static int g_OverlayHeight = 0;
static bool g_GLFuncsLoaded = false;

typedef BOOL (WINAPI *wglSwapBuffers_t)(HDC);
wglSwapBuffers_t g_owglSwapBuffers = nullptr;

// We need glext.h for framebuffer functions. Define minimal GL typedefs and function pointers
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE0 0x84C0
// Additional constants used by the code
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5

typedef void (APIENTRY *PFNGLGENFRAMEBUFFERSPROC) (GLsizei n, GLuint *framebuffers);
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFERPROC) (GLenum target, GLuint framebuffer);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC) (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC) (GLsizei n, const GLuint *framebuffers);
typedef void (APIENTRY *PFNGLGENVERTEXARRAYSPROC) (GLsizei n, GLuint *arrays);
typedef void (APIENTRY *PFNGLBINDVERTEXARRAYPROC) (GLuint array);
typedef void (APIENTRY *PFNGLDELETEVERTEXARRAYSPROC) (GLsizei n, const GLuint *arrays);
typedef void (APIENTRY *PFNGLUSEPROGRAMPROC) (GLuint program);
typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC) (GLenum type);
typedef void (APIENTRY *PFNGLSHADERSOURCEPROC) (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
typedef void (APIENTRY *PFNGLCOMPILESHADERPROC) (GLuint shader);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC) (void);
typedef void (APIENTRY *PFNGLATTACHSHADERPROC) (GLuint program, GLuint shader);
typedef void (APIENTRY *PFNGLLINKPROGRAMPROC) (GLuint program);
typedef void (APIENTRY *PFNGLDELETESHADERPROC) (GLuint shader);
typedef void (APIENTRY *PFNGLDELETEPROGRAMPROC) (GLuint program);
typedef void (APIENTRY *PFNGLGETSHADERIVPROC) (GLuint shader, GLenum pname, GLint *params);
typedef void (APIENTRY *PFNGLGETPROGRAMIVPROC) (GLuint program, GLenum pname, GLint *params);
typedef void (APIENTRY *PFNGLGETSHADERINFOLOGPROC) (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC) (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLint (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC) (GLuint program, const GLchar *name);
typedef void (APIENTRY *PFNGLUNIFORM1IPROC) (GLint location, GLint v0);
typedef void (APIENTRY *PFNGLACTIVETEXTUREPROC) (GLenum texture);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC) (GLenum target);

static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
static PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
static PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
static PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
static PFNGLCREATESHADERPROC glCreateShader = nullptr;
static PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
static PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
static PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
static PFNGLATTACHSHADERPROC glAttachShader = nullptr;
static PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
static PFNGLDELETESHADERPROC glDeleteShader = nullptr;
static PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
static PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
static PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
static PFNGLUNIFORM1IPROC glUniform1i = nullptr;
static PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;

// Helper to load GL functions via wglGetProcAddress
void LoadGLFunctions() {
    if (g_GLFuncsLoaded) return;
    HMODULE hMod = GetModuleHandleA("opengl32.dll");
    typedef void* (WINAPI *wglGetProcAddress_t)(const char*);
    wglGetProcAddress_t wglGetProcAddress = (wglGetProcAddress_t)GetProcAddress(hMod, "wglGetProcAddress");
    #define LOAD(name) name = (decltype(name))wglGetProcAddress(#name);
    LOAD(glGenFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glDeleteFramebuffers);
    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glDeleteVertexArrays);
    LOAD(glUseProgram);
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glDeleteShader);
    LOAD(glDeleteProgram);
    LOAD(glGetShaderiv);
    LOAD(glGetProgramiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glGetProgramInfoLog);
    LOAD(glGetUniformLocation);
    LOAD(glUniform1i);
    LOAD(glCheckFramebufferStatus);
    LOAD(glActiveTexture);
    #undef LOAD
    g_GLFuncsLoaded = (glGenFramebuffers != nullptr);
}

bool CreateOverlayResources(int width, int height) {
    if (!g_GLFuncsLoaded) return false;
    
    // Create Texture
    if (g_OverlayTexture) glDeleteTextures(1, &g_OverlayTexture);
    glGenTextures(1, &g_OverlayTexture);
    glBindTexture(GL_TEXTURE_2D, g_OverlayTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    // Create FBO
    if (g_OverlayFBO) glDeleteFramebuffers(1, &g_OverlayFBO);
    glGenFramebuffers(1, &g_OverlayFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, g_OverlayFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_OverlayTexture, 0);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Log("OpenGL FBO Incomplete!");
        return false;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind
    
    // Create Shader Program
    if (!g_ComposeProgram) {
        const char* vsCode = 
            "#version 330 core\n"
            "out vec2 UV;\n"
            "void main() {\n"
            "   float x = float((gl_VertexID & 1) << 2);\n"
            "   float y = float((gl_VertexID & 2) << 1);\n"
            "   UV = vec2(x * 0.5, y * 0.5);\n"
            "   gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);\n"
            "}\n";
            
        const char* psCode = 
            "#version 330 core\n"
            "in vec2 UV;\n"
            "out vec4 FragColor;\n"
            "uniform sampler2D overlayTex;\n"
            "void main() {\n"
            "   FragColor = texture(overlayTex, UV);\n"
            "}\n";
            
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vsCode, NULL);
        glCompileShader(vs);
        
        GLuint ps = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(ps, 1, &psCode, NULL);
        glCompileShader(ps);
        
        g_ComposeProgram = glCreateProgram();
        glAttachShader(g_ComposeProgram, vs);
        glAttachShader(g_ComposeProgram, ps);
        glLinkProgram(g_ComposeProgram);
        
        glDeleteShader(vs);
        glDeleteShader(ps);
    }
    
    // Create Empty VAO
    if (!g_ComposeVAO) {
        glGenVertexArrays(1, &g_ComposeVAO);
    }
    
    g_OverlayWidth = width;
    g_OverlayHeight = height;
    return true;
}

BOOL WINAPI hook_wglSwapBuffers(HDC hDc) {
    static bool init = false;
    
    if (!init) {
        g_hWnd = WindowFromDC(hDc);
        LoadGLFunctions();
        
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplOpenGL3_Init(); 
        
        // Get GL Version
        const GLubyte* renderer = glGetString(GL_RENDERER);
        if(renderer) g_RendererName = std::string((const char*)renderer);
        
        init = true;
    }

    // Check Window Size
    RECT rect;
    GetClientRect(g_hWnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    
    if (g_GLFuncsLoaded && (width != g_OverlayWidth || height != g_OverlayHeight || !g_OverlayFBO)) {
        CreateOverlayResources(width, height);
    }

    CheckLimiter();

    // --- RENDER TO TEXTURE ---
    GLint last_fbo = 0;
    GLint last_viewport[4];
    GLboolean last_blend = glIsEnabled(GL_BLEND);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_cull = glIsEnabled(GL_CULL_FACE);
    GLint last_prog = 0;
    GLint last_vao = 0;
    
    if (g_GLFuncsLoaded && g_OverlayFBO) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
        glGetIntegerv(GL_VIEWPORT, last_viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vao);
        
        glBindFramebuffer(GL_FRAMEBUFFER, g_OverlayFBO);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    RenderOverlay();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    // --- COMPOSITE TO BACKBUFFER ---
    if (g_GLFuncsLoaded && g_OverlayFBO) {
        glBindFramebuffer(GL_FRAMEBUFFER, last_fbo); // Restore default/previous FBO
        glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        glUseProgram(g_ComposeProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_OverlayTexture);
        GLint loc = glGetUniformLocation(g_ComposeProgram, "overlayTex");
        if(loc >= 0) glUniform1i(loc, 0);
        
        glBindVertexArray(g_ComposeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3); // Full screen triangle
        
        // Restore State
        glBindVertexArray(last_vao);
        glUseProgram(last_prog);
        if (last_depth) glEnable(GL_DEPTH_TEST);
        if (last_cull) glEnable(GL_CULL_FACE);
        if (!last_blend) glDisable(GL_BLEND);
    }

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
