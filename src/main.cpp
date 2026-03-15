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
// 1. Filter State & Params (ALL OFF BY DEFAULT)
// ==========================================
namespace RF {
    // GL Resources
    GLuint screen_tex = 0;
    GLuint pingpong_fbo[2] = {0, 0};
    GLuint pingpong_tex[2] = {0, 0};
    GLuint quad_vbo = 0, quad_ebo = 0;

    // Shader Programs
    GLuint prog_draw = 0;
    GLuint prog_master = 0;
    GLuint prog_sharpen = 0;
    GLuint prog_gaussian = 0;
    GLuint prog_dof = 0;
    GLuint prog_outline = 0;

    bool resources_ready = false;
    bool focus_pending = false;
    int current_preset = 0;

    // Filter Parameters (ALL FEATURES OFF BY DEFAULT)
    struct FilterParams {
        // Base Adjustment
        bool enable_master = false;
        float brightness = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float temperature = 0.0f;
        float vignette = 0.0f;

        // Stylize
        bool enable_bw = false;
        bool enable_sepia = false;
        float sepia_intensity = 0.8f;
        float film_grain = 0.0f;

        // Effects
        bool enable_sharpen = false;
        float sharpen_intensity = 0.5f;
        
        bool enable_outline = false;
        float outline_thresh = 0.2f;
        float outline_color = 0.0f;  // 0.0 = 黑色
        bool enable_posterize = false;  // 新增：色调分离开关
        float posterize_levels = 8.0f;  // 新增：色调分离等级

        // Bokeh DOF
        bool enable_dof = false;
        ImVec2 focus_point = ImVec2(0.5f, 0.5f);
        float focus_radius = 0.15f;
        float blur_strength = 1.0f;
        float transition = 0.2f;
        float chromatic = 0.0f;
    };
    
    FilterParams params;
    
    // Preset Definition
    struct Preset {
        const char* name;
        FilterParams p;
    };

    void ApplyPreset(int idx) {
        Preset presets[] = {
            {"Original", {false, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, false, false, 0.8f, 0.0f, false, 0.5f, false, 0.2f, 0.0f, false, 8.0f, false, {0.5f,0.5f}, 0.15f, 1.0f, 0.2f, 0.0f}},
            {"Fresh & Clean", {true, 0.05f, 1.1f, 1.15f, 0.1f, 0.15f, false, false, 0.0f, 0.05f, false, 0.3f, false, 0.2f, 0.0f, false, 8.0f, false, {0.5f,0.5f}, 0.15f, 1.0f, 0.2f, 0.0f}},
            {"Vintage Film", {true, 0.0f, 1.05f, 0.85f, -0.2f, 0.35f, false, true, 0.6f, 0.15f, false, 0.2f, false, 0.2f, 0.0f, false, 8.0f, false, {0.5f,0.5f}, 0.15f, 1.0f, 0.2f, 0.0f}},
            {"High Contrast B&W", {true, 0.0f, 1.3f, 0.0f, 0.0f, 0.2f, true, false, 0.0f, 0.0f, true, 0.8f, false, 0.2f, 0.0f, false, 8.0f, false, {0.5f,0.5f}, 0.15f, 1.0f, 0.2f, 0.0f}},
            {"Cinematic", {true, -0.03f, 1.15f, 0.95f, -0.1f, 0.25f, false, false, 0.0f, 0.03f, false, 0.4f, false, 0.2f, 0.0f, false, 8.0f, true, {0.5f,0.5f}, 0.12f, 1.5f, 0.15f, 0.02f}}
        };
        
        if (idx >= 0 && idx < 5) {
            bool dof_was_on = params.enable_dof;
            ImVec2 prev_focus = params.focus_point;
            params = presets[idx].p;
            if (dof_was_on && !params.enable_dof) {
                params.enable_dof = true;
                params.focus_point = prev_focus;
            }
        }
    }
}

// ==========================================
// 2. Shader Source
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

