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
#include <cmath>

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "RenderFusion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ==========================================
// 1. 滤镜全局状态与参数
// ==========================================
namespace RF {
    // GL 资源
    GLuint screen_tex = 0;
    GLuint pingpong_fbo[2] = {0, 0};
    GLuint pingpong_tex[2] = {0, 0};
    GLuint quad_vbo = 0, quad_ebo = 0;

    // 着色器程序
    GLuint prog_draw = 0;
    GLuint prog_color = 0;
    GLuint prog_sharpen = 0;
    GLuint prog_gaussian = 0;
    GLuint prog_dof = 0;

    // 资源初始化标志
    bool resources_ready = false;

    // 交互状态
    bool focus_pending = false;

    // 滤镜参数
    struct FilterParams {
        // 基础画质调整
        bool enable_correction = false;
        float brightness = 0.0f;  // -0.5 ~ 0.5
        float contrast = 1.0f;    // 0.6 ~ 1.8
        float saturation = 1.0f;  // 0.0 ~ 2.0

        // 风格化
        bool enable_sepia = false;
        float sepia_intensity = 0.8f;

        // 锐化
        bool enable_sharpen = false;
        float sharpen_intensity = 0.5f;

        // 景深特效
        bool enable_dof = false;
        ImVec2 focus_point = ImVec2(0.5f, 0.5f);
        float focus_radius = 0.15f;
        float blur_strength = 1.0f;
        float transition = 0.2f;
    };
    FilterParams params;
}

// ==========================================
// 2. 着色器源码
// ==========================================
const char* g_quad_vert = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)";

// 基础画面拷贝
const char* g_frag_draw = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

// 综合颜色调整（亮度/对比度/饱和度/棕褐色）
const char* g_frag_color = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uSepia;
uniform int uEnableSepia;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;

    // 亮度 & 对比度
    result = result + uBrightness;
    result = (result - 0.5) * uContrast + 0.5;

    // 饱和度
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, uSaturation);

    // 棕褐色调
    if (uEnableSepia == 1) {
        vec3 sepiaColor;
        sepiaColor.r = dot(result, vec3(0.393, 0.769, 0.189));
        sepiaColor.g = dot(result, vec3(0.349, 0.686, 0.168));
        sepiaColor.b = dot(result, vec3(0.272, 0.534, 0.131));
        result = mix(result, sepiaColor, uSepia);
    }

    gl_FragColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
)";

// 锐化
const char* g_frag_sharpen = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec3 center = texture2D(uTexture, vTexCoord).rgb;
    // 3x3 拉普拉斯边缘检测
    vec3 sampleTL = texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb;
    vec3 sampleT  = texture2D(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb;
    vec3 sampleTR = texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb;
    vec3 sampleL  = texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb;
    vec3 sampleR  = texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb;
    vec3 sampleBL = texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb;
    vec3 sampleB  = texture2D(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb;
    vec3 sampleBR = texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb;

    vec3 edge = center * 8.0 - (sampleTL + sampleT + sampleTR + sampleL + sampleR + sampleBL + sampleB + sampleBR);
    vec3 result = center + edge * uIntensity;
    
    gl_FragColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
)";

// 可分离高斯模糊
const char* g_frag_gaussian = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
uniform float uRadius;

void main() {
    vec4 result = vec4(0.0);
    // 9-tap 高斯权重
    float weights[5]; 
    weights[0] = 0.227027; weights[1] = 0.1945946; 
    weights[2] = 0.1216216; weights[3] = 0.054054; 
    weights[4] = 0.016216;
    
    result += texture2D(uTexture, vTexCoord) * weights[0];
    for(int i = 1; i < 5; i++) {
        vec2 offset = uDirection * uTexelSize * float(i) * uRadius;
        result += texture2D(uTexture, vTexCoord + offset) * weights[i];
        result += texture2D(uTexture, vTexCoord - offset) * weights[i];
    }
    gl_FragColor = result;
}
)";

// 景深合成
const char* g_frag_dof = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTex_Sharp;
uniform sampler2D uTex_Blur;
uniform vec2 uFocusPoint;
uniform float uFocusRadius;
uniform float uTransition;
uniform float uBlurStrength;

void main() {
    vec4 sharp = texture2D(uTex_Sharp, vTexCoord);
    vec4 blur = texture2D(uTex_Blur, vTexCoord);

    // 计算像素到对焦点的距离
    float dist = distance(vTexCoord, uFocusPoint);
    // 平滑步进计算模糊系数
    float blurFactor = smoothstep(uFocusRadius, uFocusRadius + uTransition, dist);
    blurFactor *= uBlurStrength;

    gl_FragColor = mix(sharp, blur, blurFactor);
}
)";

