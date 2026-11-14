// mainwindow.cpp — Qt main window + startup splash screen.
#include "mainwindow.h"
#include "chord_engine.hpp"
#include "player.hpp"


// ======================================================================
//   Splash screen
// ======================================================================

#include <QFile>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QLabel>
#include <QPixmap>
#include <QSettings>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSequentialAnimationGroup>
#include <QTimer>
#include <QVBoxLayout>

SplashScreen::SplashScreen(const QString& imageResourcePath,
                          int fadeInMs, int holdMs, int fadeOutMs,
                          QWidget* parent)
    : QWidget(parent),
      fade_in_ms_(fadeInMs), hold_ms_(holdMs), fade_out_ms_(fadeOutMs) {
    // Frameless, always-on-top, no taskbar entry, and sized to cover the
    // ENTIRE screen with solid black -- this is what stops the full-screen
    // MainWindow underneath from showing through anywhere.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::SplashScreen);
    setStyleSheet(QStringLiteral("background-color: black;"));

    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        setGeometry(screen->geometry());
    } else {
        resize(1920, 1080);
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    image_label_ = new QLabel(this);
    image_label_->setAlignment(Qt::AlignCenter);
    image_label_->setStyleSheet(QStringLiteral("background-color: transparent;"));
    layout->addWidget(image_label_, 1);

    const QPixmap pix(imageResourcePath);
    if (!pix.isNull()) {
        // Scale the logo to a reasonable fraction of the screen, preserving
        // aspect ratio, rather than showing it at native pixel size (which
        // may be far too small or too large depending on the source image
        // and the display).
        const QSize screenSize = this->size();
        const QSize maxLogoSize(screenSize.width() * 3 / 5, screenSize.height() * 3 / 5);
        image_label_->setPixmap(pix.scaled(maxLogoSize, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation));
    }

    // ----- Logo fade-IN (child-widget effect -- correct for a QLabel) -----
    logo_effect_ = new QGraphicsOpacityEffect(image_label_);
    logo_effect_->setOpacity(0.0);
    image_label_->setGraphicsEffect(logo_effect_);

    // Splash window itself starts fully opaque (solid black covering
    // everything); only the logo fades in on top of it at first.
    setWindowOpacity(1.0);

    fade_in_anim_ = new QPropertyAnimation(logo_effect_, "opacity", this);
    fade_in_anim_->setDuration(fade_in_ms_);
    fade_in_anim_->setStartValue(0.0);
    fade_in_anim_->setEndValue(1.0);
    fade_in_anim_->setEasingCurve(QEasingCurve::InOutCubic);
    // NOT started here: heavy construction/startup work runs right after
    // the splash appears and would stall the wall-clock animation
    // mid-fade (or skip it entirely). start_fade_in() is called by
    // main() from a clean event-loop tick instead.

    // The hold is armed when the fade-in completes; the fade-out starts
    // once BOTH the hold has elapsed AND the main window is ready. The
    // fade-in completion also tells the main window to begin its heavy
    // loading — but DEFERRED by one event-loop tick in main_qt.cpp, so
    // the signal emission (and the animation that sent it) fully
    // unwinds before anything heavy or nested-event-loopy runs.
    connect(fade_in_anim_, &QAbstractAnimation::finished, this, [this]() {
        phase_ = Phase::Holding;
        emit fade_in_finished();
        QTimer::singleShot(hold_ms_, this, [this]() {
            hold_done_ = true;
            maybe_start_fade_out();
        });
    });

    // ----- Splash window fade-OUT (top-level windowOpacity -- correct
    // for the window itself, NOT a graphics effect; see header note) -----
    // NOT started here: the fade-out is gated by notify_ready() (main
    // window finished loading) AND the minimum hold time.
    fade_out_anim_ = new QPropertyAnimation(this, "windowOpacity", this);
    fade_out_anim_->setDuration(fade_out_ms_);
    fade_out_anim_->setStartValue(1.0);
    fade_out_anim_->setEndValue(0.0);
    fade_out_anim_->setEasingCurve(QEasingCurve::OutCubic);

    // The moment the fade-out begins, hand the screen to the main window
    // (via the revealUnderneath signal). The receiver raises, activates
    // AND FORCE-PAINTS the main window NOW — while the still-top-most
    // splash covers the screen — so the opacity fade reveals an
    // already-rendered window. Without the forced paint the window may
    // never have been composited (it was fully covered since birth),
    // and the fade shows the desktop/garbage before the window pops in.
    connect(fade_out_anim_, &QPropertyAnimation::stateChanged, this,
        [this](QAbstractAnimation::State newState, QAbstractAnimation::State) {
            if (newState == QAbstractAnimation::Running && phase_ == Phase::FadingOut) {
                emit revealUnderneath();
            }
        });

    connect(fade_out_anim_, &QAbstractAnimation::finished, this, [this]() {
        phase_ = Phase::Done;
        emit finished();
    });
}

// =========================================================
//  start_fade_in / notify_ready / maybe_start_fade_out /
//  show_message
//
//  Phase-guarded lifecycle: the fade-out starts only after BOTH gates
//  — (1) the fade-in played and the minimum hold elapsed, and (2) the
//  main window signaled startup_ready. A slow load EXTENDS the hold;
//  it never cuts it short. All transitions check phase_, so calls from
//  nested event loops or out-of-order signals are harmless no-ops.
// =========================================================
void SplashScreen::start_fade_in() {
    if (phase_ != Phase::BeforeFadeIn || !fade_in_anim_) return;
    phase_ = Phase::FadingIn;
    fade_in_anim_->start(QAbstractAnimation::KeepWhenStopped);
}

void SplashScreen::notify_ready() {
    ready_ = true;
    maybe_start_fade_out();
}

void SplashScreen::maybe_start_fade_out() {
    if (phase_ != Phase::Holding || !ready_ || !hold_done_) return;

    phase_ = Phase::FadingOut;
    fade_out_anim_->start(QAbstractAnimation::KeepWhenStopped);
}

void SplashScreen::show_message(const QString& text) {
    if (text.isEmpty()) {
        if (message_label_) message_label_->hide();
        return;
    }
    if (!message_label_) {
        message_label_ = new QLabel(this);
        message_label_->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        message_label_->setStyleSheet(QStringLiteral(
            "background-color: transparent; color: #9a9a9a; font-size: 16px;"));
    }
    message_label_->setText(text);
    message_label_->adjustSize();
    // Float it near the bottom WITHOUT touching the layout — putting it
    // in the layout would push the logo up by the text height every time
    // a message appears or changes.
    message_label_->move((width() - message_label_->width()) / 2,
                         height() - message_label_->height() - 36);
    message_label_->show();
    message_label_->raise();
    // Paint synchronously: callers often start a blocking load right
    // after this call, and the message must be on screen before that.
    repaint();
}


// ======================================================================
//   Main window
// ======================================================================

// Qt main window for the PSRstudio style player.
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QComboBox>
#include <QDial>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMenu>
#include <QMouseEvent>
#include <QListWidget>
#include <QScrollArea>
#include <QFontMetrics>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QEventLoop>

#include <QHash>

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

namespace {

// Minimum time (ms) each startup status line displays on the splash
// before the next queued line may appear (see showSplashStatus).
constexpr qint64 kSplashMinDisplayMs = 1000;

// =========================================================
//  mixerArt — pre-rendered mixer control art, cached
//
//  Loads <name>.png for the mixer knobs/sliders, scaled to `px`
//  logical pixels at the current device pixel ratio. Looked up FIRST
//  in <Documents>/PSRstudio/mixer/ — so art can be hot-swapped by
//  dropping a new file there and restarting, no rebuild — then in the
//  compiled-in resources (resources/mixer/, see resources.qrc).
//  Results (including "absent") are cached per (name, size, ratio).
//
//  Asset conventions:
//    knob.png          square, transparent bg, front view, NO pointer
//                      baked in — the app draws the indicator on top.
//    slider_track.png  narrow vertical strip; stretched to the groove,
//                      dimmed above the handle, bright below it.
//    slider_handle.png the handle, drawn at the style's handle rect.
//
//  A null return means no asset exists — callers fall back to
//  procedural painting, so the app works with or without the art.
// =========================================================
QPixmap mixerArt(const QString& name, const QSizeF& px, qreal dpr) {
    static QHash<QString, QPixmap> cache;

    const QString key = QStringLiteral("%1_%2x%3_%4")
                            .arg(name, QString::number(int(px.width())),
                                 QString::number(int(px.height())),
                                 QString::number(int(dpr * 4)));
    const auto it = cache.find(key);
    if (it != cache.end()) return *it;

    QPixmap master;
    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString dirs[] = {
        docs + QStringLiteral("/PSRstudio/mixer"),
        QStringLiteral(":/resources/mixer"),
    };
    for (const QString& dir : dirs) {
        const QString path = dir + QLatin1Char('/') + name + QStringLiteral(".png");
        if (QFileInfo::exists(path) && master.load(path)) break;
    }

    QPixmap result;
    if (!master.isNull()) {
        result = master.scaled(int(px.width() * dpr), int(px.height() * dpr),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        result.setDevicePixelRatio(dpr);
    }
    cache.insert(key, result);
    return result;
}

// Convert a std::filesystem::path to a QString, preserving UTF-8.
// We can't use QString::fromUtf8(path.u8string()) directly because
// path.u8string() returns std::u8string (std::basic_string<char8_t>),
// and Qt's fromUtf8 overloads don't accept that on MSVC. So we
// reinterpret the char8_t bytes as char and pass them through.
inline QString pathToQString(const std::filesystem::path& p) {
    const auto u8s = p.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char*>(u8s.data()),
        static_cast<qsizetype>(u8s.size()));
}

// =========================================================
//  VerticalLabel — a QLabel-like widget that draws its text
//  rotated 90° clockwise (reading top-to-bottom).
//
//  Used for channel labels alongside the vertical volume slider
//  in the Mixer window. The label sits to the LEFT of the slider,
//  both stretch vertically, and the channel number + part name
//  reads downward alongside the slider.
// =========================================================
class VerticalLabel : public QWidget {
public:
    explicit VerticalLabel(const QString& text = QString(), QWidget* parent = nullptr)
        : QWidget(parent), text_(text) {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::MinimumExpanding);
    }

    void setText(const QString& t) {
        if (text_ == t) return;
        text_ = t;
        updateGeometry();
        update();
    }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics fm(font());
        const int text_w = fm.horizontalAdvance(text_) + 4;
        const int font_h = fm.height() + 4;
        // Rotated: width is the font height, height is the text width.
        return QSize(font_h, text_w);
    }

    [[nodiscard]] QSize minimumSizeHint() const override {
        const QFontMetrics fm(font());
        const int font_h = fm.height() + 4;
        return QSize(font_h, fm.height() + 4);  // any height ≥ font height is fine
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.save();
        p.setPen(QColor(0x8b, 0x94, 0x9e));
        QFont f = font();
        f.setPointSize(8);
        p.setFont(f);
        p.translate(width(), 0);
        p.rotate(90);
        p.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text_);
        p.restore();
    }

private:
    QString text_;
};

// =========================================================
//  MixerDial — a lean QDial for the mixer strips
//
//  The stock QDial, when matched by a stylesheet rule, paints through
//  the stylesheet engine on every frame — with 48 dials in the mixer
//  that made the window take ~0.5 s to open. This subclass draws
//  itself with a handful of QPainter primitives instead (same dark
//  panel + blue-accent look as before), and the mixer stylesheet no
//  longer contains a QDial rule, so instances never route through
//  QStyleSheetStyle. Slider semantics (range, signals, dragging) are
//  inherited unchanged from QDial.
// =========================================================
class MixerDial : public QDial {
public:
    explicit MixerDial(QWidget* parent = nullptr) : QDial(parent) {
        setRange(0, 127);
        setNotchesVisible(false);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int side = std::min(width(), height());

        // Pre-rendered art (when present): one cached blit for the knob
        // body, then only the dynamic pointer on top. Falls back to
        // procedural painting when no asset exists.
        const QPixmap base = mixerArt(QStringLiteral("knob"),
                                      QSizeF(side, side), devicePixelRatioF());
        if (!base.isNull()) {
            p.drawPixmap(0, 0, base);

            constexpr double kStartDeg = 225.0;   // QDial convention: min at
            constexpr double kSweepDeg = 270.0;   // bottom-left, 270° sweep
            const double frac = (maximum() > minimum())
                ? double(value() - minimum()) / double(maximum() - minimum())
                : 0.0;
            const double rad = qDegreesToRadians(kStartDeg - kSweepDeg * frac);
            const QPointF c = rect().center();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(0xc9, 0xd1, 0xd9), std::max(2, side / 22),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(c + QPointF(std::cos(rad) * side * 0.20, -std::sin(rad) * side * 0.20),
                       c + QPointF(std::cos(rad) * side * 0.38, -std::sin(rad) * side * 0.38));
            return;
        }

        p.setRenderHint(QPainter::Antialiasing);

        // Dark rounded panel.
        p.setPen(QPen(QColor(0x30, 0x36, 0x3d), 1));
        p.setBrush(QColor(0x16, 0x1b, 0x22));
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 4, 4);

        // Track arc: QDial convention — 270° sweep, minimum at 225°
        // (bottom-left), clockwise to bottom-right.
        const QRectF arc_rect = QRectF(rect()).adjusted(
            side * 0.18, side * 0.18, -side * 0.18, -side * 0.18);
        constexpr double kStartDeg = 225.0;
        constexpr double kSweepDeg = 270.0;
        const double frac = (maximum() > minimum())
            ? double(value() - minimum()) / double(maximum() - minimum())
            : 0.0;

        const int pen_w = std::max(3, side / 12);
        p.setPen(QPen(QColor(0x30, 0x36, 0x3d), pen_w, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(arc_rect, int(kStartDeg * 16), int(-kSweepDeg * 16));

        if (frac > 0.0) {
            p.setPen(QPen(QColor(0x58, 0xa6, 0xff), pen_w, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_rect, int(kStartDeg * 16), int(-kSweepDeg * frac * 16));
        }

        // Pointer notch at the current angle (clockwise from start).
        const double rad = qDegreesToRadians(kStartDeg - kSweepDeg * frac);
        const QPointF c = rect().center();
        const QPen ptr_pen(QColor(0xc9, 0xd1, 0xd9), std::max(2, side / 22));
        p.setPen(ptr_pen);
        p.drawLine(c + QPointF(std::cos(rad) * side * 0.22, -std::sin(rad) * side * 0.22),
                   c + QPointF(std::cos(rad) * side * 0.36, -std::sin(rad) * side * 0.36));
    }
};

// =========================================================
//  MixerSlider — a lean QSlider for the mixer strips
//
//  Same reasoning as MixerDial: the stylesheet-drawn vertical sliders
//  (gradient handle, groove, sub-page rules) paint through the
//  stylesheet engine on every frame. This subclass draws the same
//  look with plain QPainter primitives, using the style's own groove/
//  handle geometry so hit areas and positions stay exactly what
//  QSlider would compute.
// =========================================================
class MixerSlider : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);

        // Groove/handle geometry from the style itself — positions and
        // interaction stay identical to the stock widget.
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
        const QRect groove = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

        // Pre-rendered art (when present): dim track over the full
        // range, bright over the filled part, then the handle image.
        const qreal dpr = devicePixelRatioF();
        const QPixmap track_img = mixerArt(QStringLiteral("slider_track"),
                                           QSizeF(trackW(), groove.height()), dpr);
        const QPixmap handle_img = mixerArt(QStringLiteral("slider_handle"),
                                            QSizeF(handle.width(), handle.height()), dpr);
        if (!track_img.isNull() && !handle_img.isNull()) {
            // Full track, dimmed...
            QRectF track(groove.center().x() - trackW() / 2.0, groove.top(),
                         trackW(), groove.height());
            p.setOpacity(0.35);
            p.drawPixmap(track.topLeft(), track_img);

            // ...bright from the handle down (value increases upwards).
            const qreal fill_top = std::max<qreal>(handle.center().y(), track.top());
            if (fill_top < track.bottom()) {
                const qreal t0 = (fill_top - track.top()) / track.height();
                p.setOpacity(1.0);
                p.drawPixmap(QRectF(track.left(), fill_top, track.width(),
                                    track.bottom() - fill_top).toRect(),
                             track_img,
                             QRectF(0, t0 * track_img.height(), track_img.width(),
                                    track_img.height() - t0 * track_img.height()).toRect());
            }

            p.setOpacity(1.0);
            p.drawPixmap(handle.topLeft(), handle_img);
            return;
        }

        // ---- Procedural fallback (no art assets) ----
        p.setRenderHint(QPainter::Antialiasing);

        // 6 px rounded groove column, centred on the slider.
        QRectF track(groove.center().x() - 3.0, groove.top(), 6.0, groove.height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x21, 0x26, 0x2d));
        p.drawRoundedRect(track, 3, 3);

        // Filled (sub-page) section below the handle: value increases
        // upwards on a vertical slider, so fill from the handle down.
        const qreal fill_top = std::max<qreal>(handle.center().y(), track.top());
        if (fill_top < track.bottom()) {
            p.setBrush(QColor(0x1f, 0x6f, 0xeb));
            p.drawRoundedRect(
                QRectF(track.left(), fill_top, track.width(),
                       track.bottom() - fill_top), 3, 3);
        }

        // Handle: blue gradient pill, lighter on hover.
        QLinearGradient g(handle.topLeft(), handle.topRight());
        if (underMouse()) {
            g.setColorAt(0, QColor(0x79, 0xb8, 0xff));
            g.setColorAt(1, QColor(0x38, 0x8b, 0xfd));
        } else {
            g.setColorAt(0, QColor(0x58, 0xa6, 0xff));
            g.setColorAt(1, QColor(0x1f, 0x6f, 0xeb));
        }
        p.setPen(QPen(QColor(0x1f, 0x6f, 0xeb), 1));
        p.setBrush(g);
        p.drawRoundedRect(QRectF(handle).adjusted(0, 0, -1, -1), 3, 3);
    }

private:
    // Track column width in logical pixels.
    static qreal trackW() { return 6.0; }
};

constexpr const char* kMainNames[4]   = { "Main A", "Main B", "Main C", "Main D" };
constexpr const char* kIntroNames[3]  = { "Intro A", "Intro B", "Intro C" };
constexpr const char* kFillNames[4]   = { "Fill In AA", "Fill In BB", "Fill In CC", "Fill In DD" };
constexpr const char* kEndingNames[3] = { "Ending A", "Ending B", "Ending C" };
constexpr const char* kBreakName      = "Fill In BA";

