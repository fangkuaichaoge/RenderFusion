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
#include <fstream>
#include <nlohmann/json.hpp>

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
    GLuint prog_tiktok = 0;     // TikTok RGB分层
    // Art Style Shaders
        GLuint prog_cel = 0;         // 赛璐璐卡通
        GLuint prog_chinese = 0;     // 中国画
        GLuint prog_sketch = 0;      // 素描
        GLuint prog_anime = 0;       // 二次元平面
        GLuint prog_comic = 0;       // 美漫画

    bool resources_ready = false;
    bool shaders_valid = false;
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
        
        // Art Styles
        int art_style = 0;      // 0=Off, 1=Cel Anime, 2=Chinese Painting, 3=Sketch, 4=Anime Flat, 5=Comic
        float art_intensity = 1.0f;
        
        // TikTok RGB Split
        bool enable_tiktok = false;
        float tiktok_offset = 0.005f;  // RGB偏移量
        float tiktok_intensity = 1.0f;
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
            {"Original", {false, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, false, false, 0.8f, 0.0f, false, 0.5f, false, 0.15f, 1.0f}},
            // Manga B&W: 黑白漫画风格（高对比度黑白 + 描边）
            {"Manga B&W", {true, 0.05f, 1.15f, 0.0f, 0.0f, 0.0f, true, false, 0.0f, 0.0f, false, 0.5f, true, 0.12f, 1.0f}}
        };
        
        if (idx >= 0 && idx < 2) {
            params = presets[idx].p;
        }
    }

    // Check if any multi-pass effect is enabled
    bool IsMultiPassEnabled() {
        return params.enable_outline;
    }
}

// ===================== Configuration File Support =====================
namespace Config {
    // Path for configuration file
    const char* CONFIG_PATH = "/storage/emulated/0/games/RenderFusion/config.json";
    
    // Ensure directory exists
    void EnsureConfigDir() {
        // Create directory if it doesn't exist
        system("mkdir -p /storage/emulated/0/games/RenderFusion");
    }
    
    // Load configuration from file
    bool LoadConfig() {
        std::ifstream file(CONFIG_PATH);
        if (!file.is_open()) {
            LOGI("Config file not found: %s", CONFIG_PATH);
            return false;
        }
        
        nlohmann::json j;
        try {
            file >> j;
            file.close();
        } catch (const std::exception& e) {
            LOGE("Error parsing config file: %s", e.what());
            file.close();
            return false;
        }
        
        LOGI("Loading configuration from: %s", CONFIG_PATH);
        
        // Load all filter parameters from config
        if (j.contains("enable_master")) RF::params.enable_master = j["enable_master"];
        if (j.contains("brightness")) RF::params.brightness = j["brightness"];
        if (j.contains("contrast")) RF::params.contrast = j["contrast"];
        if (j.contains("saturation")) RF::params.saturation = j["saturation"];
        if (j.contains("temperature")) RF::params.temperature = j["temperature"];
        if (j.contains("vignette")) RF::params.vignette = j["vignette"];
        if (j.contains("enable_bw")) RF::params.enable_bw = j["enable_bw"];
        if (j.contains("enable_sepia")) RF::params.enable_sepia = j["enable_sepia"];
        if (j.contains("sepia_intensity")) RF::params.sepia_intensity = j["sepia_intensity"];
        if (j.contains("film_grain")) RF::params.film_grain = j["film_grain"];
        if (j.contains("enable_sharpen")) RF::params.enable_sharpen = j["enable_sharpen"];
        if (j.contains("sharpen_intensity")) RF::params.sharpen_intensity = j["sharpen_intensity"];
        if (j.contains("enable_outline")) RF::params.enable_outline = j["enable_outline"];
        if (j.contains("outline_thresh")) RF::params.outline_thresh = j["outline_thresh"];
        if (j.contains("outline_opacity")) RF::params.outline_opacity = j["outline_opacity"];

        if (j.contains("art_style")) RF::params.art_style = j["art_style"];
        if (j.contains("art_intensity")) RF::params.art_intensity = j["art_intensity"];
        if (j.contains("enable_tiktok")) RF::params.enable_tiktok = j["enable_tiktok"];
        if (j.contains("tiktok_offset")) RF::params.tiktok_offset = j["tiktok_offset"];
        if (j.contains("tiktok_intensity")) RF::params.tiktok_intensity = j["tiktok_intensity"];
        
        LOGI("Configuration loaded successfully");
        return true;
    }
    
