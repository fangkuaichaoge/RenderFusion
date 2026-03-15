#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <cmath>
#include <mutex>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define RF_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "RenderFusion", __VA_ARGS__)

// ==================== 全局状态管理 ====================
namespace RF {
    // 基础状态
    bool initialized = false;
    int width = 0, height = 0;
    EGLContext target_ctx = EGL_NO_CONTEXT;
    EGLSurface target_surface = EGL_NO_SURFACE;

    // GL 资源
    GLuint screen_tex = 0;
    GLuint pingpong_fbo[2] = {0, 0};
    GLuint pingpong_tex[2] = {0, 0};
    GLuint vbo = 0, ebo = 0;

    // 着色器程序
    GLuint prog_draw = 0;
    GLuint prog_color = 0;
    GLuint prog_sharpen = 0;
    GLuint prog_gaussian = 0;
    GLuint prog_dof = 0;

    // 交互状态
    bool focus_pending = false; // 是否等待点击设置焦点

    // 滤镜参数
    struct Params {
        // 基础
        bool enable_correction = false;
        float brightness = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        
        // 风格化
        bool enable_sepia = false;
        float sepia = 0.8f;

        // 特效
        bool enable_sharpen = false;
        float sharpen = 0.5f;

        // 景深
        bool enable_dof = false;
        ImVec2 focus_point = ImVec2(0.5f, 0.5f);
        float focus_radius = 0.15f;
        float blur_strength = 1.0f;
        float transition = 0.2f;
    };
    Params params;
}

// ==================== 着色器源码 ====================
const char* g_vert = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)";

const char* g_frag_draw = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

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

    result = result + uBrightness;
    result = (result - 0.5) * uContrast + 0.5;

    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, uSaturation);

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

const char* g_frag_sharpen = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec3 center = texture2D(uTexture, vTexCoord).rgb;
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

const char* g_frag_gaussian = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
uniform float uRadius;

void main() {
    vec4 result = vec4(0.0);
    float weights[5]; weights[0] = 0.227027; weights[1] = 0.1945946; weights[2] = 0.1216216; weights[3] = 0.054054; weights[4] = 0.016216;
    
    result += texture2D(uTexture, vTexCoord) * weights[0];
    for(int i = 1; i < 5; i++) {
        vec2 offset = uDirection * uTexelSize * float(i) * uRadius;
        result += texture2D(uTexture, vTexCoord + offset) * weights[i];
        result += texture2D(uTexture, vTexCoord - offset) * weights[i];
    }
    gl_FragColor = result;
}
)";

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
    float dist = distance(vTexCoord, uFocusPoint);
    float blurFactor = smoothstep(uFocusRadius, uFocusRadius + uTransition, dist);
    blurFactor *= uBlurStrength;
    gl_FragColor = mix(sharp, blur, blurFactor);
}
)";

// ==================== GL 工具函数 ====================
GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    return shader;
}

GLuint LinkProgram(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void InitResources(int w, int h) {
    if (RF::screen_tex != 0) {
        glDeleteTextures(1, &RF::screen_tex);
        glDeleteTextures(2, RF::pingpong_tex);
        glDeleteFramebuffers(2, RF::pingpong_fbo);
        if(RF::vbo) glDeleteBuffers(1, &RF::vbo);
        if(RF::ebo) glDeleteBuffers(1, &RF::ebo);
    }

    glGenTextures(1, &RF::screen_tex);
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

    float vertices[] = { -1, 1, 0, 1,  -1, -1, 0, 0,  1, -1, 1, 0,  1, 1, 1, 1 };
    unsigned short indices[] = { 0, 1, 2, 0, 2, 3 };
    glGenBuffers(1, &RF::vbo);
    glGenBuffers(1, &RF::ebo);
    glBindBuffer(GL_ARRAY_BUFFER, RF::vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    if (RF::prog_draw == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_vert);
        RF::prog_draw     = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_draw));
        RF::prog_color    = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_color));
        RF::prog_sharpen  = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_sharpen));
        RF::prog_gaussian = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_gaussian));
        RF::prog_dof      = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_dof));
    }

    RF::width = w;
    RF::height = h;
}

void BindQuad(GLuint prog) {
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, RF::vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::ebo);
    GLint pos = glGetAttribLocation(prog, "aPosition");
    GLint tex = glGetAttribLocation(prog, "aTexCoord");
    if (pos >= 0) { glEnableVertexAttribArray(pos); glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0); }
    if (tex >= 0) { glEnableVertexAttribArray(tex); glVertexAttribPointer(tex, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); }
}