const char* g_frag_draw = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

const char* g_frag_master = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uTime;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uTemperature;
uniform float uVignette;
uniform int uEnableBW;
uniform int uEnableSepia;
uniform float uSepia;
uniform float uGrain;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898,78.233))) * 43758.5453123); }

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;

    result = result + uBrightness;
    result = (result - 0.5) * uContrast + 0.5;

    vec3 warmFilter = vec3(1.0, 0.9, 0.8);
    vec3 coolFilter = vec3(0.8, 0.9, 1.0);
    vec3 tempFilter = mix(vec3(1.0), uTemperature > 0.0 ? warmFilter : coolFilter, abs(uTemperature));
    result *= tempFilter;

    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, uSaturation);

    if (uEnableBW == 1) result = vec3(gray);

    if (uEnableSepia == 1) {
        vec3 sepiaColor;
        sepiaColor.r = dot(result, vec3(0.393, 0.769, 0.189));
        sepiaColor.g = dot(result, vec3(0.349, 0.686, 0.168));
        sepiaColor.b = dot(result, vec3(0.272, 0.534, 0.131));
        result = mix(result, sepiaColor, uSepia);
    }

    vec2 center = vec2(0.5);
    float dist = distance(vTexCoord, center);
    float vignette = smoothstep(0.8, 0.3, dist * uVignette + (1.0 - uVignette));
    result *= vignette;

    if (uGrain > 0.0) {
        float noise = (random(vTexCoord + uTime) - 0.5) * uGrain;
        result += noise;
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
uniform float uChromatic;
void main() {
    vec4 sharp = texture2D(uTex_Sharp, vTexCoord);
    vec4 blur = texture2D(uTex_Blur, vTexCoord);
    if (uChromatic > 0.0) {
        float dist = distance(vTexCoord, uFocusPoint);
        float ca = dist * uChromatic * 2.0;
        vec2 offset = (vTexCoord - uFocusPoint) * ca;
        sharp.r = texture2D(uTex_Sharp, vTexCoord + offset).r;
        sharp.b = texture2D(uTex_Sharp, vTexCoord - offset).b;
    }
    float dist = distance(vTexCoord, uFocusPoint);
    float blurFactor = smoothstep(uFocusRadius, uFocusRadius + uTransition, dist);
    blurFactor *= uBlurStrength;
    gl_FragColor = mix(sharp, blur, blurFactor);
}
)";

// 修复：分离描边和色调分离效果
const char* g_frag_outline = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uThresh;
uniform float uColor;
uniform int uEnablePosterize;  // 新增：是否启用色调分离
uniform float uPosterizeLevels; // 新增：色调分离等级
void main() {
    vec3 center = texture2D(uTexture, vTexCoord).rgb;
    float grayCenter = dot(center, vec3(0.299, 0.587, 0.114));
    
    // Sobel边缘检测
    float gx = 0.0; float gy = 0.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -1.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  1.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -2.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  2.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -1.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -2.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -1.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  1.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  2.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  1.0;
    
    float edge = sqrt(gx*gx + gy*gy);
    
    if (edge > uThresh) {
        // 边缘使用指定颜色（黑色或白色）
        gl_FragColor = vec4(vec3(uColor), 1.0);
    } else {
        // 非边缘区域：根据开关决定是否进行色调分离
        vec3 result = center;
        if (uEnablePosterize == 1) {
            result = floor(center * uPosterizeLevels) / uPosterizeLevels;
        }
        gl_FragColor = vec4(result, 1.0);
    }
}
)";

static float g_Time = 0.0f;