constexpr const char* kChordSuffixes[] = {
    "",         // None
    "",         // Maj
    "m",        // Min
    "7",        // Dom7
    "maj7",     // Maj7
    "m7",       // Min7
    "dim",      // Dim
    "dim7",     // Dim7
    "aug",      // Aug
    "sus4",     // Sus4
    "sus2",     // Sus2
    "6",        // Maj6
    "m6",       // Min6
    "9",        // Dom9
    "mM7",      // MinMaj7
    "add9",     // Add9
    "7sus4",    // Sus4_7
    "m9",       // Min9
    "maj9",     // Maj9
    "m7b5",     // Min7b5
    "7b5",      // Dom7b5
    "7#11",     // Dom7s11
    "7b9",      // Dom7b9
    "7#9",      // Dom7s9
    "7b13",     // Dom7b13
    "7aug",     // Dom7Aug
    "maj7#11",  // Maj7s11
    "maj7aug",  // Maj7Aug
    "6/9",      // Maj69
    "m(add9)",  // MinAdd9
    "m11",      // Min11
    "13",       // Dom13
    "5",        // Power
    "mM7(b5)",  // MinMaj7b5
    "Maj7(b5)", // Maj7b5
    "b5",       // Majb5
    "7(11)",    // Dom11
    "N.C.",     // NoChord
};
constexpr int kNumChordSuffixes = static_cast<int>(sizeof(kChordSuffixes) / sizeof(kChordSuffixes[0]));

QString formatChord(int root, int type, int bass) {
    if (root < 0 || type == 0) return QStringLiteral("-");
    const char* suffix = (type >= 0 && type < kNumChordSuffixes) ? kChordSuffixes[type] : "?";
    const auto name = chord::kNoteNames[static_cast<std::size_t>(root & 0xF)];
    QString s = QString::fromUtf8(name.data(), static_cast<int>(name.size())) + suffix;
    // Slash bass: the player's named bass note, when it differs from the
    // chord root (C/E, C/G, ...).
    if (bass >= 0 && (bass % 12) != (root % 12)) {
        const auto bass_name = chord::kNoteNames[static_cast<std::size_t>(bass % 12)];
        s += QLatin1Char('/') +
             QString::fromUtf8(bass_name.data(), static_cast<int>(bass_name.size()));
    }
    return s;
}

} // namespace

// =========================================================
//  Construction / teardown
// =========================================================
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);

    controller_.set_remap(&remap_);
    controller_.set_channel_model(&model_);
    // Note: set_midi_output is called when the user connects a backend.

    controller_.set_playback_callback([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            onPlaybackFinished();
        }, Qt::QueuedConnection);
    });

    buildUi();
    populateMidiOutputPorts();
    populateMidiInputPorts();

    // Determine the remaps folder: <Documents>/PSRstudio/remaps
    // (on the user's machine this resolves to C:\Users\DELL\Documents\PSRstudio\remaps)
    {
        const QString docs = QStandardPaths::writableLocation(
            QStandardPaths::DocumentsLocation);
        remaps_folder_ = std::filesystem::path(docs.toStdString()) / "PSRstudio" / "remaps";
    }
    // Best-effort: create the folder so the user can drop .remap files in.
    std::error_code ec;
    std::filesystem::create_directories(remaps_folder_, ec);

    // Populate the remap combobox and auto-load default.remap if present.
    refreshRemapCombo();
    // refreshRemapCombo() already auto-selects default.remap if it exists.
    // Sync UI state to the active remap.
    if (remap_.montage_enabled()) {
        montage_check_->setChecked(true);
    }

    // Load the voice list from <Documents>/PSRstudio/voices.csv
    // (used by the voice selector popup in the Mixer window).
    loadVoiceCsv();

    // The model exists from the first instruction and always holds all 16
    // channels; show it right away (the voice names need voices.csv, which
    // was just loaded) and keep the mixer in step with it every 200 ms.
    rebuildPartNames();
    refreshMixerFromModel(true);
    mixer_timer_ = new QTimer(this);
    mixer_timer_->setInterval(33);
    connect(mixer_timer_, &QTimer::timeout, this, [this]() {
        refreshMixerFromModel(false);
    });
    mixer_timer_->start();

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(80);
    connect(poll_timer_, &QTimer::timeout, this, &MainWindow::onTimer);
    poll_timer_->start();

    // Chord-engine collection window (50 ms) must be expired far more
    // often than the 80 ms display poll, or chord changes land late.
    chord_timer_ = new QTimer(this);
    chord_timer_->setTimerType(Qt::PreciseTimer);
    chord_timer_->setInterval(5);
    connect(chord_timer_, &QTimer::timeout, this, [this]() {
        if (midi_in_open_) midi_in_.tick();
        // Split-point capture: the WinMM callback stores the picked key
        // here; adopt it the moment it arrives (5 ms worst-case latency).
        if (split_capture_armed_) {
            const int note = midi_in_.take_captured_note();
            if (note >= 0) {
                chord::g_split_point.store(note, std::memory_order_relaxed);
                split_capture_armed_ = false;
                updateSplitButtonText();
            }
        }
    });
    chord_timer_->start();

    updateSectionButtons();
    updateTransportButtons();

    // Heavy startup work (settings restore, device connects, SoundFont
    // load, last-style autoload) does NOT run here: main() calls
    // begin_startup_load() when the splash's logo fade-in completes, so
    // it all runs behind the fully-visible splash.
}

MainWindow::~MainWindow() = default;

void MainWindow::begin_startup_load() {
    // Restores the last session from the INI, auto-connects the devices,
    // auto-loads the last style, then emits startup_ready() — which
    // releases the splash's fade-out (still honoring the 3-second
    // minimum hold).
    loadSettings();
}

// =========================================================
//  showSplashStatus / showNextSplashStatus — startup progress lines
//
//  Every line displays on the splash for at least 1 second, so the
//  startup sequence reads as a deliberate, professional progression.
//  Lines arriving faster than that are queued FIFO and shown in
//  order — replacing (rather than queueing) would drop steps entirely
//  when several complete within the same second, e.g. the fast
//  Hardware MIDI path (restore → MIDI out → MIDI in → style all
//  inside the first second). g_startup_splash is non-null only during
//  startup, so calls after startup are no-ops.
// =========================================================
void MainWindow::showSplashStatus(const QString& msg) {
    if (!g_startup_splash) return;
    if (!splash_msg_timer_) {
        splash_msg_timer_ = new QTimer(this);
        splash_msg_timer_->setSingleShot(true);
        connect(splash_msg_timer_, &QTimer::timeout,
                this, &MainWindow::showNextSplashStatus);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (splash_has_current_ && now - splash_msg_shown_at_ < kSplashMinDisplayMs) {
        splash_msg_queue_.append(msg);      // wait its turn behind the current line
        // Kick off (or keep) the display chain: without this, a line
        // enqueued while no timer is running would never be shown —
        // the splash would stick on the current line forever.
        if (!splash_msg_timer_->isActive()) {
            splash_msg_timer_->start(static_cast<int>(
                kSplashMinDisplayMs - (now - splash_msg_shown_at_)));
        }
        return;
    }
    // First line ever, or the current line has had its full second.
    g_startup_splash->show_message(msg);
    splash_msg_shown_at_ = now;
    splash_has_current_  = !msg.isEmpty();
    if (!splash_msg_queue_.isEmpty())       // defensive: keep the chain going
        splash_msg_timer_->start(kSplashMinDisplayMs);
}

void MainWindow::showNextSplashStatus() {
    if (splash_msg_queue_.isEmpty()) return;
    if (!g_startup_splash) {                // splash vanished mid-sequence
        splash_msg_queue_.clear();
        if (splash_ready_on_drain_) {
            splash_ready_on_drain_ = false;
            emit startup_ready();
        }
        return;
    }
    const QString msg = splash_msg_queue_.takeFirst();
    g_startup_splash->show_message(msg);
    splash_msg_shown_at_ = QDateTime::currentMSecsSinceEpoch();
    splash_has_current_  = !msg.isEmpty();
    if (splash_msg_queue_.isEmpty()) {
        if (splash_ready_on_drain_) {       // final clear displayed: release
            splash_ready_on_drain_ = false;
            emit startup_ready();
        }
    } else {
        splash_msg_timer_->start(kSplashMinDisplayMs);
    }
}

// =========================================================
//  finishStartupSequence — clear the splash line, then release
//
//  Enqueues the final clear (honouring the FIFO 1 s minimum display)
//  and emits startup_ready() only once the queue has drained, so the
//  last status line always completes its second BEFORE the fade-out
//  starts.
// =========================================================
void MainWindow::finishStartupSequence() {
    if (!g_startup_splash) {   // splash already gone: nothing to wait for
        emit startup_ready();
        return;
    }
    showSplashStatus(QString());
    if (splash_msg_queue_.isEmpty()) {
        emit startup_ready();            // clear happened immediately
    } else {
        splash_ready_on_drain_ = true;   // release when the clear displays
    }
}

// =========================================================
//  buildUi
// =========================================================
void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("PSRstudio"));

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildTitleBar(root);

    auto* bodyWidget = new QWidget(central);
    root->addWidget(bodyWidget, 1);
    auto* body = new QVBoxLayout(bodyWidget);

    // ----- Audio output backend selection -----
    auto* backendBox = new QGroupBox(QStringLiteral("Audio Output"), central);
    auto* backendLayout = new QVBoxLayout(backendBox);

    // Radio buttons for backend selection
    auto* radioRow = new QHBoxLayout();
    radio_winmidi_ = new QRadioButton(QStringLiteral("Hardware MIDI"), backendBox);
    radio_fluidsynth_ = new QRadioButton(QStringLiteral("FluidSynth (SoundFont)"), backendBox);
    radio_winmidi_->setChecked(true);
    radioRow->addWidget(radio_winmidi_);
    radioRow->addWidget(radio_fluidsynth_);
    radioRow->addStretch(1);
    backendLayout->addLayout(radioRow);

    // WinMM row
    auto* winmmRow = new QHBoxLayout();
    midi_out_combo_ = new QComboBox(backendBox);
    midi_out_connect_ = new QPushButton(QStringLiteral("Connect"), backendBox);
    midi_out_status_ = new QLabel(QStringLiteral("Not connected"), backendBox);
    winmmRow->addWidget(new QLabel(QStringLiteral("Port:"), backendBox));
    winmmRow->addWidget(midi_out_combo_, 1);
    winmmRow->addWidget(midi_out_connect_);
    winmmRow->addWidget(midi_out_status_);
    backendLayout->addLayout(winmmRow);

    // FluidSynth row
    auto* fluidRow = new QHBoxLayout();
    fluidRow->addWidget(new QLabel(QStringLiteral("SF:"), backendBox));
    soundfont_edit_ = new QLineEdit(backendBox);
    soundfont_edit_->setPlaceholderText(QStringLiteral("Select a .sf2 file..."));
    soundfont_browse_ = new QPushButton(QStringLiteral("Browse..."), backendBox);
    fluid_status_ = new QLabel(QStringLiteral("Not connected"), backendBox);
    fluidRow->addWidget(soundfont_edit_, 1);
    fluidRow->addWidget(soundfont_browse_);
    fluidRow->addWidget(fluid_status_);
    backendLayout->addLayout(fluidRow);

    // FluidSynth settings row
    auto* fluidSettingsRow = new QHBoxLayout();
    fluidSettingsRow->addWidget(new QLabel(QStringLiteral("Gain:"), backendBox));
    gain_slider_ = new QSlider(Qt::Horizontal, backendBox);
    gain_slider_->setRange(0, 100);
    gain_slider_->setValue(50);
    gain_slider_->setMaximumWidth(150);
    reverb_check_ = new QCheckBox(QStringLiteral("Reverb"), backendBox);
    reverb_check_->setChecked(true);
    chorus_check_ = new QCheckBox(QStringLiteral("Chorus"), backendBox);
    chorus_check_->setChecked(true);
    fluidSettingsRow->addWidget(gain_slider_);
    fluidSettingsRow->addWidget(reverb_check_);
    fluidSettingsRow->addWidget(chorus_check_);
    fluidSettingsRow->addStretch(1);
    backendLayout->addLayout(fluidSettingsRow);

    // MONTAGE ESP VST mode checkbox
    auto* montageRow = new QHBoxLayout();
    montage_check_ = new QCheckBox(QStringLiteral("MONTAGE ESP VST (remap channels 9-16 → 1-8)"), backendBox);
    montage_check_->setToolTip(QStringLiteral(
        "When checked, all MIDI channels are remapped to channels 1-8.\n"
        "This is for use with the Yamaha Montage ESP VST hosted externally\n"
        "(e.g., via loopMIDI). Channels 9-16 (drums, bass, chords, etc.)\n"
        "are remapped to 1-8 to match the Montage's Part 1-8 layout.\n\n"
        "The MONTAGE config is owned by the active remap profile — toggle\n"
        "this to override the profile's @montage enabled= flag at runtime."));
    montageRow->addWidget(montage_check_);
    montageRow->addStretch(1);
    backendLayout->addLayout(montageRow);

    // "Patch MegaVoices" toggle: silence MegaVoice articulation notes
    // (C6 and above) on MegaVoice channels, for sound sources without
    // MegaVoice support.
    auto* patchMegaRow = new QHBoxLayout();
    patch_mega_check_ = new QCheckBox(QStringLiteral("Patch MegaVoices"), backendBox);
    patch_mega_check_->setToolTip(QStringLiteral(
        "Silence MegaVoice articulation notes (C6 and above) on channels\n"
        "that use a MegaVoice voice (bank MSB=8). For sound sources without\n"
        "MegaVoice support: those notes would otherwise sound wrong or\n"
        "unintended. Takes effect on new notes; already-sounding notes are\n"
        "silenced at the next chord change."));
    patchMegaRow->addWidget(patch_mega_check_);
    patchMegaRow->addStretch(1);
    backendLayout->addLayout(patchMegaRow);

    // Remap profile picker row — choose which .remap file is active.
    auto* remapRow = new QHBoxLayout();
    remapRow->addWidget(new QLabel(QStringLiteral("Remap:"), backendBox));
    remap_combo_ = new QComboBox(backendBox);
    remap_combo_->setToolTip(QStringLiteral(
        "Selects a .remap profile from the remaps folder.\n"
        "A .remap file is a text file describing voice/bank/PC remapping,\n"
        "MONTAGE channel remap, and CC#93→CC#94 redirect.\n"
        "If a file named 'default.remap' exists in the folder,\n"
        "it is loaded automatically at startup."));
    remap_browse_ = new QPushButton(QStringLiteral("Browse..."), backendBox);
    remap_browse_->setToolTip(QStringLiteral(
        "Load a .remap file from anywhere on disk (does not need to be in\n"
        "the remaps folder). The selected profile becomes active but does\n"
        "not appear in the dropdown unless you copy it to the folder."));
    remap_reload_ = new QPushButton(QStringLiteral("Reload"), backendBox);
    remap_reload_->setToolTip(QStringLiteral(
        "Re-scan the remaps folder and refresh this dropdown. Use this\n"
        "after adding or removing .remap files."));
    remap_status_ = new QLabel(QStringLiteral("(not loaded)"), backendBox);
    remap_status_->setStyleSheet(QStringLiteral("color: #888;"));
    remapRow->addWidget(remap_combo_, 1);
    remapRow->addWidget(remap_browse_);
    remapRow->addWidget(remap_reload_);
    remapRow->addWidget(remap_status_, 2);
    backendLayout->addLayout(remapRow);

    body->addWidget(backendBox);

    // Disable FluidSynth controls initially (WinMM is selected)
    soundfont_edit_->setEnabled(false);
    soundfont_browse_->setEnabled(false);
    gain_slider_->setEnabled(false);
    reverb_check_->setEnabled(false);
    chorus_check_->setEnabled(false);

    // Connect signals
    connect(radio_winmidi_, &QRadioButton::toggled, this, &MainWindow::onBackendChanged);
    connect(midi_out_connect_, &QPushButton::clicked, this, &MainWindow::onConnectMidiOut);
    connect(soundfont_browse_, &QPushButton::clicked, this, &MainWindow::onBrowseSoundfont);
    connect(gain_slider_, &QSlider::valueChanged, this, &MainWindow::onGainChanged);
    connect(reverb_check_, &QCheckBox::toggled, this, &MainWindow::onReverbToggled);
    connect(chorus_check_, &QCheckBox::toggled, this, &MainWindow::onChorusToggled);
    connect(montage_check_, &QCheckBox::toggled, this, &MainWindow::onMontageToggled);
    connect(patch_mega_check_, &QCheckBox::toggled, this, [](bool on) {
        player::g_patch_megavoices.store(on, std::memory_order_relaxed);
    });
    connect(remap_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onRemapSelected);
    connect(remap_browse_, &QPushButton::clicked, this, &MainWindow::onRemapBrowse);
    connect(remap_reload_, &QPushButton::clicked, this, &MainWindow::onRemapReload);

    // ----- MIDI input row -----
    auto* midiInBox = new QGroupBox(QStringLiteral("MIDI Input (Chord Detection)"), central);
    auto* inLayout = new QHBoxLayout(midiInBox);
    midi_in_combo_ = new QComboBox(midiInBox);
    midi_in_connect_ = new QPushButton(QStringLiteral("Connect"), midiInBox);
    midi_in_status_ = new QLabel(QStringLiteral("Not connected"), midiInBox);
    inLayout->addWidget(midi_in_combo_, 1);
    inLayout->addWidget(midi_in_connect_);
    inLayout->addWidget(midi_in_status_);
    connect(midi_in_connect_, &QPushButton::clicked, this, &MainWindow::onConnectMidiIn);
    body->addWidget(midiInBox);

    // ----- Load + status row -----
    auto* loadRow = new QHBoxLayout();
    load_button_ = new QPushButton(QStringLiteral("Load Style / MIDI..."), central);
    status_label_ = new QLabel(QStringLiteral("No file loaded"), central);
    status_label_->setWordWrap(true);
    loadRow->addWidget(load_button_);
    loadRow->addWidget(status_label_, 1);
    body->addLayout(loadRow);
    connect(load_button_, &QPushButton::clicked, this, &MainWindow::onLoadClicked);

    // ----- Transport row -----
    auto* transportRow = new QHBoxLayout();
    autofill_button_ = new QPushButton(QStringLiteral("Auto Fill: ON"), central);
    autofill_button_->setCheckable(true);
    autofill_button_->setChecked(true);
    play_button_ = new QPushButton(QStringLiteral("Start"), central);
    play_button_->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #4CAF50; color: white; font-weight: bold; }"));

    auto* tempoLabel = new QLabel(QStringLiteral("Tempo:"), central);
    tempo_spin_ = new QSpinBox(central);
    tempo_spin_->setRange(player::kTempoBpmMin, player::kTempoBpmMax);
    tempo_spin_->setValue(120);

    mixer_button_ = new QPushButton(QStringLiteral("Mixer"), central);
    mixer_button_->setCheckable(true);
    mixer_button_->setToolTip(QStringLiteral(
        "Show the channel mixer.\n"
        "All 16 MIDI channels are always shown, each with a vertical strip\n"
        "with volume, reverb, chorus, pan, plus mute (M) and solo (S)."));

    // Split Point picker: click, then press a key on the MIDI input —
    // that key becomes the new chord-detection split point.
    split_button_ = new QPushButton(central);
    split_button_->setToolTip(QStringLiteral(
        "Split Point — click, then press the LOWEST key of your left\n"
        "hand on the MIDI keyboard. Keys at or above it are ignored\n"
        "by chord detection (right-hand / melody zone).\n"
        "Click again while waiting to cancel."));

    // Chord recognition mode picker (ignored in Full Keyboard mode).
    chord_mode_combo_ = new QComboBox(central);
    chord_mode_combo_->addItem(QStringLiteral("Single Finger"));
    chord_mode_combo_->addItem(QStringLiteral("Multi Finger"));
    chord_mode_combo_->addItem(QStringLiteral("Fingered"));
    chord_mode_combo_->addItem(QStringLiteral("Fingered On Bass"));
    chord_mode_combo_->addItem(QStringLiteral("Full Keyboard"));
    chord_mode_combo_->setToolTip(QStringLiteral(
        "Chord recognition mode:\n"
        "Single Finger — root = Maj, +black key left = m, +white key\n"
        "  left = 7, +both = m7. Strict: other patterns hold the chord.\n"
        "Multi Finger — Single Finger rules, then full chord matching.\n"
        "Fingered — 3+ keys, all chords; inversions use the root bass.\n"
        "Fingered On Bass — like Fingered, but the lowest key is the\n"
        "  bass (only mode with slash chords, e.g. C/E).\n"
        "Full Keyboard — the whole keyboard feeds chord detection."));
    // Show the actual internal mode: chord_mode_ and the engine default
    // are Fingered (index 2), so the combo must NOT start on item 0.
    chord_mode_combo_->setCurrentIndex(2);

    // Master volume slider — global scale factor applied to every
    // channel's CC#7 (volume) before sending. Range 0-100, default 100
    // (no scaling). At 50, every channel's CC#7 is multiplied by 0.5.
    master_volume_label_ = new QLabel(QStringLiteral("Master: 100"), central);
    master_volume_slider_ = new QSlider(Qt::Horizontal, central);
    master_volume_slider_->setRange(0, 100);
    master_volume_slider_->setValue(model_.master_volume());
    master_volume_slider_->setFixedWidth(120);
    master_volume_slider_->setToolTip(QStringLiteral(
        "Master volume — applies a global scale factor to every\n"
        "channel's CC#7 (volume) before sending to the synth.\n"
        "Range 0-100. Default 100 = no scaling.\n"
        "At 50, every channel's volume is halved.\n"
        "Does not affect the per-channel volume sliders — only the\n"
        "value that gets sent to the synth."));

    transportRow->addWidget(autofill_button_);
    transportRow->addWidget(play_button_);
    transportRow->addWidget(mixer_button_);
    transportRow->addWidget(split_button_);
    transportRow->addWidget(chord_mode_combo_);
    transportRow->addStretch(1);
    transportRow->addWidget(master_volume_label_);
    transportRow->addWidget(master_volume_slider_);
    transportRow->addSpacing(12);
    transportRow->addWidget(tempoLabel);
    transportRow->addWidget(tempo_spin_);
    body->addLayout(transportRow);

    connect(autofill_button_, &QPushButton::toggled, this, &MainWindow::onAutoFillToggled);
    connect(play_button_, &QPushButton::clicked, this, &MainWindow::onPlayClicked);
    connect(tempo_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onTempoChanged);
    connect(mixer_button_, &QPushButton::clicked, this, &MainWindow::onMixerToggled);
    connect(split_button_, &QPushButton::clicked, this, &MainWindow::onSplitPointClicked);
    connect(chord_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onChordModeChanged);
    updateSplitButtonText();
    connect(master_volume_slider_, &QSlider::valueChanged,
            this, &MainWindow::onMasterVolumeChanged);

    // ----- Live display row -----
    auto* displayBox = new QGroupBox(QStringLiteral("Now Playing"), central);
    auto* displayLayout = new QFormLayout(displayBox);
    chord_label_ = new QLabel(QStringLiteral("-"), displayBox);
    section_label_ = new QLabel(QStringLiteral("-"), displayBox);
    measure_label_ = new QLabel(QStringLiteral("Measure - / -"), displayBox);
    beat_label_ = new QLabel(QStringLiteral("Beat - / -"), displayBox);
    displayLayout->addRow(QStringLiteral("Chord:"), chord_label_);
    displayLayout->addRow(QStringLiteral("Section:"), section_label_);
    displayLayout->addRow(QStringLiteral(""), measure_label_);
    displayLayout->addRow(QStringLiteral(""), beat_label_);
    body->addWidget(displayBox);

    // ----- Channel controls are in the Mixer overlay -----
    // The mixer is an overlay panel inside the main window (not a separate
    // OS window), shown/hidden via the Mixer button in the transport row
    // (see onMixerToggled). It ALWAYS contains all 16 channel strips, built
    // once here by buildMixerStrips(), and when shown it fills the whole
    // window area below the title bar, edge to edge (layoutMixerOverlay()).
    mixer_window_ = new QWidget(this);
    mixer_window_->setWindowTitle(QStringLiteral("PSRstudio — Mixer"));
    mixer_window_->setAttribute(Qt::WA_NoMousePropagation, true);
    // Solid background so the window isn't see-through on some platforms.
    mixer_window_->setAutoFillBackground(true);
    mixer_window_->installEventFilter(this);
    // Modern dark DAW-style stylesheet for the mixer window.
    // This cascades to all child widgets. Individual strip widgets
    // get their own panel-style background via setStyleSheet in
    // buildMixerStrips().
    mixer_window_->setStyleSheet(QStringLiteral(
        "background-color: #0d1117;"
        "QLabel { color: #c9d1d9; }"
        // NOTE: no QSlider or QDial rules here on purpose — the mixer
        // uses MixerDial / MixerSlider (custom-painted subclasses); QSS
        // rules would match the subclasses too and push every control
        // back through the slow stylesheet painting path.
        "QPushButton {"
        "  background-color: #21262d;"
        "  color: #8b949e;"
        "  border: 1px solid #30363d;"
        "  border-radius: 4px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #30363d;"
        "  color: #c9d1d9;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #1f6feb;"
        "  color: #ffffff;"
        "}"
    ));
    // Layout: header bar (title + close button) on top, the 16 strips
    // below filling all remaining space (no scrolling — the strips share
    // the available width equally).
    auto* mixer_outer = new QVBoxLayout(mixer_window_);
    mixer_outer->setContentsMargins(0, 0, 0, 0);
    mixer_outer->setSpacing(0);

    mixer_header_ = new QWidget(mixer_window_);
    mixer_header_->setFixedHeight(40);
    mixer_header_->setStyleSheet(QStringLiteral("background-color: #161b22;"));
    auto* mixer_hl = new QHBoxLayout(mixer_header_);
    mixer_hl->setContentsMargins(14, 0, 0, 0);
    mixer_hl->setSpacing(0);
    auto* mixer_title = new QLabel(QStringLiteral("Mixer"), mixer_header_);
    mixer_title->setStyleSheet(QStringLiteral(
        "color: #c9d1d9; font-weight: bold; font-size: 13px;"
        "background: transparent; border: none;"));
    auto* mixer_close = new QPushButton(QStringLiteral("\u2715"), mixer_header_);
    mixer_close->setFixedSize(44, 40);
    mixer_close->setCursor(Qt::PointingHandCursor);
    mixer_close->setToolTip(QStringLiteral("Close the mixer"));
    mixer_close->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #8b949e; border: none;"
        "  border-radius: 0; font-size: 16px; }"
        "QPushButton:hover { background-color: #e81123; color: #ffffff; }"
        "QPushButton:pressed { background-color: #b0101d; color: #ffffff; }"));
    connect(mixer_close, &QPushButton::clicked, this, [this]() {
        if (mixer_button_) mixer_button_->setChecked(false);
        if (mixer_window_) mixer_window_->hide();
    });
    mixer_hl->addWidget(mixer_title);
    mixer_hl->addStretch(1);
    mixer_hl->addWidget(mixer_close);
    mixer_outer->addWidget(mixer_header_);

    mixer_container_ = new QWidget(mixer_window_);
    mixer_outer->addWidget(mixer_container_, 1);

    mixer_layout_ = new QHBoxLayout(mixer_container_);
    mixer_layout_->setContentsMargins(8, 8, 8, 8);
    mixer_layout_->setSpacing(6);

    buildMixerStrips();
    layoutMixerOverlay();
    // Initially hidden — shown when the Mixer button is checked.
    mixer_window_->hide();
    // Pre-render the mixer once, offscreen: grab() forces the full
    // polish / layout / font / paint pipeline for all ~200 widgets NOW,
    // behind the splash, instead of on the user's first Mixer click.
    mixer_window_->grab();

    // ----- Section buttons (single row, grouped, square) -----
    // Moved to the bottom per user request. The Mixer window (above)
    // is the per-channel mixer; the section buttons are the play-time
    // section selector.
    // Display labels for the square buttons (short labels per user request).
    // The full style-section names (kIntroNames / kMainNames / kEndingNames /
    // kBreakName) are still used everywhere else for section lookup and
    // highlighting — only the on-screen label changes here.
    static constexpr const char* kIntroLabels[3]  = { "I", "II", "III" };
    static constexpr const char* kMainLabels[4]   = { "A", "B", "C", "D" };
    static constexpr const char* kEndingLabels[3] = { "I", "II", "III" };

    // Square button side length (px). Fixed so every button stays a square
    // regardless of layout stretch.
    constexpr int kSqBtn = 36;

    auto* sectionRow = new QHBoxLayout();
    sectionRow->setSpacing(12);   // visual gap between groups

    // --- Intro group ---
    auto* introBox = new QGroupBox(QStringLiteral("Intro"), central);
    auto* introLayout = new QHBoxLayout(introBox);
    introLayout->setSpacing(2);
    introLayout->setContentsMargins(8, 4, 8, 4);
    for (int i = 0; i < 3; ++i) {
        intro_buttons_[i] = new QPushButton(QString::fromLatin1(kIntroLabels[i]), introBox);
        intro_buttons_[i]->setFixedSize(kSqBtn, kSqBtn);
        intro_buttons_[i]->setToolTip(QString::fromUtf8(kIntroNames[i]));
        introLayout->addWidget(intro_buttons_[i]);
        connect(intro_buttons_[i], &QPushButton::clicked, this,
                [this, i]() { onIntroButtonClicked(i); });
    }
    sectionRow->addWidget(introBox);

    // --- Main group ---
    auto* mainBox = new QGroupBox(QStringLiteral("Main"), central);
    auto* mainLayout = new QHBoxLayout(mainBox);
    mainLayout->setSpacing(2);
    mainLayout->setContentsMargins(8, 4, 8, 4);
    for (int i = 0; i < 4; ++i) {
        main_buttons_[i] = new QPushButton(QString::fromLatin1(kMainLabels[i]), mainBox);
        main_buttons_[i]->setFixedSize(kSqBtn, kSqBtn);
        main_buttons_[i]->setCheckable(true);
        main_buttons_[i]->setToolTip(QString::fromUtf8(kMainNames[i]));
        mainLayout->addWidget(main_buttons_[i]);
        connect(main_buttons_[i], &QPushButton::clicked, this,
                [this, i]() { onMainButtonClicked(i); });
    }
    sectionRow->addWidget(mainBox);

    // --- Break group (single button) ---
    auto* breakBox = new QGroupBox(QStringLiteral("Break"), central);
    auto* breakLayout = new QHBoxLayout(breakBox);
    breakLayout->setSpacing(2);
    breakLayout->setContentsMargins(8, 4, 8, 4);
    break_button_ = new QPushButton(QStringLiteral("BA"), breakBox);
    break_button_->setFixedSize(kSqBtn, kSqBtn);
    break_button_->setToolTip(QStringLiteral("Fill In BA (Break)"));
    breakLayout->addWidget(break_button_);
    connect(break_button_, &QPushButton::clicked, this, &MainWindow::onBreakButtonClicked);
    sectionRow->addWidget(breakBox);

    // --- Ending group ---
    auto* endingBox = new QGroupBox(QStringLiteral("Ending"), central);
    auto* endingLayout = new QHBoxLayout(endingBox);
    endingLayout->setSpacing(2);
    endingLayout->setContentsMargins(8, 4, 8, 4);
    for (int i = 0; i < 3; ++i) {
        ending_buttons_[i] = new QPushButton(QString::fromLatin1(kEndingLabels[i]), endingBox);
        ending_buttons_[i]->setFixedSize(kSqBtn, kSqBtn);
        ending_buttons_[i]->setToolTip(QString::fromUtf8(kEndingNames[i]));
        endingLayout->addWidget(ending_buttons_[i]);
        connect(ending_buttons_[i], &QPushButton::clicked, this,
                [this, i]() { onEndingButtonClicked(i); });
    }
    sectionRow->addWidget(endingBox);

    sectionRow->addStretch(1);
    body->addLayout(sectionRow);
}