    // Save configuration to file
    bool SaveConfig() {
        EnsureConfigDir();
        
        // Create JSON object with all parameters
        nlohmann::json j;
        j["enable_master"] = RF::params.enable_master;
        j["brightness"] = RF::params.brightness;
        j["contrast"] = RF::params.contrast;
        j["saturation"] = RF::params.saturation;
        j["temperature"] = RF::params.temperature;
        j["vignette"] = RF::params.vignette;
        j["enable_bw"] = RF::params.enable_bw;
        j["enable_sepia"] = RF::params.enable_sepia;
        j["sepia_intensity"] = RF::params.sepia_intensity;
        j["film_grain"] = RF::params.film_grain;
        j["enable_sharpen"] = RF::params.enable_sharpen;
        j["sharpen_intensity"] = RF::params.sharpen_intensity;
        j["enable_outline"] = RF::params.enable_outline;
        j["outline_thresh"] = RF::params.outline_thresh;
        j["outline_opacity"] = RF::params.outline_opacity;

        j["art_style"] = RF::params.art_style;
        j["art_intensity"] = RF::params.art_intensity;
        j["enable_tiktok"] = RF::params.enable_tiktok;
        j["tiktok_offset"] = RF::params.tiktok_offset;
        j["tiktok_intensity"] = RF::params.tiktok_intensity;
        
        std::ofstream file(CONFIG_PATH);
        if (!file.is_open()) {
            LOGE("Failed to open config file for writing: %s", CONFIG_PATH);
            return false;
        }
        
        file << j.dump(4); // Pretty print with 4-space indentation
        file.close();
        
        LOGI("Configuration saved to: %s", CONFIG_PATH);
        return true;
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

// 漫画风格黑白 - 重构版：自然灰度过渡，保留层次细节
float mangaGray(vec3 color) {
    vec3 hsv = rgb2hsv(color);
    float hue = hsv.x;        // 色调 0-1
    float sat = hsv.y;        // 饱和度 0-1
    float val = hsv.z;        // 明度 0-1
    
    // 基础灰度 - 使用标准权重
    float gray = dot(color, vec3(0.299, 0.587, 0.114));
    
    // 根据色调微调 - 更柔和的处理
    // hue: 0=红, 0.16=黄, 0.33=绿, 0.5=青, 0.66=蓝, 0.83=紫
    float warmth = 0.0;
    if (hue < 0.17) {
        // 红到黄：暖色，轻微提亮
        warmth = 0.08 * (1.0 - abs(hue - 0.08) / 0.08);
    } else if (hue > 0.58 && hue < 0.75) {
        // 蓝到青：冷色，轻微压暗
        warmth = -0.05 * (1.0 - abs(hue - 0.66) / 0.08);
    }
    
    // 高饱和度区域：轻微增强对比
    float satBoost = sat * 0.05;
    
    // 综合灰度值
    gray = gray + warmth + satBoost;
    
    // 适度的对比度处理 - 不要过度
    gray = (gray - 0.5) * 1.25 + 0.5;
    
    // 细腻色调分离 - 6阶，更平滑
    float levels = 6.0;
    gray = floor(gray * levels + 0.5) / levels;
    
    // 柔和平滑处理
    gray = smoothstep(0.1, 0.9, gray);
    
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

// Outline Shader - 优化版：柔和描边，使用深色原色
const char* g_frag_outline = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uThresh;
uniform float uOpacity;

void main() {
    vec4 original = texture2D(uTexture, vTexCoord);

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
    
    // 柔和过渡而非生硬的二值化
    float isEdge = smoothstep(uThresh * 0.6, uThresh * 1.5, edge);
    
    // 描边使用深色版本的原色，而非纯黑
    vec3 darkOutline = original.rgb * 0.2;
    vec3 finalColor = mix(original.rgb, darkOutline, isEdge * uOpacity);
    
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



// TikTok RGB Split Shader - 红/蓝分层效果
const char* g_frag_tiktok = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uOffset;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    
    // RGB 分层偏移
    float r = texture2D(uTexture, vTexCoord + vec2(uOffset, 0.0)).r;
    float g = color.g;
    float b = texture2D(uTexture, vTexCoord - vec2(uOffset, 0.0)).b;
    
    vec3 rgbSplit = vec3(r, g, b);
    
    // 混合原色和分层效果
    vec3 result = mix(color.rgb, rgbSplit, uIntensity);
    
    gl_FragColor = vec4(result, color.a);
}
)";

// ==========================================
// Art Style Shaders (性能优化版)
// ==========================================

// 赛璐璐卡通画风 (Cel Shading) - 重构版：鲜艳原色，柔和描边
const char* g_frag_cel = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 保持原色鲜艳度，轻微提升
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.08);
    
    // 细腻色调分离 - 根据亮度动态调整阶数
    float levels = 8.0;
    vec3 quantized = floor(result * (levels - 0.01)) / levels;
    
    // 平滑混合，保留自然过渡
    result = mix(result, quantized, 0.55);
    
    // 轻微对比度增强
    result = (result - 0.5) * 1.05 + 0.5;
    
    // 柔和描边 - 使用较亮的描边色
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
    float outline = smoothstep(0.06, 0.22, edge);
    