// ==========================================
// 3. GL Utils
// ==========================================
GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
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
    if (RF::screen_tex != 0) {
        glDeleteTextures(1, &RF::screen_tex);
        glDeleteTextures(2, RF::pingpong_tex);
        glDeleteFramebuffers(2, RF::pingpong_fbo);
        if(RF::quad_vbo) glDeleteBuffers(1, &RF::quad_vbo);
        if(RF::quad_ebo) glDeleteBuffers(1, &RF::quad_ebo);
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
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, RF::pingpong_tex[i], 0);
    }

    float vertices[] = { -1, 1, 0, 1,  -1, -1, 0, 0,  1, -1, 1, 0,  1, 1, 1, 1 };
    unsigned short indices[] = { 0, 1, 2, 0, 2, 3 };
    glGenBuffers(1, &RF::quad_vbo);
    glGenBuffers(1, &RF::quad_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, RF::quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::quad_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    if (RF::prog_draw == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_draw     = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_draw));
        RF::prog_master    = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_master));
        RF::prog_sharpen   = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_sharpen));
        RF::prog_gaussian  = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_gaussian));
        RF::prog_dof       = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_dof));
        RF::prog_outline   = LinkProgram(CompileShader(GL_VERTEX_SHADER, g_quad_vert), CompileShader(GL_FRAGMENT_SHADER, g_frag_outline));
        LOGI("All shaders compiled successfully");
    }

    RF::resources_ready = true;
}

void BindQuad(GLuint prog) {
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, RF::quad_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::quad_ebo);
    GLint pos = glGetAttribLocation(prog, "aPosition");
    GLint tex = glGetAttribLocation(prog, "aTexCoord");
    if (pos >= 0) { glEnableVertexAttribArray(pos); glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0); }
    if (tex >= 0) { glEnableVertexAttribArray(tex); glVertexAttribPointer(tex, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); }
}

