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
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// ===================== GL ERROR CHECK MACRO =====================
#define CHECK_GL_ERROR() { \
    GLenum err = glGetError(); \
    if (err != GL_NO_ERROR) { \
        LOGE("GL ERROR at line %d: 0x%x", __LINE__, err); \
    } \
}

#define SAFE_UNIFORM(loc, func, ...) if(loc >= 0) { func(loc, __VA_ARGS__); CHECK_GL_ERROR(); }

// ==========================================
// 1. Filter State & Params (ALL OFF BY DEFAULT)
// ==========================================
namespace RF {
    // GL Resources
    GLuint screen_tex = 0;
    GLuint fbo = 0;
    GLuint fbo_tex = 0;
    GLuint fbo2 = 0;        // 第二个 FBO（乒乓渲染）
    GLuint fbo_tex2 = 0;    // 第二个 FBO 纹理
    GLuint quad_vbo = 0, quad_ebo = 0;

    // Shader Programs
    GLuint prog_base = 0;       // 基础画面绘制（永远可用，保底不黑屏）
    GLuint prog_master = 0;     // 基础滤镜单Pass
    GLuint prog_outline = 0;    // 描边
    GLuint prog_gaussian = 0;   // 高斯模糊
    GLuint prog_dof = 0;        // 景深
    // Art Style Shaders
        GLuint prog_cel = 0;         // 赛璐璐卡通
        GLuint prog_chinese = 0;     // 中国画
        GLuint prog_sketch = 0;      // 素描
        GLuint prog_anime = 0;       // 二次元平面
        GLuint prog_comic = 0;       // 美漫画

    bool resources_ready = false;
    bool shaders_valid = false;
    bool focus_pending = false;
    int current_preset = 0;

    // Filter Parameters (ALL OFF BY DEFAULT, NO FORCED BLACK EDGE)
    struct FilterParams {
        // Base Adjustment
        bool enable_master = false;
        float brightness = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float temperature = 0.0f;
        float vignette = 0.0f; // 默认0，无黑边

        // Stylize
        bool enable_bw = false;
        bool enable_sepia = false;
        float sepia_intensity = 0.8f;
        float film_grain = 0.0f;

        // Effects
        bool enable_sharpen = false;
        float sharpen_intensity = 0.5f;
        
        // Outline (Pure Black)
        bool enable_outline = false;
        float outline_thresh = 0.15f;
        float outline_opacity = 1.0f;

        // Bokeh DOF
        bool enable_dof = false;
        ImVec2 focus_point = ImVec2(0.5f, 0.5f);
        float focus_radius = 0.15f;
        float blur_strength = 1.0f;
        float transition = 0.2f;
        float chromatic = 0.0f;
        
        // Art Styles
        int art_style = 0;      // 0=Off, 1=Cel Anime, 2=Chinese Painting, 3=Sketch, 4=Anime Flat, 5=Comic
        float art_intensity = 1.0f;
    };
    
    FilterParams params;
    
    // Preset Definition (FIXED: NO FORCED DARK EDGE, NO OVER-DARKEN)
    struct Preset {
        const char* name;
        FilterParams p;
    };

    void ApplyPreset(int idx) {
        Preset presets[] = {
            // Original: 完全无修改，无黑边，无滤镜
            {"Original", {false, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, false, false, 0.8f, 0.0f, false, 0.5f, false, 0.15f, 1.0f, false, {0.5f,0.5f}, 0.15f, 1.0f, 0.2f, 0.0f}},
            // Manga B&W: 黑白漫画风格（高对比度黑白 + 描边）
            {"Manga B&W", {true, 0.05f, 1.15f, 0.0f, 0.0f, 0.0f, true, false, 0.0f, 0.0f, false, 0.5f, true, 0.12f, 1.0f, false, {0.5f,0.5f}, 0.15f, 1.0f, 0.2f, 0.0f}}
        };
        
        if (idx >= 0 && idx < 2) {
            params = presets[idx].p;
        }
    }

    // Check if any multi-pass effect is enabled
    bool IsMultiPassEnabled() {
        return params.enable_outline || params.enable_dof;
    }
}

// ==========================================
// 2. Shader Source (FIXED BRIGHTNESS CALCULATION)
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

// BASE SHADER: 100% WORKING, NO BLACK SCREEN
const char* g_frag_base = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

// MASTER FILTER: FIXED BRIGHTNESS, NO FORCED DARKEN, MANGA-STYLE B&W
const char* g_frag_master = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uTime;

// Base Params
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uTemperature;
uniform float uVignette;

// Stylize
uniform int uEnableBW;
uniform int uEnableSepia;
uniform float uSepia;
uniform float uGrain;

// Sharpen
uniform int uEnableSharpen;
uniform float uSharpness;
uniform vec2 uTexelSize;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898,78.233))) * 43758.5453123); }