    // 描边用中灰色调，柔和自然
    vec3 outlineColor = result * 0.35 + vec3(0.05);
    result = mix(result, outlineColor, outline * 0.65);
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 中国画风格 (Chinese Painting) - 淡雅水墨版
const char* g_frag_chinese = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898,78.233))) * 43758.5453123); }

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 降低饱和度 - 淡雅感
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 0.6);
    
    // 整体提亮 - 留白效果
    result = result * 0.85 + 0.12;
    
    // 水墨晕染 - 柔和扩散
    vec3 ink = result * 0.2;
    ink += texture2D(uTexture, vTexCoord + vec2(-1.5, 0.0) * uTexelSize).rgb * 0.12;
    ink += texture2D(uTexture, vTexCoord + vec2( 1.5, 0.0) * uTexelSize).rgb * 0.12;
    ink += texture2D(uTexture, vTexCoord + vec2( 0.0,-1.5) * uTexelSize).rgb * 0.12;
    ink += texture2D(uTexture, vTexCoord + vec2( 0.0, 1.5) * uTexelSize).rgb * 0.12;
    ink += texture2D(uTexture, vTexCoord + vec2(-2.5, -2.5) * uTexelSize).rgb * 0.04;
    ink += texture2D(uTexture, vTexCoord + vec2( 2.5, -2.5) * uTexelSize).rgb * 0.04;
    ink += texture2D(uTexture, vTexCoord + vec2(-2.5,  2.5) * uTexelSize).rgb * 0.04;
    ink += texture2D(uTexture, vTexCoord + vec2( 2.5,  2.5) * uTexelSize).rgb * 0.04;
    
    result = mix(result, ink, 0.35);
    
    // 淡墨色调分离 - 6阶，层次感
    vec3 quantized = floor(result * 5.99) / 6.0;
    result = mix(result, quantized, 0.25);
    
    // 宣纸纹理 - 淡淡的
    float paper = random(vTexCoord * 300.0);
    result = result + paper * 0.02 - 0.01;
    
    // 淡墨勾勒 - 非常轻微
    float c4 = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    float c5 = dot(texture2D(uTexture, vTexCoord + vec2( 1.2, 0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c6 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0,-1.2) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c7 = dot(texture2D(uTexture, vTexCoord + vec2( 0.0, 1.2) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float c0 = dot(texture2D(uTexture, vTexCoord + vec2(-1.2, 0.0) * uTexelSize).rgb, vec3(0.299, 0.587, 0.114));
    float edge = abs(c4 - c5) + abs(c4 - c6) + abs(c4 - c7) + abs(c4 - c0);
    float outline = smoothstep(0.015, 0.08, edge);
    
    // 淡墨勾勒 - 用浅色
    vec3 lightInk = result * 0.85;
    result = mix(result, lightInk, outline * 0.3);
    
    // 最终整体淡雅调
    result = result * 0.95 + 0.025;
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 素描风格 (Sketch / Pencil) - 重构版：柔和铅笔，保留色调
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
    
    // 轻微提亮，保持亮度
    gray = gray * 1.05 + 0.02;
    
    // Sobel 边缘 - 柔和版
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
    
    // 柔和铅笔线条
    float lines = 1.0 - smoothstep(0.0, 0.45, edge);
    
    // 精细斜线阴影 - 根据亮度调整
    vec2 uv = vTexCoord * 180.0;
    float pattern1 = step(0.5, fract(uv.x + uv.y));
    float pattern2 = step(0.5, fract(uv.x - uv.y));
    
    float hatch = 0.0;
    if(gray < 0.6) hatch += pattern1 * 0.1 * (0.6 - gray) / 0.6;
    if(gray < 0.4) hatch += pattern2 * 0.15 * (0.4 - gray) / 0.4;
    if(gray < 0.25) hatch += 0.18 * (0.25 - gray) / 0.25;
    
    // 细腻纸张纹理
    float paper = random(vTexCoord * 350.0) * 0.03;
    
    float result = lines - hatch + paper * 0.25;
    result = clamp(result, 0.0, 1.0);
    
    // 暖色调铅笔 - 保留原色调
    vec3 pencil = vec3(result * 0.98, result * 0.96, result * 0.93);
    
    // 混合原色调，让素描也有色彩
    vec3 tinted = pencil * (color.rgb + vec3(0.1)) * 1.1;
    pencil = mix(pencil, tinted, 0.3);
    
    gl_FragColor = vec4(mix(color.rgb, pencil, uIntensity), color.a);
}
)";

// 二次元平面风格 (Anime Flat Style) - 重构版：鲜艳原色，细腻层次
const char* g_frag_anime = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 提升饱和度 - 动漫风格更鲜艳
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.1);
    
    // 细腻色调分离 - 根据亮度自适应阶数
    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    float levels = mix(6.0, 10.0, lum); // 亮部更多层次
    vec3 quantized = floor(result * (levels - 0.01)) / levels;
    
    // 混合原色，保持自然过渡
    result = mix(result, quantized, 0.4);
    
    // 微妙的明暗层次 - 非常轻微的对比度增强
    result = (result - 0.5) * 1.03 + 0.5;
    
    // 轻微提亮暗部
    result = result * 0.97 + 0.015;
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 美漫画风格 (Comic Book Style) - 重构版：鲜艳原色，夸张效果
const char* g_frag_comic = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 提升饱和度 - 漫画风格更鲜艳
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.15);
    
    // 对比度增强 - 漫画的夸张感
    result = (result - 0.5) * 1.15 + 0.5;
    
    // 色调分离 - 5阶
    vec3 quantized = floor(result * 4.99) / 5.0;
    result = mix(result, quantized, 0.45);
    
    // 半色调点阵效果 - 更细腻
    vec2 dotUV = vTexCoord * 120.0;
    float dotPattern = distance(fract(dotUV), vec2(0.5));
    float luminance = dot(result, vec3(0.299, 0.587, 0.114));
    dotPattern = smoothstep(0.3, 0.6, dotPattern) * smoothstep(0.9, 0.3, luminance);
    result *= 1.0 - dotPattern * 0.08;
    
    // 柔和描边
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
    float outline = smoothstep(0.07, 0.2, edge);
    
    // 描边用中灰色调
    vec3 outlineColor = result * 0.3 + vec3(0.08);
    result = mix(result, outlineColor, outline * 0.6);
    
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

    // TikTok RGB Split
    if (RF::prog_tiktok == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_tiktok = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_tiktok));
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

    // Pass 3: DOF (Only if FBO is valid) - Moved to the end to apply as final effect
    // Pass 4: TikTok RGB Split 
    if (RF::prog_tiktok != 0 && RF::params.enable_tiktok) {
        GLuint src_tex = final_tex;
        GLuint dst_fbo = use_fbo ? ((src_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo) : 0;
        GLuint dst_tex = use_fbo ? ((src_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex) : RF::screen_tex;
        if (src_tex == RF::screen_tex && use_fbo) { dst_fbo = RF::fbo; dst_tex = RF::fbo_tex; }

        glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
        BindQuad(RF::prog_tiktok);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);

        GLint loc;
        loc = glGetUniformLocation(RF::prog_tiktok, "uTexture");
        SAFE_UNIFORM(loc, glUniform1i, 0);
        loc = glGetUniformLocation(RF::prog_tiktok, "uOffset");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.tiktok_offset);
        loc = glGetUniformLocation(RF::prog_tiktok, "uIntensity");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.tiktok_intensity);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        final_tex = dst_tex;
        CHECK_GL_ERROR();
    }

    // Pass 5: Art Style
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

    // Pass 6: Outline (Moved before DOF to allow proper叠加)
    if (use_fbo && RF::prog_outline != 0 && RF::params.enable_outline) {
        // 读取 final_tex，写入到另一个 FBO（乒乓渲染）
        GLuint src_tex = final_tex;
        GLuint dst_fbo = (src_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint dst_tex = (src_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
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

    // ============================================
    // 现代深色玻璃态主题 - Premium Dark Glass Theme
    // ============================================
    
    // 核心色板 - 高级深色调
    ImVec4 bg_darkest(0.035f, 0.038f, 0.048f, 0.96f);       // 最深背景
    ImVec4 bg_dark(0.055f, 0.058f, 0.072f, 0.98f);          // 深色背景
    ImVec4 bg_medium(0.095f, 0.098f, 0.115f, 1.0f);         // 中等背景
    ImVec4 bg_light(0.135f, 0.140f, 0.165f, 1.0f);          // 浅色背景
    ImVec4 bg_hover(0.175f, 0.180f, 0.210f, 1.0f);          // 悬停背景
    
    // 主色调 - 科技蓝紫渐变系
    ImVec4 accent_primary(0.28f, 0.56f, 0.92f, 1.0f);       // 主强调色
    ImVec4 accent_hover(0.35f, 0.65f, 0.98f, 1.0f);         // 悬停强调
    ImVec4 accent_active(0.22f, 0.48f, 0.85f, 1.0f);        // 激活强调
    ImVec4 accent_glow(0.32f, 0.60f, 0.95f, 0.35f);         // 发光效果
    
    // 文字色阶
    ImVec4 text_primary(0.95f, 0.96f, 0.98f, 1.0f);         // 主要文字
    ImVec4 text_secondary(0.72f, 0.74f, 0.78f, 1.0f);       // 次要文字
    ImVec4 text_muted(0.50f, 0.52f, 0.58f, 1.0f);           // 提示文字
    ImVec4 text_disabled(0.35f, 0.38f, 0.42f, 1.0f);        // 禁用文字
    
    // 成功/警告色
    ImVec4 success(0.18f, 0.78f, 0.52f, 1.0f);              // 成功绿
    ImVec4 warning(0.95f, 0.72f, 0.18f, 1.0f);              // 警告橙
    
    // ============================================
    // 颜色配置
    // ============================================
    
    // 窗口背景 - 玻璃态效果
    c[ImGuiCol_WindowBg] = bg_dark;
    c[ImGuiCol_ChildBg] = ImVec4(0.065f, 0.068f, 0.085f, 0.92f);
    c[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.058f, 0.075f, 0.98f);
    
    // 标题栏
    c[ImGuiCol_TitleBg] = ImVec4(0.045f, 0.048f, 0.062f, 0.95f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.065f, 0.068f, 0.085f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.035f, 0.038f, 0.052f, 0.85f);
    
    // 帧背景 - 控件背景
    c[ImGuiCol_FrameBg] = bg_medium;
    c[ImGuiCol_FrameBgHovered] = bg_light;
    c[ImGuiCol_FrameBgActive] = bg_hover;
    
    // 按钮
    c[ImGuiCol_Button] = bg_light;
    c[ImGuiCol_ButtonHovered] = accent_hover;
    c[ImGuiCol_ButtonActive] = accent_active;
    
    // 滑块
    c[ImGuiCol_SliderGrab] = accent_primary;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    
    // 复选框
    c[ImGuiCol_CheckMark] = accent_primary;
    
    // 文字
    c[ImGuiCol_Text] = text_primary;
    c[ImGuiCol_TextDisabled] = text_disabled;
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent_primary.x, accent_primary.y, accent_primary.z, 0.35f);
    
    // 标题头
    c[ImGuiCol_Header] = bg_light;
    c[ImGuiCol_HeaderHovered] = ImVec4(accent_primary.x, accent_primary.y, accent_primary.z, 0.25f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent_primary.x, accent_primary.y, accent_primary.z, 0.4f);
    
    // 标签页
    c[ImGuiCol_Tab] = bg_medium;
    c[ImGuiCol_TabHovered] = accent_hover;
    c[ImGuiCol_TabActive] = accent_primary;
    c[ImGuiCol_TabUnfocused] = bg_medium;
    c[ImGuiCol_TabUnfocusedActive] = bg_light;
    
    // 分隔线
    c[ImGuiCol_Separator] = ImVec4(0.18f, 0.20f, 0.25f, 0.6f);
    c[ImGuiCol_SeparatorHovered] = accent_primary;
    c[ImGuiCol_SeparatorActive] = accent_hover;
    
    // 边框
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.30f, 0.45f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    
    // 滚动条
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.055f, 0.058f, 0.075f, 0.75f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.30f, 0.38f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.40f, 0.50f, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.48f, 0.50f, 0.62f, 1.0f);
    
    // 表格
    c[ImGuiCol_TableHeaderBg] = bg_medium;
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.18f, 0.20f, 0.26f, 0.8f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.14f, 0.18f, 0.5f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.08f, 0.10f, 0.14f, 0.35f);
    
    // 拖拽
    c[ImGuiCol_DragDropTarget] = ImVec4(accent_primary.x, accent_primary.y, accent_primary.z, 0.9f);
    
    // 导航高亮
    c[ImGuiCol_NavHighlight] = accent_primary;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    
    // 模态窗口遮罩
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.75f);

    // ============================================
    // 样式配置 - 现代圆角设计
    // ============================================
    
    // 圆角设置 - 统一的圆角风格
    s.WindowRounding = 18.0f;
    s.ChildRounding = 14.0f;
    s.FrameRounding = 10.0f;
    s.PopupRounding = 12.0f;
    s.ScrollbarRounding = 9.0f;
    s.GrabRounding = 8.0f;
    s.TabRounding = 10.0f;
    s.LogSliderDeadzone = 4.0f;
    
    // 内边距 - 舒适的间距
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(14, 10);
    s.ItemSpacing = ImVec2(14, 10);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.CellPadding = ImVec2(8, 6);
    s.TouchExtraPadding = ImVec2(4, 4);
    s.IndentSpacing = 22.0f;
    s.ColumnsMinSpacing = 8.0f;
    
    // 边框 - 精细边框
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.5f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    
    // 滚动条
    s.ScrollbarSize = 16.0f;
    
    // 窗口最小尺寸
    s.WindowMinSize = ImVec2(200, 200);
    
    // 对齐和位置
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ColorButtonPosition = ImGuiDir_Right;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    
    // 安全区域
    s.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);
    
    // 抗锯齿
    s.AntiAliasedLines = true;
    s.AntiAliasedFill = true;
    
    // 曲线细分
    s.CurveTessellationTol = 1.25f;
    s.CircleTessellationMaxError = 0.3f;
    
    // Alpha 混合
    s.Alpha = 1.0f;
    s.DisabledAlpha = 0.58f;
}