// ==================== 渲染逻辑 ====================
void RenderFilters() {
    // 保存状态
    GLint last_prog, last_fbo, last_tex, last_vp[4], last_active_tex;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_tex);
    glGetIntegerv(GL_VIEWPORT, last_vp);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend = glIsEnabled(GL_BLEND);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);

    // 拷贝屏幕
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, RF::width, RF::height, 0);
    glViewport(0, 0, RF::width, RF::height);

    GLuint current_tex = RF::screen_tex;
    int ping_idx = 0;

    // Pass 1: 颜色校正
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
        glUniform1f(glGetUniformLocation(RF::prog_color, "uSepia"), RF::params.sepia);
        
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
        glUniform2f(glGetUniformLocation(RF::prog_sharpen, "uTexelSize"), 1.0f/RF::width, 1.0f/RF::height);
        glUniform1f(glGetUniformLocation(RF::prog_sharpen, "uIntensity"), RF::params.sharpen);
        
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        current_tex = RF::pingpong_tex[ping_idx];
        ping_idx = 1 - ping_idx;
    }

    // Pass 3: 景深
    GLuint final_tex = current_tex;
    if (RF::params.enable_dof) {
        // 1. 模糊
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_gaussian);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_gaussian, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uTexelSize"), 1.0f/RF::width, 1.0f/RF::height);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uDirection"), 1.0f, 0.0f);
        glUniform1f(glGetUniformLocation(RF::prog_gaussian, "uRadius"), RF::params.blur_strength * 4.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        ping_idx = 1 - ping_idx;

        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        glBindTexture(GL_TEXTURE_2D, RF::pingpong_tex[1-ping_idx]);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uDirection"), 0.0f, 1.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        GLuint blurred_tex = RF::pingpong_tex[ping_idx];

        // 2. 合成
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

    // Final: 画回屏幕
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    BindQuad(RF::prog_draw);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, final_tex);
    glUniform1i(glGetUniformLocation(RF::prog_draw, "uTexture"), 0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    // 恢复状态
    glUseProgram(last_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glActiveTexture(last_active_tex);
    glBindTexture(GL_TEXTURE_2D, last_tex);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST);
    if (last_depth) glEnable(GL_DEPTH_TEST);
    if (last_blend) glEnable(GL_BLEND);
}