// HSV 转换
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// 漫画风格黑白 - 基于色调和饱和度的智能判定
float mangaGray(vec3 color) {
    vec3 hsv = rgb2hsv(color);
    float hue = hsv.x;        // 色调 0-1
    float sat = hsv.y;        // 饱和度 0-1
    float val = hsv.z;        // 明度 0-1
    
    // 基础灰度
    float gray = dot(color, vec3(0.299, 0.587, 0.114));
    
    // 根据色调调整：暖色（红橙黄）稍亮，冷色（蓝紫）稍暗
    // hue: 0=红, 0.16=黄, 0.33=绿, 0.5=青, 0.66=蓝, 0.83=紫
    float warmth = 0.0;
    if (hue < 0.17) {
        // 红到黄：暖色，提亮
        warmth = 0.15 * (1.0 - abs(hue - 0.08) / 0.08);
    } else if (hue > 0.58 && hue < 0.75) {
        // 蓝到青：冷色，压暗
        warmth = -0.1 * (1.0 - abs(hue - 0.66) / 0.08);
    } else if (hue > 0.8) {
        // 紫色：中性偏暗
        warmth = -0.05;
    }
    
    // 高饱和度区域：增强对比，让颜色更分明
    // 低饱和度区域：保持原样
    float satBoost = sat * 0.1;
    
    // 综合灰度值
    gray = gray + warmth + satBoost;
    
    // 高对比度处理
    gray = (gray - 0.5) * 1.6 + 0.5;
    
    // 色调分离 - 4色阶更细腻的漫画效果
    float levels = 4.0;
    gray = floor(gray * levels + 0.5) / levels;
    
    // 最终平滑处理
    gray = smoothstep(0.15, 0.85, gray);
    
    return clamp(gray, 0.0, 1.0);
}

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;

    // Sharpen (inline, single pass)
    if (uEnableSharpen == 1) {
        vec3 sampleTL = texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb;
        vec3 sampleT  = texture2D(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb;
        vec3 sampleTR = texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb;
        vec3 sampleL  = texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb;
        vec3 sampleR  = texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb;
        vec3 sampleBL = texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb;
        vec3 sampleB  = texture2D(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb;
        vec3 sampleBR = texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb;
        vec3 edge = result * 8.0 - (sampleTL + sampleT + sampleTR + sampleL + sampleR + sampleBL + sampleB + sampleBR);
        result = result + edge * uSharpness;
    }

    // Brightness & Contrast (FIXED: 先对比度后亮度，避免压暗画面)
    result = (result - 0.5) * uContrast + 0.5;
    result = result + uBrightness;

    // Temperature
    vec3 warmFilter = vec3(1.0, 0.9, 0.8);
    vec3 coolFilter = vec3(0.8, 0.9, 1.0);
    vec3 tempFilter = mix(vec3(1.0), uTemperature > 0.0 ? warmFilter : coolFilter, abs(uTemperature));
    result *= tempFilter;

    // Saturation
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, uSaturation);

    // Black & White - MANGA STYLE (智能色调判定)
    if (uEnableBW == 1) {
        gray = mangaGray(result);
        result = vec3(gray);
    }

    // Sepia Tone
    if (uEnableSepia == 1) {
        vec3 sepiaColor;
        sepiaColor.r = dot(result, vec3(0.393, 0.769, 0.189));
        sepiaColor.g = dot(result, vec3(0.349, 0.686, 0.168));
        sepiaColor.b = dot(result, vec3(0.272, 0.534, 0.131));
        result = mix(result, sepiaColor, uSepia);
    }

    // Vignette (FIXED: 仅当数值>0时生效，无强制黑边)
    if (uVignette > 0.0) {
        vec2 center = vec2(0.5);
        float dist = distance(vTexCoord, center);
        float vignette = smoothstep(0.8, 0.3, dist * uVignette + (1.0 - uVignette));
        result *= vignette;
    }

    // Film Grain
    if (uGrain > 0.0) {
        float noise = (random(vTexCoord + uTime) - 0.5) * uGrain;
        result += noise;
    }

    // Safe Clamp (避免画面死黑死白)
    gl_FragColor = vec4(clamp(result, 0.001, 0.999), color.a);
}
)";

// Outline Shader (Pure Black Overlay, NO STYLE CHANGE)
const char* g_frag_outline = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uThresh;
uniform float uOpacity;

void main() {
    vec4 original = texture2D(uTexture, vTexCoord);
    float grayCenter = dot(original.rgb, vec3(0.299, 0.587, 0.114));

    // Sobel Edge Detection
    float gx = 0.0, gy = 0.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -1.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  1.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -2.0;
    gx += dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  2.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -1.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) * -2.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  1.0;
    gy += dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114)) *  2.0;

    float edge = sqrt(gx*gx + gy*gy);
    float isEdge = step(uThresh, edge);
    vec3 finalColor = mix(original.rgb, vec3(0.0), isEdge * uOpacity);
    gl_FragColor = vec4(finalColor, original.a);
}
)";

// Gaussian Blur Shader
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
    gl_FragColor = clamp(result, 0.0, 1.0);
}
)";

// DOF Shader
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
    gl_FragColor = mix(sharp, blur, clamp(blurFactor, 0.0, 1.0));
}
)";

// ==========================================
// Art Style Shaders (性能优化版)
// ==========================================