// ==========================================
// 6. UI Drawing Helper Functions
// ==========================================

// 绘制渐变矩形
static void DrawGradientRect(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, ImU32 col_top, ImU32 col_bottom, bool horizontal = false) {
    if (horizontal) {
        draw_list->AddRectFilledMultiColor(p_min, p_max, col_top, col_bottom, col_bottom, col_top);
    } else {
        draw_list->AddRectFilledMultiColor(p_min, p_max, col_top, col_top, col_bottom, col_bottom);
    }
}

// 绘制带阴影的分割线
static void DrawSeparatorLine(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, float thickness = 1.0f) {
    ImU32 col_shadow = IM_COL32(0, 0, 0, 40);
    ImU32 col_line = IM_COL32(45, 50, 65, 120);
    draw_list->AddLine(ImVec2(p_min.x, p_min.y + 1), ImVec2(p_max.x, p_max.y + 1), col_shadow, thickness + 1.0f);
    draw_list->AddLine(p_min, p_max, col_line, thickness);
}

// 绘制卡片背景
static void DrawCardBackground(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, float rounding = 12.0f) {
    ImU32 bg_col = IM_COL32(18, 20, 28, 200);
    ImU32 border_col = IM_COL32(55, 60, 75, 80);
    draw_list->AddRectFilled(p_min, p_max, bg_col, rounding);
    draw_list->AddRect(p_min, p_max, border_col, rounding, 0, 1.0f);
}

