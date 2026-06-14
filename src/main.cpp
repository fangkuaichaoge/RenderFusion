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

// ===================== PreloaderInput Touch Callback =====================
typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x, float y);

struct PreloaderInput_Interface {
    void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn callback);
};

typedef PreloaderInput_Interface* (*GetPreloaderInput_Fn)();

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
        GLuint prog_oil = 0;         // 油画

    // Season Shaders
    GLuint prog_spring = 0;      // 春季
    GLuint prog_summer = 0;      // 夏季
    GLuint prog_autumn = 0;      // 金秋
    GLuint prog_winter = 0;      // 冬季

    // Pixel Art Shader
    GLuint prog_pixel = 0;       // 像素画

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
        int art_style = 0;      // 0=Off, 1=Cel Anime, 2=Chinese Painting, 3=Sketch, 4=Anime Flat, 5=Comic, 6=Oil Painting
        float art_intensity = 1.0f;
        
        // TikTok RGB Split
        bool enable_tiktok = false;
        float tiktok_offset = 0.005f;  // RGB偏移量
        float tiktok_intensity = 1.0f;
        
        // Seasons (四季滤镜)
        bool enable_season_rotation = false;  // 启用四季自动轮换
        int season = 0;            // 0=Off, 1=Spring, 2=Summer, 3=Autumn, 4=Winter
        float season_intensity = 1.0f;
        
        // Pixel Art (像素画)
        bool enable_pixel = false;
        int pixel_size = 2;            // 像素大小: 1, 2, 3, 4
        int pixel_palette = 2;         // 调色板: 1=GameBoy, 2=Sweetie16, 3=Endesga32
        float pixel_intensity = 1.0f;
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
            {"Original", {false, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, false, false, 0.8f, 0.0f, false, 0.5f, false, 0.15f, 1.0f, 0, 1.0f, false, 0.005f, 1.0f, false, 0, 1.0f}},
            // Manga B&W: 黑白漫画风格（高对比度黑白 + 描边）
            {"Manga B&W", {true, 0.05f, 1.15f, 0.0f, 0.0f, 0.0f, true, false, 0.0f, 0.0f, false, 0.5f, true, 0.12f, 1.0f, 0, 1.0f, false, 0.005f, 1.0f, false, 0, 1.0f}}
        };
        
        if (idx >= 0 && idx < 2) {
            params = presets[idx].p;
        }
    }

    // Check if any multi-pass effect is enabled
    bool IsMultiPassEnabled() {
        return params.enable_outline;
    }

    // 根据系统时间计算当前季节（四季轮换）
    // 时间安排：从6点开始，每3小时一个季节，12小时一轮
    // 6:00-9:00=春, 9:00-12:00=夏, 12:00-15:00=秋, 15:00-18:00=冬
    // 18:00-21:00=春, 21:00-24:00=夏, 0:00-3:00=秋, 3:00-6:00=冬
    int CalculateSeasonByTime() {
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        int hour = timeinfo->tm_hour;  // 0-23
        
        // 计算从6点开始经过的小时数（处理跨天情况）
        int hours_from_6am = (hour - 6 + 24) % 24;
        
        // 每3小时一个季节：0=春, 1=夏, 2=秋, 3=冬
        int season_index = (hours_from_6am / 3) % 4;
        
        // 返回季节值（1=春, 2=夏, 3=秋, 4=冬）
        return season_index + 1;
    }
}

// ===================== MD3 Style Configuration =====================
namespace MD3Style {
    // Primary Colors (Red default)
    inline ImVec4 Primary(0.937f, 0.267f, 0.267f, 1.0f);           // Red 500
    inline ImVec4 PrimaryLight(1.0f, 0.576f, 0.576f, 1.0f);        // Red 300
    inline ImVec4 PrimaryDark(0.812f, 0.118f, 0.118f, 1.0f);       // Red 700
    
    // Secondary Colors
    inline ImVec4 Secondary(0.035f, 0.718f, 0.718f, 1.0f);         // Teal 500
    
    // Surface Colors (Dark theme default)
    inline ImVec4 Surface(0.118f, 0.118f, 0.118f, 1.0f);
    inline ImVec4 SurfaceVariant(0.196f, 0.196f, 0.196f, 1.0f);
    inline ImVec4 SurfaceContainer(0.157f, 0.157f, 0.157f, 1.0f);
    inline ImVec4 SurfaceContainerHigh(0.220f, 0.220f, 0.220f, 1.0f);
    inline ImVec4 SurfaceContainerHighest(0.278f, 0.278f, 0.278f, 1.0f);
    
    // Background Colors
    inline ImVec4 Background(0.078f, 0.078f, 0.078f, 0.95f);
    inline ImVec4 BackgroundDim(0.059f, 0.059f, 0.059f, 0.95f);
    
    // Text Colors
    inline ImVec4 OnPrimary(1.0f, 1.0f, 1.0f, 1.0f);
    inline ImVec4 OnSurface(0.957f, 0.957f, 0.957f, 1.0f);
    inline ImVec4 OnSurfaceVariant(0.706f, 0.706f, 0.706f, 1.0f);
    inline ImVec4 Outline(0.451f, 0.451f, 0.451f, 1.0f);
    inline ImVec4 OutlineVariant(0.278f, 0.278f, 0.278f, 1.0f);
    
    // Success Colors
    inline ImVec4 Success(0.298f, 0.686f, 0.314f, 1.0f);
    
    // Background Theme Mode
    enum BackgroundTheme {
        THEME_DARK = 0,
        THEME_LIGHT,
        THEME_AMOLED
    };
    inline int CurrentBackgroundTheme = THEME_DARK;
    
    // Light theme colors
    inline ImVec4 LightSurface(0.98f, 0.98f, 0.98f, 1.0f);
    inline ImVec4 LightSurfaceVariant(0.92f, 0.92f, 0.92f, 1.0f);
    inline ImVec4 LightBackground(1.0f, 1.0f, 1.0f, 0.98f);
    inline ImVec4 LightOnSurface(0.1f, 0.1f, 0.1f, 1.0f);
    inline ImVec4 LightOnSurfaceVariant(0.45f, 0.45f, 0.45f, 1.0f);
    
    // Dynamic Island State
    inline float IslandWidth = 120.0f;
    inline float IslandHeight = 36.0f;
    inline float IslandTargetWidth = 120.0f;
    inline float IslandTargetHeight = 36.0f;
    inline float IslandAnimSpeed = 8.0f;
    inline float IslandHoverTime = 0.0f;
    inline ImVec2 IslandPos = ImVec2(0.5f, 50.0f);  // Normalized x (0-1), absolute y
    inline bool IslandDragging = false;
    inline ImVec2 IslandDragOffset = ImVec2(0, 0);
    inline ImVec4 IslandBgColor(0.937f, 0.267f, 0.267f, 1.0f);  // Custom island background
    
    // Preset color schemes
    enum ColorScheme {
        SCHEME_RED = 0,
        SCHEME_PURPLE,
        SCHEME_BLUE,
        SCHEME_GREEN,
        SCHEME_ORANGE,
        SCHEME_PINK,
        SCHEME_CYAN,
        SCHEME_MOONLIGHT,
        SCHEME_CUSTOM
    };
    
    inline int CurrentScheme = SCHEME_RED;
    
    // Color scheme definitions
    struct ColorSchemeDef {
        ImVec4 primary;
        ImVec4 primaryLight;
        ImVec4 primaryDark;
        ImVec4 secondary;
        const char* name;
    };
    