// 赛璐璐卡通画风 (Cel Shading) - 优化版
const char* g_frag_cel = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    
    // 快速边缘检测 (Sobel)
    float c0 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c1 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c2 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c3 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c4 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c5 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c6 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c7 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    
    float gx = -c0 + c1 - 2.0*c4 + 2.0*c5 - c2 + c3;
    float gy = -c0 - 2.0*c6 - c1 + c2 + 2.0*c7 + c3;
    float edge = sqrt(gx*gx + gy*gy);
    float outline = smoothstep(0.1, 0.18, edge);  // 更高的阈值，减少描边
    
    // 6阶色调分离（更细腻）
    vec3 result = floor(color.rgb * 5.99) / 6.0;
    
    // 轻微饱和度提升
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.08);  // 降低饱和度提升
    
    // 描边
    result = mix(result, vec3(0.0), outline * 0.85);
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 中国画风格 (Chinese Painting) - 亮色调版
const char* g_frag_chinese = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898,78.233))) * 43758.5453123); }

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    
    // 亮色调中国画 - 青绿山水风格
    vec3 result = color.rgb;
    
    // 轻微提亮
    result = result * 1.08 + 0.05;
    
    // 青绿色调映射
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    
    // 亮部偏暖（淡黄/米色），暗部偏冷（青绿）
    vec3 warmTone = vec3(1.0, 0.95, 0.85);   // 暖色调（绢纸/宣纸）
    vec3 coolTone = vec3(0.4, 0.65, 0.55);   // 青绿色
    
    // 根据亮度混合冷暖色调
    result = mix(coolTone * gray * 1.2, warmTone, smoothstep(0.3, 0.7, gray));
    
    // 保留部分原色
    result = mix(result, color.rgb, 0.35);
    
    // 柔和色调分离
    result = floor(result * 7.99) / 8.0;
    
    // 轻微笔触纹理
    float paper = random(vTexCoord * 120.0) * 0.04;
    result = result * (1.0 + paper * 0.3);
    
    // 边缘检测 - 毛笔勾勒
    float c4 = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    float c5 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0, 0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c6 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0,-1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c7 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0, 1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float edge = abs(c4 - c5) + abs(c4 - c6) + abs(c4 - c7);
    float outline = smoothstep(0.04, 0.15, edge);
    
    // 淡墨勾勒（不是纯黑）
    result = mix(result, vec3(0.2, 0.25, 0.22), outline * 0.55);
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 素描风格 (Sketch / Pencil)
const char* g_frag_sketch = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898,78.233))) * 43758.5453123); }

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    
    // 保持亮度
    gray = gray * 1.05 + 0.02;
    
    // Sobel 边缘
    float c0 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c1 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c2 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c3 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c4 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c5 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c6 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c7 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    
    float gx = -c0 + c1 - 2.0*c4 + 2.0*c5 - c2 + c3;
    float gy = -c0 - 2.0*c6 - c1 + c2 + 2.0*c7 + c3;
    float edge = sqrt(gx*gx + gy*gy);
    
    // 铅笔线条
    float lines = 1.0 - smoothstep(0.0, 0.4, edge);
    
    // 斜线阴影
    vec2 uv = vTexCoord * 120.0;
    float pattern1 = step(0.5, fract(uv.x + uv.y));
    float pattern2 = step(0.5, fract(uv.x - uv.y));
    
    float hatch = 0.0;
    if(gray < 0.7) hatch += pattern1 * 0.2 * (0.7 - gray) / 0.7;
    if(gray < 0.5) hatch += pattern2 * 0.3 * (0.5 - gray) / 0.5;
    if(gray < 0.3) hatch += 0.35 * (0.3 - gray) / 0.3;
    
    // 纸张纹理
    float paper = random(vTexCoord * 200.0) * 0.08;
    
    float result = lines - hatch + paper * 0.35;
    result = clamp(result, 0.0, 1.0);
    
    // 暖色铅笔
    vec3 pencil = vec3(result * 0.98, result * 0.96, result * 0.92);
    
    gl_FragColor = vec4(mix(color.rgb, pencil, uIntensity), color.a);
}
)";

// 二次元平面风格 (Anime Flat Style) - 无描边，纯平面
const char* g_frag_anime = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    
    // 平面风格 - 清晰色调分离，无描边
    vec3 result = color.rgb;
    
    // 轻微提亮
    result = result * 1.03 + 0.02;
    
    // 清晰的色调分离 (5阶，平面感)
    result = floor(result * 4.99) / 5.0;
    
    // 轻微饱和度保持
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.02);
    
    // 平面风格：无描边
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 美漫画风格 (Comic Book Style)
const char* g_frag_comic = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    
    // 保持原始亮度
    vec3 result = color.rgb;
    
    // 轻微对比度
    result = (result - 0.5) * 1.1 + 0.5;
    
    // 柔和色调分离 (6阶)
    result = floor(result * 5.99) / 6.0;
    
    // 轻微饱和度
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.1);
    
    // 轻微半色调点阵效果
    vec2 dotUV = vTexCoord * 80.0;
    float dotPattern = distance(fract(dotUV), vec2(0.5));
    float luminance = dot(result, vec3(0.299, 0.587, 0.114));
    dotPattern = smoothstep(0.2, 0.5, dotPattern) * smoothstep(0.9, 0.4, luminance);
    result *= 1.0 - dotPattern * 0.12;
    
    // 粗描边（仅明显边缘）
    float c0 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c1 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c2 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c3 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c4 = dot(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c5 = dot(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c6 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c7 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    
    float gx = -c0 + c1 - 2.0*c4 + 2.0*c5 - c2 + c3;
    float gy = -c0 - 2.0*c6 - c1 + c2 + 2.0*c7 + c3;
    float edge = sqrt(gx*gx + gy*gy);
    float outline = smoothstep(0.08, 0.18, edge);
    
    result = mix(result, vec3(0.05), outline * 0.85);
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

static float g_Time = 0.0f;

// ==========================================
// 3. GL Utils (SAFE COMPILE)
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
        glDeleteShader(shader);
        return 0;
    }
    LOGI("Shader compiled successfully, type: 0x%x", type);
    return shader;
}