// =========================================================
//  Backend selection
// =========================================================
void MainWindow::onBackendChanged() {
    const bool useFluid = radio_fluidsynth_->isChecked();

    // Enable/disable appropriate controls
    midi_out_combo_->setEnabled(!useFluid);
    midi_out_connect_->setEnabled(!useFluid);
    soundfont_edit_->setEnabled(useFluid);
    soundfont_browse_->setEnabled(useFluid);
    gain_slider_->setEnabled(useFluid);
    reverb_check_->setEnabled(useFluid);
    chorus_check_->setEnabled(useFluid);

    // Disconnect whatever is currently connected. Switching away from
    // FluidSynth therefore disconnects the SoundFont automatically.
    disconnectActiveOutput();

    // Switching to FluidSynth auto-connects when a SoundFont is already
    // selected — no need to press Connect. Skipped while a load is
    // already in progress, and when no (existing) file has been picked.
    // Startup note: loadSettings() calls this before the saved path is
    // restored (empty edit -> no auto-connect) and connects explicitly
    // afterwards.
    if (useFluid && !fluid_loading_
        && !soundfont_edit_->text().isEmpty()
        && QFile::exists(soundfont_edit_->text())) {
        onConnectFluidSynth();
    }
}

// =========================================================
//  disconnectActiveOutput — shut down whichever backend is active
// =========================================================
void MainWindow::disconnectActiveOutput() {
    // Detach the model first: from here on it only keeps its state, and
    // nothing more is sent to a device that is about to go away. It is
    // re-synced in full the next time a device is connected.
    model_.detach_sink();

    if (win_midi_open_) {
        win_midi_out_.all_notes_off();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        win_midi_out_.stop_thread();
        win_midi_out_.close();
        win_midi_open_ = false;
        midi_out_status_->setText(QStringLiteral("Not connected"));
    }
    if (fluid_open_) {
        fluid_midi_out_.all_notes_off();
        fluid_midi_out_.close();
        fluid_open_ = false;
        fluid_status_->setText(QStringLiteral("Not connected"));
    }
    active_midi_out_ = nullptr;
    controller_.set_midi_output(nullptr);
    updateTransportButtons();
}

// =========================================================
//  MIDI port population
// =========================================================
void MainWindow::populateMidiOutputPorts() {
    midi_out_combo_->clear();
    for (const auto& p : midi::WinMidiOutput::enumerate_ports()) {
        midi_out_combo_->addItem(QString::fromUtf8(p.name.c_str()), QVariant(p.index));
    }
}

void MainWindow::populateMidiInputPorts() {
    midi_in_combo_->clear();
    for (const auto& p : midi::WinMidiInput::enumerate_ports()) {
        midi_in_combo_->addItem(QString::fromUtf8(p.name.c_str()), QVariant(p.index));
    }
}

// =========================================================
//  WinMM connect / disconnect
// =========================================================
void MainWindow::onConnectMidiOut() {
    if (win_midi_open_) {
        disconnectActiveOutput();
        return;
    }

    // Disconnect FluidSynth if it's open
    if (fluid_open_) disconnectActiveOutput();

    const int sel = midi_out_combo_->currentIndex();
    if (sel < 0) {
        showOverlay(QStringLiteral("No MIDI port selected."), 6000);
        return;
    }
    const int port = midi_out_combo_->itemData(sel).toInt();

    if (win_midi_out_.open(port)) {
        win_midi_open_ = true;
        active_midi_out_ = &win_midi_out_;
        controller_.set_midi_output(active_midi_out_);
        midi_out_status_->setText(QStringLiteral("Connected"));

        // Send GM On + XG On
        static const prs::u8 gmon[] = { 0x7E, 0x7F, 0x09, 0x01 };
        static const prs::u8 xgon[] = { 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00 };
        win_midi_out_.sysex_nowait(gmon);
        win_midi_out_.sysex_nowait(xgon);
        win_midi_out_.sysex_flush();
        win_midi_out_.start_thread();

        // Make the freshly-connected synth mirror the channel model: synth
        // reset, the loaded style's header SysEx (if any), then all 16
        // channels (voice + volume / pan / reverb / chorus / ...).
        syncDeviceToModel();
    } else {
        showOverlay(QStringLiteral("Failed to open MIDI port."), 6000);
        midi_out_status_->setText(QStringLiteral("Error"));
    }
    updateTransportButtons();
}

// =========================================================
//  FluidSynth: browse for SoundFont
// =========================================================
void MainWindow::onBrowseSoundfont() {
    const QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("Select SoundFont File"), QString(),
        QStringLiteral("SoundFont Files (*.sf2 *.sf3);;All Files (*.*)"));
    if (path.isEmpty()) return;
    soundfont_edit_->setText(path);

    // Instant-load: a freshly picked SoundFont connects right away — no
    // need to press Connect. If FluidSynth is already running, the new
    // font replaces the old one (disconnect, then reconnect).
    if (!radio_fluidsynth_->isChecked() || fluid_loading_) return;
    if (fluid_open_) disconnectActiveOutput();
    onConnectFluidSynth();
}

