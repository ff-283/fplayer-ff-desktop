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
        "#2c2c2e",                    // surfaceTile2
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
        "#cc785c",                    // primary (warm coral, per DESIGN.md)
        "#a9583e",                    // primaryFocus (coral active)
        "#faf9f5",                    // canvas (warm cream, per DESIGN.md)
        "#f5f0e8",                    // canvasElevated (surface-soft)
        "#efe9de",                    // surfaceTile1 (surface-card)
        "#e8e0d2",                    // surfaceTile2 (surface-cream-strong)
        "#e6dfd8",                    // surfaceTile3 (hairline tone)
        "#d1d1d6",                    // surfaceBlack
        "#141413",                    // ink (warm near-black)
        "#6c6a64",                    // inkMuted
        "#8e8b82",                    // inkDisabled (muted-soft)
        "#e6dfd8",                    // hairline
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
inline QString globalStyleSheet(const ThemeColors& c, const QString& customPrimary = {}, const QString& customPrimaryFocus = {})
{
    const QString primary = customPrimary.isEmpty() ? QString::fromLatin1(c.primary) : customPrimary;
    const QString primaryFocus = customPrimaryFocus.isEmpty() ? QString::fromLatin1(c.primaryFocus) : customPrimaryFocus;
    QString qss = QStringLiteral(
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
        "QMenuBar{background:%8;color:%2;border:none;padding:4px 8px;font-size:12px;}"
        "QMenuBar::item{background:transparent;padding:6px 12px;border-radius:6px;}"
        "QMenuBar::item:selected{background:%4;}"
        // ── Menu ──
        "QMenu{background:%8;color:%3;border:1px solid %4;border-radius:8px;padding:4px;}"
        "QMenu::item{padding:6px 24px 6px 12px;border-radius:6px;}"
        "QMenu::item:selected{background:%6;color:#ffffff;}"
        "QMenu::separator{height:1px;background:%4;margin:4px 8px;}"
        // ── PushButton ──
        "QPushButton{background:transparent;border:none;color:%3;border-radius:%9px;padding:6px 14px;}"
        "QPushButton:hover{background:%13;color:%3;}"
        "QPushButton:pressed{background:%13;color:%3;}"
        "QPushButton:disabled{color:%12;}"
        "QPushButton[role=\"primary\"]{background:%6;border:none;color:#ffffff;font-weight:600;border-radius:%9px;padding:6px 16px;}"
        "QPushButton[role=\"primary\"]:hover{background:%11;}"
        "QPushButton[role=\"primary\"]:pressed{background:%6;}"
        "QPushButton[role=\"primary\"]:disabled{background:%4;color:%12;}"
        // ── ToolButton ──
        "QToolButton{background:transparent;border:none;color:%3;border-radius:%9px;padding:4px 8px;}"
        "QToolButton:hover{background:%13;color:%3;}"
        // ── LineEdit / TextEdit ──
        "QLineEdit,QTextEdit{"
        "background:transparent;border:none;border-radius:%9px;color:%3;padding:6px 10px;}"
        "QLineEdit:hover,QTextEdit:hover{background:%13;}"
        "QLineEdit:focus,QTextEdit:focus{background:%13;}"
        "QLineEdit::placeholder{color:%12;}"
        // ── ComboBox ──
        "QComboBox{background:transparent;border:none;border-radius:%9px;color:%3;padding:6px 28px 6px 10px;}"
        "QComboBox:hover{background:%13;}"
        "QComboBox:focus{background:%13;}"
        "QComboBox:disabled{color:%12;}"
        "QComboBox::drop-down{background:transparent;border:none;width:20px;subcontrol-position:center right;subcontrol-origin:padding;border-top-right-radius:%9px;border-bottom-right-radius:%9px;}"
        "QComboBox::drop-down:hover,QComboBox::drop-down:focus{background:transparent;}"
        "QComboBox::down-arrow{image:url(:/icon/chevron-down.svg);width:12px;height:8px;}"
        "QComboBox::down-arrow:disabled{image:url(:/icon/chevron-down.svg);}"
        "QComboBox QAbstractItemView{"
        "background:%8;color:%3;border:none;border-radius:%9px;padding:4px;outline:none;}"
        "QComboBox QAbstractItemView::item{min-height:28px;padding:5px 12px;border-radius:6px;}"
        "QComboBox QAbstractItemView::item:hover{background:%13;color:%3;}"
        "QComboBox QAbstractItemView::item:selected{background:%6;color:#ffffff;}"
        // ── SpinBox ──
        "QSpinBox,QDoubleSpinBox{background:transparent;border:none;border-radius:%9px;color:%3;padding:6px 10px;}"
        "QSpinBox:hover,QDoubleSpinBox:hover,QSpinBox:focus,QDoubleSpinBox:focus{background:%13;}"
        "QSpinBox:disabled,QDoubleSpinBox:disabled{color:%12;}"
        "QSpinBox::up-button,QDoubleSpinBox::up-button{background:transparent;border:none;width:20px;subcontrol-position:top right;subcontrol-origin:padding;border-top-right-radius:%9px;}"
        "QSpinBox::up-button:hover,QDoubleSpinBox::up-button:hover{background:%13;}"
        "QSpinBox::up-arrow,QDoubleSpinBox::up-arrow{image:url(:/icon/chevron-up.svg);width:10px;height:6px;}"
        "QSpinBox::down-button,QDoubleSpinBox::down-button{background:transparent;border:none;width:20px;subcontrol-position:bottom right;subcontrol-origin:padding;border-bottom-right-radius:%9px;}"
        "QSpinBox::down-button:hover,QDoubleSpinBox::down-button:hover{background:%13;}"
        "QSpinBox::down-arrow,QDoubleSpinBox::down-arrow{image:url(:/icon/chevron-down.svg);width:10px;height:6px;}"
        // ── ScrollBar ──
        "QScrollBar:vertical{background:%5;width:8px;border-radius:4px;margin:0;}"
        "QScrollBar::handle:vertical{background:%10;border-radius:4px;min-height:30px;}"
        "QScrollBar::handle:vertical:hover{background:%6;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar:horizontal{background:%5;height:8px;border-radius:4px;margin:0;}"
        "QScrollBar::handle:horizontal{background:%10;border-radius:4px;min-width:30px;}"
        "QScrollBar::handle:horizontal:hover{background:%6;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}"
        // ── Slider ──
        "QSlider::groove:horizontal{background:%4;height:4px;border-radius:2px;}"
        "QSlider::handle:horizontal{background:%10;border:1px solid %6;width:14px;margin:-5px 0;border-radius:7px;}"
        "QSlider::handle:horizontal:hover{background:%6;}"
        // ── ListWidget ──
        "QListWidget{background:%5;border:1px solid %4;border-radius:%9px;color:%3;padding:4px;outline:none;}"
        "QListWidget::item{padding:6px 10px;border-radius:6px;}"
        "QListWidget::item:hover{background:%5;}"
        "QListWidget::item:selected{background:%6;color:#ffffff;}"
        // ── Toolbar / Panels ──
        "#wgtDown{background:%8;border:none;}"
        "#imagePoolToolbar{background:%8;}"
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
    );
    // NOTE: replace in descending order — otherwise %1 matches inside %10/%11/%12/%13
    qss.replace(QStringLiteral("%13"), QString::fromLatin1(c.surfaceTile2));
    qss.replace(QStringLiteral("%12"), QString::fromLatin1(c.inkDisabled));
    qss.replace(QStringLiteral("%11"), primaryFocus);
    qss.replace(QStringLiteral("%10"), QString::fromLatin1(c.inkMuted));
    qss.replace(QStringLiteral("%9"),  QString::number(radius::md));
    qss.replace(QStringLiteral("%8"),  QString::fromLatin1(c.surfaceTile1));
    qss.replace(QStringLiteral("%7"),  QString::fromLatin1(c.surfaceBlack));
    qss.replace(QStringLiteral("%6"),  primary);
    qss.replace(QStringLiteral("%5"),  QString::fromLatin1(c.canvasElevated));
    qss.replace(QStringLiteral("%4"),  QString::fromLatin1(c.hairline));
    qss.replace(QStringLiteral("%3"),  QString::fromLatin1(c.ink));
    qss.replace(QStringLiteral("%2"),  QString::fromLatin1(c.ink));
    qss.replace(QStringLiteral("%1"),  QString::fromLatin1(c.canvas));
    return qss;
}

inline QString globalStyleSheet(Theme theme, const QString& customPrimary = {}, const QString& customPrimaryFocus = {})
{
    return globalStyleSheet(colorsForTheme(theme), customPrimary, customPrimaryFocus);
}

inline QString themedIconPath(Theme theme, const QString& name)
{
    return QStringLiteral(":/icon/%1-%2.svg")
        .arg(name, theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
}

} // namespace fplayer::tokens

#endif // FPLAYER_COMMON_DESIGNTOKENS_H