GLuint LinkProgram(GLuint vs, GLuint fs) {
    if (vs == 0 || fs == 0) {
        LOGE("Link failed: invalid shader");
        return 0;
    }
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
        glDeleteProgram(prog);
        return 0;
    }
    LOGI("Program linked successfully");
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void InitFilterResources(int w, int h) {
    LOGI("Init filter resources: %dx%d", w, h);
    
    // Release old resources
    if (RF::screen_tex != 0) {
        glDeleteTextures(1, &RF::screen_tex);
        if (RF::fbo_tex != 0) glDeleteTextures(1, &RF::fbo_tex);
        if (RF::fbo_tex2 != 0) glDeleteTextures(1, &RF::fbo_tex2);
        if (RF::fbo != 0) glDeleteFramebuffers(1, &RF::fbo);
        if (RF::fbo2 != 0) glDeleteFramebuffers(1, &RF::fbo2);
        if (RF::quad_vbo) glDeleteBuffers(1, &RF::quad_vbo);
        if (RF::quad_ebo) glDeleteBuffers(1, &RF::quad_ebo);
        RF::screen_tex = 0;
        RF::fbo_tex = 0;
        RF::fbo_tex2 = 0;
        RF::fbo = 0;
        RF::fbo2 = 0;
        RF::quad_vbo = 0;
        RF::quad_ebo = 0;
        CHECK_GL_ERROR();
    }

    // 1. Create Screen Texture (SAFE FORMAT)
    glGenTextures(1, &RF::screen_tex);
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    CHECK_GL_ERROR();

    // 2. Create FBO for multi-pass effects
    glGenTextures(1, &RF::fbo_tex);
    glBindTexture(GL_TEXTURE_2D, RF::fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &RF::fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, RF::fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, RF::fbo_tex, 0);
    
    // FBO Completeness Check
    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO incomplete: 0x%x", fboStatus);
        glDeleteFramebuffers(1, &RF::fbo);
        glDeleteTextures(1, &RF::fbo_tex);
        RF::fbo = 0;
        RF::fbo_tex = 0;
    } else {
        LOGI("FBO created successfully");
    }
    
    // Create second FBO for ping-pong rendering
    glGenTextures(1, &RF::fbo_tex2);
    glBindTexture(GL_TEXTURE_2D, RF::fbo_tex2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glGenFramebuffers(1, &RF::fbo2);
    glBindFramebuffer(GL_FRAMEBUFFER, RF::fbo2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, RF::fbo_tex2, 0);
    
    fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO2 incomplete: 0x%x", fboStatus);
        glDeleteFramebuffers(1, &RF::fbo2);
        glDeleteTextures(1, &RF::fbo_tex2);
        RF::fbo2 = 0;
        RF::fbo_tex2 = 0;
    } else {
        LOGI("FBO2 created successfully");
    }
    CHECK_GL_ERROR();

    // 3. Create Fullscreen Quad
    float vertices[] = { -1, 1, 0, 1,  -1, -1, 0, 0,  1, -1, 1, 0,  1, 1, 1, 1 };
    unsigned short indices[] = { 0, 1, 2, 0, 2, 3 };
    glGenBuffers(1, &RF::quad_vbo);
    glGenBuffers(1, &RF::quad_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, RF::quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::quad_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    CHECK_GL_ERROR();

    // 4. Compile Shaders (BASE FIRST, FALLBACK)
    if (RF::prog_base == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_base = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_base));
        if (RF::prog_base == 0) {
            LOGE("FATAL: Base shader failed to compile!");
            return;
        }
    }

    // Compile other shaders
    if (RF::prog_master == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_master = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_master));
    }
    if (RF::prog_outline == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_outline = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_outline));
    }
    if (RF::prog_gaussian == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_gaussian = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_gaussian));
    }
    if (RF::prog_dof == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_dof = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_dof));
    }
    // Art style shaders
    if (RF::prog_cel == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_cel = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_cel));
    }
    if (RF::prog_chinese == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_chinese = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_chinese));
    }
    if (RF::prog_sketch == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_sketch = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_sketch));
    }
    if (RF::prog_anime == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_anime = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_anime));
    }
    if (RF::prog_comic == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_comic = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_comic));
    }

    // Check if shaders are valid
    RF::shaders_valid = (RF::prog_base != 0);
    RF::resources_ready = true;
    LOGI("Filter resources init done, shaders valid: %d", RF::shaders_valid);
}

void BindQuad(GLuint prog) {
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, RF::quad_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, RF::quad_ebo);
    GLint pos = glGetAttribLocation(prog, "aPosition");
    GLint tex = glGetAttribLocation(prog, "aTexCoord");
    if (pos >= 0) { 
        glEnableVertexAttribArray(pos); 
        glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0); 
    }
    if (tex >= 0) { 
        glEnableVertexAttribArray(tex); 
        glVertexAttribPointer(tex, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); 
    }
    CHECK_GL_ERROR();
}