    inline ColorSchemeDef Schemes[] = {
        { ImVec4(0.937f, 0.267f, 0.267f, 1.0f), ImVec4(1.0f, 0.576f, 0.576f, 1.0f), ImVec4(0.812f, 0.118f, 0.118f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Red" },
        { ImVec4(0.675f, 0.357f, 0.937f, 1.0f), ImVec4(0.824f, 0.576f, 1.0f, 1.0f), ImVec4(0.502f, 0.173f, 0.827f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Purple" },
        { ImVec4(0.129f, 0.588f, 0.953f, 1.0f), ImVec4(0.420f, 0.757f, 0.988f, 1.0f), ImVec4(0.055f, 0.365f, 0.749f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Blue" },
        { ImVec4(0.298f, 0.686f, 0.314f, 1.0f), ImVec4(0.545f, 0.835f, 0.553f, 1.0f), ImVec4(0.145f, 0.502f, 0.157f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Green" },
        { ImVec4(0.988f, 0.612f, 0.157f, 1.0f), ImVec4(1.0f, 0.812f, 0.506f, 1.0f), ImVec4(0.878f, 0.451f, 0.0f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Orange" },
        { ImVec4(0.914f, 0.318f, 0.533f, 1.0f), ImVec4(1.0f, 0.608f, 0.776f, 1.0f), ImVec4(0.776f, 0.157f, 0.369f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Pink" },
        { ImVec4(0.0f, 0.737f, 0.831f, 1.0f), ImVec4(0.333f, 0.867f, 0.925f, 1.0f), ImVec4(0.0f, 0.537f, 0.612f, 1.0f), ImVec4(0.675f, 0.357f, 0.937f, 1.0f), "Cyan" },
        { ImVec4(0.9725f, 1.0f, 0.4980f, 1.0f), ImVec4(1.0f, 1.0f, 0.7953f, 1.0f), ImVec4(0.8784f, 0.9f, 0.3980f, 1.0f), ImVec4(0.4980f, 0.5137f, 1.0f, 1.0f), "Moonlight" },
        { ImVec4(0.937f, 0.267f, 0.267f, 1.0f), ImVec4(1.0f, 0.576f, 0.576f, 1.0f), ImVec4(0.812f, 0.118f, 0.118f, 1.0f), ImVec4(0.035f, 0.718f, 0.718f, 1.0f), "Custom" }
    };
    
    inline void ApplyBackgroundTheme(int theme) {
        CurrentBackgroundTheme = theme;
        if (theme == THEME_LIGHT) {
            Surface = LightSurface;
            SurfaceVariant = LightSurfaceVariant;
            SurfaceContainer = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
            SurfaceContainerHigh = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
            SurfaceContainerHighest = ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
            Background = LightBackground;
            BackgroundDim = ImVec4(0.95f, 0.95f, 0.95f, 0.98f);
            OnSurface = LightOnSurface;
            OnSurfaceVariant = LightOnSurfaceVariant;
            Outline = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
            OutlineVariant = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        } else if (theme == THEME_AMOLED) {
            Surface = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
            SurfaceVariant = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
            SurfaceContainer = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
            SurfaceContainerHigh = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
            SurfaceContainerHighest = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
            Background = ImVec4(0.0f, 0.0f, 0.0f, 0.98f);
            BackgroundDim = ImVec4(0.0f, 0.0f, 0.0f, 0.98f);
            OnSurface = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            OnSurfaceVariant = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            Outline = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
            OutlineVariant = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
        } else {
            // Dark theme
            Surface = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);
            SurfaceVariant = ImVec4(0.196f, 0.196f, 0.196f, 1.0f);
            SurfaceContainer = ImVec4(0.157f, 0.157f, 0.157f, 1.0f);
            SurfaceContainerHigh = ImVec4(0.220f, 0.220f, 0.220f, 1.0f);
            SurfaceContainerHighest = ImVec4(0.278f, 0.278f, 0.278f, 1.0f);
            Background = ImVec4(0.078f, 0.078f, 0.078f, 0.95f);
            BackgroundDim = ImVec4(0.059f, 0.059f, 0.059f, 0.95f);
            OnSurface = ImVec4(0.957f, 0.957f, 0.957f, 1.0f);
            OnSurfaceVariant = ImVec4(0.706f, 0.706f, 0.706f, 1.0f);
            Outline = ImVec4(0.451f, 0.451f, 0.451f, 1.0f);
            OutlineVariant = ImVec4(0.278f, 0.278f, 0.278f, 1.0f);
        }
    }
    
    inline void ApplyScheme(int scheme) {
        if (scheme >= 0 && scheme <= SCHEME_CUSTOM) {
            CurrentScheme = scheme;
            if (scheme != SCHEME_CUSTOM) {
                Primary = Schemes[scheme].primary;
                PrimaryLight = Schemes[scheme].primaryLight;
                PrimaryDark = Schemes[scheme].primaryDark;
                Secondary = Schemes[scheme].secondary;
                IslandBgColor = Primary;
            }
        }
    }
    
    inline void SaveToConfig(nlohmann::json& j) {
        j["md3_scheme"] = CurrentScheme;
        j["md3_bg_theme"] = CurrentBackgroundTheme;
        j["md3_primary_r"] = Primary.x;
        j["md3_primary_g"] = Primary.y;
        j["md3_primary_b"] = Primary.z;
        j["md3_primary_light_r"] = PrimaryLight.x;
        j["md3_primary_light_g"] = PrimaryLight.y;
        j["md3_primary_light_b"] = PrimaryLight.z;
        j["md3_primary_dark_r"] = PrimaryDark.x;
        j["md3_primary_dark_g"] = PrimaryDark.y;
        j["md3_primary_dark_b"] = PrimaryDark.z;
        j["md3_secondary_r"] = Secondary.x;
        j["md3_secondary_g"] = Secondary.y;
        j["md3_secondary_b"] = Secondary.z;
        j["md3_island_bg_r"] = IslandBgColor.x;
        j["md3_island_bg_g"] = IslandBgColor.y;
        j["md3_island_bg_b"] = IslandBgColor.z;
        j["md3_island_pos_x"] = IslandPos.x;
        j["md3_island_pos_y"] = IslandPos.y;
    }
    
    inline void LoadFromConfig(const nlohmann::json& j) {
        if (j.contains("md3_bg_theme")) {
            ApplyBackgroundTheme(j["md3_bg_theme"]);
        }
        if (j.contains("md3_scheme")) {
            CurrentScheme = j["md3_scheme"];
            if (CurrentScheme == SCHEME_CUSTOM) {
                if (j.contains("md3_primary_r")) Primary.x = j["md3_primary_r"];
                if (j.contains("md3_primary_g")) Primary.y = j["md3_primary_g"];
                if (j.contains("md3_primary_b")) Primary.z = j["md3_primary_b"];
                if (j.contains("md3_primary_light_r")) PrimaryLight.x = j["md3_primary_light_r"];
                if (j.contains("md3_primary_light_g")) PrimaryLight.y = j["md3_primary_light_g"];
                if (j.contains("md3_primary_light_b")) PrimaryLight.z = j["md3_primary_light_b"];
                if (j.contains("md3_primary_dark_r")) PrimaryDark.x = j["md3_primary_dark_r"];
                if (j.contains("md3_primary_dark_g")) PrimaryDark.y = j["md3_primary_dark_g"];
                if (j.contains("md3_primary_dark_b")) PrimaryDark.z = j["md3_primary_dark_b"];
                if (j.contains("md3_secondary_r")) Secondary.x = j["md3_secondary_r"];
                if (j.contains("md3_secondary_g")) Secondary.y = j["md3_secondary_g"];
                if (j.contains("md3_secondary_b")) Secondary.z = j["md3_secondary_b"];
            } else {
                ApplyScheme(CurrentScheme);
            }
        }
        if (j.contains("md3_island_bg_r")) IslandBgColor.x = j["md3_island_bg_r"];
        if (j.contains("md3_island_bg_g")) IslandBgColor.y = j["md3_island_bg_g"];
        if (j.contains("md3_island_bg_b")) IslandBgColor.z = j["md3_island_bg_b"];
        if (j.contains("md3_island_pos_x")) IslandPos.x = j["md3_island_pos_x"];
        if (j.contains("md3_island_pos_y")) IslandPos.y = j["md3_island_pos_y"];
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
        if (j.contains("enable_season_rotation")) RF::params.enable_season_rotation = j["enable_season_rotation"];
        if (j.contains("season")) RF::params.season = j["season"];
        if (j.contains("season_intensity")) RF::params.season_intensity = j["season_intensity"];
        // Pixel Art
        if (j.contains("enable_pixel")) RF::params.enable_pixel = j["enable_pixel"];
        if (j.contains("pixel_size")) RF::params.pixel_size = j["pixel_size"];
        if (j.contains("pixel_palette")) RF::params.pixel_palette = j["pixel_palette"];
        if (j.contains("pixel_intensity")) RF::params.pixel_intensity = j["pixel_intensity"];
        
        // Load MD3 Style Configuration
        MD3Style::LoadFromConfig(j);
        
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
        j["enable_season_rotation"] = RF::params.enable_season_rotation;
        j["season"] = RF::params.season;
        j["season_intensity"] = RF::params.season_intensity;
        // Pixel Art
        j["enable_pixel"] = RF::params.enable_pixel;
        j["pixel_size"] = RF::params.pixel_size;
        j["pixel_palette"] = RF::params.pixel_palette;
        j["pixel_intensity"] = RF::params.pixel_intensity;
        
        // MD3 Style Configuration
        MD3Style::SaveToConfig(j);
        
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
// Pixel Art Shader - 像素画效果
// ==========================================
const char* g_frag_pixel = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uPixelSize;
uniform int uPalette;
uniform float uIntensity;

// 计算颜色距离（简化版RGB距离）
float colorDist(vec3 c1, vec3 c2) {
    return dot(c1-c2, c1-c2);
}

// Game Boy 调色板找最近色
vec3 findGameBoyColor(vec3 c) {
    vec3 p0 = vec3(0.059, 0.220, 0.059);
    vec3 p1 = vec3(0.188, 0.384, 0.188);
    vec3 p2 = vec3(0.545, 0.675, 0.059);
    vec3 p3 = vec3(0.608, 0.737, 0.059);
    
    float d0 = colorDist(c, p0); float d1 = colorDist(c, p1);
    float d2 = colorDist(c, p2); float d3 = colorDist(c, p3);
    
    float minD = min(min(d0, d1), min(d2, d3));
    if (minD == d0) return p0;
    if (minD == d1) return p1;
    if (minD == d2) return p2;
    return p3;
}

// Sweetie 16 调色板找最近色
vec3 findSweetie16Color(vec3 c) {
    vec3 p[16];
    p[0] = vec3(0.102, 0.110, 0.173);
    p[1] = vec3(0.200, 0.235, 0.341);
    p[2] = vec3(0.145, 0.443, 0.475);
    p[3] = vec3(0.161, 0.212, 0.435);
    p[4] = vec3(0.365, 0.153, 0.365);
    p[5] = vec3(0.694, 0.243, 0.325);
    p[6] = vec3(0.937, 0.490, 0.341);
    p[7] = vec3(1.000, 0.804, 0.459);
    p[8] = vec3(0.655, 0.941, 0.439);
    p[9] = vec3(0.220, 0.718, 0.392);
    p[10] = vec3(0.231, 0.365, 0.788);
    p[11] = vec3(0.255, 0.651, 0.965);
    p[12] = vec3(0.451, 0.937, 0.969);
    p[13] = vec3(0.580, 0.690, 0.761);
    p[14] = vec3(0.000, 0.000, 0.000);
    p[15] = vec3(1.000, 1.000, 1.000);
    
    float minDist = 999.0;
    vec3 best = c;
    for (int i = 0; i < 16; i++) {
        float d = colorDist(c, p[i]);
        if (d < minDist) { minDist = d; best = p[i]; }
    }
    return best;
}

// Endesga 32 调色板找最近色
vec3 findEndesga32Color(vec3 c) {
    vec3 p[32];
    p[0] = vec3(0.000, 0.000, 0.000);
    p[1] = vec3(0.247, 0.157, 0.196);
    p[2] = vec3(0.329, 0.243, 0.173);
    p[3] = vec3(0.455, 0.247, 0.224);
    p[4] = vec3(0.722, 0.435, 0.314);
    p[5] = vec3(0.745, 0.290, 0.184);
    p[6] = vec3(0.898, 0.231, 0.267);
    p[7] = vec3(0.620, 0.157, 0.208);
    p[8] = vec3(0.984, 0.573, 0.169);
    p[9] = vec3(0.843, 0.463, 0.263);
    p[10] = vec3(0.682, 0.463, 0.255);
    p[11] = vec3(0.800, 0.498, 0.365);
    p[12] = vec3(0.894, 0.651, 0.447);
    p[13] = vec3(0.910, 0.718, 0.588);
    p[14] = vec3(0.918, 0.831, 0.667);
    p[15] = vec3(0.961, 0.867, 0.690);
    p[16] = vec3(1.000, 0.906, 0.384);
    p[17] = vec3(0.388, 0.776, 0.302);
    p[18] = vec3(0.196, 0.451, 0.271);
    p[19] = vec3(0.098, 0.239, 0.247);
    p[20] = vec3(0.196, 0.235, 0.373);
    p[21] = vec3(0.212, 0.247, 0.357);
    p[22] = vec3(0.322, 0.373, 0.549);
    p[23] = vec3(0.357, 0.482, 0.702);
    p[24] = vec3(0.424, 0.620, 0.847);
    p[25] = vec3(0.518, 0.714, 0.922);
    p[26] = vec3(0.616, 0.784, 0.906);
    p[27] = vec3(0.663, 0.749, 0.890);
    p[28] = vec3(0.686, 0.749, 0.824);
    p[29] = vec3(0.765, 0.867, 0.910);
    p[30] = vec3(0.345, 0.408, 0.518);
    p[31] = vec3(1.000, 1.000, 1.000);
    
    float minDist = 999.0;
    vec3 best = c;
    for (int i = 0; i < 32; i++) {
        float d = colorDist(c, p[i]);
        if (d < minDist) { minDist = d; best = p[i]; }
    }
    return best;
}

// 根据调色板类型找最近色
vec3 quantizeColor(vec3 c, int palette) {
    if (palette == 1) return findGameBoyColor(c);
    if (palette == 2) return findSweetie16Color(c);
    if (palette == 3) return findEndesga32Color(c);
    return c;
}

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    
    // 像素化：将坐标对齐到像素网格
    vec2 pixelGrid = uTexelSize * uPixelSize;
    vec2 pixelCoord = floor(vTexCoord / pixelGrid) * pixelGrid + pixelGrid * 0.5;
    
    // 采样像素化后的颜色
    vec3 pixelColor = texture2D(uTexture, pixelCoord).rgb;
    
    // 颜色量化到调色板
    vec3 result = quantizeColor(pixelColor, uPalette);
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
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

// 油画风格 (Oil Painting) - 笔触效果
const char* g_frag_oil = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123); }

// 计算亮度
float luminance(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    float gray = luminance(color.rgb);
    
    // ========================================
    // 1. 轻微提亮
    // ========================================
    gray = gray * 1.05 + 0.02;
    
    // ========================================
    // 2. Sobel 边缘 - 柔和版
    // ========================================
    float c0 = luminance(texture2D(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb);
    float c1 = luminance(texture2D(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb);
    float c2 = luminance(texture2D(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb);
    float c3 = luminance(texture2D(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb);
    float c4 = luminance(texture2D(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb);
    float c5 = luminance(texture2D(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb);
    float c6 = luminance(texture2D(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb);
    float c7 = luminance(texture2D(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb);
    
    float gx = -c0 + c1 - 2.0*c4 + 2.0*c5 - c2 + c3;
    float gy = -c0 - 2.0*c6 - c1 + c2 + 2.0*c7 + c3;
    float edge = sqrt(gx*gx + gy*gy);
    
    // 柔和铅笔线条
    float lines = 1.0 - smoothstep(0.0, 0.45, edge);
    
    // ========================================
    // 3. 精细斜线阴影 - 彩色版本
    // ========================================
    vec2 uv = vTexCoord * 180.0;
    float pattern1 = step(0.5, fract(uv.x + uv.y));
    float pattern2 = step(0.5, fract(uv.x - uv.y));
    
    float hatch = 0.0;
    if(gray < 0.6) hatch += pattern1 * 0.1 * (0.6 - gray) / 0.6;
    if(gray < 0.4) hatch += pattern2 * 0.15 * (0.4 - gray) / 0.4;
    if(gray < 0.25) hatch += 0.18 * (0.25 - gray) / 0.25;
    
    // ========================================
    // 4. 纸张纹理
    // ========================================
    float paper = random(vTexCoord * 350.0) * 0.03;
    
    // 基础灰度结果
    float baseResult = lines - hatch + paper * 0.25;
    baseResult = clamp(baseResult, 0.0, 1.0);
    
    // ========================================
    // 5. 应用彩色 - 彩铅效果
    // ========================================
    // 用原色调乘以基础结果，保留颜色
    vec3 pencil = color.rgb * baseResult;
    
    // ========================================
    // 6. 彩色描边 - 边缘用深色
    // ========================================
    float outline = smoothstep(0.05, 0.22, edge);
    vec3 edgeColor = color.rgb * 0.35;  // 深色描边
    pencil = mix(pencil, edgeColor, outline * 0.6);
    
    pencil = clamp(pencil, 0.0, 1.0);
    
    gl_FragColor = vec4(mix(color.rgb, pencil, uIntensity), color.a);
}
)";

// ==========================================
// Season Shaders (四季滤镜)
// ==========================================

// 春季 (Spring) - 明亮轻盈，春日温暖感
const char* g_frag_spring = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uIntensity;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 明亮轻盈 - 整体提亮
    result = result * 1.08 + 0.04;
    
    // 春日温暖色调 - 轻微暖色
    vec3 warmth = vec3(1.0, 0.98, 0.92);
    result *= warmth;
    
    // 轻微提升饱和度 - 花朵般鲜艳
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.12);
    
    // 柔和对比度 - 轻盈不沉重
    result = (result - 0.5) * 0.92 + 0.5;
    
    // 轻微的粉色花朵色调 - 高光区域
    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    if (lum > 0.7) {
        vec3 pinkTint = vec3(1.0, 0.95, 0.97);
        result = mix(result, result * pinkTint, (lum - 0.7) * 0.5);
    }
    
    // 柔和的高光
    result = mix(result, vec3(1.0), max(0.0, lum - 0.85) * 0.3);
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 夏季 (Summer) - 夏日阳光，明亮热烈
const char* g_frag_summer = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;
uniform float uTime;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123); }

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 高饱和度 - 夏日鲜艳
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 1.25);
    
    // 夏日阳光感 - 强烈的暖色光照
    vec3 sunLight = vec3(1.08, 1.02, 0.92);
    result *= sunLight;
    
    // 整体提亮 - 阳光充足
    result = result * 1.1 + 0.05;
    
    // 强烈的阳光对比度
    result = (result - 0.5) * 1.15 + 0.5;
    
    // 模拟夏日强烈的顶光效果 - 上亮下暗的层次感
    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    
    // 阳光光晕 - 高光区域扩散
    vec3 glow = vec3(0.0);
    for (int i = -2; i <= 2; i++) {
        for (int j = -2; j <= 2; j++) {
            vec2 offset = vec2(float(i), float(j)) * uTexelSize * 2.0;
            glow += texture2D(uTexture, vTexCoord + offset).rgb;
        }
    }
    glow /= 25.0;
    // 高光区域添加温暖的光晕
    result = mix(result, glow * vec3(1.05, 0.98, 0.90), lum * 0.2);
    
    // 夏日强烈阳光 - 高光区域更亮更暖
    if (lum > 0.65) {
        vec3 sunHighlight = vec3(1.1, 1.0, 0.85);
        result = mix(result, result * sunHighlight, (lum - 0.65) * 0.6);
    }
    
    // 阳光穿透感 - 轻微的过曝效果
    if (lum > 0.85) {
        result = mix(result, vec3(1.0, 0.98, 0.92), (lum - 0.85) * 1.5);
    }
    
    // 微妙的阳光闪烁
    float sparkle = random(vTexCoord * 150.0 + uTime * 0.4);
    if (sparkle > 0.996 && lum > 0.55) {
        result += vec3(0.1, 0.08, 0.05);
    }
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 金秋 (Autumn) - 绿色→橙黄色，温暖丰收
const char* g_frag_autumn = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uIntensity;

// HSV 转换
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 转换到HSV进行色调映射
    vec3 hsv = rgb2hsv(result);
    float hue = hsv.x;
    float sat = hsv.y;
    float val = hsv.z;
    
    // 扩大绿色检测范围：包括深绿 (hue: 0.05-0.52)
    // 深绿/墨绿: 0.05-0.12
    // 普通绿: 0.12-0.35
    // 青绿: 0.35-0.52
    bool isGreen = (hue > 0.05 && hue < 0.52) && sat > 0.1;
    
    if (isGreen) {
        float targetHue;
        float greenness = 0.0;
        
        // 深绿/墨绿区域 (0.05-0.15)
        if (hue >= 0.05 && hue < 0.15) {
            // 深绿 -> 深橙红/砖红
            targetHue = mix(0.02, 0.06, (hue - 0.05) / 0.1);
            greenness = smoothstep(0.0, 0.5, (hue - 0.05) / 0.1);
        }
        // 黄绿区域 (0.15-0.22)
        else if (hue >= 0.15 && hue < 0.22) {
            // 黄绿 -> 金黄
            targetHue = mix(0.10, 0.12, (hue - 0.15) / 0.07);
            greenness = 0.5 + 0.3 * (1.0 - abs(hue - 0.185) / 0.035);
        }
        // 纯绿区域 (0.22-0.35)
        else if (hue >= 0.22 && hue < 0.35) {
            // 纯绿 -> 明亮橙黄
            targetHue = mix(0.06, 0.10, (hue - 0.22) / 0.13);
            greenness = 0.7 + 0.2 * (1.0 - abs(hue - 0.285) / 0.065);
        }
        // 青绿区域 (0.35-0.52)
        else {
            // 青绿 -> 橙色
            targetHue = mix(0.04, 0.06, (hue - 0.35) / 0.17);
            greenness = smoothstep(0.0, 0.6, (0.52 - hue) / 0.17);
        }
        
        // 提亮秋色 - 明亮的金色而非暗色
        float newSat = sat * 1.0 + 0.15; // 增加饱和度但不过度
        newSat = min(newSat, 0.95);
        
        // 提亮 - 金秋要明亮
        float newVal = val * 1.2 + 0.12;
        newVal = min(newVal, 1.0);
        
        vec3 autumnColor = hsv2rgb(vec3(targetHue, newSat, newVal));
        result = mix(result, autumnColor, greenness * 0.92);
    }
    
    // 整体暖色调 - 温暖明亮的秋日阳光
    vec3 warmTint = vec3(1.08, 1.0, 0.90);
    result *= warmTint;
    
    // 整体提亮
    result = result * 1.08 + 0.04;
    
    // 适中对比度
    result = (result - 0.5) * 1.05 + 0.5;
    
    // 金黄高光 - 明亮
    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    if (lum > 0.6) {
        vec3 goldHighlight = vec3(1.05, 0.95, 0.80);
        result = mix(result, result * goldHighlight + 0.03, (lum - 0.6) * 0.5);
    }
    
    // 高光区域更亮
    if (lum > 0.8) {
        result = mix(result, vec3(1.0, 0.95, 0.85), (lum - 0.8) * 0.5);
    }
    
    gl_FragColor = vec4(mix(color.rgb, result, uIntensity), color.a);
}
)";

// 冬季 (Winter) - 绿色→雪白/淡蓝，寒冷冰雪
const char* g_frag_winter = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;
uniform float uTime;

float random(vec2 st) { return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123); }

// HSV 转换
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    vec3 result = color.rgb;
    
    // 转换到HSV进行色调映射
    vec3 hsv = rgb2hsv(result);
    float hue = hsv.x;
    float sat = hsv.y;
    float val = hsv.z;
    
    // 扩大绿色检测范围：包括深绿 (hue: 0.05-0.52)
    bool isGreen = (hue > 0.05 && hue < 0.52) && sat > 0.08;
    
    if (isGreen) {
        float greenness = 0.0;
        
        // 深绿/墨绿区域 (0.05-0.15)
        if (hue >= 0.05 && hue < 0.15) {
            greenness = smoothstep(0.0, 0.6, (hue - 0.05) / 0.1);
        }
        // 普通绿区域 (0.15-0.35)
        else if (hue >= 0.15 && hue < 0.35) {
            greenness = 0.6 + 0.35 * (1.0 - abs(hue - 0.25) / 0.1);
        }
        // 青绿区域 (0.35-0.52)
        else {
            greenness = smoothstep(0.0, 0.6, (0.52 - hue) / 0.17);
        }
        
        // 冰雪蓝白色 - 低饱和度偏蓝色调
        float newSat = sat * 0.2 + 0.15; // 保留一定饱和度让蓝色显现
        float newVal = val * 1.08 + 0.12; // 适度提亮
        newVal = min(newVal, 0.92);
        
        // 偏蓝色调 (hue: 0.58-0.65 为蓝色范围)
        float icyHue = 0.60 + random(vTexCoord) * 0.03;
        
        vec3 icyColor = hsv2rgb(vec3(icyHue, newSat, newVal));
        result = mix(result, icyColor, greenness * 0.88);
    }
    
    // 整体蓝白色调 - 偏蓝
    vec3 coldTint = vec3(0.90, 0.95, 1.06);
    result *= coldTint;
    
    // 保持一定色彩 - 不要完全灰化
    float gray = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(gray), result, 0.75);
    
    // 添加淡蓝色覆盖
    vec3 icyOverlay = vec3(0.85, 0.92, 1.0);
    result = mix(result, result * icyOverlay, 0.25);
    
    // 适度提亮
    result = result * 1.03 + 0.04;
    
    // 柔和对比度
    result = (result - 0.5) * 0.92 + 0.5;
    
    // 冰霜高光 - 偏蓝白
    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    if (lum > 0.6) {
        vec3 frostHighlight = vec3(0.88, 0.94, 1.0);
        result = mix(result, result * frostHighlight + 0.04, (lum - 0.6) * 0.35);
    }
    
    // 微妙的雪花闪烁效果 - 偏蓝
    float snowSparkle = random(vTexCoord * 200.0 + uTime * 0.3);
    if (snowSparkle > 0.997 && lum > 0.4) {
        result += vec3(0.06, 0.08, 0.12);
    }
    
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
    // Oil Painting shader
    if (RF::prog_oil == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_oil = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_oil));
    }
    
    // Season shaders
    if (RF::prog_spring == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_spring = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_spring));
    }
    if (RF::prog_summer == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_summer = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_summer));
    }
    if (RF::prog_autumn == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_autumn = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_autumn));
    }
    if (RF::prog_winter == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_winter = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_winter));
    }
    // Pixel Art shader
    if (RF::prog_pixel == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, g_quad_vert);
        RF::prog_pixel = LinkProgram(vs, CompileShader(GL_FRAGMENT_SHADER, g_frag_pixel));
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
            case 6: prog = RF::prog_oil; break;
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

    // Pass: Season (四季滤镜)
    // 如果启用了四季轮换，则根据系统时间自动计算季节
    int current_season = RF::params.season;
    if (RF::params.enable_season_rotation) {
        current_season = RF::CalculateSeasonByTime();
    }
    
    if (use_fbo && current_season > 0) {
        GLuint src_tex = final_tex;
        GLuint dst_fbo = (src_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo;
        GLuint dst_tex = (src_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex;
        if (src_tex == RF::screen_tex) { dst_fbo = RF::fbo; dst_tex = RF::fbo_tex; }
        
        // 选择对应的季节shader
        GLuint prog = 0;
        switch (current_season) {
            case 1: prog = RF::prog_spring; break;
            case 2: prog = RF::prog_summer; break;
            case 3: prog = RF::prog_autumn; break;
            case 4: prog = RF::prog_winter; break;
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
            SAFE_UNIFORM(loc, glUniform1f, RF::params.season_intensity);
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

    // Pass 7: Pixel Art (像素画效果)
    if (RF::prog_pixel != 0 && RF::params.enable_pixel) {
        GLuint src_tex = final_tex;
        GLuint dst_fbo = use_fbo ? ((src_tex == RF::fbo_tex) ? RF::fbo2 : RF::fbo) : 0;
        GLuint dst_tex = use_fbo ? ((src_tex == RF::fbo_tex) ? RF::fbo_tex2 : RF::fbo_tex) : RF::screen_tex;
        if (src_tex == RF::screen_tex && use_fbo) { dst_fbo = RF::fbo; dst_tex = RF::fbo_tex; }

        glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
        BindQuad(RF::prog_pixel);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);

        GLint loc;
        loc = glGetUniformLocation(RF::prog_pixel, "uTexture");
        SAFE_UNIFORM(loc, glUniform1i, 0);
        loc = glGetUniformLocation(RF::prog_pixel, "uTexelSize");
        SAFE_UNIFORM(loc, glUniform2f, 1.0f/w, 1.0f/h);
        loc = glGetUniformLocation(RF::prog_pixel, "uPixelSize");
        SAFE_UNIFORM(loc, glUniform1f, (float)RF::params.pixel_size);
        loc = glGetUniformLocation(RF::prog_pixel, "uPalette");
        SAFE_UNIFORM(loc, glUniform1i, RF::params.pixel_palette);
        loc = glGetUniformLocation(RF::prog_pixel, "uIntensity");
        SAFE_UNIFORM(loc, glUniform1f, RF::params.pixel_intensity);

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

    // MD3 Style - Material Design 3
    // Window & Background
    c[ImGuiCol_WindowBg] = MD3Style::BackgroundDim;
    c[ImGuiCol_ChildBg] = MD3Style::SurfaceContainer;
    c[ImGuiCol_PopupBg] = ImVec4(MD3Style::Surface.x, MD3Style::Surface.y, MD3Style::Surface.z, 0.98f);

    // Title Bar
    c[ImGuiCol_TitleBg] = MD3Style::Surface;
    c[ImGuiCol_TitleBgActive] = MD3Style::SurfaceContainerHigh;
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(MD3Style::Surface.x, MD3Style::Surface.y, MD3Style::Surface.z, 0.80f);

    // Frame Background
    c[ImGuiCol_FrameBg] = MD3Style::SurfaceContainerHigh;
    c[ImGuiCol_FrameBgHovered] = MD3Style::SurfaceContainerHighest;
    c[ImGuiCol_FrameBgActive] = MD3Style::PrimaryDark;

    // Buttons - MD3 Filled Button Style
    c[ImGuiCol_Button] = MD3Style::Primary;
    c[ImGuiCol_ButtonHovered] = MD3Style::PrimaryLight;
    c[ImGuiCol_ButtonActive] = MD3Style::PrimaryDark;

    // Slider
    c[ImGuiCol_SliderGrab] = MD3Style::Primary;
    c[ImGuiCol_SliderGrabActive] = MD3Style::PrimaryLight;

    // Checkbox
    c[ImGuiCol_CheckMark] = MD3Style::Primary;

    // Text
    c[ImGuiCol_Text] = MD3Style::OnSurface;
    c[ImGuiCol_TextDisabled] = MD3Style::OnSurfaceVariant;
    c[ImGuiCol_TextSelectedBg] = ImVec4(MD3Style::Primary.x, MD3Style::Primary.y, MD3Style::Primary.z, 0.30f);

    // Header (Collapsing headers, trees, etc.)
    c[ImGuiCol_Header] = ImVec4(MD3Style::Primary.x, MD3Style::Primary.y, MD3Style::Primary.z, 0.15f);
    c[ImGuiCol_HeaderHovered] = ImVec4(MD3Style::Primary.x, MD3Style::Primary.y, MD3Style::Primary.z, 0.25f);
    c[ImGuiCol_HeaderActive] = ImVec4(MD3Style::Primary.x, MD3Style::Primary.y, MD3Style::Primary.z, 0.40f);

    // Tabs
    c[ImGuiCol_Tab] = MD3Style::SurfaceContainerHigh;
    c[ImGuiCol_TabHovered] = MD3Style::Primary;
    c[ImGuiCol_TabActive] = MD3Style::PrimaryLight;
    c[ImGuiCol_TabUnfocused] = MD3Style::SurfaceContainer;
    c[ImGuiCol_TabUnfocusedActive] = MD3Style::SurfaceContainerHigh;

    // Separator
    c[ImGuiCol_Separator] = MD3Style::OutlineVariant;
    c[ImGuiCol_SeparatorHovered] = MD3Style::Primary;
    c[ImGuiCol_SeparatorActive] = MD3Style::PrimaryLight;

    // Border
    c[ImGuiCol_Border] = MD3Style::OutlineVariant;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg] = ImVec4(MD3Style::Surface.x, MD3Style::Surface.y, MD3Style::Surface.z, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = MD3Style::OutlineVariant;
    c[ImGuiCol_ScrollbarGrabHovered] = MD3Style::Outline;
    c[ImGuiCol_ScrollbarGrabActive] = MD3Style::Primary;

    // Table
    c[ImGuiCol_TableHeaderBg] = MD3Style::SurfaceContainer;
    c[ImGuiCol_TableBorderStrong] = MD3Style::OutlineVariant;
    c[ImGuiCol_TableBorderLight] = MD3Style::OutlineVariant;
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(MD3Style::Primary.x, MD3Style::Primary.y, MD3Style::Primary.z, 0.05f);

    // Navigation
    c[ImGuiCol_NavHighlight] = MD3Style::Primary;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

    // Modal Window Dim
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);

    // Resize Grip
    c[ImGuiCol_ResizeGrip] = ImVec4(MD3Style::Primary.x, MD3Style::Primary.y, MD3Style::Primary.z, 0.30f);
    c[ImGuiCol_ResizeGripHovered] = MD3Style::Primary;
    c[ImGuiCol_ResizeGripActive] = MD3Style::PrimaryLight;

    // Plot
    c[ImGuiCol_PlotLines] = MD3Style::OnSurfaceVariant;
    c[ImGuiCol_PlotLinesHovered] = MD3Style::Primary;
    c[ImGuiCol_PlotHistogram] = MD3Style::Primary;
    c[ImGuiCol_PlotHistogramHovered] = MD3Style::PrimaryLight;

    // Drag Drop
    c[ImGuiCol_DragDropTarget] = MD3Style::Secondary;

    // MD3 Style Settings
    s.WindowRounding = 16.0f;
    s.ChildRounding = 12.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 12.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 8.0f;
    s.TabRounding = 8.0f;
    s.LogSliderDeadzone = 4.0f;

    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(12, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.IndentSpacing = 24.0f;
    s.CellPadding = ImVec2(8, 6);

    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;

    s.ScrollbarSize = 16.0f;
    s.WindowMinSize = ImVec2(240, 240);

    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ColorButtonPosition = ImGuiDir_Right;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    s.AntiAliasedLines = true;
    s.AntiAliasedFill = true;
}

// ==========================================
// 6. UI Drawing
// ==========================================

// Draw Dynamic Island Style Button (Draggable)
static bool DrawDynamicIsland(bool* clicked) {
    ImGuiIO& io = ImGui::GetIO();
    
    float dt = io.DeltaTime;
    MD3Style::IslandWidth += (MD3Style::IslandTargetWidth - MD3Style::IslandWidth) * MD3Style::IslandAnimSpeed * dt;
    MD3Style::IslandHeight += (MD3Style::IslandTargetHeight - MD3Style::IslandHeight) * MD3Style::IslandAnimSpeed * dt;
    
    float width = MD3Style::IslandWidth;
    float height = MD3Style::IslandHeight;
    
    // Calculate position from normalized coords
    float centerX = MD3Style::IslandPos.x * io.DisplaySize.x;
    float centerY = MD3Style::IslandPos.y;
    
    ImVec2 min(centerX - width * 0.5f, centerY - height * 0.5f);
    ImVec2 max(centerX + width * 0.5f, centerY + height * 0.5f);
    
    bool hovered = (io.MousePos.x >= min.x && io.MousePos.x <= max.x && io.MousePos.y >= min.y && io.MousePos.y <= max.y);
    
    // Handle dragging
    if (MD3Style::IslandDragging) {
        if (io.MouseDown[0]) {
            // Update position while dragging
            float newCenterX = io.MousePos.x - MD3Style::IslandDragOffset.x;
            float newCenterY = io.MousePos.y - MD3Style::IslandDragOffset.y;
            
            // Clamp to screen bounds (manual clamp for older ImGui compatibility)
            float normX = newCenterX / io.DisplaySize.x;
            MD3Style::IslandPos.x = normX < 0.15f ? 0.15f : (normX > 0.85f ? 0.85f : normX);
            MD3Style::IslandPos.y = newCenterY < 30.0f ? 30.0f : (newCenterY > io.DisplaySize.y * 0.5f ? io.DisplaySize.y * 0.5f : newCenterY);
        } else {
            MD3Style::IslandDragging = false;
        }
    } else if (hovered && io.MouseDown[0] && !MD3Style::IslandDragging) {
        // Start dragging
        MD3Style::IslandDragging = true;
        MD3Style::IslandDragOffset = ImVec2(io.MousePos.x - centerX, io.MousePos.y - centerY);
    }
    
    if (hovered && !MD3Style::IslandDragging) {
        MD3Style::IslandHoverTime += dt;
    } else {
        MD3Style::IslandHoverTime = 0.0f;
    }
    
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    
    float rounding = height * 0.5f;
    
    // Draw shadow
    ImVec2 shadowOffset(0, 4);
    ImVec2 shadowMin(min.x + shadowOffset.x - 4, min.y + shadowOffset.y - 4);
    ImVec2 shadowMax(max.x + shadowOffset.x + 4, max.y + shadowOffset.y + 4);
    ImU32 shadowColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.25f));
    draw_list->AddRectFilled(shadowMin, shadowMax, shadowColor, rounding + 2);
    
    // Background color (use custom island color)
    ImU32 bgColor;
    if (MD3Style::IslandDragging) {
        bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            MD3Style::IslandBgColor.x * 0.8f,
            MD3Style::IslandBgColor.y * 0.8f,
            MD3Style::IslandBgColor.z * 0.8f,
            1.0f
        ));
        MD3Style::IslandTargetWidth = 100.0f;
        MD3Style::IslandTargetHeight = 40.0f;
    } else if (hovered) {
        bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            MD3Style::IslandBgColor.x * 1.1f,
            MD3Style::IslandBgColor.y * 1.1f,
            MD3Style::IslandBgColor.z * 1.1f,
            1.0f
        ));
        MD3Style::IslandTargetWidth = 140.0f;
        MD3Style::IslandTargetHeight = 42.0f;
    } else {
        bgColor = ImGui::ColorConvertFloat4ToU32(MD3Style::IslandBgColor);
        MD3Style::IslandTargetWidth = 120.0f;
        MD3Style::IslandTargetHeight = 36.0f;
    }
    
    draw_list->AddRectFilled(min, max, bgColor, rounding, ImDrawFlags_RoundCornersAll);
    
    // Glow effect
    if (hovered && MD3Style::IslandHoverTime > 0.1f && !MD3Style::IslandDragging) {
        float glowAlpha = 0.2f * sinf(MD3Style::IslandHoverTime * 3.0f);
        ImVec4 glowColor = ImVec4(
            MD3Style::IslandBgColor.x,
            MD3Style::IslandBgColor.y,
            MD3Style::IslandBgColor.z,
            glowAlpha
        );
        ImVec2 glowMin(min.x - 3, min.y - 3);
        ImVec2 glowMax(max.x + 3, max.y + 3);
        draw_list->AddRect(glowMin, glowMax, ImGui::ColorConvertFloat4ToU32(glowColor), rounding + 3, ImDrawFlags_RoundCornersAll, 2.0f);
    }
    
    // Draw text/icon
    ImVec2 textSize = ImGui::CalcTextSize("RF");
    ImVec2 textPos(centerX - textSize.x * 0.5f, centerY - textSize.y * 0.5f);
    
    // Text color based on background brightness
    float brightness = MD3Style::IslandBgColor.x * 0.299f + MD3Style::IslandBgColor.y * 0.587f + MD3Style::IslandBgColor.z * 0.114f;
    ImU32 textColor = brightness > 0.5f ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.1f, 0.1f, 0.1f, 1.0f)) : ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    draw_list->AddText(textPos, textColor, "RF");
    
    // Handle click (only if not dragging)
    if (hovered && io.MouseClicked[0] && !MD3Style::IslandDragging) {
        *clicked = true;
        return true;
    }
    
    return false;
}