// ==========================================
// 3. GL 工具函数
// ==========================================
GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    // 编译错误检测
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOGE("Shader compile failed: %s", infoLog);
    }
    return shader;
}

GLuint LinkProgram(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    // 链接错误检测
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(prog, 512, nullptr, infoLog);
        LOGE("Program link failed: %s", infoLog);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void InitFilterResources(int w, int h) {
    // 释放旧资源
    if (RF::screen_tex != 0) {
        glDeleteTextures(1, &RF::screen_tex);
        glDeleteTextures(2, RF::pingpong_tex);
        glDeleteFramebuffers(2, RF::pingpong_fbo);
        if (RF::quad_vbo) glDeleteBuffers(1, &RF::quad_vbo);
        if (RF::quad_ebo) glDeleteBuffers(1, &RF::quad_ebo);
    }

    // 1. 创建屏幕拷贝纹理
    glGenTextures(1, &RF::screen_tex);
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 2. 创建 Ping-Pong FBO
    glGenTextures(2, RF::pingpong_tex);
    glGenFramebuffers(2, RF::pingpong_fbo);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, RF::pingpong_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, RF::pingpong_tex[i], 0);
    }

    // 3. 创建全屏四边形
    float vertices[] = { -1, 1, 0, 1,  -1, -1, 0, 0,  1, -1, 1, 0,  1, 1, 1, 1 };
    unsigned short indices[] = { 0, 1, 2, 0, 2, 3 };
    glGenBuffers(1, &RF::quad_vbo);
    glGenBuffers(1, &RF::quad_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, RF::quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::quad_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // 4. 编译着色器（仅第一次初始化）
    if (RF::prog_draw == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_draw     = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_draw));
        RF::prog_color    = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_color));
        RF::prog_sharpen  = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_sharpen));
        RF::prog_gaussian = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_gaussian));
        RF::prog_dof      = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_dof));
        LOGI("Filter shaders compiled successfully");
    }

    RF::resources_ready = true;
    LOGI("Filter resources initialized: %dx%d", w, h);
}

void BindQuad(GLuint prog) {
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, RF::quad_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::quad_ebo);

    GLint pos_loc = glGetAttribLocation(prog, "aPosition");
    GLint tex_loc = glGetAttribLocation(prog, "aTexCoord");

    if (pos_loc >= 0) {
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    }
    if (tex_loc >= 0) {
        glEnableVertexAttribArray(tex_loc);
        glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    }
}