// ==========================================
// 4. Filter Rendering (100% NO BLACK SCREEN)
// ==========================================
void RenderFilters(int w, int h) {
    if (!RF::resources_ready || !RF::shaders_valid) {
        LOGW("Resources not ready, skip filter rendering");
        return;
    }
    g_Time += 0.016f;

    // Save Full GL State
    GLint last_prog, last_fbo, last_tex, last_vp[4], last_active, last_read_fbo, last_draw_fbo;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &last_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &last_draw_fbo);
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

    // ==========================================
    // STEP 1: COPY SCREEN TO TEXTURE (FIXED BLACK SCREEN ROOT)
    // ==========================================
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); // Force read from default framebuffer (screen)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, RF::screen_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w, h, 0);
    CHECK_GL_ERROR();

    // ==========================================
    // STEP 2: RENDER FILTERS (Ping-Pong Rendering)
    // ==========================================
    GLuint final_tex = RF::screen_tex;
    bool use_fbo = RF::fbo != 0 && RF::fbo_tex != 0 && RF::fbo2 != 0 && RF::fbo_tex2 != 0;

    // Pass 1: Master Filter (Single Pass, NO FBO, SAFE)
    if (RF::prog_master != 0 && (RF::params.enable_master || RF::params.enable_bw || RF::params.enable_sepia || RF::params.enable_sharpen)) {
        GLuint target_fbo = use_fbo ? RF::fbo : 0;
        GLuint target_tex = use_fbo ? RF::fbo_tex : RF::screen_tex;

        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        BindQuad(RF::prog_master);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, final_tex);
        
        GLint loc;
        loc = glGetUniformLocation(RF::prog_master, "uTexture");
        SAFE_UNIFORM(loc, glUniform1i, 0);
        loc = glGetUniformLocation(RF::prog_master, "uTime");
        SAFE_UNIFORM(loc, glUniform1f, g_Time);
        loc = glGetUniformLocation(RF::prog_master, "uBrightness");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.brightness);
        loc = glGetUniformLocation(RF::prog_master, "uContrast");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.contrast);
        loc = glGetUniformLocation(RF::prog_master, "uSaturation");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.saturation);
        loc = glGetUniformLocation(RF::prog_master, "uTemperature");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.temperature);
        loc = glGetUniformLocation(RF::prog_master, "uVignette");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.vignette);
        loc = glGetUniformLocation(RF::prog_master, "uEnableBW");
        SAFE_UNIFORM(loc, glUniform1i, RF::params.enable_bw ? 1 : 0);
        loc = glGetUniformLocation(RF::prog_master, "uEnableSepia");
        SAFE_UNIFORM(loc, glUniform1i, RF::params.enable_sepia ? 1 : 0);
        loc = glGetUniformLocation(RF::prog_master, "uSepia");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.sepia_intensity);
        loc = glGetUniformLocation(RF::prog_master, "uGrain");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.film_grain);
        loc = glGetUniformLocation(RF::prog_master, "uEnableSharpen");
        SAFE_UNIFORM(loc, glUniform1i, RF::params.enable_sharpen ? 1 : 0);
        loc = glGetUniformLocation(RF::prog_master, "uSharpness");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.sharpen_intensity);
        loc = glGetUniformLocation(RF::prog_master, "uTexelSize");
        SAFE_UNIFORM(loc, glUniform2f, 1.0f/w, 1.0f/h);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        final_tex = target_tex;
        CHECK_GL_ERROR();
    }

    // Pass 2: Outline (Ping-Pong rendering to avoid texture feedback loop)
    if (use_fbo && RF::prog_outline != 0 && RF::params.enable_outline) {
        // 读取 final_tex，写入到另一个 FBO（乒乓渲染）
        GLuint src_tex = final_tex;
        GLuint dst_fbo = (src_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint dst_tex = (src_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
        
        // 如果是从 screen_tex 开始，默认写入 fbo
        if (src_tex == RF::screen_tex) {
            dst_fbo = RF::fbo;
            dst_tex = RF::fbo_tex;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
        BindQuad(RF::prog_outline);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);
        
        GLint loc;
        loc = glGetUniformLocation(RF::prog_outline, "uTexture");
        SAFE_UNIFORM(loc, glUniform1i, 0);
        loc = glGetUniformLocation(RF::prog_outline, "uTexelSize");
        SAFE_UNIFORM(loc, glUniform2f, 1.0f/w, 1.0f/h);
        loc = glGetUniformLocation(RF::prog_outline, "uThresh");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.outline_thresh);
        loc = glGetUniformLocation(RF::prog_outline, "uOpacity");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.outline_opacity);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        final_tex = dst_tex;
        CHECK_GL_ERROR();
    }

    // Pass 3: DOF (Only if FBO is valid)
    if (use_fbo && RF::prog_gaussian != 0 && RF::prog_dof != 0 && RF::params.enable_dof) {
        // 确定当前源纹理，选择另一个 FBO 作为目标
        GLuint sharp_tex = final_tex;
        GLuint blur_dst_fbo = (sharp_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint blur_dst_tex = (sharp_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
        if (sharp_tex == RF::screen_tex) { blur_dst_fbo = RF::fbo; blur_dst_tex = RF::fbo_tex; }
        
        // Blur Pass 1: Horizontal
        glBindFramebuffer(GL_FRAMEBUFFER, blur_dst_fbo);
        BindQuad(RF::prog_gaussian);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sharp_tex);
        
        GLint loc;
        loc = glGetUniformLocation(RF::prog_gaussian, "uTexture");
        SAFE_UNIFORM(loc, glUniform1i, 0);
        loc = glGetUniformLocation(RF::prog_gaussian, "uTexelSize");
        SAFE_UNIFORM(loc, glUniform2f, 1.0f/w, 1.0f/h);
        loc = glGetUniformLocation(RF::prog_gaussian, "uDirection");
        SAFE_UNIFORM(loc, glUniform2f, 1.0f, 0.0f);
        loc = glGetUniformLocation(RF::prog_gaussian, "uRadius");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.blur_strength * 4.0f);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        GLuint blurred_tex = blur_dst_tex;

        // Blur Pass 2: Vertical (乒乓：从 blur_dst_tex 读取，写入 sharp_tex 的 FBO)
        GLuint blur2_dst_fbo = (blurred_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint blur2_dst_tex = (blurred_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
        
        glBindFramebuffer(GL_FRAMEBUFFER, blur2_dst_fbo);
        BindQuad(RF::prog_gaussian);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, blurred_tex);
        loc = glGetUniformLocation(RF::prog_gaussian, "uDirection");
        SAFE_UNIFORM(loc, glUniform2f, 0.0f, 1.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        GLuint final_blur_tex = blur2_dst_tex;

        // DOF Composite (乒乓渲染)
        GLuint dof_dst_fbo = (final_blur_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint dof_dst_tex = (final_blur_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
        
        glBindFramebuffer(GL_FRAMEBUFFER, dof_dst_fbo);
        BindQuad(RF::prog_dof);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sharp_tex);
        loc = glGetUniformLocation(RF::prog_dof, "uTex_Sharp");
        SAFE_UNIFORM(loc, glUniform1i, 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, final_blur_tex);
        loc = glGetUniformLocation(RF::prog_dof, "uTex_Blur");
        SAFE_UNIFORM(loc, glUniform1i, 1);
        
        loc = glGetUniformLocation(RF::prog_dof, "uFocusPoint");
        SAFE_UNIFORM(loc, glUniform2f, RF::params.focus_point.x, RF::params.focus_point.y);
        loc = glGetUniformLocation(RF::prog_dof, "uFocusRadius");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.focus_radius);
        loc = glGetUniformLocation(RF::prog_dof, "uTransition");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.transition);
        loc = glGetUniformLocation(RF::prog_dof, "uBlurStrength");
        SAFE_UNIFORM(loc, glUniform1f, 1.0f);
        loc = glGetUniformLocation(RF::prog_dof, "uChromatic");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.chromatic);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        final_tex = dof_dst_tex;
        CHECK_GL_ERROR();
    }

    // Pass 4: Art Style (Cel, Ink, Painting, Oil, Sketch)
    if (use_fbo && RF::params.art_style > 0) {
        GLuint src_tex = final_tex;
        GLuint dst_fbo = (src_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint dst_tex = (src_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
        if (src_tex == RF::screen_tex) { dst_fbo = RF::fbo; dst_tex = RF::fbo_tex; }
        
        // 选择对应的 shader
        GLuint prog = 0;
        switch (RF::params.art_style) {
            case 1: prog = RF::prog_cel; break;
            case 2: prog = RF::prog_chinese; break;
            case 3: prog = RF::prog_sketch; break;
            case 4: prog = RF::prog_anime; break;
            case 5: prog = RF::prog_comic; break;
        }
        
        if (prog != 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
            BindQuad(prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src_tex);
            
            GLint loc;
            loc = glGetUniformLocation(prog, "uTexture");
            SAFE_UNIFORM(loc, glUniform1i, 0);
            loc = glGetUniformLocation(prog, "uTexelSize");
            SAFE_UNIFORM(loc, glUniform2f, 1.0f/w, 1.0f/h);
            loc = glGetUniformLocation(prog, "uIntensity");
            SAFE_UNIFORM(loc, glUniform1f, RF::params.art_intensity);
            loc = glGetUniformLocation(prog, "uTime");
            SAFE_UNIFORM(loc, glUniform1f, g_Time);

            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
            final_tex = dst_tex;
            CHECK_GL_ERROR();
        }
    }

    // ==========================================
    // STEP 3: DRAW FINAL RESULT TO SCREEN (NO CLEAR, NO OVERWRITE)
    // ==========================================
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // Force draw to screen
    BindQuad(RF::prog_base); // Use base shader, 100% working
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, final_tex);
    GLint loc = glGetUniformLocation(RF::prog_base, "uTexture");
    SAFE_UNIFORM(loc, glUniform1i, 0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    CHECK_GL_ERROR();

    // ==========================================
    // STEP 4: RESTORE ALL GL STATE
    // ==========================================
    glUseProgram(last_prog);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, last_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, last_draw_fbo);
    glActiveTexture(last_active);
    glBindTexture(GL_TEXTURE_2D, last_tex);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    CHECK_GL_ERROR();
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

    // 现代深色主题配色
    ImVec4 bg_dark(0.08f, 0.08f, 0.10f, 0.96f);
    ImVec4 bg_medium(0.12f, 0.12f, 0.15f, 1.0f);
    ImVec4 bg_light(0.18f, 0.18f, 0.22f, 1.0f);
    ImVec4 accent(0.40f, 0.75f, 0.95f, 1.0f);      // 青蓝色主题色
    ImVec4 accent_hover(0.50f, 0.85f, 1.0f, 1.0f);
    ImVec4 accent_active(0.30f, 0.65f, 0.85f, 1.0f);
    ImVec4 text_main(0.95f, 0.95f, 0.97f, 1.0f);
    ImVec4 text_dim(0.60f, 0.60f, 0.65f, 1.0f);

    c[ImGuiCol_WindowBg] = bg_dark;
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.13f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.16f, 1.0f);
    c[ImGuiCol_FrameBg] = bg_light;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.35f, 1.0f);
    c[ImGuiCol_Button] = bg_light;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = accent_active;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Text] = text_main;
    c[ImGuiCol_TextDisabled] = text_dim;
    c[ImGuiCol_Header] = bg_light;
    c[ImGuiCol_HeaderHovered] = accent;
    c[ImGuiCol_HeaderActive] = accent_active;
    c[ImGuiCol_Tab] = bg_medium;
    c[ImGuiCol_TabHovered] = accent;
    c[ImGuiCol_TabActive] = accent_active;
    c[ImGuiCol_TabUnfocused] = bg_medium;
    c[ImGuiCol_TabUnfocusedActive] = bg_light;
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.30f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.25f, 0.8f);

    s.WindowRounding = 20.0f;
    s.ChildRounding = 14.0f;
    s.FrameRounding = 10.0f;
    s.GrabRounding = 8.0f;
    s.TabRounding = 10.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(14, 8);
    s.ItemSpacing = ImVec2(10, 6);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.ScrollbarRounding = 12.0f;
    s.ScrollbarSize = 12.0f;
}