// ==================== UI 绘制 ====================
void DrawMenu() {
    static bool show_menu = true;
    ImGuiIO& io = ImGui::GetIO();

    // 触发按钮
    ImGui::SetNextWindowPos(ImVec2(10.0f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.0f, 0.5f));
    ImGui::Begin("##Trigger", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
    ImGui::SetWindowFontScale(1.3f);
    if (ImGui::Button(show_menu ? "HIDE" : "MENU", ImVec2(90, 60))) show_menu = !show_menu;
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(2);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();

    if (!show_menu) return;

    // 主窗口
    ImGui::SetNextWindowSize(ImVec2(520, 680), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));

    ImGui::Begin("RenderFusion", &show_menu, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    
    // 1. 顶部标题栏
    ImVec2 win_pos = ImGui::GetWindowPos();
    ImVec2 win_size = ImGui::GetWindowSize();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // 彩虹渐变条
    float time = (float)ImGui::GetTime();
    ImU32 col1 = ImColor(ImVec4(0.2f, 0.5f, 1.0f, 1.0f));
    ImU32 col2 = ImColor(ImVec4(1.0f, 0.3f, 0.8f, 1.0f));
    draw_list->AddRectFilledMultiColor(win_pos, ImVec2(win_pos.x + win_size.x, win_pos.y + 4.0f), col1, col2, col1, col2);

    ImGui::SetCursorPosY(12.0f);
    ImGui::SetCursorPosX(20.0f);
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "RenderFusion");
    ImGui::SameLine(win_size.x - 100);
    ImGui::SetCursorPosY(10.0f);
    if (ImGui::SmallButton("Close")) show_menu = false;
    ImGui::Separator();

    ImGui::SetCursorPosY(30.0f);
    ImGui::SetCursorPosX(20.0f);
    
    // 2. 内容区域
    ImGui::BeginChild("Content", ImVec2(win_size.x - 40, win_size.y - 50), false);
    
    if (ImGui::CollapsingHeader("Image Adjustments", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(10);
        ImGui::Checkbox("Enable Correction", &RF::params.enable_correction);
        ImGui::SameLine();
        ImGui::Checkbox("Sepia Tone", &RF::params.enable_sepia);
        
        if (RF::params.enable_correction) {
            ImGui::PushItemWidth(280);
            ImGui::SliderFloat("Brightness", &RF::params.brightness, -0.4f, 0.4f);
            ImGui::SliderFloat("Contrast", &RF::params.contrast, 0.6f, 1.8f);
            ImGui::SliderFloat("Saturation", &RF::params.saturation, 0.0f, 2.0f);
            ImGui::PopItemWidth();
        }
        if (RF::params.enable_sepia) {
            ImGui::PushItemWidth(280);
            ImGui::SliderFloat("Sepia Intensity", &RF::params.sepia, 0.0f, 1.0f);
            ImGui::PopItemWidth();
        }
        ImGui::Unindent(10);
    }

    ImGui::Dummy(ImVec2(0, 10));

    if (ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(10);
        ImGui::Checkbox("Sharpen", &RF::params.enable_sharpen);
        if (RF::params.enable_sharpen) {
            ImGui::PushItemWidth(280);
            ImGui::SliderFloat("Intensity", &RF::params.sharpen, 0.0f, 1.5f);
            ImGui::PopItemWidth();
        }
        ImGui::Unindent(10);
    }

    ImGui::Dummy(ImVec2(0, 10));

    if (ImGui::CollapsingHeader("Bokeh Depth of Field", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(10);
        ImGui::Checkbox("Enable DOF", &RF::params.enable_dof);
        
        if (RF::params.enable_dof) {
            // 点击设置焦点
            if (ImGui::Button(RF::focus_pending ? "Click on Screen!" : "Set Focus Point", ImVec2(-1, 35))) {
                RF::focus_pending = !RF::focus_pending;
            }
            
            // 检测点击（简单实现）
            if (RF::focus_pending && io.MouseClicked[0]) {
                // 检查是否点击在菜单外
                ImVec2 mouse_pos = io.MousePos;
                if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                    RF::params.focus_point = ImVec2(mouse_pos.x / RF::width, mouse_pos.y / RF::height);
                    RF::focus_pending = false;
                }
            }

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Text("Focus: (%.2f, %.2f)", RF::params.focus_point.x, RF::params.focus_point.y);
            
            ImGui::PushItemWidth(280);
            ImGui::SliderFloat("Clear Radius", &RF::params.focus_radius, 0.05f, 0.5f);
            ImGui::SliderFloat("Transition", &RF::params.transition, 0.05f, 0.5f);
            ImGui::SliderFloat("Blur Strength", &RF::params.blur_strength, 0.0f, 3.0f);
            ImGui::PopItemWidth();
        }
        ImGui::Unindent(10);
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ==================== Hook 部分 ====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

static void SetupImGui() {
    if (RF::initialized) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.4f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    RF::initialized = true;
    RF_LOGI("ImGui Initialized");
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;

    EGLContext ctx = eglGetCurrentContext();
    
    // 防掉保护
    if (ctx != RF::target_ctx || s != RF::target_surface) {
        if (RF::initialized) {
            RF_LOGI("EGL Context changed! Resetting.");
            RF::initialized = false;
            RF::target_ctx = EGL_NO_CONTEXT;
            RF::target_surface = EGL_NO_SURFACE;
            RF::screen_tex = 0; // 标记资源重置
        }
        return orig_eglSwapBuffers(d, s);
    }

    EGLint w, h;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglSwapBuffers(d, s);

    if (RF::target_ctx == EGL_NO_CONTEXT) {
        RF::target_ctx = ctx;
        RF::target_surface = s;
    }

    if (w != RF::width || h != RF::height || RF::screen_tex == 0) {
        InitResources(w, h);
    }

    SetupImGui();

    // 渲染滤镜
    RenderFilters();

    // 渲染 UI
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(RF::width, RF::height);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return orig_eglSwapBuffers(d, s);
}

static void* mainthread(void*) {
    sleep(3);
    GlossInit(true);
    
    GHandle hegl = GlossOpen("libEGL.so");
    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        RF_LOGI("EGL Hooked");
    }
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