// ==========================================
// 4. Filter Rendering (Switch-Controlled, Safe)
// ==========================================
void RenderFilters(int w, int h) {
    if (!RF::resources_ready) return;
    g_Time += 0.016f;

    // Save GL State
    GLint last_prog, last_fbo, last_tex, last_vp[4], last_active;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active);
    glGetIntegerv(GL_VIEWPORT, last_vp);
    GLboolean last_blend = glIsEnabled(GL_BLEND);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glViewport(0, 0, w, h);

    // Step 1: Copy screen to texture
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w, h, 0);

    GLuint current_tex = RF::screen_tex;
    int ping_idx = 0;
    bool has_processed = false;  // 标记是否有任何滤镜被应用

    // Step 2: Filter Chain (ONLY RUN WHEN SWITCH IS ON)
    // Pass 1: Master Adjustment
    if (RF::params.enable_master || RF::params.enable_bw || RF::params.enable_sepia) {
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_master);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_master, "uTexture"), 0);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uTime"), g_Time);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uBrightness"), RF::params.brightness);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uContrast"), RF::params.contrast);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uSaturation"), RF::params.saturation);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uTemperature"), RF::params.temperature);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uVignette"), RF::params.vignette);
        glUniform1i(glGetUniformLocation(RF::prog_master, "uEnableBW"), RF::params.enable_bw ? 1 : 0);
        glUniform1i(glGetUniformLocation(RF::prog_master, "uEnableSepia"), RF::params.enable_sepia ? 1 : 0);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uSepia"), RF::params.sepia_intensity);
        glUniform1f(glGetUniformLocation(RF::prog_master, "uGrain"), RF::params.film_grain);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        current_tex = RF::pingpong_tex[ping_idx];
        ping_idx = 1 - ping_idx;
        has_processed = true;
    }

    // Pass 2: Outline (Toon)
    if (RF::params.enable_outline) {
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_outline);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_outline, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(RF::prog_outline, "uTexelSize"), 1.0f/w, 1.0f/h);
        glUniform1f(glGetUniformLocation(RF::prog_outline, "uThresh"), RF::params.outline_thresh);
        glUniform1f(glGetUniformLocation(RF::prog_outline, "uColor"), RF::params.outline_color);
        // 新增：传递色调分离参数
        glUniform1i(glGetUniformLocation(RF::prog_outline, "uEnablePosterize"), RF::params.enable_posterize ? 1 : 0);
        glUniform1f(glGetUniformLocation(RF::prog_outline, "uPosterizeLevels"), RF::params.posterize_levels);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        current_tex = RF::pingpong_tex[ping_idx];
        ping_idx = 1 - ping_idx;
        has_processed = true;
    }

    // Pass 3: Sharpen
    if (RF::params.enable_sharpen) {
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_sharpen);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_sharpen, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(RF::prog_sharpen, "uTexelSize"), 1.0f/w, 1.0f/h);
        glUniform1f(glGetUniformLocation(RF::prog_sharpen, "uIntensity"), RF::params.sharpen_intensity);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        current_tex = RF::pingpong_tex[ping_idx];
        ping_idx = 1 - ping_idx;
        has_processed = true;
    }

    // Pass 4: Bokeh DOF
    GLuint final_tex = current_tex;
    if (RF::params.enable_dof) {
        // Blur Pass 1: Horizontal
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        BindQuad(RF::prog_gaussian);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, current_tex);
        glUniform1i(glGetUniformLocation(RF::prog_gaussian, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uTexelSize"), 1.0f/w, 1.0f/h);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uDirection"), 1.0f, 0.0f);
        glUniform1f(glGetUniformLocation(RF::prog_gaussian, "uRadius"), RF::params.blur_strength * 4.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        ping_idx = 1 - ping_idx;

        // Blur Pass 2: Vertical
        glBindFramebuffer(GL_FRAMEBUFFER, RF::pingpong_fbo[ping_idx]);
        glBindTexture(GL_TEXTURE_2D, RF::pingpong_tex[1-ping_idx]);
        glUniform2f(glGetUniformLocation(RF::prog_gaussian, "uDirection"), 0.0f, 1.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        GLuint blurred_tex = RF::pingpong_tex[ping_idx];

        // DOF Composite
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
        glUniform1f(glGetUniformLocation(RF::prog_dof, "uChromatic"), RF::params.chromatic);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        final_tex = RF::pingpong_tex[ping_idx];
        has_processed = true;
    }

    // Step 3: Draw final result back to screen (NO glClear, avoid black screen)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    BindQuad(RF::prog_draw);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, final_tex);
    glUniform1i(glGetUniformLocation(RF::prog_draw, "uTexture"), 0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    // Restore GL State
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
// 5. UI State & Theme
// ==========================================
static bool g_ShowUI = true;
static float g_FontScale = 1.0f;
static ImFont* g_UIFont = nullptr;
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

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

static void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    ImVec4 bg_dark(0.10f, 0.10f, 0.10f, 0.98f);
    ImVec4 bg_medium(0.15f, 0.15f, 0.15f, 1.0f);
    ImVec4 gold(0.85f, 0.70f, 0.30f, 1.0f);
    ImVec4 text_main(0.95f, 0.95f, 0.95f, 1.0f);

    c[ImGuiCol_WindowBg] = bg_dark;
    c[ImGuiCol_ChildBg] = bg_medium;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    c[ImGuiCol_ButtonActive] = gold;
    c[ImGuiCol_SliderGrab] = gold;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.80f, 0.40f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_Text] = text_main;
    c[ImGuiCol_Header] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.70f, 0.30f, 0.5f);

    s.WindowRounding = 16.0f;
    s.ChildRounding = 12.0f;
    s.FrameRounding = 8.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(12, 10);
    s.ItemSpacing = ImVec2(10, 8);
}