// 绘制发光边框
static void DrawGlowBorder(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, ImU32 glow_color, float rounding = 14.0f) {
    for (int i = 3; i > 0; i--) {
        float alpha = (0.15f / i);
        ImU32 glow = IM_COL32(
            (glow_color >> 0) & 0xFF,
            (glow_color >> 8) & 0xFF,
            (glow_color >> 16) & 0xFF,
            (int)(alpha * 255)
        );
        draw_list->AddRect(
            ImVec2(p_min.x - i, p_min.y - i),
            ImVec2(p_max.x + i, p_max.y + i),
            glow, rounding + i, 0, 1.5f
        );
    }
}

// 绘制圆形按钮
static bool CircleButton(const char* str_id, ImVec2 center, float radius, ImU32 color, ImU32 hover_color) {
    ImGui::PushID(str_id);
    ImGui::SetCursorScreenPos(ImVec2(center.x - radius - 10, center.y - radius - 10));
    ImGui::InvisibleButton("##btn", ImVec2((radius + 10) * 2, (radius + 10) * 2));
    
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImU32 current_color = hovered ? hover_color : color;
    
    // 外发光
    if (hovered) {
        for (int i = 4; i > 0; i--) {
            ImU32 glow = IM_COL32(
                (hover_color >> 0) & 0xFF,
                (hover_color >> 8) & 0xFF,
                (hover_color >> 16) & 0xFF,
                25 * i
            );
            draw_list->AddCircle(center, radius + i * 2, glow, 32, 2.0f);
        }
    }
    
    // 主圆
    draw_list->AddCircleFilled(center, radius, current_color, 32);
    
    // 内边框
    ImU32 inner_border = IM_COL32(255, 255, 255, hovered ? 50 : 25);
    draw_list->AddCircle(center, radius - 1.5f, inner_border, 32, 1.5f);
    
    ImGui::PopID();
    return clicked;
}

// 绘制精致滑块
static bool StyledSliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.2f", float width = -1) {
    ImGui::PushID(label);
    
    ImVec2 label_size = ImGui::CalcTextSize(label);
    float slider_width = (width < 0) ? ImGui::GetContentRegionAvail().x : width;
    
    // 标签行
    ImGui::TextColored(ImVec4(0.58f, 0.60f, 0.68f, 1.0f), "%s", label);
    ImGui::SameLine(slider_width - 50);
    ImGui::TextColored(ImVec4(0.72f, 0.74f, 0.80f, 1.0f), "%.2f", *v);
    
    ImGui::SetNextItemWidth(slider_width);
    
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.16f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.18f, 0.19f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.28f, 0.56f, 0.92f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.35f, 0.65f, 0.98f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    
    bool changed = ImGui::SliderFloat("##slider", v, v_min, v_max, "");
    
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::PopID();
    
    return changed;
}