// =========================================================
//  FluidSynth connect / disconnect
// =========================================================
void MainWindow::onConnectFluidSynth() {
    if (fluid_open_) {
        disconnectActiveOutput();
        return;
    }
    if (fluid_loading_) return;   // a load is already in progress

    // Disconnect WinMM if it's open
    if (win_midi_open_) disconnectActiveOutput();

    const QString sfPath = soundfont_edit_->text();
    if (sfPath.isEmpty()) {
        showOverlay(QStringLiteral("Please select a SoundFont file."), 6000);
        return;
    }

    fluid_midi_out_.set_soundfont(sfPath.toStdString());
    fluid_midi_out_.set_audio_driver(fluidsynth::AudioDriver::Wasapi);
    fluid_midi_out_.set_sample_rate(48000);
    fluid_midi_out_.set_gain(static_cast<float>(gain_slider_->value()) / 100.0f);

    // ----- Open FluidSynth on a worker thread -----
    // Loading a large .sf2 (or a compressed .sf3) can take seconds.
    // open() runs off the UI thread and a nested event loop keeps the
    // window painting; a small overlay appears if the load takes
    // longer than 500 ms. Controls that could interfere with the load
    // are disabled until it finishes.
    fluid_loading_ = true;
    soundfont_browse_->setEnabled(false);
    soundfont_edit_->setEnabled(false);
    radio_winmidi_->setEnabled(false);
    radio_fluidsynth_->setEnabled(false);

    std::atomic<bool> open_done{false};
    bool open_ok = false;
    std::thread worker([this, &open_done, &open_ok]() {
        try {
            open_ok = fluid_midi_out_.open();
        } catch (...) {
            open_ok = false;
        }
        open_done.store(true, std::memory_order_release);
    });

    // The loading notice stays hidden unless the load is still running
    // 500 ms after it started. During startup it goes to the SPLASH
    // (which covers the whole screen — an overlay here would be
    // invisible), through the 1 s-per-line queue; at all other times it
    // uses the main-window overlay.
    QTimer show_timer;
    show_timer.setSingleShot(true);
    connect(&show_timer, &QTimer::timeout, this, [this]() {
        const QString msg = QStringLiteral("Loading Soundfont. Please wait...");
        if (g_startup_splash) showSplashStatus(msg);
        else                  showOverlay(msg, 0);
    });
    show_timer.start(500);

    QEventLoop loop;
    QTimer poll_timer;
    connect(&poll_timer, &QTimer::timeout, &loop, [&loop, &open_done]() {
        if (open_done.load(std::memory_order_acquire)) loop.quit();
    });
    poll_timer.start(20);
    loop.exec();   // processes UI events while the worker loads

    worker.join();
    show_timer.stop();
    showSplashStatus(QString());   // startup: queue the clear (≥1 s rule)
    closeOverlay();   // no-op if the overlay was never shown

    radio_winmidi_->setEnabled(true);
    radio_fluidsynth_->setEnabled(true);
    const bool useFluid = radio_fluidsynth_->isChecked();
    soundfont_edit_->setEnabled(useFluid);
    soundfont_browse_->setEnabled(useFluid);
    fluid_loading_ = false;

    if (!open_ok) {
        showOverlay(QStringLiteral("Failed to initialize FluidSynth."), 6000);
        fluid_status_->setText(QStringLiteral("Error"));
        updateTransportButtons();
        return;
    }

    // Apply reverb/chorus settings
    fluid_midi_out_.set_reverb(reverb_check_->isChecked());
    fluid_midi_out_.set_chorus(chorus_check_->isChecked());

    fluid_open_ = true;
    active_midi_out_ = &fluid_midi_out_;
    controller_.set_midi_output(active_midi_out_);
    fluid_status_->setText(QStringLiteral("Connected"));

    // Same as onConnectMidiOut: the new synth mirrors the channel model.
    syncDeviceToModel();

    updateTransportButtons();
}

// =========================================================
//  showOverlay / closeOverlay — generic overlay notice
//
//  A small centered overlay label for transient messages ("Loading
//  Soundfont. Please wait...", etc.).
//
//    showOverlay(msg, ms)  ms > 0: auto-hide after ms milliseconds.
//                          ms = 0: persists until closed.
//    closeOverlay()        hides it and stops the auto-hide timer.
//
//  Calling showOverlay again while it is visible updates the text and
//  restarts the timer. The label is lazily created and transparent for
//  mouse events, so it never blocks clicks on widgets underneath.
// =========================================================
void MainWindow::showOverlay(const QString& txtmsg, int time_ms) {
    if (!overlay_label_) {
        overlay_label_ = new QLabel(this);
        overlay_label_->setStyleSheet(QStringLiteral(
            "background-color: #161b22; color: #e6e6e6;"
            "border: 1px solid #444; border-radius: 4px;"
            "padding: 8px 16px; font-size: 16px;"));
        // Long messages wrap to the next line instead of stretching the
        // bubble across the window; centred so multi-line text looks right.
        overlay_label_->setWordWrap(true);
        overlay_label_->setAlignment(Qt::AlignCenter);
        // A notice, not a dialog: let clicks fall through to the
        // widgets underneath.
        overlay_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
        overlay_timer_ = new QTimer(overlay_label_);
        overlay_timer_->setSingleShot(true);
        connect(overlay_timer_, &QTimer::timeout, overlay_label_, &QWidget::hide);
    }
    overlay_label_->setText(txtmsg);
    // Re-fit for the new message: ideal single-line width, capped at
    // three-fifths of the window — anything longer wraps and the label
    // grows downward instead (heightForWidth accounts for the wrapping).
    const int max_w = std::max(200, width() * 3 / 5);
    const int w = std::min(overlay_label_->sizeHint().width(), max_w);
    overlay_label_->setFixedWidth(w);
    overlay_label_->resize(w, overlay_label_->heightForWidth(w));
    centerOverlay(overlay_label_);
    overlay_label_->raise();
    overlay_label_->show();

    if (time_ms > 0) {
        overlay_timer_->start(time_ms);
    } else {
        overlay_timer_->stop();        // time_ms == 0: stay until closed
    }
}

void MainWindow::closeOverlay() {
    if (overlay_timer_) overlay_timer_->stop();
    if (overlay_label_) overlay_label_->hide();
}

// =========================================================
//  FluidSynth settings handlers
// =========================================================
void MainWindow::onGainChanged(int value) {
    if (fluid_open_) {
        // Real-time update: calls fluid_synth_set_gain() directly, no
        // close/reopen needed.
        fluid_midi_out_.set_gain_runtime(static_cast<float>(value) / 100.0f);
    }
}

void MainWindow::onReverbToggled(bool checked) {
    if (fluid_open_) {
        fluid_midi_out_.set_reverb(checked);
    }
}

void MainWindow::onChorusToggled(bool checked) {
    if (fluid_open_) {
        fluid_midi_out_.set_chorus(checked);
    }
}

void MainWindow::onMontageToggled(bool checked) {
    remap_.set_montage_enabled(checked);
    // MONTAGE mode changes the OUTPUT channel layout (CASM bypassed, all
    // style channels folded into 1-8; 9-16 idle), so the model is derived
    // again from the loaded style and the device re-synced.
    onRemapProfileChanged();
}

// =========================================================
//  Remap profile picker
// =========================================================
void MainWindow::refreshRemapCombo() {
    if (!remap_combo_) return;
    // Block signals while we rebuild so currentIndexChanged doesn't fire
    // for every removeItemAt / addItem we do.
    const bool wasBlocked = remap_combo_->blockSignals(true);
    remap_combo_->clear();
    remap_paths_.clear();

    // Item 0 is always "no remap" (empty path — voices pass through
    // to the destination unchanged).
    remap_combo_->addItem(QStringLiteral("<no remap>"));
    remap_paths_.emplace_back();

    // Scan the remaps folder for .remap files.
    auto files = prs::VoiceRemap::scan_folder(remaps_folder_);
    int default_idx = -1;
    for (const auto& path : files) {
        const QString display = pathToQString(path.stem());
        remap_combo_->addItem(display);
        remap_paths_.push_back(path);
        // Match "default" (case-insensitive) on the stem, not extension.
        if (default_idx < 0) {
            QString stem = display.toLower();
            if (stem == QStringLiteral("default")) {
                default_idx = static_cast<int>(remap_paths_.size() - 1);
            }
        }
    }
    remap_combo_->blockSignals(wasBlocked);

    // Auto-load default.remap if it exists. Otherwise, leave the combobox
    // on the built-in defaults (item 0) — but the remap_ object may have
    // already been initialized (fallback rules only) by its constructor,
    // so we don't need to do anything special.
    if (default_idx > 0) {
        remap_combo_->setCurrentIndex(default_idx);
        // currentIndexChanged was suppressed; load the file manually.
        onRemapSelected(default_idx);
    } else {
        remap_combo_->setCurrentIndex(0);
        // Item 0 is the built-in defaults; sync status label.
        updateRemapStatusLabel();
    }
}

void MainWindow::onRemapSelected(int index) {
    if (!remap_combo_ || index < 0 || index >= static_cast<int>(remap_paths_.size())) {
        return;
    }
    const auto& path = remap_paths_[static_cast<std::size_t>(index)];
    if (path.empty()) {
        // Built-in defaults — reset the remap to its constructor state.
        remap_ = prs::VoiceRemap{};
    } else {
        if (!remap_.load_profile_from_file(path)) {
            showOverlay(QStringLiteral("Failed to load '%1'. Keeping the previous remap.")
                            .arg(pathToQString(path)), 6000);
            updateRemapStatusLabel();
            return;
        }
    }
    // Block signals while we sync the checkbox state to the remap's
    // montage_enabled flag — we don't want onMontageToggled to fire
    // and re-derive the model twice.
    const bool wasBlocked = montage_check_->blockSignals(true);
    montage_check_->setChecked(remap_.montage_enabled());
    montage_check_->blockSignals(wasBlocked);

    updateRemapStatusLabel();
    // The new profile may change voices, the channel layout (Montage) and
    // the chorus redirect: re-derive the model and re-sync the device.
    onRemapProfileChanged();
}

void MainWindow::onRemapBrowse() {
    const QString startDir = pathToQString(remaps_folder_);
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load Remap Profile"), startDir,
        QStringLiteral("Remap profiles (*.remap);;All Files (*.*)"));
    if (path.isEmpty()) return;
    const auto stdPath = std::filesystem::path(path.toStdString());
    if (!remap_.load_profile_from_file(stdPath)) {
        showOverlay(QStringLiteral("Failed to load '%1'.").arg(path), 6000);
        return;
    }
    const bool wasBlocked = montage_check_->blockSignals(true);
    montage_check_->setChecked(remap_.montage_enabled());
    montage_check_->blockSignals(wasBlocked);
    updateRemapStatusLabel();
    // If the browsed file is in the remaps folder, also select it in the
    // combobox; otherwise just update the status label (the file isn't in
    // the dropdown).
    const auto canonical = [](const std::filesystem::path& p) {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : c;
    };
    const auto browsed = canonical(stdPath);
    for (int i = 0; i < static_cast<int>(remap_paths_.size()); ++i) {
        if (canonical(remap_paths_[static_cast<std::size_t>(i)]) == browsed) {
            const bool wasB = remap_combo_->blockSignals(true);
            remap_combo_->setCurrentIndex(i);
            remap_combo_->blockSignals(wasB);
            break;
        }
    }
    onRemapProfileChanged();
}

void MainWindow::onRemapReload() {
    refreshRemapCombo();
}

// =========================================================
//  Model <-> style / device synchronisation
// =========================================================

// The remap profile or MONTAGE mode changed. Everything derived from it is
// recomputed: the chorus redirect, the style's channel layout / voices in
// the model, and (through the model) the device.
void MainWindow::onRemapProfileChanged() {
    model_.set_chorus_redirect(remap_.redirect_chorus_to_variation());

    if (controller_.style()) {
        // Notes sounding on channels about to change role would hang.
        if (controller_.is_playing() && active_midi_out_) {
            active_midi_out_->all_notes_off();
        }
        // Full reset: the channel layout may have changed, so channels the
        // style does not use go back to the startup defaults.
        applyStyleToModel(/*full_reset=*/true);
    } else {
        // No style: the model is unchanged, but the chorus redirect may
        // have changed what goes on the wire — re-send everything.
        model_.sync_all();
        rebuildPartNames();
        refreshMixerFromModel(true);
    }
}

// Store the loaded style's channel state in the model (which sends it to
// the device if one is connected).
void MainWindow::applyStyleToModel(bool full_reset) {
    const prs::PrsStyle* style = controller_.style();
    if (!style) return;

    const auto setups = style->build_channel_setup(remap_);

    // Style load: channels the style leaves alone keep their state, except
    // with MONTAGE on where channels 9-16 must stay idle (reset to the
    // startup defaults). A layout change resets every channel the style
    // doesn't define.
    std::uint16_t reset_mask = 0;
    if (full_reset)                   reset_mask = 0xFFFF;
    else if (remap_.montage_enabled()) reset_mask = 0xFF00;

    model_.apply_setup(setups, reset_mask);
    picked_voice_idx_.fill(-1);   // the style's voices replace any picked ones

    rebuildPartNames();
    refreshMixerFromModel(true);
}

// A device was just opened: synth reset, the style's header SysEx, then
// all 16 channels from the model.
void MainWindow::syncDeviceToModel() {
    if (!active_midi_out_) return;

    prs::PrsStyle::send_synth_init(*active_midi_out_);
    if (const prs::PrsStyle* style = controller_.style()) {
        style->send_header_sysex(*active_midi_out_);
    }
    model_.attach_sink_and_sync(active_midi_out_);
}

void MainWindow::updateRemapStatusLabel() {
    if (!remap_status_) return;
    QString text;
    if (remap_.source_path().empty()) {
        if (remap_.name().empty()) {
            text = QStringLiteral("No remap (voices pass through)");
        } else {
            text = QString::fromUtf8(remap_.name());
        }
    } else {
        const QString name = remap_.name().empty()
            ? pathToQString(remap_.source_path().stem())
            : QString::fromUtf8(remap_.name());
        text = name;
    }
    text += QStringLiteral("  —  %1 entries")
                .arg(static_cast<qulonglong>(remap_.size()));
    remap_status_->setText(text);
    remap_status_->setToolTip(QString::fromUtf8(remap_.description()));
}

// =========================================================
//  Voice selector — load CSV, show popup menu, override voice
// =========================================================

// Voice CSV format:  name,msb,lsb,pc,category
//   msb, lsb, pc are RAW MIDI values, all 0-127 (PC is the number that
//   goes on the wire — it is NOT 1-based). The category column is optional.
void MainWindow::loadVoiceCsv() {
    voice_list_.clear();
    // Look for voices.csv in <Documents>/PSRstudio/
    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    auto csv_path = std::filesystem::path(docs.toStdString()) /
                    "PSRstudio" / "voices.csv";
    if (!std::filesystem::exists(csv_path)) {
        std::cout << "[voices] No voices.csv found at " << csv_path.string() << "\n";
        return;
    }
    std::ifstream f(csv_path);
    if (!f) {
        std::cerr << "[voices] Cannot open " << csv_path.string() << "\n";
        return;
    }

    const auto trim = [](std::string str) {
        const char* ws = " \t\r\n";
        const auto b = str.find_first_not_of(ws);
        if (b == std::string::npos) return std::string{};
        const auto e = str.find_last_not_of(ws);
        return str.substr(b, e - b + 1);
    };
    // Whole-field integer parse ("12abc" fails, unlike std::stoi).
    const auto parse_int = [](const std::string& str, int& out) {
        if (str.empty()) return false;
        const auto r = std::from_chars(str.data(), str.data() + str.size(), out);
        return r.ec == std::errc{} && r.ptr == str.data() + str.size();
    };

    int skipped = 0;
    bool saw_pc_zero = false, saw_pc_128 = false, first_row = true;
    std::string line;
    while (std::getline(f, line)) {
        if (first_row && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);   // UTF-8 BOM
        }
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Simple CSV split (no quoted fields). Every field is trimmed, which
        // also removes the '\r' of Windows line endings — otherwise the last
        // row's category ("Strings") and the others ("Strings\r") would end
        // up as two different categories.
        std::vector<std::string> fields;
        std::string field;
        for (char c : line) {
            if (c == ',') { fields.push_back(trim(field)); field.clear(); }
            else field += c;
        }
        fields.push_back(trim(field));
        if (fields.size() < 4) { ++skipped; first_row = false; continue; }

        VoiceEntry v;
        int msb = 0, lsb = 0, pc = 0;
        if (!parse_int(fields[1], msb) || !parse_int(fields[2], lsb) ||
            !parse_int(fields[3], pc)) {
            // A non-numeric first row is the header — skip it silently.
            if (!first_row) {
                std::cerr << "[voices] Skipping unparsable row: " << line << "\n";
                ++skipped;
            }
            first_row = false;
            continue;
        }
        first_row = false;

        // MSB / LSB must be 0-127. PC is accepted up to 128 ONLY so a
        // 1-based file can be detected below.
        if (msb < 0 || msb > 127 || lsb < 0 || lsb > 127 || pc < 0 || pc > 128) {
            std::cerr << "[voices] Out-of-range value (MSB/LSB/PC must be 0-127), skipping: "
                      << line << "\n";
            ++skipped;
            continue;
        }
        if (pc == 0)   saw_pc_zero = true;
        if (pc == 128) saw_pc_128 = true;

        v.name = fields[0];
        v.msb = msb; v.lsb = lsb; v.pc = pc;
        if (fields.size() > 4) v.category = fields[4];
        voice_list_.push_back(std::move(v));
    }

    // Safety net: a PC of 128 can only exist in a 1-based list. If the file
    // has 128 and no 0, treat the whole file as 1-based and shift it down
    // so that EVERYTHING after this point is plain 0-127 MIDI.
    if (saw_pc_128 && !saw_pc_zero) {
        std::cerr << "[voices] voices.csv contains PC 128 and no PC 0: it looks 1-based. "
                     "Converting to 0-127. Please fix the file (PC must be 0-127).\n";
        for (auto& v : voice_list_) --v.pc;
    } else if (saw_pc_128) {
        std::cerr << "[voices] voices.csv has PC 128, which is invalid (must be 0-127); "
                     "those rows were dropped.\n";
        std::erase_if(voice_list_, [](const VoiceEntry& v) { return v.pc > 127; });
    }

    std::cout << "[voices] Loaded " << voice_list_.size() << " voices from "
              << csv_path.string();
    if (skipped) std::cout << "  (" << skipped << " rows skipped)";
    std::cout << "\n";

    rebuild_voice_index();
}

// =========================================================
//  rebuild_voice_index — hash map for voiceIndexFor
//
//  Maps (msb<<16 | lsb<<8 | pc) to all matching voice_list_ indices,
//  in CSV order. Built once when the CSV loads; voiceIndexFor() then
//  answers "which voice is this channel using?" with a hash lookup
//  instead of scanning thousands of entries per channel per refresh.
// =========================================================
void MainWindow::rebuild_voice_index() {
    voice_by_addr_.clear();
    for (int i = 0; i < static_cast<int>(voice_list_.size()); ++i) {
        const VoiceEntry& v = voice_list_[static_cast<std::size_t>(i)];
        const auto key = static_cast<std::uint32_t>(
            (v.msb << 16) | (v.lsb << 8) | v.pc);
        voice_by_addr_[key].push_back(i);
    }
}

// =========================================================
//  Voice selector window — Genos-style: category buttons on the left,
//  2x5 grid of voices on the right, page tabs (P1..Pn) underneath.
// =========================================================

namespace {
// Genos-style voice grid: 2 columns x 5 rows per page, filled column-major
// (first 5 voices down the left column, next 5 down the right).
constexpr int kVoiceRows      = 5;
constexpr int kVoiceCols      = 2;
constexpr int kVoicesPerPage  = kVoiceRows * kVoiceCols;
constexpr int kVoiceTabW      = 46;  // fixed width of one P-tab (px)
constexpr int kVoiceNavBtnW   = 36;  // width of the prev / next buttons
constexpr int kVoicePageLblW  = 64;  // width of the "n/N" label
constexpr int kVoiceLeftW     = 260; // width of the category panel

// '&' in a QPushButton label is a mnemonic marker — escape it so
// names like "Piano&E.Piano" display literally.
inline QString escapeAmp(const QString& s) {
    QString out = s;
    out.replace(QStringLiteral("&"), QStringLiteral("&&"));
    return out;
}

// Delete every widget/spacer in a layout.
inline void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
}
} // namespace