// ==========================================
// 4. 核心滤镜渲染逻辑
// ==========================================
void RenderFilters(int w, int h) {
    if (!RF::resources_ready) return;

    // 保存滤镜渲染的GL状态
    GLint last_prog, last_fbo, last_tex, last_vp[4], last_active;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active);
    glGetIntegerv(GL_VIEWPORT, last_vp);
    GLboolean last_blend = glIsEnabled(GL_BLEND);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, w, h);

    // 1. 拷贝当前屏幕画面到纹理
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w, h, 0);

    GLuint current_tex = RF::screen_tex;
    int ping_idx = 0;

    // 2. 滤镜链处理
    // Pass 1: 颜色校正 & 风格化
    if (RF::params.enable_correction || RF::params.enable_sepia) {
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_color);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_color, "uTexture"), 0);
        glUniform1f(glGetUniformLocation(RF::prog_color, "uBrightness"), RF::params.enable_correction ? RF::params.brightness : 0.0f);
        glUniform1f(glGetUniformLocation(RF::prog_color, "uContrast"), RF::params.enable_correction ? RF::params.contrast : 1.0f);
        glUniform1f(glGetUniformLocation(RF::prog_color, "uSaturation"), RF::params.enable_correction ? RF::params.saturation : 1.0f);
        glUniform1i(glGetUniformLocation(RF::prog_color, "uEnableSepia"), RF::params.enable_sepia ? 1 : 0);
        glUniform1f(glGetUniformLocation(RF::prog_color, "uSepia"), RF::params.sepia_intensity);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        current_tex = RF::pingpong_tex[ping_idx];
        ping_idx = 1 - ping_idx;
    }

    // Pass 2: 锐化
    if (RF::params.enable_sharpen) {
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_sharpen);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_sharpen, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(RF::prog_sharpen, "uTexelSize"), 1.0f / w, 1.0f / h);
        glUniform1f(glGetUniformLocation(RF::prog_sharpen, "uIntensity"), RF::params.sharpen_intensity);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        current_tex = RF::pingpong_tex[ping_idx];
        ping_idx = 1 - ping_idx;
    }

    // Pass 3: 景深特效
    GLuint final_tex = current_tex;
    if (RF::params.enable_dof) {
        // 第一步：水平模糊
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_gaussian);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_gaussian, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uTexelSize"), 1.0f / w, 1.0f / h);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uDirection"), 1.0f, 0.0f);
        glUniform1f(glGetUniformLocation(RF::prog_gaussian, "uRadius"), RF::params.blur_strength * 4.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        ping_idx = 1 - ping_idx;

        // 第二步：垂直模糊
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        glBindTexture(GL_TEXTURE_2D, RF::pingpong_tex[1 - ping_idx]);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uDirection"), 0.0f, 1.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        GLuint blurred_tex = RF::pingpong_tex[ping_idx];

        // 第三步：景深合成
        ping_idx = 1 - ping_idx;
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_dof);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_dof, "uTex_Sharp"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, blurred_tex);
        glUniform1i(glGetUniformLocation(RF::prog_dof, "uTex_Blur"), 1);

        glUniform2f(glGetUniformLocation(RF::prog_dof, "uFocusPoint"), RF::params.focus_point.x, RF::params.focus_point.y);
        glUniform1f(glGetUniformLocation(RF::prog_dof, "uFocusRadius"), RF::params.focus_radius);
        glUniform1f(glGetUniformLocation(RF::prog_dof, "uTransition"), RF::params.transition);
        glUniform1f(glGetUniformLocation(RF::prog_dof, "uBlurStrength"), 1.0f);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        final_tex = RF::pingpong_tex[ping_idx];
    }

    // 3. 把处理完的画面画回屏幕
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    BindQuad(RF::prog_draw);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, final_tex);
    glUniform1i(glGetUniformLocation(RF::prog_draw, "uTexture"), 0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    // 恢复GL状态
    glUseProgram(last_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glActiveTexture(last_active);
    glBindTexture(GL_TEXTURE_2D, last_tex);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

// ==========================================
// 5. UI 状态与主题
// ==========================================
static bool g_ShowUI = true;
static float g_FontScale = 1.0f;
static ImFont* g_UIFont = nullptr;

// ImGui 全局状态
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

// GL 全状态保护（照搬稳定版逻辑）
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

// UI 主题
static void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // RenderFusion 蓝紫科技主题
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.1f, 0.1f, 0.15f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.2f, 0.4f, 0.8f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.25f, 0.45f, 0.9f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.55f, 1.0f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.35f, 0.8f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.2f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.3f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.3f, 0.5f, 1.0f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.3f, 0.3f, 0.5f, 0.5f);
    c[ImGuiCol_Header] = ImVec4(0.2f, 0.3f, 0.6f, 0.6f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.35f, 0.7f, 0.8f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.3f, 0.4f, 0.8f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.2f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.45f, 0.9f, 0.8f);
    c[ImGuiCol_TabActive] = ImVec4(0.2f, 0.4f, 0.8f, 1.0f);

    // 圆角与间距
    s.WindowRounding = 12.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.WindowPadding = ImVec2(16, 12);
    s.FramePadding = ImVec2(10, 8);
    s.ItemSpacing = ImVec2(10, 8);
}

