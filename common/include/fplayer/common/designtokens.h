#ifndef FPLAYER_COMMON_DESIGNTOKENS_H
#define FPLAYER_COMMON_DESIGNTOKENS_H

#include <QString>

namespace fplayer::tokens {

// ── Theme ────────────────────────────────────────────
enum class Theme { Dark = 0, Light = 1 };

// ── Color tokens (per-theme) ─────────────────────────
struct ThemeColors {
    const char* primary;
    const char* primaryFocus;
    const char* canvas;
    const char* canvasElevated;
    const char* surfaceTile1;
    const char* surfaceTile2;
    const char* surfaceTile3;
    const char* surfaceBlack;
    const char* ink;
    const char* inkMuted;
    const char* inkDisabled;
    const char* hairline;
    const char* hairlineSoft;
    const char* systemOrange;
    const char* errorRed;
};

inline ThemeColors darkColors()
{
    return {
        "#2997ff",                    // primary
        "#47a8ff",                    // primaryFocus
        "#0d0d0f",                    // canvas
        "#141416",                    // canvasElevated
        "#1a1a1c",                    // surfaceTile1
        "#1c1c1e",                    // surfaceTile2
        "#1e1e20",                    // surfaceTile3
        "#000000",                    // surfaceBlack
        "#f5f5f7",                    // ink
        "#a1a1a6",                    // inkMuted
        "#6e6e73",                    // inkDisabled
        "#2a2a2c",                    // hairline
        "rgba(255,255,255,0.08)",    // hairlineSoft
        "#ff9f0a",                    // systemOrange
        "#ff453a",                    // errorRed
    };
}

inline ThemeColors lightColors()
{
    return {
        "#0066cc",                    // primary (Action Blue)
        "#0071e3",                    // primaryFocus
        "#ffffff",                    // canvas
        "#f5f5f7",                    // canvasElevated (parchment)
        "#fafafc",                    // surfaceTile1 (pearl)
        "#f0f0f0",                    // surfaceTile2
        "#e8e8ea",                    // surfaceTile3
        "#d1d1d6",                    // surfaceBlack
        "#1d1d1f",                    // ink (near-black)
        "#6e6e73",                    // inkMuted
        "#a1a1a6",                    // inkDisabled
        "#e0e0e0",                    // hairline
        "rgba(0,0,0,0.08)",          // hairlineSoft
        "#ff9f0a",                    // systemOrange
        "#ff453a",                    // errorRed
    };
}

inline ThemeColors colorsForTheme(Theme theme)
{
    return theme == Theme::Light ? lightColors() : darkColors();
}

// ── Border Radius ─────────────────────────────────────
namespace radius {
inline constexpr int none = 0;
inline constexpr int xs   = 4;
inline constexpr int sm   = 6;
inline constexpr int md   = 8;
inline constexpr int lg   = 12;
inline constexpr int xl   = 18;
inline constexpr int pill = 18;
inline constexpr int full = 9999;
} // namespace radius

// ── Spacing ───────────────────────────────────────────
namespace spacing {
inline constexpr int xxs     = 4;
inline constexpr int xs      = 8;
inline constexpr int sm      = 12;
inline constexpr int md      = 16;
inline constexpr int lg      = 24;
inline constexpr int xl      = 32;
inline constexpr int xxl     = 48;
inline constexpr int section = 80;
} // namespace spacing

// ── Font Sizes ────────────────────────────────────────
namespace fontSize {
inline constexpr int micro      = 10;
inline constexpr int caption    = 12;
inline constexpr int body       = 13;
inline constexpr int bodyStrong = 14;
inline constexpr int display    = 17;
} // namespace fontSize

// ── Global Style Sheet ────────────────────────────────
inline QString globalStyleSheet(const ThemeColors& c)
{
    return QStringLiteral(
        // ── Root / Dialogs ──
        "QWidget#CaptureWindow{background:%1;color:%2;}"
        "QDialog{background:%1;color:%3;}"
        // ── Labels ──
        "QLabel{color:%3;}"
        // ── CheckBox ──
        "QCheckBox{color:%3;spacing:6px;}"
        "QCheckBox::indicator{width:16px;height:16px;}"
        "QCheckBox::indicator:unchecked{border:1px solid %4;background:%5;border-radius:4px;}"
        "QCheckBox::indicator:checked{border:1px solid %6;background:%6;border-radius:4px;}"
        // ── MenuBar ──
        "QMenuBar{background:%7;color:%2;border:none;padding:4px 8px;font-size:12px;}"
        "QMenuBar::item{background:transparent;padding:6px 12px;border-radius:6px;}"
        "QMenuBar::item:selected{background:%4;}"
        // ── Menu ──
        "QMenu{background:%8;color:%3;border:1px solid %4;border-radius:8px;padding:4px;}"
        "QMenu::item{padding:6px 24px 6px 12px;border-radius:6px;}"
        "QMenu::item:selected{background:%6;color:#ffffff;}"
        "QMenu::separator{height:1px;background:%4;margin:4px 8px;}"
        // ── PushButton ──
        "QPushButton{background:transparent;border:1px solid %4;color:%3;border-radius:%9px;padding:5px 12px;}"
        "QPushButton:hover{background:%5;border-color:%10;}"
        "QPushButton:pressed{background:%8;border-color:%10;}"
        "QPushButton[role=\"primary\"]{background:%6;border:1px solid %6;color:#ffffff;font-weight:600;border-radius:%9px;padding:6px 16px;}"
        "QPushButton[role=\"primary\"]:hover{background:%11;border-color:%11;}"
        "QPushButton[role=\"primary\"]:pressed{background:%6;border-color:%6;}"
        "QPushButton[role=\"primary\"]:disabled{background:%4;border-color:%4;color:%12;}"
        // ── ToolButton ──
        "QToolButton{color:%3;border-radius:6px;padding:3px 8px;}"
        "QToolButton:hover{background:%5;}"
        // ── ComboBox / LineEdit / SpinBox / ListWidget / TextEdit ──
        "QComboBox,QLineEdit,QSpinBox,QAbstractSpinBox,QListWidget,QTextEdit{"
        "background:%5;border:1px solid %4;border-radius:%13px;color:%3;padding:4px 6px;}"
        "QLineEdit::placeholder{color:%12;}"
        "QComboBox:disabled,QLineEdit:disabled,QSpinBox:disabled,QAbstractSpinBox:disabled,QTextEdit:disabled,QPushButton:disabled{color:%12;}"
        "QComboBox:focus,QLineEdit:focus,QSpinBox:focus,QAbstractSpinBox:focus,QTextEdit:focus{background:%8;border-color:%10;}"
        "QComboBox::drop-down{border-left:1px solid %4;width:22px;}"
        "QComboBox::down-arrow{image:none;width:0;height:0;border-left:5px solid transparent;border-right:5px solid transparent;border-top:6px solid %10;}"
        "QComboBox QAbstractItemView{background:%8;color:%3;border:1px solid %4;selection-background-color:%6;selection-color:#ffffff;}"
        // ── Slider ──
        "QSlider::groove:horizontal{background:%4;height:4px;border-radius:2px;}"
        "QSlider::handle:horizontal{background:%10;border:1px solid %6;width:14px;margin:-5px 0;border-radius:7px;}"
        "QSlider::handle:horizontal:hover{background:%6;}"
        // ── ListWidget ──
        "QListWidget::item{padding:6px;border-radius:6px;}"
        "QListWidget::item:hover{background:%5;}"
        "QListWidget::item:selected{background:%6;color:#ffffff;}"
        // ── Toolbar / Panels ──
        "#wgtDown{background:%8;border:none;}"
        "#wgtOperate{background:transparent;}"
        "#wgtDevices{background:transparent;}"
        // ── Compose Mode ──
        "#composeLeftPanel{background:%5;border:none;border-radius:8px;}"
        "#composePreviewHost{background:%1;border:1px solid %4;border-radius:8px;}"
        "#composeMdiArea{border:none;background:%7;}"
        "#composeSourceItem{background:%8;border-radius:6px;}"
        "#composeSourceItem[composeState=\"normal\"]{border:1px solid %4;}"
        "#composeSourceItem[composeState=\"selected\"]{border:2px solid #b388ff;}"
        "#composeSourceItem[composeState=\"crop\"]{border:2px solid #ffd166;}"
    )
        .arg(
            c.canvas,                              // %1  root bg
            c.ink,                                 // %2  root text
            c.ink,                                 // %3  label / general text
            c.hairline,                            // %4  borders
            c.canvasElevated,                      // %5  elevated bg
            c.primary,                             // %6  accent
            c.surfaceBlack,                        // %7  menubar bg
            c.surfaceTile1,                        // %8  menu/sunken bg
            QString::number(radius::pill),         // %9  button radius
            c.inkMuted,                            // %10 muted / hover accent
            c.primaryFocus,                        // %11 primary hover
            c.inkDisabled,                         // %12 disabled text
            QString::number(radius::sm)            // %13 input radius
        );
}

inline QString globalStyleSheet(Theme theme)
{
    return globalStyleSheet(colorsForTheme(theme));
}

} // namespace fplayer::tokens

#endif // FPLAYER_COMMON_DESIGNTOKENS_H