static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);
    ImGuiIO& io = ImGui::GetIO();

    // ============================================
    // Dynamic Island Toggle Button
    // ============================================
    if (!g_ShowUI) {
        bool clicked = false;
        if (DrawDynamicIsland(&clicked)) {
            if (clicked) g_ShowUI = true;
        }
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // ============================================
    // Main Window - Modern Card-Based Design
    // ============================================
    ImGui::SetNextWindowSize(ImVec2(400, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::Begin("RenderFusion", &g_ShowUI, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // ============================================
    // Custom Title Bar
    // ============================================
    ImVec2 titleBarSize(ImGui::GetContentRegionAvail().x, 36);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    
    // Title text
    ImGui::SetCursorPosY(8);
    ImGui::PushStyleColor(ImGuiCol_Text, MD3Style::Primary);
    ImGui::Text("RenderFusion");
    ImGui::PopStyleColor();
    
    // Close button (X)
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24);
    ImGui::SetCursorPosY(4);
    ImGui::PushStyleColor(ImGuiCol_Text, MD3Style::OnSurfaceVariant);
    if (ImGui::Button("×", ImVec2(28, 28))) {
        g_ShowUI = false;
    }
    ImGui::PopStyleColor();
    
    ImGui::PopStyleColor(3);
    ImGui::SetCursorPosY(44);

    // ============================================
    // Preset Cards (Horizontal)
    // ============================================
    float cardWidth = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
    
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 10));
    
    // Original Preset Card
    ImGui::PushStyleColor(ImGuiCol_Button, RF::current_preset == 0 ? MD3Style::Primary : MD3Style::SurfaceContainerHigh);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, RF::current_preset == 0 ? MD3Style::PrimaryLight : MD3Style::SurfaceContainerHighest);
    if (ImGui::Button("Original", ImVec2(cardWidth, 44))) {
        RF::current_preset = 0;
        RF::ApplyPreset(0);
        Config::SaveConfig();
    }
    ImGui::PopStyleColor(2);
    
    ImGui::SameLine();
    
    // Manga Preset Card
    ImGui::PushStyleColor(ImGuiCol_Button, RF::current_preset == 1 ? MD3Style::Primary : MD3Style::SurfaceContainerHigh);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, RF::current_preset == 1 ? MD3Style::PrimaryLight : MD3Style::SurfaceContainerHighest);
    if (ImGui::Button("Manga B&W", ImVec2(cardWidth, 44))) {
        RF::current_preset = 1;
        RF::ApplyPreset(1);
        Config::SaveConfig();
    }
    ImGui::PopStyleColor(2);
    
    ImGui::PopStyleVar(2);
    ImGui::Spacing();

    // ============================================
    // Collapsible Sections (Cards)
    // ============================================
    
    // Helper function for section headers
    auto DrawSectionHeader = [](const char* label, bool* open) {
        ImGui::PushStyleColor(ImGuiCol_Header, MD3Style::SurfaceContainer);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MD3Style::SurfaceContainerHigh);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, MD3Style::SurfaceContainerHighest);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
        
        bool result = ImGui::CollapsingHeader(label, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        return result;
    };

    // ============================================
    // Basic Adjustments Section
    // ============================================
    static bool adjustOpen = true;
    if (DrawSectionHeader("Basic Adjustments", &adjustOpen)) {
        ImGui::Spacing();
        
        // Master Toggle
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        if (ImGui::Checkbox("Enable Master", &RF::params.enable_master)) {
            Config::SaveConfig();
        }
        ImGui::PopStyleVar();
        
        ImGui::Spacing();
        
        // Sliders with modern styling
        ImGui::PushItemWidth(-1);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
        
        struct SliderInfo { const char* label; float* value; float min; float max; const char* format; };
        SliderInfo sliders[] = {
            {"Brightness", &RF::params.brightness, -0.5f, 0.5f, "%.2f"},
            {"Contrast", &RF::params.contrast, 0.6f, 1.8f, "%.2f"},
            {"Saturation", &RF::params.saturation, 0.0f, 2.0f, "%.2f"},
            {"Temperature", &RF::params.temperature, -1.0f, 1.0f, "%.2f"},
            {"Vignette", &RF::params.vignette, 0.0f, 1.0f, "%.2f"}
        };
        
        for (auto& s : sliders) {
            ImGui::TextColored(MD3Style::OnSurfaceVariant, "%s", s.label);
            if (ImGui::SliderFloat(("##" + std::string(s.label)).c_str(), s.value, s.min, s.max, s.format)) {
                Config::SaveConfig();
            }
        }
        
        ImGui::PopStyleVar();
        ImGui::PopItemWidth();
    }
    
    ImGui::Spacing();

    // ============================================
    // Art Style Section
    // ============================================
    static bool styleOpen = false;
    if (DrawSectionHeader("Art Style", &styleOpen)) {
        ImGui::Spacing();
        
        const char* art_styles[] = {"Off", "Cel Anime", "Chinese Painting", "Sketch", "Anime Flat", "Comic", "Color Pencil"};
        ImGui::PushItemWidth(-1);
        if (ImGui::Combo("##ArtStyle", &RF::params.art_style, art_styles, IM_ARRAYSIZE(art_styles))) {
            if (RF::params.art_style > 0) RF::params.art_intensity = 1.0f;
            Config::SaveConfig();
        }
        ImGui::PopItemWidth();
        
        if (RF::params.art_style > 0) {
            ImGui::TextColored(MD3Style::OnSurfaceVariant, "Intensity");
            ImGui::PushItemWidth(-1);
            if (ImGui::SliderFloat("##ArtInt", &RF::params.art_intensity, 0.0f, 1.0f, "%.2f")) Config::SaveConfig();
            ImGui::PopItemWidth();
        }
        
        ImGui::Spacing();
        
        // Quick toggles
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        if (ImGui::Checkbox("Manga B&W", &RF::params.enable_bw)) Config::SaveConfig();
        if (ImGui::Checkbox("Vintage Sepia", &RF::params.enable_sepia)) Config::SaveConfig();
        if (ImGui::Checkbox("Black Outline", &RF::params.enable_outline)) Config::SaveConfig();
        ImGui::PopStyleVar();
    }
    
    ImGui::Spacing();

    // ============================================
    // Effects Section
    // ============================================
    static bool effectsOpen = false;
    if (DrawSectionHeader("Effects", &effectsOpen)) {
        ImGui::Spacing();
        
        // Season
        ImGui::TextColored(MD3Style::OnSurfaceVariant, "Season Filter");
        const char* seasons[] = {"Off", "Spring", "Summer", "Autumn", "Winter"};
        ImGui::PushItemWidth(-1);
        if (ImGui::Combo("##Season", &RF::params.season, seasons, IM_ARRAYSIZE(seasons))) {
            Config::SaveConfig();
        }
        ImGui::PopItemWidth();
        
        if (RF::params.season > 0) {
            ImGui::PushItemWidth(-1);
            if (ImGui::SliderFloat("##SeasonInt", &RF::params.season_intensity, 0.0f, 1.0f, "%.2f")) Config::SaveConfig();
            ImGui::PopItemWidth();
        }
        
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        if (ImGui::Checkbox("Sharpen", &RF::params.enable_sharpen)) Config::SaveConfig();
        if (ImGui::Checkbox("TikTok RGB Split", &RF::params.enable_tiktok)) Config::SaveConfig();
        if (ImGui::Checkbox("Pixel Art", &RF::params.enable_pixel)) Config::SaveConfig();
        ImGui::PopStyleVar();
    }
    
    ImGui::Spacing();

    // ============================================
    // Theme Section
    // ============================================
    static bool themeOpen = false;
    if (DrawSectionHeader("Theme Settings", &themeOpen)) {
        ImGui::Spacing();
        
        // Background Theme
        ImGui::TextColored(MD3Style::OnSurfaceVariant, "Background");
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0f);
        float themeBtnWidth = (ImGui::GetContentRegionAvail().x - 12) / 3.0f;
        
        const char* bg_themes[] = {"Dark", "Light", "AMOLED"};
        for (int i = 0; i < 3; i++) {
            bool selected = (MD3Style::CurrentBackgroundTheme == i);
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? MD3Style::Primary : MD3Style::SurfaceContainerHigh);
            if (ImGui::Button(bg_themes[i], ImVec2(themeBtnWidth, 28))) {
                MD3Style::ApplyBackgroundTheme(i);
                SetupStyle();
                Config::SaveConfig();
            }
            ImGui::PopStyleColor();
            if (i < 2) ImGui::SameLine();
        }
        ImGui::PopStyleVar();
        
        ImGui::Spacing();
        
        // Accent Colors
        ImGui::TextColored(MD3Style::OnSurfaceVariant, "Accent Color");
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        float colorBtnWidth = (ImGui::GetContentRegionAvail().x - 14) / 4.0f;
        
        const char* color_names[] = {"Red", "Purple", "Blue", "Green", "Orange", "Pink", "Cyan", "Moon"};
        for (int i = 0; i < 8; i++) {
            bool selected = (MD3Style::CurrentScheme == i);
            ImGui::PushStyleColor(ImGuiCol_Button, MD3Style::Schemes[i].primary);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, MD3Style::Schemes[i].primaryLight);
            
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Border, MD3Style::OnSurface);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            }
            
            if (ImGui::Button(color_names[i], ImVec2(colorBtnWidth, 26))) {
                MD3Style::ApplyScheme(i);
                SetupStyle();
                Config::SaveConfig();
            }
            
            if (selected) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleColor(2);
            
            if ((i + 1) % 4 != 0) ImGui::SameLine();
        }
        ImGui::PopStyleVar();
        
        ImGui::Spacing();
        
        // Island Color
        ImGui::TextColored(MD3Style::OnSurfaceVariant, "Island Color");
        ImGui::PushItemWidth(-1);
        if (ImGui::ColorEdit4("##IslandColor", (float*)&MD3Style::IslandBgColor, ImGuiColorEditFlags_NoInputs)) {
            Config::SaveConfig();
        }
        ImGui::PopItemWidth();
    }

    ImGui::End();
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