// 绘制开关控件
static bool StyledCheckbox(const char* label, bool* v) {
    ImGui::PushID(label);
    
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 label_size = ImGui::CalcTextSize(label);
    float height = 28.0f;
    float width = 52.0f;
    float radius = height * 0.5f;
    
    ImGui::InvisibleButton("##toggle", ImVec2(width + label_size.x + 12, height + 6));
    bool hovered = ImGui::IsItemHovered();
    bool changed = ImGui::IsItemClicked();
    
    if (changed) *v = !*v;
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    // 背景轨道
    ImU32 track_color = *v ? IM_COL32(28, 92, 165, 255) : IM_COL32(35, 38, 48, 255);
    if (hovered) {
        track_color = *v ? IM_COL32(35, 105, 185, 255) : IM_COL32(45, 48, 60, 255);
    }
    
    // 发光效果
    if (*v && hovered) {
        draw_list->AddRectRounded(ImVec2(pos.x - 2, pos.y + 2), ImVec2(pos.x + width + 2, pos.y + height + 4), 
                                   IM_COL32(35, 105, 185, 60), radius + 2);
    }
    
    draw_list->AddRectFilled(ImVec2(pos.x, pos.y + 3), ImVec2(pos.x + width, pos.y + height + 3), 
                              track_color, radius);
    
    // 滑块
    float knob_x = *v ? (pos.x + width - radius - 3) : (pos.x + radius + 3);
    ImU32 knob_color = *v ? IM_COL32(80, 180, 255, 255) : IM_COL32(120, 125, 135, 255);
    if (hovered) {
        knob_color = *v ? IM_COL32(100, 200, 255, 255) : IM_COL32(140, 145, 155, 255);
    }
    
    // 滑块发光
    if (*v) {
        draw_list->AddCircleFilled(ImVec2(knob_x, pos.y + height / 2 + 3), radius - 1, 
                                    IM_COL32(80, 180, 255, 80));
    }
    
    draw_list->AddCircleFilled(ImVec2(knob_x, pos.y + height / 2 + 3), radius - 3, knob_color);
    draw_list->AddCircle(ImVec2(knob_x, pos.y + height / 2 + 3), radius - 3, 
                          IM_COL32(255, 255, 255, 40), 16, 1.5f);
    
    // 标签文字
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width + 12, pos.y + 6));
    ImGui::TextColored(*v ? ImVec4(0.95f, 0.96f, 0.98f, 1.0f) : ImVec4(0.65f, 0.68f, 0.75f, 1.0f), "%s", label);
    
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 12));
    ImGui::PopID();
    
    return changed;
}

// 绘制预设按钮
static bool PresetButton(const char* label, bool selected, ImVec2 size) {
    ImGui::PushID(label);
    
    ImU32 bg_normal = IM_COL32(25, 28, 38, 255);
    ImU32 bg_selected = IM_COL32(35, 90, 170, 255);
    ImU32 bg_hovered = selected ? IM_COL32(45, 105, 190, 255) : IM_COL32(35, 40, 52, 255);
    ImU32 text_normal = IM_COL32(160, 165, 175, 255);
    ImU32 text_selected = IM_COL32(245, 248, 255, 255);
    
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? bg_selected : bg_normal);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_selected);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? text_selected : text_normal);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, selected ? 1.0f : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(60, 140, 220, 200));
    
    bool clicked = ImGui::Button(label, size);
    
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    
    return clicked;
}