void MainWindow::buildVoiceWindow() {
    // Overlay panel inside the main window (not a separate OS window).
    voice_window_ = new QWidget(this);
    voice_window_->setAttribute(Qt::WA_StyledBackground, true);
    voice_window_->setAttribute(Qt::WA_NoMousePropagation, true);
    voice_window_->setObjectName(QStringLiteral("voiceWindow"));
    voice_window_->setWindowTitle(QStringLiteral("PSRstudio — Voice Selector"));
    voice_window_->resize(860, 520);
    voice_window_->installEventFilter(this);
    voice_window_->setStyleSheet(QStringLiteral(
        "QWidget#voiceWindow { background-color: #0d1117; }"
        "QWidget#voiceHeader { background-color: #3d6fd8; }"
        "QLabel#voiceTitle { color: white; font-size: 16px; font-weight: bold; }"
        "QLabel#voicePath  { color: #8b949e; font-size: 11px; padding: 4px 10px; }"

        // Search box (header)
        "QLineEdit#voiceSearch {"
        "  background-color: rgba(255,255,255,0.18); color: white;"
        "  border: 1px solid rgba(255,255,255,0.35); border-radius: 13px;"
        "  padding: 2px 10px; font-size: 12px;"
        "  selection-background-color: #1f6feb;"
        "  placeholder-text-color: rgba(255,255,255,0.70);"
        "}"
        "QLineEdit#voiceSearch:focus {"
        "  background-color: rgba(255,255,255,0.28); border-color: #ffffff;"
        "}"
        "QLabel#voicePage  { color: #c9d1d9; font-size: 11px; padding: 0 6px; }"

        // Category buttons (left)
        "QPushButton#catBtn {"
        "  background-color: #21262d; color: #c9d1d9;"
        "  border: 1px solid #30363d; border-radius: 6px;"
        "  font-size: 12px; font-weight: bold; padding: 4px;"
        "}"
        "QPushButton#catBtn:hover { border-color: #58a6ff; }"
        "QPushButton#catBtn:checked {"
        "  background-color: #f0f3f6; color: #0d1117; border-color: #ffffff;"
        "}"

        // Voice buttons (right grid)
        "QPushButton#voiceBtn {"
        "  background-color: #161b22; color: #e6edf3;"
        "  border: 1px solid #30363d; border-radius: 4px;"
        "  font-size: 14px; text-align: left; padding: 6px 14px;"
        "}"
        "QPushButton#voiceBtn:hover { background-color: #21262d; border-color: #58a6ff; }"
        "QPushButton#voiceBtn:pressed { background-color: #1f6feb; }"
        "QPushButton#voiceBtn:checked {"
        "  background-color: #1f6feb; color: white; border-color: #58a6ff;"
        "}"

        // Page tabs (P1..P5)
        "QPushButton#pageTab {"
        "  background-color: #21262d; color: #8b949e;"
        "  border: 1px solid #30363d;"
        "  border-top-left-radius: 6px; border-top-right-radius: 6px;"
        "  min-width: 44px; min-height: 28px; font-weight: bold;"
        "}"
        "QPushButton#pageTab:hover { color: #c9d1d9; }"
        "QPushButton#pageTab:checked {"
        "  background-color: #f0f3f6; color: #0d1117; border-color: #ffffff;"
        "}"

        // Prev / next / close / reset
        "QPushButton#navBtn {"
        "  background-color: transparent; color: #c9d1d9; border: none;"
        "  font-size: 16px; min-width: 36px; min-height: 28px;"
        "}"
        "QPushButton#navBtn:hover { color: #58a6ff; }"
        "QPushButton#navBtn:disabled { color: #484f58; }"
        "QPushButton#closeBtn {"
        "  background-color: transparent; color: white; border: none;"
        "  font-size: 18px; min-width: 44px; min-height: 44px;"
        "}"
        "QPushButton#closeBtn:hover { background-color: #e81123; }"
        "QPushButton#resetBtn {"
        "  background-color: #21262d; color: #f0883e;"
        "  border: 1px solid #30363d; border-radius: 6px;"
        "  font-weight: bold; min-height: 36px;"
        "}"
        "QPushButton#resetBtn:hover { border-color: #f0883e; }"
    ));

    auto* root = new QVBoxLayout(voice_window_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ----- Header bar -----
    auto* header = new QWidget(voice_window_);
    header->setObjectName(QStringLiteral("voiceHeader"));
    header->setFixedHeight(44);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(14, 0, 0, 0);
    voice_title_label_ = new QLabel(QStringLiteral("Voice"), header);
    voice_title_label_->setObjectName(QStringLiteral("voiceTitle"));
    auto* close_btn = new QPushButton(QStringLiteral("\u2715"), header);
    close_btn->setObjectName(QStringLiteral("closeBtn"));
    close_btn->setCursor(Qt::PointingHandCursor);
    connect(close_btn, &QPushButton::clicked, this, [this]() {
        if (voice_window_) voice_window_->hide();
    });

    voice_search_edit_ = new QLineEdit(header);
    voice_search_edit_->setObjectName(QStringLiteral("voiceSearch"));
    voice_search_edit_->setPlaceholderText(QStringLiteral("Search voices\u2026"));
    voice_search_edit_->setClearButtonEnabled(true);
    voice_search_edit_->setFixedSize(190, 26);
    connect(voice_search_edit_, &QLineEdit::textChanged, this,
            [this](const QString& t) { applyVoiceSearch(t); });

    hl->addWidget(voice_title_label_);
    hl->addStretch(1);
    hl->addWidget(voice_search_edit_);
    hl->addSpacing(8);
    hl->addWidget(close_btn);
    root->addWidget(header);

    // ----- Body: categories (left) + voices (right) -----
    auto* body = new QHBoxLayout();
    body->setContentsMargins(10, 10, 10, 4);
    body->setSpacing(12);

    // Left: category buttons in a 2-column grid + Reset
    auto* left = new QWidget(voice_window_);
    left->setFixedWidth(kVoiceLeftW);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(8);

    voice_category_grid_ = new QGridLayout();
    voice_category_grid_->setSpacing(6);
    ll->addLayout(voice_category_grid_);
    ll->addStretch(1);

    auto* reset_btn = new QPushButton(QStringLiteral("\u21BA  Reset to style voice"), left);
    reset_btn->setObjectName(QStringLiteral("resetBtn"));
    reset_btn->setCursor(Qt::PointingHandCursor);
    connect(reset_btn, &QPushButton::clicked, this, [this]() {
        const int ch = voice_strip_idx_;
        if (ch >= 0 && ch < chmodel::kChannels) {
            // Hand the channel's voice back to the style: drop the user
            // lock, then re-apply the voice the style defines for it (if
            // it defines one — otherwise the channel simply keeps its
            // current voice and follows later style events again).
            model_.unlock(ch, chmodel::Param::Voice);
            picked_voice_idx_[static_cast<std::size_t>(ch)] = -1;
            if (const prs::PrsStyle* style = controller_.style()) {
                for (const auto& su : style->build_channel_setup(remap_)) {
                    if (su.ch == ch) {
                        model_.set_voice(ch, su.msb, su.lsb, su.program,
                                         chmodel::Source::Setup);
                    }
                }
            }
            refreshMixerFromModel(true);
        }
        if (voice_window_) voice_window_->hide();
    });
    ll->addWidget(reset_btn);

    // Build category buttons in order of first appearance in the CSV
    // (so you control the order by ordering voices.csv).
    voice_category_buttons_.clear();
    voice_category_names_.clear();
    for (const auto& v : voice_list_) {
        const QString cat = v.category.empty() ? QStringLiteral("Other")
                                               : QString::fromStdString(v.category);
        if (!voice_category_names_.contains(cat)) voice_category_names_.push_back(cat);
    }
    for (int i = 0; i < voice_category_names_.size(); ++i) {
        const QString& cat = voice_category_names_[i];
        auto* b = new QPushButton(escapeAmp(cat), left);
        b->setObjectName(QStringLiteral("catBtn"));
        b->setCheckable(true);
        b->setMinimumHeight(46);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, cat]() {
            onCategorySelected(cat);
        });
        voice_category_grid_->addWidget(b, i / 2, i % 2);
        voice_category_buttons_.push_back(b);
    }
    body->addWidget(left);

    // Right: 2x5 voice grid + pager row
    auto* right = new QVBoxLayout();
    right->setSpacing(6);

    voice_grid_ = new QGridLayout();
    voice_grid_->setSpacing(4);
    for (int r = 0; r < kVoiceRows; ++r) voice_grid_->setRowStretch(r, 1);
    for (int c = 0; c < kVoiceCols; ++c) voice_grid_->setColumnStretch(c, 1);
    right->addLayout(voice_grid_, 1);

    auto* pager = new QHBoxLayout();
    pager->setSpacing(2);
    voice_page_tabs_layout_ = new QHBoxLayout();
    voice_page_tabs_layout_->setSpacing(2);
    pager->addLayout(voice_page_tabs_layout_);
    pager->addStretch(1);

    voice_prev_btn_ = new QPushButton(QStringLiteral("\u25C0"), voice_window_);
    voice_prev_btn_->setObjectName(QStringLiteral("navBtn"));
    voice_prev_btn_->setFixedWidth(kVoiceNavBtnW);
    voice_next_btn_ = new QPushButton(QStringLiteral("\u25B6"), voice_window_);
    voice_next_btn_->setObjectName(QStringLiteral("navBtn"));
    voice_next_btn_->setFixedWidth(kVoiceNavBtnW);
    voice_page_label_ = new QLabel(QStringLiteral("1/1"), voice_window_);
    voice_page_label_->setObjectName(QStringLiteral("voicePage"));
    voice_page_label_->setFixedWidth(kVoicePageLblW);
    voice_page_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(voice_prev_btn_, &QPushButton::clicked, this, [this]() { setVoicePage(voice_page_ - 1); });
    connect(voice_next_btn_, &QPushButton::clicked, this, [this]() { setVoicePage(voice_page_ + 1); });
    pager->addWidget(voice_prev_btn_);
    pager->addWidget(voice_next_btn_);
    pager->addWidget(voice_page_label_);
    right->addLayout(pager);

    body->addLayout(right, 1);
    root->addLayout(body, 1);

    // ----- Footer path -----
    voice_path_label_ = new QLabel(QStringLiteral("Preset:/"), voice_window_);
    voice_path_label_->setObjectName(QStringLiteral("voicePath"));
    root->addWidget(voice_path_label_);
}

void MainWindow::showVoiceWindow(int strip_idx) {
    if (voice_list_.empty()) {
        showOverlay(QStringLiteral("No voices.csv found."), 6000);
        return;
    }
    if (strip_idx < 0 || strip_idx >= static_cast<int>(channel_strips_.size())) return;

    voice_strip_idx_ = strip_idx;

    if (!voice_window_) buildVoiceWindow();

    voice_title_label_->setText(QStringLiteral("Channel %1 Voice").arg(strip_idx + 1));

    // Size the overlay to fit inside the main window, then center it.
    // (Done before building the pages: the number of P-tabs depends on width.)
    {
        const int top = title_bar_ ? title_bar_->height() : 0;
        const int w = std::clamp(width()  - 60,        720, 860);
        const int h = std::clamp(height() - top - 60,  440, 520);
        voice_window_->resize(w, h);
        centerOverlay(voice_window_);
    }

    // ----- Work out which voice this channel is currently using -----
    // Always the MODEL's voice (what the synth really has), matched against
    // voices.csv by MSB/LSB/PC.
    const auto catOf = [](const auto& v) {
        return v.category.empty() ? QStringLiteral("Other")
                                  : QString::fromStdString(v.category);
    };

    voice_selected_idx_ = voiceIndexFor(strip_idx, model_.channel(strip_idx));

    // Open on that voice's category and page; if it isn't in the list,
    // fall back to the last-viewed category (or the first).
    QString open_cat;
    int open_page = 0;
    if (voice_selected_idx_ >= 0) {
        open_cat = catOf(voice_list_[static_cast<std::size_t>(voice_selected_idx_)]);
        int pos = 0;   // position of the voice within its category
        for (int i = 0; i < voice_selected_idx_; ++i) {
            if (catOf(voice_list_[static_cast<std::size_t>(i)]) == open_cat) ++pos;
        }
        open_page = pos / kVoicesPerPage;
    } else {
        open_cat = voice_current_category_;
        if (open_cat.isEmpty() && !voice_category_names_.isEmpty()) {
            open_cat = voice_category_names_.front();
        }
    }

    // Clear any previous search text without triggering a search.
    if (voice_search_edit_) {
        const bool was = voice_search_edit_->blockSignals(true);
        voice_search_edit_->clear();
        voice_search_edit_->blockSignals(was);
    }
    onCategorySelected(open_cat, open_page);

    voice_window_->show();
    voice_window_->raise();
}

void MainWindow::onCategorySelected(const QString& category, int page) {
    voice_current_category_ = category;

    // Picking a category leaves search mode (and clears the search box).
    voice_in_search_ = false;
    if (voice_search_edit_ && !voice_search_edit_->text().isEmpty()) {
        const bool was = voice_search_edit_->blockSignals(true);
        voice_search_edit_->clear();
        voice_search_edit_->blockSignals(was);
    }

    // Collect the voice indices belonging to this category.
    voice_category_indices_.clear();
    for (int i = 0; i < static_cast<int>(voice_list_.size()); ++i) {
        const auto& v = voice_list_[static_cast<std::size_t>(i)];
        const QString cat = v.category.empty() ? QStringLiteral("Other")
                                               : QString::fromStdString(v.category);
        if (cat == category) voice_category_indices_.push_back(i);
    }

    // Highlight the active category button.
    for (int i = 0; i < voice_category_buttons_.size(); ++i) {
        voice_category_buttons_[i]->setChecked(voice_category_names_[i] == category);
    }

    if (voice_path_label_) {
        voice_path_label_->setText(QStringLiteral("Preset:/%1/").arg(category));
    }

    setVoicePage(page);
}

// =========================================================
//  applyVoiceSearch — filter ALL voices (every category) by name.
//  Results use the same 2x5 paged grid. Clearing the box returns to
//  the category/page that was showing before the search started.
// =========================================================
void MainWindow::applyVoiceSearch(const QString& text) {
    const QString q = text.trimmed();

    if (q.isEmpty()) {
        if (voice_in_search_) {
            onCategorySelected(voice_current_category_, voice_page_before_search_);
        }
        return;
    }

    if (!voice_in_search_) {
        voice_in_search_ = true;
        voice_page_before_search_ = voice_page_;
    }

    voice_category_indices_.clear();
    for (int i = 0; i < static_cast<int>(voice_list_.size()); ++i) {
        const auto& v = voice_list_[static_cast<std::size_t>(i)];
        if (QString::fromStdString(v.name).contains(q, Qt::CaseInsensitive)) {
            voice_category_indices_.push_back(i);
        }
    }

    for (auto* b : voice_category_buttons_) b->setChecked(false);

    if (voice_path_label_) {
        voice_path_label_->setText(QStringLiteral("Search:/%1/   (%2 result%3)")
            .arg(q).arg(voice_category_indices_.size())
            .arg(voice_category_indices_.size() == 1 ? "" : "s"));
    }

    setVoicePage(0);
}

void MainWindow::setVoicePage(int page) {
    if (!voice_grid_) return;

    const int count = static_cast<int>(voice_category_indices_.size());
    const int total_pages = std::max(1, (count + kVoicesPerPage - 1) / kVoicesPerPage);
    page = std::clamp(page, 0, total_pages - 1);
    voice_page_ = page;

    // Rebuild the 2x5 grid for this page.
    clearLayout(voice_grid_);
    const int first = page * kVoicesPerPage;
    const int last  = std::min(first + kVoicesPerPage, count);

    for (int n = first; n < last; ++n) {
        const int idx = voice_category_indices_[static_cast<std::size_t>(n)];
        const auto& v = voice_list_[static_cast<std::size_t>(idx)];
        const int slot = n - first;
        const int row = slot % kVoiceRows;   // column-major fill
        const int col = slot / kVoiceRows;

        auto* btn = new QPushButton(escapeAmp(QString::fromStdString(v.name)));
        btn->setObjectName(QStringLiteral("voiceBtn"));
        btn->setCheckable(true);
        btn->setMinimumHeight(52);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(QStringLiteral("MSB=%1 LSB=%2 PC=%3").arg(v.msb).arg(v.lsb).arg(v.pc));
        if (idx == voice_selected_idx_) btn->setChecked(true);
        connect(btn, &QPushButton::clicked, this, [this, idx]() {
            onVoiceSelected(voice_strip_idx_, idx);
            if (voice_window_) voice_window_->hide();
        });
        voice_grid_->addWidget(btn, row, col);
    }

    // Keep the grid at a constant 5x2 size even on a partly-filled page.
    for (int r = 0; r < kVoiceRows; ++r) voice_grid_->setRowStretch(r, 1);
    for (int c = 0; c < kVoiceCols; ++c) voice_grid_->setColumnStretch(c, 1);

    // ----- Page tabs: as many as fit in the pager row -----
    // Pages are shown in sets (e.g. P1..P8, then P9..P16, ...). The set
    // always contains the current page, so stepping past the last tab
    // with the arrows flips to the next set automatically.
    constexpr int kTabGap   = 2;
    constexpr int kPagerPad = 12;   // minimum gap between tabs and the nav buttons
    const int right_w = voice_window_->width() - kVoiceLeftW - 12 /*body spacing*/ - 20 /*body margins*/;
    const int nav_w   = kVoiceNavBtnW * 2 + kVoicePageLblW + kTabGap * 4 + kPagerPad;
    const int tabs_per_set = std::max(1, (right_w - nav_w) / (kVoiceTabW + kTabGap));

    clearLayout(voice_page_tabs_layout_);
    const int tab_base = (page / tabs_per_set) * tabs_per_set;
    const int tab_end  = std::min(tab_base + tabs_per_set, total_pages);
    for (int p = tab_base; p < tab_end; ++p) {
        auto* tab = new QPushButton(QStringLiteral("P%1").arg(p + 1));
        tab->setObjectName(QStringLiteral("pageTab"));
        tab->setFixedWidth(kVoiceTabW);
        tab->setCheckable(true);
        tab->setChecked(p == page);
        tab->setCursor(Qt::PointingHandCursor);
        connect(tab, &QPushButton::clicked, this, [this, p]() { setVoicePage(p); });
        voice_page_tabs_layout_->addWidget(tab);
    }

    if (voice_prev_btn_) voice_prev_btn_->setEnabled(page > 0);
    if (voice_next_btn_) voice_next_btn_->setEnabled(page < total_pages - 1);
    if (voice_page_label_) {
        voice_page_label_->setText(QStringLiteral("%1/%2").arg(page + 1).arg(total_pages));
    }
}

