// ===================== System Header Files =====================
#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "RenderFusion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ===================== UI State =====================
static bool g_ShowUI = true;
static bool g_Expanded = true;
static float g_FontScale = 1.0f;
static ImFont* g_UIFont = nullptr;

// ===================== ImGui Render Global State =====================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

// ===================== GL State Protection (照搬 ZoomMod 的核心) =====================
struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst, bSrcA, bDstA;
    GLboolean blend, cull, depth, scissor, stencil, dither;
    GLint frontFace, activeTexture;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.bDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDstA);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.activeTexture);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFuncSeparate(s.bSrc, s.bDst, s.bSrcA, s.bDstA);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    s.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    s.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glFrontFace(s.frontFace);
}

// ===================== Theme Style (简化版) =====================
static void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // 清爽的 RenderFusion 蓝紫主题
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.94f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.2f, 0.4f, 0.8f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.25f, 0.45f, 0.9f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.55f, 1.0f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.35f, 0.8f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.2f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.3f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.3f, 0.5f, 1.0f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);

    s.WindowRounding = 12.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.WindowPadding = ImVec2(16, 12);
    s.FramePadding = ImVec2(10, 8);
}

// ===================== UI Drawing =====================
static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);
    ImGuiIO& io = ImGui::GetIO();

    // 1. 侧边悬浮球
    if (!g_ShowUI) {
        ImGui::SetNextWindowPos(ImVec2(20, io.DisplaySize.y * 0.5f), ImGuiCond_Always);
        ImGui::Begin("##Reopen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 50.0f);
        if (ImGui::Button("R", ImVec2(60, 60))) g_ShowUI = true;
        ImGui::PopStyleVar();
        ImGui::End();
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // 2. 主面板
    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGui::Begin("RenderFusion", &g_ShowUI, ImGuiWindowFlags_NoSavedSettings);

    // 标题
    ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "RenderFusion v1.0");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 10));

    // 测试区域
    ImGui::Text("Display: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Text("Mouse: (%.0f, %.0f)", io.MousePos.x, io.MousePos.y);
    
    ImGui::Dummy(ImVec2(0, 10));
    
    static float test_val = 0.5f;
    ImGui::SliderFloat("Test Slider", &test_val, 0.0f, 1.0f);
    
    static bool test_check = true;
    ImGui::Checkbox("Test Checkbox", &test_check);

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "ImGui is Working!");

    ImGui::End();

    if (g_UIFont) ImGui::PopFont();
}

// ===================== ImGui Initialization (照搬 ZoomMod) =====================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    
    LOGI("Setting up ImGui...");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 自动字体缩放
    float baseScale = (float)g_Height / 720.0f;
    g_FontScale = std::clamp(baseScale, 1.0f, 2.0f);

    ImFontConfig cfg;
    cfg.SizePixels = (float)(int)(20.0f * g_FontScale);
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    g_UIFont = io.Fonts->AddFontDefault(&cfg);

    // 关键：这里传 nullptr，和 ZoomMod 一致
    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    SetupStyle();
    g_Initialized = true;
    LOGI("ImGui Setup Complete!");
}

static void Render() {
    if (!g_Initialized) return;
    
    GLState s; 
    SaveGL(s); // 关键：保存所有状态

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s); // 关键：恢复所有状态
}

// ===================== Hook Function Pointers =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== EGL Render Hook (核心逻辑照搬) =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, s);
    
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d, s);

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, s);

    // 关键：检测 EGL_RENDER_BUFFER，只在后台缓冲初始化
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf;
        eglQuerySurface(d, s, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
            LOGI("Target acquired: %dx%d", w, h);
        }
    }

    if (ctx != g_TargetContext || s != g_TargetSurface)
        return orig_eglSwapBuffers(d, s);

    g_Width = w; 
    g_Height = h;
    
    Setup();
    Render();
    
    return orig_eglSwapBuffers(d, s);
}

// ===================== Input Hook (你的版本) =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
    }
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*e);
    }
    return r;
}

static void HookInput() {
    void* handle = GlossOpen("libinput.so");
    if (handle) {
        void* s1 = (void*)GlossSymbol(handle, "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
        if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);
        
        void* s2 = (void*)GlossSymbol(handle, "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
        if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
        LOGI("Input Hooked");
    }
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    sleep(3);
    LOGI("RenderFusion loaded");
    GlossInit(true);
    
    // Hook EGL
    GHandle egl = GlossOpen("libEGL.so");
    if (egl) {
        void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    }
    
    // Hook Input
    HookInput();
    
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