// ==========================================
// 7. UI Drawing
// ==========================================
static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // ============================================
    // Floating Toggle Button - 精致浮动按钮
    // ============================================
    if (!g_ShowUI) {
        ImVec2 center(30, io.DisplaySize.y * 0.5f);
        
        // 发光背景
        for (int i = 5; i > 0; i--) {
            draw_list->AddCircleFilled(center, 28 + i * 3, IM_COL32(28, 92, 175, 15 * i));
        }
        
        ImU32 btn_color = IM_COL32(35, 95, 175, 240);
        ImU32 btn_hover = IM_COL32(50, 120, 200, 255);
        
        // 检测点击
        ImGui::SetCursorScreenPos(ImVec2(center.x - 35, center.y - 35));
        ImGui::InvisibleButton("##toggle_main", ImVec2(70, 70));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();
        
        // 主按钮
        ImU32 current_color = hovered ? btn_hover : btn_color;
        draw_list->AddCircleFilled(center, 26, current_color, 32);
        
        // 内发光边框
        draw_list->AddCircle(center, 25, IM_COL32(255, 255, 255, hovered ? 60 : 30), 32, 2.0f);
        
        // RF 文字
        ImVec2 text_size = ImGui::CalcTextSize("RF");
        draw_list->AddText(ImVec2(center.x - text_size.x / 2, center.y - text_size.y / 2 - 1), 
                           IM_COL32(255, 255, 255, 255), "RF");
        
        if (clicked) g_ShowUI = true;
        
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // ============================================
    // Main Window - 主窗口
    // ============================================
    ImGui::SetNextWindowSize(ImVec2(480, 680), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.042f, 0.055f, 0.97f));
    
    ImGui::Begin("##MainWindow", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
    
    ImVec2 win_pos = ImGui::GetWindowPos();
    ImVec2 win_size = ImGui::GetWindowSize();
    
    // ============================================
    // Header - 标题栏
    // ============================================
    // 渐变背景
    DrawGradientRect(draw_list, 
                     ImVec2(win_pos.x, win_pos.y),
                     ImVec2(win_pos.x + win_size.x, win_pos.y + 72),
                     IM_COL32(12, 14, 20, 255),
                     IM_COL32(20, 22, 30, 255));
    
    // 底部分割线
    DrawSeparatorLine(draw_list, 
                      ImVec2(win_pos.x + 20, win_pos.y + 72),
                      ImVec2(win_pos.x + win_size.x - 20, win_pos.y + 72));
    
    // Logo 标题
    ImGui::SetCursorPos(ImVec2(22, 22));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.62f, 0.95f, 1.0f));
    ImGui::TextUnformatted("RenderFusion");
    ImGui::PopStyleColor();
    
    // 副标题
    ImGui::SetCursorPos(ImVec2(22, 44));
    ImGui::TextColored(ImVec4(0.45f, 0.48f, 0.55f, 1.0f), "Advanced Filter Engine");
    
    // 关闭按钮
    ImVec2 close_center(win_pos.x + win_size.x - 32, win_pos.y + 36);
    ImGui::SetCursorScreenPos(ImVec2(close_center.x - 18, close_center.y - 18));
    ImGui::InvisibleButton("##close", ImVec2(36, 36));
    bool close_hovered = ImGui::IsItemHovered();
    bool close_clicked = ImGui::IsItemClicked();
    
    if (close_clicked) g_ShowUI = false;
    
    ImU32 close_bg = close_hovered ? IM_COL32(180, 60, 70, 200) : IM_COL32(40, 42, 50, 180);
    draw_list->AddCircleFilled(close_center, 14, close_bg, 24);
    draw_list->AddText(ImVec2(close_center.x - 4, close_center.y - 7), 
                       IM_COL32(255, 255, 255, close_hovered ? 255 : 180), "X");
    
    ImGui::SetCursorPosY(80);
    
    // ============================================
    // Presets Section - 预设区域
    // ============================================
    ImGui::SetCursorPosX(20);
    ImGui::TextColored(ImVec4(0.50f, 0.52f, 0.58f, 1.0f), "PRESETS");
    
    ImGui::SetCursorPosX(20);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
    
    const char* preset_names[] = {"Original", "Manga B&W"};
    float btn_width = (win_size.x - 50) / 2.0f;
    
    for (int i = 0; i < 2; i++) {
        ImGui::SetCursorPosX(20 + i * (btn_width + 10));
        if (PresetButton(preset_names[i], RF::current_preset == i, ImVec2(btn_width, 44))) {
            RF::current_preset = i;
            RF::ApplyPreset(i);
            Config::SaveConfig();
        }
        if (i == 0) {
            ImGui::SameLine();
        }
    }
    
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16);
    
    // 分隔线
    ImGui::SetCursorPosX(20);
    ImVec2 sep_start = ImGui::GetCursorScreenPos();
    DrawSeparatorLine(draw_list, sep_start, ImVec2(sep_start.x + win_size.x - 40, sep_start.y));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16);
    
    // ============================================
    // Tabs Content - 标签页内容
    // ============================================
    ImGui::BeginChild("##Content", ImVec2(win_size.x - 8, win_size.y - 220), false, ImGuiWindowFlags_NoBackground);
    
    if (ImGui::BeginTabBar("##Tabs", ImGuiTabBarFlags_None)) {
        
        // ------------------------------------
        // Adjust Tab - 基础调整
        // ------------------------------------
        if (ImGui::BeginTabItem("Adjust")) {
            ImGui::SetCursorPosY(12);
            
            // Master开关
            if (StyledCheckbox("Enable Master", &RF::params.enable_master)) {
                Config::SaveConfig();
            }
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
            
            // 滑块区域
            float slider_width = win_size.x - 50;
            
            if (StyledSliderFloat("Brightness", &RF::params.brightness, -0.5f, 0.5f, "%.2f", slider_width)) {
                Config::SaveConfig();
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            
            if (StyledSliderFloat("Contrast", &RF::params.contrast, 0.6f, 1.8f, "%.2f", slider_width)) {
                Config::SaveConfig();
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            
            if (StyledSliderFloat("Saturation", &RF::params.saturation, 0.0f, 2.0f, "%.2f", slider_width)) {
                Config::SaveConfig();
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            
            if (StyledSliderFloat("Temperature", &RF::params.temperature, -1.0f, 1.0f, "%.2f", slider_width)) {
                Config::SaveConfig();
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            
            if (StyledSliderFloat("Vignette", &RF::params.vignette, 0.0f, 1.0f, "%.2f", slider_width)) {
                Config::SaveConfig();
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            
            if (StyledSliderFloat("Film Grain", &RF::params.film_grain, 0.0f, 0.3f, "%.3f", slider_width)) {
                Config::SaveConfig();
            }
            
            ImGui::EndTabItem();
        }
        
        // ------------------------------------
        // Stylize Tab - 艺术风格
        // ------------------------------------
        if (ImGui::BeginTabItem("Stylize")) {
            ImGui::SetCursorPosY(12);
            
            // Art Style 选择
            ImGui::SetCursorPosX(20);
            ImGui::TextColored(ImVec4(0.50f, 0.52f, 0.58f, 1.0f), "ART STYLE");
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
            
            ImGui::SetCursorPosX(20);
            ImGui::SetNextItemWidth(win_size.x - 50);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            
            const char* art_styles[] = {"Off", "Cel Anime", "Chinese Painting", "Sketch", "Anime Flat", "Comic"};
            if (ImGui::Combo("##ArtStyle", &RF::params.art_style, art_styles, IM_ARRAYSIZE(art_styles))) {
                if (RF::params.art_style > 0) RF::params.art_intensity = 1.0f;
                Config::SaveConfig();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            
            // 艺术风格强度
            if (RF::params.art_style > 0) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
                if (StyledSliderFloat("Intensity", &RF::params.art_intensity, 0.0f, 1.0f, "%.2f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
                
                ImGui::SetCursorPosX(20);
                ImGui::TextColored(ImVec4(0.40f, 0.70f, 0.95f, 0.9f), "  %s applied", art_styles[RF::params.art_style]);
            }
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12);
            
            // 分隔线
            ImGui::SetCursorPosX(20);
            ImVec2 s_pos = ImGui::GetCursorScreenPos();
            DrawSeparatorLine(draw_list, s_pos, ImVec2(s_pos.x + win_size.x - 50, s_pos.y));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12);
            
            // Manga B&W
            if (StyledCheckbox("Manga B&W", &RF::params.enable_bw)) {
                Config::SaveConfig();
            }
            if (RF::params.enable_bw) {
                ImGui::SetCursorPosX(72);
                ImGui::TextColored(ImVec4(0.45f, 0.48f, 0.55f, 1.0f), "High contrast comic style");
            }
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
            
            // Sepia
            if (StyledCheckbox("Vintage Sepia", &RF::params.enable_sepia)) {
                Config::SaveConfig();
            }
            if (RF::params.enable_sepia) {
                if (StyledSliderFloat("Sepia Intensity", &RF::params.sepia_intensity, 0.0f, 1.0f, "%.2f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
            }
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
            
            // Outline
            if (StyledCheckbox("Black Outline", &RF::params.enable_outline)) {
                Config::SaveConfig();
            }
            if (RF::params.enable_outline) {
                if (StyledSliderFloat("Threshold", &RF::params.outline_thresh, 0.05f, 0.5f, "%.2f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
                if (StyledSliderFloat("Opacity", &RF::params.outline_opacity, 0.0f, 1.0f, "%.2f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
            }
            
            ImGui::EndTabItem();
        }
        
        // ------------------------------------
        // Effects Tab - 特效
        // ------------------------------------
        if (ImGui::BeginTabItem("Effects")) {
            ImGui::SetCursorPosY(12);
            
            // Sharpen
            if (StyledCheckbox("Sharpen", &RF::params.enable_sharpen)) {
                Config::SaveConfig();
            }
            if (RF::params.enable_sharpen) {
                if (StyledSliderFloat("Sharpness", &RF::params.sharpen_intensity, 0.0f, 1.5f, "%.2f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
            }
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12);
            
            // TikTok RGB
            if (StyledCheckbox("TikTok RGB Split", &RF::params.enable_tiktok)) {
                Config::SaveConfig();
            }
            if (RF::params.enable_tiktok) {
                if (StyledSliderFloat("Offset", &RF::params.tiktok_offset, 0.0f, 0.05f, "%.3f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
                if (StyledSliderFloat("RGB Intensity", &RF::params.tiktok_intensity, 0.0f, 1.0f, "%.2f", win_size.x - 50)) {
                    Config::SaveConfig();
                }
            }
            
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
    
    ImGui::EndChild();
    
    // ============================================
    // Bottom Buttons - 底部按钮
    // ============================================
    ImVec2 bottom_pos(win_pos.x, win_pos.y + win_size.y - 68);
    
    // 底部背景
    DrawGradientRect(draw_list,
                     ImVec2(bottom_pos.x, bottom_pos.y),
                     ImVec2(bottom_pos.x + win_size.x, bottom_pos.y + 68),
                     IM_COL32(15, 17, 23, 255),
                     IM_COL32(10, 12, 18, 255));
    
    // 顶部边框
    DrawSeparatorLine(draw_list,
                      ImVec2(bottom_pos.x + 20, bottom_pos.y),
                      ImVec2(bottom_pos.x + win_size.x - 20, bottom_pos.y));
    
    // 按钮区域
    float btn_w = 140;
    float btn_h = 44;
    float btn_spacing = 16;
    float total_width = btn_w * 2 + btn_spacing;
    float start_x = (win_size.x - total_width) / 2;
    
    ImGui::SetCursorPos(ImVec2(start_x, win_size.y - 58));
    
    // Save Button
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(35, 95, 170, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50, 115, 195, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(30, 80, 150, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 248, 255, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    
    if (ImGui::Button("Save Config", ImVec2(btn_w, btn_h))) {
        Config::SaveConfig();
    }
    
    ImGui::SameLine(0, btn_spacing);
    
    // Reset Button
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(45, 48, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 65, 78, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(38, 42, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 165, 175, 255));
    
    if (ImGui::Button("Reset All", ImVec2(btn_w, btn_h))) {
        RF::current_preset = 0;
        RF::ApplyPreset(0);
        Config::SaveConfig();
    }
    
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar();
    
    // ============================================
    // Window Border Glow - 窗口边框发光
    // ============================================
    ImU32 glow_color = IM_COL32(35, 95, 175, 60);
    draw_list->AddRect(ImVec2(win_pos.x - 1, win_pos.y - 1), 
                       ImVec2(win_pos.x + win_size.x + 1, win_pos.y + win_size.y + 1),
                       glow_color, 20.0f, 0, 1.5f);
    
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    
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

// ===================== Input Hook =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
        if (h) return;
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
        if (h) return;
    }
}

static void* MainThread(void*) {
    sleep(3);
    LOGI("RenderFusion Pro loaded");
    GlossInit(true);
    
    // Load configuration from file if it exists
    Config::LoadConfig();
    
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