void MainWindow::onVoiceSelected(int strip_idx, int voice_idx) {
    if (strip_idx < 0 || strip_idx >= chmodel::kChannels) return;
    if (voice_idx < 0 || voice_idx >= static_cast<int>(voice_list_.size())) return;

    const auto& v = voice_list_[static_cast<std::size_t>(voice_idx)];

    // The strip index IS the output MIDI channel. The model stores the
    // voice, sends bank MSB / LSB / PC to the connected device in one go,
    // and locks it, so the style's own CC#0 / CC#32 / PC events no longer
    // override the user's pick. v.msb / v.lsb / v.pc are raw MIDI values
    // (0-127) — used as-is.
    model_.set_voice(strip_idx, v.msb, v.lsb, v.pc, chmodel::Source::User);
    picked_voice_idx_[static_cast<std::size_t>(strip_idx)] = voice_idx;

    // Show it right away (the 200 ms timer would get there too).
    refreshMixerFromModel(true);

    std::cout << "[voices] Channel " << (strip_idx + 1)
              << " set to: " << v.name
              << " (" << v.msb << "/" << v.lsb << "/" << v.pc << ")\n";
}

void MainWindow::onVoiceWindowClosed() {
    // Nothing to do — the strip_idx is already stored
}

// =========================================================
//  Mixer — all 16 output channels, always
//
//  The Mixer is an overlay panel with exactly 16 vertical strips (one
//  per OUTPUT MIDI channel), built ONCE at startup. It fills the whole
//  window area below the title bar, edge to edge, and is purely a VIEW
//  and CONTROL surface for chmodel::ChannelModel:
//
//    control moved   -> ChannelModel::set_*(..., Source::User)
//                       (stores it, locks it against style events, and
//                        sends it to the connected device)
//    anything else   -> (style / MIDI-file events, XG SysEx, style load,
//                        remap change, ...) changes the model; every
//                        200 ms refreshMixerFromModel() copies the model
//                        into the widgets if its version changed
//
//      +------+
//      | M  S |   mute / solo (PSRstudio-side, effective volume 0)
//      | Voice|   <- voice name; click = voice selector
//      | Ch n |   <- vertical label: channel number + CASM part name
//      |  ||  |   <- vertical volume slider (CC#7)
//      |  64  |
//      |  ()  |   <- reverb dial (CC#91)
//      |  ()  |   <- chorus dial (CC#93, or CC#94 with the Montage redirect)
//      |  ()  |   <- pan dial (CC#10)
//      +------+
//
//  Expression and variation are tracked by the model but deliberately
//  have no controls here.
// =========================================================
namespace {

// Appearance of the mute / solo buttons when active. The inactive look
// comes from the mixer window's global QPushButton rule (an empty
// style sheet on the button reverts to it).
const QString& muteOnStyle() {
    static const QString s = QStringLiteral(
        "QPushButton {"
        "  background-color: #f85149;"
        "  color: #ffffff;"
        "  border: 1px solid #f85149;"
        "  border-radius: 4px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover { background-color: #da3633; }"
        "QPushButton:pressed { background-color: #b62324; }");
    return s;
}
const QString& soloOnStyle() {
    static const QString s = QStringLiteral(
        "QPushButton {"
        "  background-color: #d29922;"
        "  color: #0d1117;"
        "  border: 1px solid #d29922;"
        "  border-radius: 4px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover { background-color: #bb8009; }"
        "QPushButton:pressed { background-color: #9e6a03; }");
    return s;
}

// Pan value display: L..C..R
QString panText(int v) {
    if (v == 64) return QStringLiteral("C");
    if (v < 64)  return QStringLiteral("L%1").arg(64 - v);
    return QStringLiteral("R%1").arg(v - 64);
}

constexpr int kMixerMargin  = 8;    // around the strip row
constexpr int kMixerSpacing = 6;    // between strips
constexpr int kBtnH         = 26;   // mute / solo button height
constexpr int kSliderMinH   = 120;  // volume slider minimum height

} // namespace

void MainWindow::buildMixerStrips() {
    if (!mixer_layout_ || !mixer_container_) return;

    // Monospace font for value labels — gives a professional
    // DAW-like appearance where values are easy to read at a glance.
    const QString val_label_style = QStringLiteral(
        "color: #8b949e;"
        "font-family: 'Consolas', 'Courier New', monospace;"
        "font-size: 10px;"
        "font-weight: bold;"
        "background: transparent;"
        "border: none;"
    );
    const QString vol_val_style = QStringLiteral(
        "color: #58a6ff;"
        "font-family: 'Consolas', 'Courier New', monospace;"
        "font-size: 11px;"
        "font-weight: bold;"
        "background: transparent;"
        "border: none;"
    );

    for (int ch = 0; ch < chmodel::kChannels; ++ch) {
        ChannelStrip& strip = channel_strips_[static_cast<std::size_t>(ch)];

        // The strip column widget — owns all child widgets. Ignored width
        // policy + equal layout stretch = the 16 strips share the width
        // exactly equally, whatever their content.
        auto* strip_widget = new QWidget(mixer_container_);
        strip_widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        strip.strip_widget = strip_widget;
        // Dark panel with subtle border — gives each channel strip
        // a clear visual boundary like a hardware mixer channel.
        strip_widget->setStyleSheet(QStringLiteral(
            "background-color: #161b22;"
            "border: 1px solid #30363d;"
            "border-radius: 6px;"
        ));
        auto* strip_layout = new QVBoxLayout(strip_widget);
        strip_layout->setContentsMargins(4, 6, 4, 6);
        strip_layout->setSpacing(3);
        strip_layout->setAlignment(Qt::AlignTop);

        // M and S buttons at the TOP of the strip (side-by-side).
        auto* ms_row = new QHBoxLayout();
        ms_row->setSpacing(2);
        strip.mute_btn = new QPushButton(QStringLiteral("M"), strip_widget);
        strip.mute_btn->setCheckable(true);
        strip.mute_btn->setFixedSize(34, kBtnH);
        strip.mute_btn->setToolTip(QStringLiteral("Mute this channel (sends CC#7 = 0)"));
        strip.solo_btn = new QPushButton(QStringLiteral("S"), strip_widget);
        strip.solo_btn->setCheckable(true);
        strip.solo_btn->setFixedSize(34, kBtnH);
        strip.solo_btn->setToolTip(QStringLiteral(
            "Solo this channel. While any channel is soloed, all\n"
            "non-soloed channels are effectively muted."));
        ms_row->addWidget(strip.mute_btn);
        ms_row->addWidget(strip.solo_btn);
        strip_layout->addLayout(ms_row);

        // Voice label below the M/S buttons (the voice name, or the raw
        // MSB/LSB/PC when voices.csv doesn't know it). Clicking it opens
        // the voice selector. Its text is elided to the strip width in
        // refreshMixerFromModel().
        strip.label = new QLabel(QStringLiteral("-"), strip_widget);
        strip.label->setAlignment(Qt::AlignCenter);
        strip.label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        strip.label->setStyleSheet(QStringLiteral(
            "color: #58a6ff; font-weight: bold; font-size: 11px;"
            "background: transparent; border: none;"));
        strip.label->setToolTip(QStringLiteral(
            "MIDI channel %1 — click to select a voice").arg(ch + 1));
        strip.label->setTextInteractionFlags(Qt::NoTextInteraction);
        strip.label->setCursor(Qt::PointingHandCursor);
        strip_layout->addWidget(strip.label);

        // Wire label click → voice selector (see eventFilter). The strip
        // index IS the MIDI channel.
        strip.label->installEventFilter(this);
        strip.label->setProperty("strip_idx", ch);

        // Horizontal row: vertical label (channel name) + vertical volume
        // slider. Both stretch vertically. Widths are fine-tuned in
        // layoutMixerOverlay() to the available strip width.
        auto* slider_row = new QHBoxLayout();
        slider_row->setSpacing(0);

        auto* vlabel = new VerticalLabel(QStringLiteral("Ch %1").arg(ch + 1), strip_widget);
        vlabel->setMinimumHeight(kSliderMinH);
        vlabel->setToolTip(QStringLiteral("Channel %1").arg(ch + 1));
        strip.vlabel = vlabel;
        slider_row->addWidget(vlabel, 0, Qt::AlignLeft);

        // Vertical volume slider (0-127)
        strip.volume = new MixerSlider(Qt::Vertical, strip_widget);
        strip.volume->setRange(0, 127);
        strip.volume->setValue(64);
        strip.volume->setMinimumHeight(kSliderMinH);
        strip.volume->setToolTip(QStringLiteral("Volume (CC#7)"));
        // Center the slider in the strip: the vertical label sits at the
        // left edge and an invisible spacer of the SAME width mirrors it on
        // the right, so the slider lands in the middle of the strip.
        strip.volume->setFixedWidth(32);   // refined in layoutMixerOverlay()
        slider_row->addStretch(1);
        slider_row->addWidget(strip.volume, 0, Qt::AlignHCenter);
        slider_row->addStretch(1);
        slider_row->addSpacing(vlabel->sizeHint().width());

        strip_layout->addLayout(slider_row, 1);  // the slider row takes the vertical space

        // Volume value label
        strip.vol_val = new QLabel(QStringLiteral("64"), strip_widget);
        strip.vol_val->setAlignment(Qt::AlignCenter);
        strip.vol_val->setMinimumWidth(24);
        strip.vol_val->setStyleSheet(vol_val_style);
        strip_layout->addWidget(strip.vol_val);

        // Reverb dial + value
        strip.reverb = new MixerDial(strip_widget);
        strip.reverb->setRange(0, 127);
        strip.reverb->setValue(0);
        strip.reverb->setFixedSize(44, 44);   // refined in layoutMixerOverlay()
        strip.reverb->setToolTip(QStringLiteral("Reverb send (CC#91)"));
        strip_layout->addWidget(strip.reverb, 0, Qt::AlignCenter);
        strip.rev_val = new QLabel(QStringLiteral("0"), strip_widget);
        strip.rev_val->setAlignment(Qt::AlignCenter);
        strip.rev_val->setMinimumWidth(24);
        strip.rev_val->setStyleSheet(val_label_style);
        strip_layout->addWidget(strip.rev_val);

        // Chorus dial + value
        strip.chorus = new MixerDial(strip_widget);
        strip.chorus->setRange(0, 127);
        strip.chorus->setValue(0);
        strip.chorus->setFixedSize(44, 44);
        strip.chorus->setToolTip(QStringLiteral("Chorus send (CC#93)"));
        strip_layout->addWidget(strip.chorus, 0, Qt::AlignCenter);
        strip.cho_val = new QLabel(QStringLiteral("0"), strip_widget);
        strip.cho_val->setAlignment(Qt::AlignCenter);
        strip.cho_val->setMinimumWidth(24);
        strip.cho_val->setStyleSheet(val_label_style);
        strip_layout->addWidget(strip.cho_val);

        // Pan dial + value
        strip.pan = new MixerDial(strip_widget);
        strip.pan->setRange(0, 127);
        strip.pan->setValue(64);
        strip.pan->setFixedSize(44, 44);
        strip.pan->setToolTip(QStringLiteral("Pan (CC#10, 64 = center)"));
        strip_layout->addWidget(strip.pan, 0, Qt::AlignCenter);
        strip.pan_val = new QLabel(panText(64), strip_widget);
        strip.pan_val->setAlignment(Qt::AlignCenter);
        strip.pan_val->setMinimumWidth(24);
        strip.pan_val->setStyleSheet(val_label_style);
        strip_layout->addWidget(strip.pan_val);

        // Add a small stretch at the bottom so the strip doesn't spread out.
        strip_layout->addStretch(1);

        // ----- Wire up signals -----
        // `ch` is captured by value: strip index == MIDI channel, fixed for
        // the lifetime of the window (the strips are never rebuilt).
        // Every handler updates its own value label immediately and then
        // hands the change to the model as Source::User, which stores it,
        // locks it against style events and sends it to the device.
        // (refreshMixerFromModel() sets widgets with signals blocked, so it
        // never re-enters these handlers.)
        connect(strip.volume, &QSlider::valueChanged, this, [this, ch](int v) {
            auto& s = channel_strips_[static_cast<std::size_t>(ch)];
            if (s.vol_val) s.vol_val->setText(QString::number(v));
            model_.set_cc(ch, 7, v, chmodel::Source::User);
        });

        connect(strip.mute_btn, &QPushButton::clicked, this, [this, ch]() {
            auto& s = channel_strips_[static_cast<std::size_t>(ch)];
            const bool on = s.mute_btn->isChecked();
            s.mute_btn->setStyleSheet(on ? muteOnStyle() : QString());
            model_.set_mute(ch, on);
        });

        connect(strip.solo_btn, &QPushButton::clicked, this, [this, ch]() {
            auto& s = channel_strips_[static_cast<std::size_t>(ch)];
            const bool on = s.solo_btn->isChecked();
            s.solo_btn->setStyleSheet(on ? soloOnStyle() : QString());
            model_.set_solo(ch, on);
        });

        connect(strip.reverb, &QDial::valueChanged, this, [this, ch](int v) {
            auto& s = channel_strips_[static_cast<std::size_t>(ch)];
            if (s.rev_val) s.rev_val->setText(QString::number(v));
            model_.set_cc(ch, 91, v, chmodel::Source::User);
        });

        connect(strip.chorus, &QDial::valueChanged, this, [this, ch](int v) {
            auto& s = channel_strips_[static_cast<std::size_t>(ch)];
            if (s.cho_val) s.cho_val->setText(QString::number(v));
            model_.set_cc(ch, 93, v, chmodel::Source::User);
        });

        connect(strip.pan, &QDial::valueChanged, this, [this, ch](int v) {
            auto& s = channel_strips_[static_cast<std::size_t>(ch)];
            if (s.pan_val) s.pan_val->setText(panText(v));
            model_.set_cc(ch, 10, v, chmodel::Source::User);
        });

        // Equal share of the width for every strip.
        mixer_layout_->addWidget(strip_widget, 1);
    }
}

// =========================================================
//  layoutMixerOverlay — the mixer fills the window area below the
//  title bar, edge to edge, and its contents adapt to the width.
//
//  The central widget fills the whole window, so the overlay is simply
//  (0, title bar height) .. (window width, window height). The 16
//  strips share the width equally (layout stretch); here we only size
//  the fixed-size controls (dials, M/S buttons, slider width) to what
//  one strip can afford, so 16 strips fit without scrolling even on a
//  narrow screen.
// =========================================================
void MainWindow::layoutMixerOverlay() {
    if (!mixer_window_) return;

    const int top = title_bar_ ? title_bar_->height() : 0;
    mixer_window_->setGeometry(0, top, width(), std::max(0, height() - top));

    const int avail_w = width() - 2 * kMixerMargin
                        - (chmodel::kChannels - 1) * kMixerSpacing;
    const int strip_w = std::max(40, avail_w / chmodel::kChannels);
    // inside a strip: 4 px layout margins + 1 px border on each side
    const int inner_w = std::max(30, strip_w - 10);

    const int dial_px = std::clamp(inner_w - 6, 28, 48);
    const int btn_w   = std::clamp((inner_w - 2) / 2, 18, 34);

    for (auto& s : channel_strips_) {
        if (!s.strip_widget) continue;
        s.reverb->setFixedSize(dial_px, dial_px);
        s.chorus->setFixedSize(dial_px, dial_px);
        s.pan->setFixedSize(dial_px, dial_px);
        s.mute_btn->setFixedSize(btn_w, kBtnH);
        s.solo_btn->setFixedSize(btn_w, kBtnH);

        // slider row = [vertical label][slider, centred][mirror spacer]
        const int vlw = s.vlabel->sizeHint().width();
        s.volume->setFixedWidth(std::clamp(inner_w - 2 * vlw - 2, 24, 32));
    }

    mixer_strip_inner_w_ = std::max(20, inner_w);

    // Label elision depends on the strip width.
    if (mixer_ever_refreshed_) refreshMixerFromModel(true);
}

// =========================================================
//  rebuildPartNames — CASM part names per OUTPUT channel
//
//  Uses the first CSEG (like the channel setup) and the same routing
//  as playback (CASM dst_ch, or the Montage fold into 1-8), so the
//  name shown next to "Ch N" is the part that really plays there.
// =========================================================
void MainWindow::rebuildPartNames() {
    for (auto& n : part_names_) n.clear();

    const prs::PrsStyle* style = controller_.style();
    if (!style) return;

    const auto& casm = style->casm();
    if (casm.csegs.empty()) return;
    for (const auto& cc : casm.csegs[0].channels) {
        const int out = remap_.route_channel(cc.src_ch, cc.dst_ch) & 0x0F;
        part_names_[static_cast<std::size_t>(out)] = cc.part_name;
    }
}

// =========================================================
//  voiceIndexFor — which voices.csv entry is channel `ch` using?
//
//  Matched by MSB / LSB / PC against the model's voice, via the
//  prebuilt voice_by_addr_ hash map (O(1) lookup). If several entries
//  share the same address, the one the user last picked for this
//  channel wins (when it still matches); otherwise the first in CSV
//  order.
// =========================================================
int MainWindow::voiceIndexFor(int ch, const chmodel::Channel& c) const {
    const auto key = static_cast<std::uint32_t>(
        (c.msb << 16) | (c.lsb << 8) | c.program);
    const auto it = voice_by_addr_.find(key);
    if (it == voice_by_addr_.end()) return -1;
    const std::vector<int>& candidates = it->second;

    if (ch >= 0 && ch < chmodel::kChannels) {
        const int pi = picked_voice_idx_[static_cast<std::size_t>(ch)];
        if (pi >= 0 && pi < static_cast<int>(voice_list_.size()) &&
            std::find(candidates.begin(), candidates.end(), pi) != candidates.end()) {
            return pi;
        }
    }
    return candidates.front();
}