// ==========================================
// 6. 滤镜控制面板 UI
// ==========================================
static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);
    ImGuiIO& io = ImGui::GetIO();

    // 侧边悬浮球（收起状态）
    if (!g_ShowUI) {
        ImGui::SetNextWindowPos(ImVec2(20, io.DisplaySize.y * 0.5f), ImGuiCond_Always);
        ImGui::Begin("##Reopen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 50.0f);
        if (ImGui::Button("RF", ImVec2(70, 70))) g_ShowUI = true;
        ImGui::PopStyleVar();
        ImGui::End();
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // 主控制面板
    ImGui::SetNextWindowSize(ImVec2(480, 650), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGui::Begin("RenderFusion", &g_ShowUI, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

    // 标题栏
    ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "RenderFusion v1.0");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
    if (ImGui::Button("Hide", ImVec2(70, 30))) g_ShowUI = false;
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));

    // 点击设置对焦点逻辑
    if (RF::focus_pending && io.MouseClicked[0] && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        RF::params.focus_point = ImVec2(io.MousePos.x / g_Width, io.MousePos.y / g_Height);
        RF::focus_pending = false;
    }

    // 分 Tab 页控制
    if (ImGui::BeginTabBar("FilterTabs")) {
        // 基础画质 Tab
        if (ImGui::BeginTabItem("Image")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Checkbox("Enable Image Correction", &RF::params.enable_correction);
            ImGui::SameLine();
            ImGui::Checkbox("Sepia Tone", &RF::params.enable_sepia);
            
            ImGui::Dummy(ImVec2(0, 8));
            if (RF::params.enable_correction) {
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Brightness", &RF::params.brightness, -0.5f, 0.5f);
                ImGui::SliderFloat("Contrast", &RF::params.contrast, 0.6f, 1.8f);
                ImGui::SliderFloat("Saturation", &RF::params.saturation, 0.0f, 2.0f);
                ImGui::PopItemWidth();
            }
            if (RF::params.enable_sepia) {
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Sepia Intensity", &RF::params.sepia_intensity, 0.0f, 1.0f);
                ImGui::PopItemWidth();
            }
            ImGui::EndTabItem();
        }

        // 特效 Tab
        if (ImGui::BeginTabItem("Effects")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Checkbox("Sharpen", &RF::params.enable_sharpen);
            if (RF::params.enable_sharpen) {
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Sharpen Intensity", &RF::params.sharpen_intensity, 0.0f, 1.5f);
                ImGui::PopItemWidth();
            }
            ImGui::EndTabItem();
        }

        // 景深 Tab
        if (ImGui::BeginTabItem("Bokeh DOF")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Checkbox("Enable Depth of Field", &RF::params.enable_dof);
            
            if (RF::params.enable_dof) {
                ImGui::Dummy(ImVec2(0, 8));
                // 焦点设置按钮
                if (ImGui::Button(RF::focus_pending ? "Click Screen to Set Focus!" : "Set Focus Point", ImVec2(-1, 40))) {
                    RF::focus_pending = !RF::focus_pending;
                }
                ImGui::Text("Current Focus: (%.2f, %.2f)", RF::params.focus_point.x, RF::params.focus_point.y);
                
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Clear Radius", &RF::params.focus_radius, 0.05f, 0.5f);
                ImGui::SliderFloat("Transition Softness", &RF::params.transition, 0.05f, 0.5f);
                ImGui::SliderFloat("Blur Strength", &RF::params.blur_strength, 0.0f, 3.0f);
                ImGui::PopItemWidth();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    if (g_UIFont) ImGui::PopFont();
}

// ==========================================
// 7. ImGui 初始化与渲染
// ==========================================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    
    LOGI("Setting up ImGui...");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 自动适配屏幕的字体缩放
    float baseScale = (float)g_Height / 720.0f;
    g_FontScale = std::clamp(baseScale, 1.0f, 2.0f);

    ImFontConfig cfg;
    cfg.SizePixels = (float)(int)(20.0f * g_FontScale);
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    g_UIFont = io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    SetupStyle();

    g_Initialized = true;
    LOGI("ImGui Setup Complete!");
}

static void RenderUI() {
    if (!g_Initialized) return;
    
    GLState s; 
    SaveGL(s);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

// ==========================================
// 8. Hook 函数指针
// ==========================================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ==========================================
// 9. EGL 核心 Hook（渲染入口）
// ==========================================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, s);
    
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d, s);

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, s);

    // 目标缓冲检测
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf;
        eglQuerySurface(d, s, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
            LOGI("Target acquired: %dx%d", w, h);
        }
    }

    // Context 丢失保护
    if (ctx != g_TargetContext || s != g_TargetSurface) {
        if (g_Initialized || RF::resources_ready) {
            LOGI("Context Lost, Resetting All Resources");
            // 重置 ImGui
            g_Initialized = false;
            g_TargetContext = EGL_NO_CONTEXT;
            g_TargetSurface = EGL_NO_SURFACE;
            // 重置滤镜资源
            RF::resources_ready = false;
            RF::screen_tex = 0;
        }
        return orig_eglSwapBuffers(d, s);
    }

    g_Width = w; 
    g_Height = h;

    // 初始化资源
    if (!RF::resources_ready) InitFilterResources(w, h);
    Setup();

    // 【关键顺序】先渲染滤镜处理画面 → 再渲染UI
    RenderFilters(w, h);
    RenderUI();

    return orig_eglSwapBuffers(d, s);
}

// ==========================================
// 10. Input Hook（触摸事件）
// ==========================================
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

// ==========================================
// 11. 主线程入口
// ==========================================
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