// ==========================================
// 6. UI Drawing
// ==========================================
static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);
    ImGuiIO& io = ImGui::GetIO();

    // Floating Button - 更精致的设计
    if (!g_ShowUI) {
        ImGui::SetNextWindowPos(ImVec2(20, io.DisplaySize.y * 0.5f), ImGuiCond_Always);
        ImGui::Begin("##Reopen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 40.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.75f, 0.95f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.85f, 1.0f, 1.0f));
        if (ImGui::Button(" RF ", ImVec2(60, 60))) g_ShowUI = true;
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::End();
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // Main Window - 更紧凑的尺寸
    ImGui::SetNextWindowSize(ImVec2(480, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGui::Begin("RenderFusion", &g_ShowUI, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImVec2 win_size = ImGui::GetWindowSize();
    
    // Header with darker background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y),
        ImVec2(ImGui::GetWindowPos().x + win_size.x, ImGui::GetWindowPos().y + 50),
        IM_COL32(15, 15, 20, 255)
    );
    
    ImGui::SetCursorPos(ImVec2(20, 14));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.75f, 0.95f, 1.0f));
    ImGui::TextUnformatted("RenderFusion");
    ImGui::PopStyleColor();
    
    ImGui::SameLine(win_size.x - 56);
    ImGui::SetCursorPosY(10);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    if (ImGui::Button(" X ", ImVec2(36, 32))) g_ShowUI = false;
    ImGui::PopStyleVar();
    
    ImGui::SetCursorPosY(54);

    // Focus point click logic
    if (RF::focus_pending && io.MouseClicked[0] && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        RF::params.focus_point = ImVec2(io.MousePos.x / g_Width, io.MousePos.y / g_Height);
        RF::focus_pending = false;
    }

    // 主内容区
    ImGui::BeginChild("MainContent", ImVec2(0, -55), false, ImGuiWindowFlags_NoBackground);

    // 预设按钮 - 横向排列
    ImGui::SetCursorPosX(16);
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f), "Presets");
    ImGui::Dummy(ImVec2(0, 6));
    
    ImGui::SetCursorPosX(16);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
    
    const char* preset_names[] = {"Original", "Manga B&W"};
    for (int i = 0; i < 2; i++) {
        bool is_selected = (RF::current_preset == i);
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.75f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.75f, 1.0f));
        }
        
        if (ImGui::Button(preset_names[i], ImVec2(210, 36))) {
            RF::current_preset = i;
            RF::ApplyPreset(i);
        }
        
        ImGui::PopStyleColor(2);
        if (i < 1) ImGui::SameLine();
    }
    ImGui::PopStyleVar(2);
    
    ImGui::Dummy(ImVec2(0, 16));

    // Tabs 区域
    ImGui::BeginChild("TabsArea", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
    
    if (ImGui::BeginTabBar("ControlTabs", ImGuiTabBarFlags_None)) {
        // Adjust Tab
        if (ImGui::BeginTabItem("Adjust")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Enable", &RF::params.enable_master);
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::PushItemWidth(-20);
            
            // 紧凑的滑块布局
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.65f, 1.0f), "Brightness");
            ImGui::SliderFloat("##Bright", &RF::params.brightness, -0.5f, 0.5f, "%.2f");
            ImGui::Dummy(ImVec2(0, 4));
            
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.65f, 1.0f), "Contrast");
            ImGui::SliderFloat("##Cont", &RF::params.contrast, 0.6f, 1.8f, "%.2f");
            ImGui::Dummy(ImVec2(0, 4));
            
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.65f, 1.0f), "Saturation");
            ImGui::SliderFloat("##Sat", &RF::params.saturation, 0.0f, 2.0f, "%.2f");
            ImGui::Dummy(ImVec2(0, 4));
            
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.65f, 1.0f), "Temperature");
            ImGui::SliderFloat("##Temp", &RF::params.temperature, -1.0f, 1.0f, "%.2f");
            ImGui::Dummy(ImVec2(0, 4));
            
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.65f, 1.0f), "Vignette");
            ImGui::SliderFloat("##Vignette", &RF::params.vignette, 0.0f, 1.0f, "%.2f");
            ImGui::Dummy(ImVec2(0, 4));
            
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.65f, 1.0f), "Film Grain");
            ImGui::SliderFloat("##Grain", &RF::params.film_grain, 0.0f, 0.3f, "%.3f");
            
            ImGui::PopItemWidth();
            ImGui::EndTabItem();
        }

        // Stylize Tab
        if (ImGui::BeginTabItem("Stylize")) {
            ImGui::Dummy(ImVec2(0, 8));
            
            // Art Style
            ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f), "Art Style");
            const char* art_styles[] = {"Off", "Cel Anime", "Chinese Painting", "Sketch", "Anime Flat", "Comic"};
            ImGui::SetNextItemWidth(-20);
            if (ImGui::Combo("##ArtStyle", &RF::params.art_style, art_styles, IM_ARRAYSIZE(art_styles))) {
                // 切换艺术风格时重置强度
                if (RF::params.art_style > 0) {
                    RF::params.art_intensity = 1.0f;
                }
            }
            
            if (RF::params.art_style > 0) {
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##ArtInt", &RF::params.art_intensity, 0.0f, 1.0f, "Intensity: %.2f");
                ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.50f, 1.0f), "%s effect applied", art_styles[RF::params.art_style]);
            }
            
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8));
            
            // Manga B&W
            ImGui::Checkbox("Manga B&W", &RF::params.enable_bw);
            if (RF::params.enable_bw) {
                ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.55f, 1.0f), "Comic-style high contrast B&W");
            }
            
            ImGui::Dummy(ImVec2(0, 8));
            
            // Sepia
            ImGui::Checkbox("Vintage Sepia", &RF::params.enable_sepia);
            if (RF::params.enable_sepia) {
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##SepiaInt", &RF::params.sepia_intensity, 0.0f, 1.0f, "Intensity: %.2f");
            }
            
            ImGui::Dummy(ImVec2(0, 8));
            
            // Outline
            ImGui::Checkbox("Black Outline", &RF::params.enable_outline);
            if (RF::params.enable_outline) {
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##OutlineThresh", &RF::params.outline_thresh, 0.05f, 0.5f, "Threshold: %.2f");
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##OutlineOpacity", &RF::params.outline_opacity, 0.0f, 1.0f, "Opacity: %.2f");
            }

            ImGui::EndTabItem();
        }

        // Effects Tab
        if (ImGui::BeginTabItem("Effects")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Sharpen", &RF::params.enable_sharpen);
            if (RF::params.enable_sharpen) {
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##SharpInt", &RF::params.sharpen_intensity, 0.0f, 1.5f, "Intensity: %.2f");
            }
            ImGui::EndTabItem();
        }

        // DOF Tab
        if (ImGui::BeginTabItem("DOF")) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Checkbox("Enable Depth of Field", &RF::params.enable_dof);
            
            if (RF::params.enable_dof) {
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushStyleColor(ImGuiCol_Button, RF::focus_pending ? ImVec4(0.90f, 0.50f, 0.40f, 1.0f) : ImVec4(0.40f, 0.75f, 0.95f, 1.0f));
                if (ImGui::Button(RF::focus_pending ? "Tap Screen!" : "Set Focus Point", ImVec2(-1, 36))) {
                    RF::focus_pending = !RF::focus_pending;
                }
                ImGui::PopStyleColor();
                ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f), "Focus: (%.2f, %.2f)", RF::params.focus_point.x, RF::params.focus_point.y);
                
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##FocusRadius", &RF::params.focus_radius, 0.05f, 0.5f, "Radius: %.2f");
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##Transition", &RF::params.transition, 0.05f, 0.5f, "Softness: %.2f");
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##BlurStr", &RF::params.blur_strength, 0.0f, 3.0f, "Blur: %.2f");
                ImGui::SetNextItemWidth(-20);
                ImGui::SliderFloat("##Chromatic", &RF::params.chromatic, 0.0f, 0.1f, "Chromatic: %.3f");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();
    ImGui::EndChild();

    // Bottom Reset Button
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 48);
    ImGui::SetCursorPosX((win_size.x - 120) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    if (ImGui::Button("Reset All", ImVec2(120, 34))) {
        RF::current_preset = 0;
        RF::ApplyPreset(0);
    }
    ImGui::PopStyleVar();

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