// =========================================================
//  refreshMixerFromModel — model -> widgets
//
//  Runs every 200 ms (and immediately after style load, remap change,
//  voice pick, ...). With force=false it is a single atomic compare
//  unless the model changed. All widget updates are done with signals
//  BLOCKED, so refreshing never sends MIDI and never marks a control
//  as user-touched. A control the user is dragging is skipped.
// =========================================================
void MainWindow::refreshMixerFromModel(bool force) {
    if (!mixer_window_) return;
    if (!force && mixer_ever_refreshed_ && model_.version() == mixer_last_version_) return;

    const chmodel::Snapshot snap = model_.snapshot();
    mixer_last_version_   = snap.version;
    mixer_ever_refreshed_ = true;

    // Set a slider / dial without emitting valueChanged.
    const auto set_value = [](QAbstractSlider* w, int v) {
        if (!w || w->isSliderDown() || w->value() == v) return;
        const QSignalBlocker blocker(w);
        w->setValue(v);
    };
    // Set a label's text only when it actually changed — unconditional
    // setText() would invalidate/repaint all 64 value labels on every
    // refresh (and on every mixer open) even when nothing changed.
    const auto set_text = [](QLabel* l, const QString& t) {
        if (!l || l->text() == t) return;
        l->setText(t);
    };
    // Set a checkable M / S button (and its look) without emitting.
    const auto set_toggle = [](QPushButton* b, bool on, const QString& on_style) {
        if (!b || b->isChecked() == on) return;
        const QSignalBlocker blocker(b);
        b->setChecked(on);
        b->setStyleSheet(on ? on_style : QString());
    };

    // Master volume (lives in the transport row).
    if (master_volume_slider_) {
        set_value(master_volume_slider_, snap.master_volume);
    }
    if (master_volume_label_) {
        master_volume_label_->setText(QStringLiteral("Master: %1").arg(snap.master_volume));
    }

    for (int ch = 0; ch < chmodel::kChannels; ++ch) {
        ChannelStrip& s = channel_strips_[static_cast<std::size_t>(ch)];
        const chmodel::Channel& c = snap.ch[static_cast<std::size_t>(ch)];
        if (!s.strip_widget) continue;

        set_value(s.volume, c.volume);
        set_value(s.reverb, c.reverb);
        set_value(s.chorus, c.chorus);
        set_value(s.pan,    c.pan);

        set_text(s.vol_val, QString::number(c.volume));
        set_text(s.rev_val, QString::number(c.reverb));
        set_text(s.cho_val, QString::number(c.chorus));
        set_text(s.pan_val, panText(c.pan));

        set_toggle(s.mute_btn, c.muted,  muteOnStyle());
        set_toggle(s.solo_btn, c.soloed, soloOnStyle());

        // Vertical label: channel number + the style's part name.
        QString v = QStringLiteral("Ch %1").arg(ch + 1);
        const std::string& part = part_names_[static_cast<std::size_t>(ch)];
        if (!part.empty()) v += QStringLiteral(": ") + QString::fromStdString(part);
        static_cast<VerticalLabel*>(s.vlabel)->setText(v);

        // Voice label: name from voices.csv, else the raw bank / program.
        const int vi = voiceIndexFor(ch, c);
        const QString full = (vi >= 0)
            ? QString::fromStdString(voice_list_[static_cast<std::size_t>(vi)].name)
            : QStringLiteral("%1/%2/%3").arg(c.msb).arg(c.lsb).arg(c.program);
        const QString shown = s.label->fontMetrics().elidedText(
            full, Qt::ElideRight, mixer_strip_inner_w_);
        if (s.label->text() != shown) {
            s.label->setText(shown);
            s.label->setToolTip(QStringLiteral(
                "MIDI channel %1 — %2\nMSB %3  LSB %4  PC %5\nClick to select a voice")
                .arg(ch + 1).arg(full).arg(c.msb).arg(c.lsb).arg(c.program));
        }
    }
}

// =========================================================
//  centerOverlay — center a child overlay panel inside the main
//  window's client area (below the custom title bar).
// =========================================================
void MainWindow::centerOverlay(QWidget* w) {
    if (!w) return;
    const int top = title_bar_ ? title_bar_->height() : 0;
    const int x = std::max(0, (width() - w->width()) / 2);
    const int y = std::max(top, top + (height() - top - w->height()) / 2);
    w->move(x, y);
}

// =========================================================
//  onMasterVolumeChanged — global master volume slider
//
//  The model scales every channel's volume by this percentage before
//  sending (and re-sends CC#7 on all 16 channels).
// =========================================================
void MainWindow::onMasterVolumeChanged(int value) {
    if (master_volume_label_) {
        master_volume_label_->setText(QStringLiteral("Master: %1").arg(value));
    }
    model_.set_master_volume(value);
}

// =========================================================
//  Mixer window show / hide
// =========================================================
void MainWindow::onMixerToggled(bool checked) {
    if (!mixer_window_) return;
    if (checked) {
        layoutMixerOverlay();
        // The 33 ms refresh timer keeps the strips in sync even while
        // the mixer is hidden, so a forced refresh is only needed when
        // the model actually changed since the last refresh (or the
        // strips have never been filled).
        if (!mixer_ever_refreshed_ || model_.version() != mixer_last_version_)
            refreshMixerFromModel(true);
        mixer_window_->show();
        mixer_window_->raise();
    } else {
        mixer_window_->hide();
    }
}