// ==========================================
// 6. UI Drawing (Full English, Fine-Tune Sliders)
// ==========================================
static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);
    ImGuiIO& io = ImGui::GetIO();

    // Floating Button
    if (!g_ShowUI) {
        ImGui::SetNextWindowPos(ImVec2(20, io.DisplaySize.y * 0.5f), ImGuiCond_Always);
        ImGui::Begin("##Reopen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 50.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.70f, 0.30f, 1.0f));
        if (ImGui::Button("RF", ImVec2(75, 75))) g_ShowUI = true;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::End();
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // Main Window
    ImGui::SetNextWindowSize(ImVec2(520, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGui::Begin("RenderFusion", &g_ShowUI, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Header
    ImVec2 win_size = ImGui::GetWindowSize();
    ImGui::SetCursorPos(ImVec2(16, 12));
    ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "Universal Filters");
    
    ImGui::SameLine(win_size.x - 60);
    ImGui::SetCursorPosY(12);
    if (ImGui::Button("X", ImVec2(36, 36))) g_ShowUI = false;
    
    ImGui::SetCursorPosY(52);
    ImGui::Separator();

    // Layout: Presets + Controls
    ImGui::SetCursorPosY(60);
    ImGui::BeginChild("MainLayout", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
    
    // Left: Preset List
    ImGui::BeginChild("Presets", ImVec2(160, 0), true, ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorPos(ImVec2(12, 12));
    ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "Custom");
    ImGui::Dummy(ImVec2(0, 8));
    
    const char* preset_names[] = {"Original", "Fresh & Clean", "Vintage Film", "High Contrast B&W", "Cinematic"};
    for (int i = 0; i < 5; i++) {
        bool is_selected = (RF::current_preset == i);
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.85f, 0.70f, 0.30f, 0.3f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.70f, 0.30f, 1.0f));
        }
        
        if (ImGui::Selectable(preset_names[i], is_selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 36))) {
            RF::current_preset = i;
            RF::ApplyPreset(i);
        }
        
        if (is_selected) ImGui::PopStyleColor(2);
    }
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // Right: Controls
    ImGui::BeginChild("Controls", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorPos(ImVec2(16, 12));

    // Focus point click logic
    if (RF::focus_pending && io.MouseClicked[0] && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        RF::params.focus_point = ImVec2(io.MousePos.x / g_Width, io.MousePos.y / g_Height);
        RF::focus_pending = false;
    }

    if (ImGui::BeginTabBar("ControlTabs")) {
        // Adjust Tab
        if (ImGui::BeginTabItem("Adjust")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Enable Adjustment", &RF::params.enable_master);
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushItemWidth(-1);
            
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Temperature");
            ImGui::SliderFloat("##Temp", &RF::params.temperature, -1.0f, 1.0f, "%.2f");
            
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Saturation");
            ImGui::SliderFloat("##Sat", &RF::params.saturation, 0.0f, 2.0f, "%.2f");
            
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Contrast");
            ImGui::SliderFloat("##Cont", &RF::params.contrast, 0.6f, 1.8f, "%.2f");
            
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Brightness");
            ImGui::SliderFloat("##Bright", &RF::params.brightness, -0.5f, 0.5f, "%.2f");
            
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Vignette");
            ImGui::SliderFloat("##Vignette", &RF::params.vignette, 0.0f, 1.0f, "%.2f");

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Film Grain");
            ImGui::SliderFloat("##Grain", &RF::params.film_grain, 0.0f, 0.3f, "%.3f");
            
            ImGui::PopItemWidth();
            ImGui::EndTabItem();
        }

        // Stylize Tab
        if (ImGui::BeginTabItem("Stylize")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Black & White", &RF::params.enable_bw);
            ImGui::SameLine();
            ImGui::Checkbox("Vintage Sepia", &RF::params.enable_sepia);
            
            if (RF::params.enable_sepia) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Sepia Intensity", &RF::params.sepia_intensity, 0.0f, 1.0f, "%.2f");
                ImGui::PopItemWidth();
            }
            
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Outline Edge", &RF::params.enable_outline);
            if (RF::params.enable_outline) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushItemWidth(-1);
                // 默认黑色，范围0-1，0=黑，1=白
                ImGui::SliderFloat("Edge Color", &RF::params.outline_color, 0.0f, 1.0f, "%.2f (0=Black, 1=White)");
                ImGui::SliderFloat("Edge Threshold", &RF::params.outline_thresh, 0.05f, 0.5f, "%.2f");
                
                // 新增：色调分离开关
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Checkbox("Enable Posterize (Toon Shading)", &RF::params.enable_posterize);
                if (RF::params.enable_posterize) {
                    ImGui::SliderFloat("Posterize Levels", &RF::params.posterize_levels, 2.0f, 16.0f, "%.0f");
                }
                ImGui::PopItemWidth();
            }

            ImGui::EndTabItem();
        }

        // Effects Tab
        if (ImGui::BeginTabItem("Effects")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Sharpen", &RF::params.enable_sharpen);
            if (RF::params.enable_sharpen) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Sharpness Intensity", &RF::params.sharpen_intensity, 0.0f, 1.5f, "%.2f");
                ImGui::PopItemWidth();
            }
            ImGui::EndTabItem();
        }

        // Bokeh DOF Tab
        if (ImGui::BeginTabItem("Bokeh DOF")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Enable Depth of Field", &RF::params.enable_dof);
            
            if (RF::params.enable_dof) {
                ImGui::Dummy(ImVec2(0, 8));
                if (ImGui::Button(RF::focus_pending ? "Tap Screen to Set Focus!" : "Set Focus Point", ImVec2(-1, 40))) {
                    RF::focus_pending = !RF::focus_pending;
                }
                ImGui::Text("Current Focus: (%.2f, %.2f)", RF::params.focus_point.x, RF::params.focus_point.y);
                
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("Clear Radius", &RF::params.focus_radius, 0.05f, 0.5f, "%.2f");
                ImGui::SliderFloat("Transition Softness", &RF::params.transition, 0.05f, 0.5f, "%.2f");
                ImGui::SliderFloat("Blur Strength", &RF::params.blur_strength, 0.0f, 3.0f, "%.2f");
                ImGui::SliderFloat("Chromatic Aberration", &RF::params.chromatic, 0.0f, 0.1f, "%.3f");
                ImGui::PopItemWidth();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // Reset Button
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 60);
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 60);
    if (ImGui::Button("Reset All", ImVec2(120, 40))) {
        RF::current_preset = 0;
        RF::ApplyPreset(0);
    }

    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::End();
    
    if (g_UIFont) ImGui::PopFont();
}