// ===================== PreloaderInput Touch Callback =====================
static bool OnTouchCallback(int action, int pointerId, float x, float y) {
    if (!g_Initialized) return false;
    
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        io.AddMouseButtonEvent(0, true);
    } else if (action == AMOTION_EVENT_ACTION_UP) {
        io.AddMouseButtonEvent(0, false);
    }
    
    return io.WantCaptureMouse;
}

static void RegisterPreloaderInputCallback() {
    void* lib = dlopen("libpreloader.so", RTLD_NOW);
    if (!lib) {
        LOGI("libpreloader.so not found, skipping PreloaderInput");
        return;
    }
    
    GetPreloaderInput_Fn getPreloaderInput = (GetPreloaderInput_Fn)dlsym(lib, "GetPreloaderInput");
    if (!getPreloaderInput) {
        LOGI("GetPreloaderInput symbol not found");
        dlclose(lib);
        return;
    }
    
    PreloaderInput_Interface* inputInterface = getPreloaderInput();
    if (inputInterface && inputInterface->RegisterTouchCallback) {
        inputInterface->RegisterTouchCallback(OnTouchCallback);
        LOGI("PreloaderInput touch callback registered");
    }
    
    dlclose(lib);
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
    
    // Register PreloaderInput touch callback
    RegisterPreloaderInputCallback();
    
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