// =========================================================
//  eventFilter — watches the Mixer window's Close event so we can
//  uncheck the Mixer button when the user closes the window via X.
//  Without this, the Mixer button would stay checked after the user
//  closes the window via X, which would be confusing.
// =========================================================
bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == mixer_window_ && event->type() == QEvent::Close) {
        if (mixer_button_) mixer_button_->setChecked(false);
    }
    // Handle channel label clicks → voice selector window
    if (event->type() == QEvent::MouseButtonPress) {
        auto* label = qobject_cast<QLabel*>(obj);
        if (label) {
            QVariant v = label->property("strip_idx");
            if (v.isValid()) {
                int strip_idx = v.toInt();
                showVoiceWindow(strip_idx);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// =========================================================
//  MIDI input connect / disconnect
// =========================================================
void MainWindow::onConnectMidiIn() {
    if (midi_in_open_) {
        midi_in_.close();
        midi_in_open_ = false;
        midi_in_status_->setText(QStringLiteral("Not connected"));
        return;
    }

    const int sel = midi_in_combo_->currentIndex();
    if (sel < 0) {
        showOverlay(QStringLiteral("No MIDI input port selected."), 6000);
        return;
    }
    const int port = midi_in_combo_->itemData(sel).toInt();

    if (midi_in_.open(port, chord_mode_, 0xFFFF)) {
        midi_in_open_ = true;
        midi_in_status_->setText(QString::fromUtf8(midi_in_.port_name().c_str()));
    } else {
        showOverlay(QStringLiteral("Failed to open MIDI input port."), 6000);
        midi_in_status_->setText(QStringLiteral("Error"));
    }
}

// =========================================================
//  File loading
// =========================================================
void MainWindow::onLoadClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Style or MIDI File"), QString(),
        QStringLiteral("Yamaha Styles & MIDI Files (*.sty *.prs *.bcs *.sst *.pst *.mid);;"
                        "All Files (*.*)"));
    if (path.isEmpty()) return;
    loadFile(path);
}

// Load a style or MIDI file (the part of "Load Style / MIDI..." after the
// file dialog).
void MainWindow::loadFile(const QString& path) {
    last_style_path_ = path;   // remembered for the startup auto-load
    const std::string stdPath = path.toStdString();
    status_label_->setText(QStringLiteral("Loading..."));
    qApp->processEvents();

    const bool isMidi = path.endsWith(QStringLiteral(".mid"), Qt::CaseInsensitive);

    if (isMidi) {
        if (controller_.load_midi_file(stdPath)) {
            const auto* info = controller_.midi_info();
            status_label_->setText(QStringLiteral("MIDI: %1  (%2s, %3 bars)")
                .arg(QString::fromStdString(info->filename))
                .arg(info->duration_sec, 0, 'f', 1)
                .arg(controller_.midi_player_obj().total_bars()));
            // A new file: forget which parameters the user had touched so
            // the file's own CC / PC events apply. The channel VALUES stay
            // as they are; the file's setup events (sent when playback
            // starts) update the model and the mixer.
            model_.clear_locks();
        } else {
            status_label_->setText(QStringLiteral("Error loading MIDI file."));
        }
    } else {
        if (controller_.load_style(stdPath)) {
            // 1. The style's header SysEx (XG effect types, drum setup...)
            //    — it may reset the synth, so it goes out BEFORE step 2.
            if (active_midi_out_) {
                controller_.style()->send_header_sysex(*active_midi_out_);
            }
            // 2. The style's channel state goes into the model, which
            //    clears all user locks and dumps ALL 16 channels to the
            //    device (if connected), so both stay in sync.
            applyStyleToModel(/*full_reset=*/false);

            // 2b. MegaVoice detection — warn that articulations will
            //     sound odd on unsupported hardware/software. Triggered
            //     when any style voice uses a MegaVoice bank (MSB=8,
            //     LSB 0-16).
            bool mega = std::any_of(controller_.style()->voices().begin(),
                                    controller_.style()->voices().end(),
                [](const prs::ChVoice& v) {
                    return v.valid && transpose::megavoice::is_mega_bank(v.bank_msb,
                                                                          v.bank_lsb);
                });
            if (mega) {
                const QString msg = QStringLiteral(
                    "MegaVoices detected. MegaVoices will produce odd-sounding "
                    "high octave or odd velocity notes on unsupported "
                    "hardware/software.");
                showOverlay(msg, 3000);
            }

            int bpm = static_cast<int>(60000000.0 / controller_.style()->tempo_us() + 0.5);
            if (bpm < player::kTempoBpmMin) bpm = player::kTempoBpmMin;
            if (bpm > player::kTempoBpmMax) bpm = player::kTempoBpmMax;

            suppress_tempo_signal_ = true;
            tempo_spin_->setValue(bpm);
            suppress_tempo_signal_ = false;
            player::g_ui_tempo_bpm.store(bpm, std::memory_order_relaxed);

            setSelectedMainVisual(controller_.selected_main());

            const auto name = controller_.style()->style_name();
            const QString displayName = name.empty()
                ? path
                : QString::fromUtf8(name.data(), static_cast<int>(name.size()));
            status_label_->setText(QStringLiteral("%1  (%2 sections)")
                .arg(displayName)
                .arg(controller_.style()->sections().size()));
        } else {
            status_label_->setText(QStringLiteral("Error loading style."));
        }
    }

    updateSectionButtons();
    updateTransportButtons();
    updatePositionDisplay();
    // The mixer always shows all 16 channels; refresh its labels (part
    // names come from the loaded style, none for a MIDI file).
    rebuildPartNames();
    refreshMixerFromModel(true);
}

// =========================================================
//  Auto-fill / transport
// =========================================================
void MainWindow::onAutoFillToggled() {
    // State-driven (not flip-style): derives the new state from the
    // button itself, so programmatic setChecked() from loadSettings()
    // syncs the engine correctly, not just real clicks.
    const bool newVal = autofill_button_->isChecked();
    controller_.set_auto_fill(newVal);
    autofill_button_->setText(newVal ? QStringLiteral("Auto Fill: ON")
                                     : QStringLiteral("Auto Fill: OFF"));
}

void MainWindow::onPlayClicked() {
    if (controller_.is_playing()) {
        // Stop playback
        controller_.request_stop();
    } else {
        // Start playback
        if (active_midi_out_ && controller_.is_loaded()) {
            controller_.start_playback();
        }
    }
    updateTransportButtons();
}

void MainWindow::onStopClicked() {
    controller_.request_stop();
}

void MainWindow::onTempoChanged(int bpm) {
    if (suppress_tempo_signal_) return;
    if (bpm >= player::kTempoBpmMin && bpm <= player::kTempoBpmMax) {
        player::g_ui_tempo_bpm.store(bpm, std::memory_order_relaxed);
    }
}

// =========================================================
//  Section button handling
// =========================================================
void MainWindow::onMainButtonClicked(int main_index) {
    if (!controller_.style()) return;
    const auto s = controller_.style()->find_section(kMainNames[main_index]);
    if (!s) return;
    const int idx = static_cast<int>(*s);

    if (!controller_.is_playing()) {
        if (idx != controller_.selected_main()) {
            controller_.set_selected_main(idx);
            controller_.clear_queue();
        } else {
            controller_.toggle_queue_fill();
        }
        status_label_->setText(QString::fromStdString(controller_.queue_status_text()));
    } else {
        controller_.set_selected_main(idx);
        controller_.request_section(idx);
        status_label_->setText(QStringLiteral("Queued: %1").arg(kMainNames[main_index]));
    }
    setSelectedMainVisual(idx);
}

void MainWindow::onIntroButtonClicked(int intro_index) {
    if (!controller_.style()) return;
    const auto s = controller_.style()->find_section(kIntroNames[intro_index]);
    if (!s) return;
    const int idx = static_cast<int>(*s);

    if (!controller_.is_playing()) {
        controller_.set_queue_intro(idx);
        status_label_->setText(QString::fromStdString(controller_.queue_status_text()));
    } else {
        controller_.request_section(idx);
        status_label_->setText(QStringLiteral("Queued: %1").arg(kIntroNames[intro_index]));
    }
}

void MainWindow::onEndingButtonClicked(int ending_index) {
    if (!controller_.style()) return;
    const auto s = controller_.style()->find_section(kEndingNames[ending_index]);
    if (!s) return;
    const int idx = static_cast<int>(*s);

    if (!controller_.is_playing()) {
        controller_.set_queue_ending(idx);
        status_label_->setText(QString::fromStdString(controller_.queue_status_text()));
    } else {
        controller_.request_section(idx);
        status_label_->setText(QStringLiteral("Queued: %1").arg(kEndingNames[ending_index]));
    }
}

void MainWindow::onBreakButtonClicked() {
    if (!controller_.style()) return;
    const auto s = controller_.style()->find_section(kBreakName);
    if (!s) return;
    const int idx = static_cast<int>(*s);

    if (!controller_.is_playing()) {
        controller_.toggle_queue_break();
        status_label_->setText(QString::fromStdString(controller_.queue_status_text()));
    } else {
        controller_.request_section(idx);
        status_label_->setText(QStringLiteral("Break queued"));
    }
}

// =========================================================
//  Playback-finished notification
// =========================================================
void MainWindow::onPlaybackFinished() {
    updateTransportButtons();
    updateSectionButtons();
    updatePositionDisplay();
}

// =========================================================
//  Timer poll
// =========================================================
void MainWindow::onTimer() {
    // (Chord-window ticking now runs on chord_timer_ at 5 ms.)
    updateChordDisplay();

    if (controller_.is_playing()) {
        updatePositionDisplay();
        // Highlight the currently-playing section button
        updateSectionHighlight();
    } else {
        // When not playing, still update highlight to show pre-start queue state
        updateSectionHighlight();
    }
}

// =========================================================
//  Display update helpers
// =========================================================
void MainWindow::updateChordDisplay() {
    int root = -1;
    int bass = -1;
    chord::ChordType ct = chord::ChordType::None;
    // (void): when no chord is held, load() writes root=-1 / None /
    // bass=-1 and formatChord renders that as the empty state — the
    // bool isn't needed.
    (void)chord::g_active_chord.load(root, ct, bass);
    const int type = static_cast<int>(ct);
    chord_label_->setText(formatChord(root, type, bass));
}

// =========================================================
//  Split Point picker
// =========================================================
void MainWindow::onSplitPointClicked() {
    // Second press while waiting cancels the capture.
    if (split_capture_armed_) {
        midi_in_.cancel_capture();
        split_capture_armed_ = false;
        updateSplitButtonText();
        return;
    }

    if (!midi_in_open_) {
        split_button_->setText(QStringLiteral("Split: no MIDI in!"));
        // Restore the normal label shortly; the flag stays false so the
        // restore is skipped if the user manages to arm in the meantime.
        QTimer::singleShot(1500, this, [this]() {
            if (!split_capture_armed_) updateSplitButtonText();
        });
        return;
    }

    midi_in_.arm_split_capture();
    split_capture_armed_ = true;
    split_button_->setText(QStringLiteral("Split: press a key..."));
}

void MainWindow::updateSplitButtonText() {
    const int sp = chord::g_split_point.load(std::memory_order_relaxed);
    const int pc = ((sp % 12) + 12) % 12;
    const auto name = chord::kNoteNames[static_cast<std::size_t>(pc)];
    split_button_->setText(QStringLiteral("Split: %1%2 (%3)")
        .arg(QString::fromUtf8(name.data(), static_cast<int>(name.size())))
        .arg(sp / 12 - 1)   // octave (C-1 convention: MIDI 60 = C4)
        .arg(sp));
}

// =========================================================
//  Chord recognition mode
// =========================================================
void MainWindow::onChordModeChanged(int index) {
    // Order matches the combo items created in buildUi().
    static constexpr chord::InputMode kModes[5] = {
        chord::InputMode::SingleFinger,
        chord::InputMode::MultiFinger,
        chord::InputMode::Fingered,
        chord::InputMode::FingeredOnBass,
        chord::InputMode::FullKeyboard,
    };
    if (index < 0 || index > 4) return;
    chord_mode_ = kModes[index];
    midi_in_.engine().set_mode(chord_mode_);
}

void MainWindow::updatePositionDisplay() {
    if (controller_.mode() == playback::Mode::MidiFile) {
        const auto* info = controller_.midi_info();
        section_label_->setText(info ? QString::fromStdString(info->filename)
                                      : QStringLiteral("MIDI"));

        const int measure = controller_.midi_player_obj().current_measure();
        const int total = controller_.midi_player_obj().total_bars();
        const int beat = controller_.midi_player_obj().current_beat();
        const int ts_num = info ? info->ts_num : 4;

        measure_label_->setText(QStringLiteral("Measure %1 / %2")
            .arg(measure).arg(total > 0 ? total : 1));
        beat_label_->setText(QStringLiteral("Beat %1 / %2").arg(beat).arg(ts_num));
        return;
    }

    const int sec_idx = player::g_pos_section_idx.load(std::memory_order_relaxed);
    const int measure = player::g_pos_measure.load(std::memory_order_relaxed);
    const int beat = player::g_pos_beat.load(std::memory_order_relaxed);
    const int total_bars = player::g_pos_total_bars.load(std::memory_order_relaxed);

    if (sec_idx < 0 || !controller_.style()) {
        section_label_->setText(QStringLiteral("-"));
        measure_label_->setText(QStringLiteral("Measure - / -"));
        beat_label_->setText(QStringLiteral("Beat - / -"));
        return;
    }

    const auto& sections = controller_.style()->sections();
    if (static_cast<std::size_t>(sec_idx) < sections.size()) {
        section_label_->setText(QString::fromStdString(sections[static_cast<std::size_t>(sec_idx)].name));
    } else {
        section_label_->setText(QStringLiteral("?"));
    }

    if (total_bars > 0) {
        measure_label_->setText(QStringLiteral("Measure %1 / %2").arg(measure).arg(total_bars));
    } else {
        measure_label_->setText(QStringLiteral("Measure %1").arg(measure));
    }

    const int ts_num = controller_.style()->ts_num();
    beat_label_->setText(QStringLiteral("Beat %1 / %2").arg(beat).arg(ts_num));
}

void MainWindow::updateSectionButtons() {
    const bool hasStyle = controller_.style() != nullptr;
    const bool notMidiMode = controller_.mode() != playback::Mode::MidiFile;
    const bool enableBase = hasStyle && notMidiMode;

    auto setEnabled = [&](QPushButton* btn, const char* section_name) {
        if (!btn) return;
        const bool exists = hasStyle && controller_.style()->find_section(section_name).has_value();
        btn->setEnabled(exists && notMidiMode);
    };

    for (int i = 0; i < 3; ++i) setEnabled(intro_buttons_[i], kIntroNames[i]);
    for (int i = 0; i < 4; ++i) setEnabled(main_buttons_[i], kMainNames[i]);
    for (int i = 0; i < 3; ++i) setEnabled(ending_buttons_[i], kEndingNames[i]);
    setEnabled(break_button_, kBreakName);

    autofill_button_->setEnabled(enableBase);
    tempo_spin_->setEnabled(enableBase);
}

void MainWindow::updateTransportButtons() {
    const bool playing = controller_.is_playing();
    const bool hasOutput = active_midi_out_ != nullptr;
    const bool hasStyle = controller_.is_loaded();

    if (playing) {
        play_button_->setText(QStringLiteral("Stop"));
        play_button_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #f44336; color: white; font-weight: bold; }"));
    } else {
        play_button_->setText(QStringLiteral("Start"));
        play_button_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #4CAF50; color: white; font-weight: bold; }"));
    }
    play_button_->setEnabled(hasOutput && hasStyle);
}

void MainWindow::setSelectedMainVisual(int idx) {
    if (!controller_.style()) return;
    for (int i = 0; i < 4; ++i) {
        if (!main_buttons_[i]) continue;
        const auto s = controller_.style()->find_section(kMainNames[i]);
        if (!s) continue;
        const int sec_idx = static_cast<int>(*s);
        main_buttons_[i]->setChecked(sec_idx == idx);
    }
}

void MainWindow::updateSectionHighlight() {
    if (!controller_.style()) return;
    const int sec_idx = player::g_pos_section_idx.load(std::memory_order_relaxed);
    const int req_sec = player::g_requested_sec.load(std::memory_order_relaxed);
    const auto& sections = controller_.style()->sections();

    // Color constants
    const QString blueStyle = QStringLiteral(
        "QPushButton { background-color: #2196F3; color: white; font-weight: bold; }");
    const QString orangeStyle = QStringLiteral(
        "QPushButton { background-color: #FF9800; color: white; font-weight: bold; }");
    const QString normalStyle = QStringLiteral("");

    // Clear all button styles first
    for (int i = 0; i < 4; ++i) {
        if (main_buttons_[i]) main_buttons_[i]->setStyleSheet(normalStyle);
    }
    for (int i = 0; i < 3; ++i) {
        if (intro_buttons_[i]) intro_buttons_[i]->setStyleSheet(normalStyle);
        if (ending_buttons_[i]) ending_buttons_[i]->setStyleSheet(normalStyle);
    }
    if (break_button_) break_button_->setStyleSheet(normalStyle);

    // Get current and requested section names
    std::string cur_name;
    if (sec_idx >= 0 && static_cast<std::size_t>(sec_idx) < sections.size()) {
        cur_name = sections[static_cast<std::size_t>(sec_idx)].name;
    }
    std::string req_name;
    if (req_sec >= 0 && static_cast<std::size_t>(req_sec) < sections.size()) {
        req_name = sections[static_cast<std::size_t>(req_sec)].name;
    }

    // Determine if the currently-playing section is a fill, and which Main it belongs to
    // NOTE: Break ("Fill In BA") is NOT a fill for Main A — it returns to whichever
    // Main was playing. So we exclude Break from the fill detection.
    bool cur_is_fill = false;
    int cur_fill_main = -1;  // index 0-3 for Main A-D
    bool cur_is_break = false;
    if (!cur_name.empty()) {
        if (cur_name == kBreakName) {
            cur_is_break = true;
        } else {
            for (int i = 0; i < 4; ++i) {
                if (cur_name == kFillNames[i]) {
                    cur_is_fill = true;
                    cur_fill_main = i;
                    break;
                }
            }
        }
    }

    // Determine if the requested section is a fill
    bool req_is_fill = false;
    int req_fill_main = -1;
    bool req_is_break = false;
    if (!req_name.empty()) {
        if (req_name == kBreakName) {
            req_is_break = true;
        } else {
            for (int i = 0; i < 4; ++i) {
                if (req_name == kFillNames[i]) {
                    req_is_fill = true;
                    req_fill_main = i;
                    break;
                }
            }
        }
    }

    // === BLUE: actively playing ===
    // Main buttons: blue when their Main section is playing
    // Also blue when Break is playing (the Main stays active during Break)
    for (int i = 0; i < 4; ++i) {
        if (!main_buttons_[i]) continue;
        if (cur_name == kMainNames[i]) {
            main_buttons_[i]->setStyleSheet(blueStyle);
        }
        // When Break is playing, the selected Main stays blue
        if (cur_is_break) {
            int sel_main = player::g_selected_main.load(std::memory_order_relaxed);
            const auto s = controller_.style()->find_section(kMainNames[i]);
            if (s && static_cast<int>(*s) == sel_main) {
                main_buttons_[i]->setStyleSheet(blueStyle);
            }
        }
    }
    // Intro buttons: blue when their Intro is playing
    for (int i = 0; i < 3; ++i) {
        if (!intro_buttons_[i]) continue;
        if (cur_name == kIntroNames[i]) {
            intro_buttons_[i]->setStyleSheet(blueStyle);
        }
    }
    // Ending buttons: blue when their Ending is playing
    for (int i = 0; i < 3; ++i) {
        if (!ending_buttons_[i]) continue;
        if (cur_name == kEndingNames[i]) {
            ending_buttons_[i]->setStyleSheet(blueStyle);
        }
    }

    // === ORANGE: queued or fill ===
    // Main buttons: orange when a fill for that Main is playing or queued
    for (int i = 0; i < 4; ++i) {
        if (!main_buttons_[i]) continue;
        // If currently playing a fill for this Main
        if (cur_is_fill && cur_fill_main == i) {
            main_buttons_[i]->setStyleSheet(orangeStyle);
        }
        // If a fill for this Main is queued (requested)
        if (req_is_fill && req_fill_main == i) {
            main_buttons_[i]->setStyleSheet(orangeStyle);
        }
        // If this Main is queued (requested) and not currently playing
        if (!req_name.empty() && req_name == kMainNames[i] && cur_name != kMainNames[i]) {
            main_buttons_[i]->setStyleSheet(orangeStyle);
        }
    }

    // Intro buttons: orange when queued (before or during playback)
    for (int i = 0; i < 3; ++i) {
        if (!intro_buttons_[i]) continue;
        // If this Intro is queued
        if (!req_name.empty() && req_name == kIntroNames[i] && cur_name != kIntroNames[i]) {
            intro_buttons_[i]->setStyleSheet(orangeStyle);
        }
        // Also check the pre-start queue state
        if (!controller_.is_playing()) {
            if (controller_.queue_kind() == playback::QueueKind::Intro &&
                controller_.queue_index() >= 0) {
                const auto& qsec = sections[static_cast<std::size_t>(controller_.queue_index())];
                if (qsec.name == kIntroNames[i]) {
                    intro_buttons_[i]->setStyleSheet(orangeStyle);
                }
            }
        }
    }

    // Ending buttons: orange when queued (before or during playback)
    for (int i = 0; i < 3; ++i) {
        if (!ending_buttons_[i]) continue;
        // If this Ending is queued
        if (!req_name.empty() && req_name == kEndingNames[i] && cur_name != kEndingNames[i]) {
            ending_buttons_[i]->setStyleSheet(orangeStyle);
        }
        // Also check the pre-start queue state
        if (!controller_.is_playing()) {
            if (controller_.queue_kind() == playback::QueueKind::Ending &&
                controller_.queue_index() >= 0) {
                const auto& qsec = sections[static_cast<std::size_t>(controller_.queue_index())];
                if (qsec.name == kEndingNames[i]) {
                    ending_buttons_[i]->setStyleSheet(orangeStyle);
                }
            }
        }
    }

    // Break button: orange when playing or queued (treated like a fill for Main A)
    if (break_button_) {
        if (cur_is_break) {
            break_button_->setStyleSheet(orangeStyle);
        }
        if (req_is_break) {
            break_button_->setStyleSheet(orangeStyle);
        }
        // Also check the pre-start queue state
        if (!controller_.is_playing() &&
            controller_.queue_kind() == playback::QueueKind::Break) {
            break_button_->setStyleSheet(orangeStyle);
        }
    }
}

// =========================================================
//  Shutdown
// =========================================================
// =========================================================
//  Settings persistence — <Documents>/PSRstudio/PSRstudio.ini
//
//  Written once per session (closeEvent, per the on-close-only policy)
//  and read back at construction with full auto-restore: backend, ports
//  (stored by NAME so replugged devices survive reordering), SoundFont,
//  remap profile, split point, chord mode, tempo, master volume, gain,
//  reverb/chorus, MONTAGE, auto-fill, last style path. Everything that
//  fails to resolve (device absent, file moved) is skipped silently.
// =========================================================
QString MainWindow::settings_path() const {
    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    return docs + QStringLiteral("/PSRstudio/PSRstudio.ini");
}

void MainWindow::loadSettings() {
    // Startup progress goes to the splash's status line (no-op after
    // startup): each step below can take visible time.
    showSplashStatus(QStringLiteral("Restoring previous session..."));

    std::error_code ec;
    std::filesystem::create_directories(remaps_folder_, ec);   // ensures <Documents>/PSRstudio

    QSettings s(settings_path(), QSettings::IniFormat);
    if (s.value(QStringLiteral("backend/everSaved"), false).toBool() == false &&
        !QFile::exists(settings_path())) {
        return;   // first launch: nothing to restore
    }

    // ----- Backend + device ports -----
    const bool fluid = s.value(QStringLiteral("backend/active"),
                               QStringLiteral("winmm")).toString()
                           == QStringLiteral("fluidsynth");
    {
        QSignalBlocker b1(radio_winmidi_);
        QSignalBlocker b2(radio_fluidsynth_);
        radio_fluidsynth_->setChecked(fluid);
        radio_winmidi_->setChecked(!fluid);
    }
    onBackendChanged();

    // Ports are stored by NAME and resolved against the current
    // enumeration — a replugged/reordered device keeps working, and an
    // absent one is skipped silently instead of erroring at startup.
    bool out_found = false;
    bool in_found = false;
    const QString out_name = s.value(QStringLiteral("backend/midiOut")).toString();
    if (!out_name.isEmpty()) {
        const int idx = midi_out_combo_->findText(out_name);
        if (idx >= 0) {
            midi_out_combo_->setCurrentIndex(idx);
            out_found = true;
        }
    }
    const QString in_name = s.value(QStringLiteral("backend/midiIn")).toString();
    if (!in_name.isEmpty()) {
        const int idx = midi_in_combo_->findText(in_name);
        if (idx >= 0) {
            midi_in_combo_->setCurrentIndex(idx);
            in_found = true;
        }
    }

    const QString sf = s.value(QStringLiteral("backend/soundfont")).toString();
    if (!sf.isEmpty()) soundfont_edit_->setText(sf);

    // ----- Remap profile (by display name, before the state block so the
    // profile's MONTAGE flag is synced first and the saved override wins) -----
    const QString remap_name = s.value(QStringLiteral("backend/remap")).toString();
    if (!remap_name.isEmpty()) {
        const int idx = remap_combo_->findText(remap_name);
        if (idx >= 0 && idx != remap_combo_->currentIndex()) {
            remap_combo_->setCurrentIndex(idx);
            onRemapSelected(idx);
        }
    }

    // ----- Live state -----
    const int mode_idx = s.value(QStringLiteral("state/chordMode"), 2).toInt();
    if (mode_idx >= 0 && mode_idx <= 4) {
        chord_mode_combo_->setCurrentIndex(mode_idx);   // fires onChordModeChanged
    }
    const int split = s.value(QStringLiteral("state/splitPoint"),
                              chord::g_split_point.load(std::memory_order_relaxed)).toInt();
    if (split >= 0 && split <= 127) {
        chord::g_split_point.store(split, std::memory_order_relaxed);
        updateSplitButtonText();
    }
    tempo_spin_->setValue(s.value(QStringLiteral("state/tempo"),
                                  tempo_spin_->value()).toInt());
    master_volume_slider_->setValue(s.value(QStringLiteral("state/masterVolume"),
        master_volume_slider_->value()).toInt());
    gain_slider_->setValue(s.value(QStringLiteral("state/gain"),
                                   gain_slider_->value()).toInt());
    reverb_check_->setChecked(s.value(QStringLiteral("state/reverb"),
                                      reverb_check_->isChecked()).toBool());
    chorus_check_->setChecked(s.value(QStringLiteral("state/chorus"),
                                      chorus_check_->isChecked()).toBool());
    montage_check_->setChecked(s.value(QStringLiteral("state/montage"),
                                       montage_check_->isChecked()).toBool());
    patch_mega_check_->setChecked(s.value(QStringLiteral("state/patchMegaVoices"),
                                          patch_mega_check_->isChecked()).toBool());
    autofill_button_->setChecked(s.value(QStringLiteral("state/autoFill"),
                                         autofill_button_->isChecked()).toBool());

    // ----- Last style -----
    last_style_path_ = s.value(QStringLiteral("last/stylePath")).toString();

    // ----- Auto-connect + auto-load -----
    // (onConnectFluidSynth shows the "Loading Soundfont. Please
    // wait..." splash line itself, 500 ms into a slow load.)
    if (fluid) {
        if (QFile::exists(soundfont_edit_->text())) onConnectFluidSynth();
    } else if (out_found) {
        showSplashStatus(QStringLiteral("Connecting MIDI output..."));
        onConnectMidiOut();
    }
    if (in_found) {
        showSplashStatus(QStringLiteral("Connecting MIDI input..."));
        onConnectMidiIn();
    }

    if (!last_style_path_.isEmpty() && QFile::exists(last_style_path_)) {
        // loadFile() applies the style's embedded tempo; on an AUTO-load
        // the user's saved tempo wins (it reflects how they left the
        // session). Manual style loads still adopt the style's tempo.
        const int saved_tempo = tempo_spin_->value();
        QTimer::singleShot(0, this, [this, saved_tempo]() {
            showSplashStatus(QStringLiteral("Loading style: %1...")
                                 .arg(QFileInfo(last_style_path_).fileName()));
            loadFile(last_style_path_);
            tempo_spin_->setValue(saved_tempo);
            finishStartupSequence();   // clear line (≥1 s), then release splash
        });
    } else {
        finishStartupSequence();       // nothing to load — release the splash
    }
}

void MainWindow::saveSettings() {
    QSettings s(settings_path(), QSettings::IniFormat);
    s.setValue(QStringLiteral("backend/everSaved"), true);
    s.setValue(QStringLiteral("backend/active"),
               radio_fluidsynth_->isChecked() ? QStringLiteral("fluidsynth")
                                              : QStringLiteral("winmm"));
    s.setValue(QStringLiteral("backend/midiOut"), midi_out_combo_->currentText());
    s.setValue(QStringLiteral("backend/midiIn"), midi_in_combo_->currentText());
    s.setValue(QStringLiteral("backend/soundfont"), soundfont_edit_->text());
    s.setValue(QStringLiteral("backend/remap"), remap_combo_->currentText());
    s.setValue(QStringLiteral("state/chordMode"), chord_mode_combo_->currentIndex());
    s.setValue(QStringLiteral("state/splitPoint"),
               chord::g_split_point.load(std::memory_order_relaxed));
    s.setValue(QStringLiteral("state/tempo"), tempo_spin_->value());
    s.setValue(QStringLiteral("state/masterVolume"), master_volume_slider_->value());
    s.setValue(QStringLiteral("state/gain"), gain_slider_->value());
    s.setValue(QStringLiteral("state/reverb"), reverb_check_->isChecked());
    s.setValue(QStringLiteral("state/chorus"), chorus_check_->isChecked());
    s.setValue(QStringLiteral("state/montage"), montage_check_->isChecked());
    s.setValue(QStringLiteral("state/patchMegaVoices"), patch_mega_check_->isChecked());
    s.setValue(QStringLiteral("state/autoFill"), autofill_button_->isChecked());
    s.setValue(QStringLiteral("last/stylePath"), last_style_path_);
    s.sync();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();   // persist the session (on-close-only policy)

    poll_timer_->stop();
    if (chord_timer_) chord_timer_->stop();
    if (mixer_timer_) mixer_timer_->stop();
    controller_.request_stop();
    controller_.unload();

    if (midi_in_open_) {
        midi_in_.close();
        midi_in_open_ = false;
    }

    disconnectActiveOutput();

    // Close the Mixer window too (if it's open).
    if (mixer_window_) {
        mixer_window_->hide();
    }

    QMainWindow::closeEvent(event);
}

// =========================================================
//  Custom title bar
// =========================================================
void MainWindow::buildTitleBar(QVBoxLayout* rootLayout) {
    title_bar_ = new QWidget(this);
    title_bar_->setFixedHeight(36);
    title_bar_->setStyleSheet(QStringLiteral("background-color: #1e1e1e;"));

    auto* bar = new QHBoxLayout(title_bar_);
    bar->setContentsMargins(10, 0, 0, 0);
    bar->setSpacing(0);

    auto* titleLabel = new QLabel(QStringLiteral("PSRstudio"), title_bar_);
    titleLabel->setStyleSheet(QStringLiteral("color: white; font-weight: bold;"));
    bar->addWidget(titleLabel);
    bar->addStretch(1);

    const QString btnStyle = QStringLiteral(
        "QPushButton { background-color: transparent; color: white; border: none; width: 44px; height: 36px; font-size: 14px; }"
        "QPushButton:hover { background-color: #3a3a3a; }");
    const QString closeBtnStyle = QStringLiteral(
        "QPushButton { background-color: transparent; color: white; border: none; width: 44px; height: 36px; font-size: 14px; }"
        "QPushButton:hover { background-color: #e81123; }");

    minimize_button_ = new QPushButton(QStringLiteral("\u2013"), title_bar_);
    minimize_button_->setFixedSize(44, 36);
    minimize_button_->setStyleSheet(btnStyle);
    connect(minimize_button_, &QPushButton::clicked, this, &MainWindow::onMinimizeClicked);

    maximize_button_ = new QPushButton(QStringLiteral("\u25A1"), title_bar_);
    maximize_button_->setFixedSize(44, 36);
    maximize_button_->setStyleSheet(btnStyle);
    connect(maximize_button_, &QPushButton::clicked, this, &MainWindow::onMaximizeRestoreClicked);

    close_button_ = new QPushButton(QStringLiteral("\u2715"), title_bar_);
    close_button_->setFixedSize(44, 36);
    close_button_->setStyleSheet(closeBtnStyle);
    connect(close_button_, &QPushButton::clicked, this, &MainWindow::onCloseClicked);

    bar->addWidget(minimize_button_);
    bar->addWidget(maximize_button_);
    bar->addWidget(close_button_);

    rootLayout->addWidget(title_bar_);
}

void MainWindow::onMinimizeClicked() { showMinimized(); }

void MainWindow::onMaximizeRestoreClicked() {
    if (is_maximized_custom_) {
        showNormal();
        resize(1024, 720);
        if (QScreen* screen = this->screen()) {
            const QRect avail = screen->availableGeometry();
            move(avail.center() - QPoint(width() / 2, height() / 2));
        }
        is_maximized_custom_ = false;
        maximize_button_->setText(QStringLiteral("\u25A1"));
    } else {
        showFullScreen();
        is_maximized_custom_ = true;
        maximize_button_->setText(QStringLiteral("\u25A3"));
    }
}

void MainWindow::onCloseClicked() { close(); }

// =========================================================
//  Resize — keep the mixer overlay filling the window edge to edge
// =========================================================
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    layoutMixerOverlay();
    if (voice_window_ && voice_window_->isVisible()) centerOverlay(voice_window_);
    if (overlay_label_ && overlay_label_->isVisible())
        centerOverlay(overlay_label_);
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && title_bar_ &&
        title_bar_->geometry().contains(event->pos())) {
        dragging_ = true;
        drag_start_pos_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        if (is_maximized_custom_) {
            showNormal();
            is_maximized_custom_ = false;
            maximize_button_->setText(QStringLiteral("\u25A1"));
        }
        move(event->globalPosition().toPoint() - drag_start_pos_);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
    }
    QMainWindow::mouseReleaseEvent(event);
}