// ==========================================
// 7. Init & Hook
// ==========================================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    LOGI("Setting up ImGui...");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

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
}

static void RenderUI() {
    if (!g_Initialized) return;
    GLState s; SaveGL(s);

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

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, s);
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d, s);

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, s);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf;
        eglQuerySurface(d, s, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
            LOGI("Target acquired: %dx%d", w, h);
        }
    }

    if (ctx != g_TargetContext || s != g_TargetSurface) {
        if (g_Initialized || RF::resources_ready) {
            LOGI("Context Lost, Resetting");
            g_Initialized = false;
            g_TargetContext = EGL_NO_CONTEXT;
            g_TargetSurface = EGL_NO_SURFACE;
            RF::resources_ready = false;
            RF::screen_tex = 0;
        }
        return orig_eglSwapBuffers(d, s);
    }

    g_Width = w; g_Height = h;
    if (!RF::resources_ready) InitFilterResources(w, h);
    Setup();

    RenderFilters(w, h);
    RenderUI();

    return orig_eglSwapBuffers(d, s);
}

static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_Initialized) ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}

static void HookInput() {
    void* handle = GlossOpen("libinput.so");
    if (handle) {
        void* s1 = (void*)GlossSymbol(handle, "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
        if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);
        void* s2 = (void*)GlossSymbol(handle, "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
        if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
    }
}

static void* MainThread(void*) {
    sleep(3);
    LOGI("RenderFusion Pro loaded");
    GlossInit(true);
    
    GHandle egl = GlossOpen("libEGL.so");
    if (egl) {
        void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    }
    HookInput();
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
