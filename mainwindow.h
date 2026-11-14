// mainwindow.h — Qt main window for the PSRstudio style player.
//
// MainWindow: the PSRstudio style player window. Owns:
//   - WinMidiOutput (hardware MIDI out port)
//   - FluidSynthOutput (software synth via SoundFonts)
//   - WinMidiInput (MIDI in port, for chord detection)
//   - The VoiceRemap table
//   - The ChannelModel (authoritative 16-channel state, alive from startup)
//   - The PlaybackController (owns style, midi player, sequencer, playback thread)
//   - All widgets, plus QTimer (80ms poll) and a 200ms mixer refresh timer
//
// Architecture:
//   Button clicks  -> controller.start_playback() / request_section() / etc.
//   Mixer controls -> ChannelModel (stores + sends in one step)
//   Style/MIDI events -> ChannelModel (from the playback thread)
//   QTimer::timeout -> polls controller + player::g_* atomics for display
//   mixer timer (200ms) -> if ChannelModel::version() changed, refresh
//     every mixer widget from ChannelModel::snapshot()
//   controller playback-finished callback -> QMetaObject::invokeMethod
//     back onto the UI thread -> reset UI state
//
// MIDI output backend selection:
//   The user chooses between WinMM (hardware MIDI) and FluidSynth
//   (software synth). Both implement prs::MidiSink. The active
//   backend pointer is stored in active_midi_out_ and passed to
//   the controller via set_midi_output().
//
// NOTE: the SplashScreen class lives in splashscreen.h (single
// definition; main_qt.cpp and splashscreen.cpp include it directly).
#pragma once

// ----- Project headers -----
#include "playback_controller.hpp"
#include "midi.hpp"
#include "fluidsynth_output.hpp"
#include "remap.hpp"

#include <QMainWindow>

// ======================================================================
//   Splash screen
// ======================================================================

// Startup splash screen.
//
// Sequence: the splash is a full-screen solid-black window (so nothing
// behind it -- including the already-full-screen MainWindow -- is
// visible at any point). Inside it:
//   1. Logo starts invisible.
//   2. Logo fades IN over fadeInMs.
//   3. Logo holds fully visible for holdMs.
//   4. The WHOLE SPLASH WINDOW fades OUT over fadeOutMs, revealing
//      MainWindow underneath as it goes transparent.
//   5. finished() is emitted; the caller closes/deletes this splash.
//
// Two different opacity mechanisms are used deliberately:
//   - The logo's fade-IN animates a QGraphicsOpacityEffect on the QLabel
//     (a CHILD widget) -- effects work reliably for child widgets, since
//     they composite into their parent's paint tree.
//   - The splash's fade-OUT animates QWidget::windowOpacity on the
//     splash itself (a TOP-LEVEL window) -- QGraphicsOpacityEffect is
//     unreliable on top-level windows on several platforms; windowOpacity
//     is the property Qt provides specifically for fading a whole
//     top-level window in/out.

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
QT_END_NAMESPACE

class SplashScreen : public QWidget {
    Q_OBJECT

public:
    // fadeInMs:  duration of the logo fading in from invisible.
    // holdMs:    minimum time the logo stays fully visible.
    // fadeOutMs: duration of the whole splash window fading out,
    //            revealing the main window underneath.
    explicit SplashScreen(const QString& imageResourcePath,
                          int fadeInMs = 1000,
                          int holdMs = 3000,
                          int fadeOutMs = 1000,
                          QWidget* parent = nullptr);

    // Called by the main window when all startup loading is done. The
    // fade-out then starts — but never before fadeInMs + holdMs have
    // elapsed (the 3-second minimum hold), so a slow load EXTENDS the
    // hold instead of cutting it short.
    void notify_ready();

    // Starts the logo fade-in. NOT started in the constructor on purpose:
    // heavy work runs right after the splash appears, and an animation
    // started before that block would skip straight to its end state.
    void start_fade_in();

    // Show (or clear, with an empty string) a small status message at
    // the bottom of the splash — e.g. "Loading Soundfont..." during
    // startup, so a slow load gives feedback instead of a frozen logo.
    void show_message(const QString& text);

private:
    void maybe_start_fade_out();

signals:
    // Emitted when the logo fade-in has fully completed. The main window
    // uses this to begin its heavy startup loading — loading earlier
    // would block the event loop and stall the fade-in animation.
    void fade_in_finished();

    // Emitted the moment the fade-out STARTS. The receiver should raise,
    // activate and force-paint the main window NOW, while the (still
    // top-most) splash covers the screen — the fade then reveals an
    // already-rendered window instead of garbage or the desktop.
    void revealUnderneath();

    // Emitted once the fade-out animation finishes. The receiver should
    // show the main window and then close/delete this splash.
    void finished();

private:
    // Explicit lifecycle: every transition is guarded by the current
    // phase, so double-invocations, signals arriving from nested event
    // loops, or out-of-order notify_ready() calls can never re-enter or
    // skip states. (The previous bool-flag version allowed the heavy
    // startup load to run re-entrantly inside the fade-in signal and
    // the fade-out to race the first paint of the main window.)
    enum class Phase {
        BeforeFadeIn,   // constructed; waiting for start_fade_in()
        FadingIn,       // logo animation running
        Holding,        // logo fully visible; minimum hold / load time
        FadingOut,      // window opacity animation running
        Done            // fade-out finished; caller closes the splash
    };
    Phase phase_ = Phase::BeforeFadeIn;
    bool ready_     = false;   // main window finished loading
    bool hold_done_ = false;   // minimum hold elapsed

    QLabel* image_label_   = nullptr;
    QLabel* message_label_ = nullptr;  // bottom status line (lazily created)
    QGraphicsOpacityEffect* logo_effect_    = nullptr;
    QPropertyAnimation*     fade_in_anim_   = nullptr;
    QPropertyAnimation*     fade_out_anim_  = nullptr;

    int fade_in_ms_;
    int hold_ms_;
    int fade_out_ms_;
};

// The splash currently on screen during startup, or nullptr once it has
// faded out (and afterwards, for the whole session). Defined in
// main_qt.cpp. Lets the main window route "Loading Soundfont..." style
// messages ONTO the splash while it covers the screen.
extern SplashScreen* g_startup_splash;
#include <QPoint>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

QT_BEGIN_NAMESPACE
class QComboBox;
class QPushButton;
class QLabel;
class QSpinBox;
class QTimer;
class QMouseEvent;
class QRadioButton;
class QLineEdit;
class QSlider;
class QCheckBox;
class QDial;
class QGroupBox;
class QGridLayout;
class QHBoxLayout;
class QEvent;
class QResizeEvent;
class QObject;
class QListWidget;
class QScrollArea;
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Begin the heavy startup work (settings restore, device connects,
    // SoundFont load, last-style autoload). Called by main() when the
    // splash's logo fade-in completes, so the loading runs behind the
    // fully-visible splash instead of stalling the fade-in animation.
    // Emits startup_ready() when everything is done.
    void begin_startup_load();

    // Routes a status line to the startup splash ("Loading Soundfont...",
    // "Loading style: ...", etc.). Lines are shown FIFO, each for at
    // least 1 second. No-op once the splash is gone, so it is safe (and
    // does nothing) at any time after startup.
    void showSplashStatus(const QString& msg);
    // Timer callback: displays the next queued line when the current
    // line's 1 s minimum display has elapsed.
    void showNextSplashStatus();
    // Final step of the startup sequence: clears the status line
    // (honouring the 1 s minimum display) and then emits startup_ready()
    // — so the fade-out never starts before the last line completes.
    void finishStartupSequence();

protected:
    void closeEvent(QCloseEvent* event) override;

    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    // Event filter: watches the Mixer window's Close event so we can
    // uncheck the Mixer button when the user closes the window via X.
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    // ----- Title bar -----
    void onMinimizeClicked();
    void onMaximizeRestoreClicked();
    void onCloseClicked();

    // ----- Backend selection -----
    void onBackendChanged();

    // ----- Transport / connection -----
    void onConnectMidiOut();
    void onConnectMidiIn();
    void onBrowseSoundfont();
    void onConnectFluidSynth();
    void onLoadClicked();
    void onAutoFillToggled();
    void onPlayClicked();
    void onStopClicked();
    void onTempoChanged(int bpm);

    // ----- Split point -----
    // "Split Point" button: click, then press a key on the MIDI input —
    // that key becomes the new chord-detection split point. Clicking
    // again while waiting cancels the capture.
    void onSplitPointClicked();

    // ----- Chord recognition mode -----
    void onChordModeChanged(int index);

    // ----- FluidSynth settings -----
    void onGainChanged(int value);
    void onReverbToggled(bool checked);
    void onChorusToggled(bool checked);

    // ----- MONTAGE ESP VST mode -----
    void onMontageToggled(bool checked);

    // ----- Remap profile picker -----
    void onRemapSelected(int index);     // combobox item chosen
    void onRemapBrowse();                // load .remap from elsewhere
    void onRemapReload();                // re-scan remaps folder
    void refreshRemapCombo();             // repopulate combobox from folder

    // ----- Channel controls -----
    // The Mixer always shows ALL 16 output channels. Every control writes
    // into the ChannelModel (which also sends the change to the device);
    // a 200 ms timer refreshes the widgets from the model, so changes made
    // by styles / MIDI files / SysEx show up too. The per-control lambdas
    // are wired in buildMixerStrips().

    // Voice selector — clicking a channel label opens a popup
    // menu listing all voices from the loaded CSV file. Selecting
    // a voice overrides the remap for that channel.
    void onVoiceSelected(int strip_idx, int voice_idx);

    void onVoiceWindowClosed();

    // Mixer window show/hide
    void onMixerToggled(bool checked);

    // Master volume (global scale factor applied to every channel's
    // CC#7 before sending). Range 0-100, default 100 = no scaling.
    void onMasterVolumeChanged(int value);

    // ----- Section buttons -----
    void onMainButtonClicked(int main_index);
    void onIntroButtonClicked(int intro_index);
    void onEndingButtonClicked(int ending_index);
    void onBreakButtonClicked();

    // ----- Timer poll -----
    void onTimer();

private:
    void buildUi();
    void buildTitleBar(class QVBoxLayout* rootLayout);
    void populateMidiOutputPorts();
    void populateMidiInputPorts();
    void updateRemapStatusLabel();          // sync status label to remap_
    void centerOverlay(QWidget* w);         // center a child overlay inside the main window
    void loadVoiceCsv();                    // load voices.csv from Documents folder
    void rebuild_voice_index();             // rebuild voice_by_addr_ from voice_list_
    void showVoiceWindow(int strip_idx);    // show voice selector window for a channel
    void buildVoiceWindow();                           // one-time construction of the voice selector
    void onCategorySelected(const QString& category, int page = 0);  // category button clicked
    void applyVoiceSearch(const QString& text);        // filter all voices by name (empty = leave search)
    void setVoicePage(int page);                       // show page N of the current category

    // ----- Mixer (always 16 strips, built once) -----
    // Build the 16 strips and wire their controls to the model.
    void buildMixerStrips();
    // Size the mixer overlay to the whole window area below the title bar
    // and adapt the strip contents to the available width.
    void layoutMixerOverlay();
    // Copy the model's state into every widget (signals blocked, so this
    // never sends MIDI or sets a lock). With force=false it does nothing
    // unless the model's version changed since the last refresh.
    void refreshMixerFromModel(bool force);
    // Part names per OUTPUT channel from the loaded style's CASM (shown
    // next to the channel number), honouring the Montage channel folding.
    void rebuildPartNames();
    // Index into voice_list_ of the voice channel `ch` currently has, or -1.
    [[nodiscard]] int voiceIndexFor(int ch, const chmodel::Channel& c) const;

    // ----- Model <-> device / style synchronisation -----
    // Make the freshly-opened `active_midi_out_` mirror the model: synth
    // reset, the loaded style's header SysEx, then a full 16-channel dump.
    void syncDeviceToModel();
    // Store the loaded style's channel state in the model (and, if a
    // device is connected, send everything). full_reset = also reset every
    // channel the style does not define (used when the channel layout
    // changed: Montage toggle / remap profile change).
    void applyStyleToModel(bool full_reset);
    // Re-derive the model after the remap profile or Montage mode changed.
    void onRemapProfileChanged();

    // Load a style or MIDI file (everything "Load Style / MIDI..." does
    // after the file dialog).
    void loadFile(const QString& path);

    // ----- Generic overlay notice -----
    // showOverlay(txtmsg, time_ms): shows a small centered overlay with
    // `txtmsg`. time_ms > 0: auto-hides after that many milliseconds.
    // time_ms == 0: persists until closed. Calling it again while
    // visible updates the text and restarts the timer.
    void showOverlay(const QString& txtmsg, int time_ms = 0);
    // Hides the overlay and stops its auto-hide timer.
    void closeOverlay();

    void updateSectionButtons();
    void updateTransportButtons();
    void updatePositionDisplay();
    void updateChordDisplay();
    void setSelectedMainVisual(int idx);
    void updateSectionHighlight();

    // Split Point button text: "Split: C3 (48)" normally, "Split: press
    // a key..." while capturing.
    void updateSplitButtonText();

    // ----- Settings persistence (<Documents>/PSRstudio/PSRstudio.ini) -----
    // loadSettings() runs at the end of the constructor: restores every
    // persisted value, auto-connects the last devices and auto-loads the
    // last style. saveSettings() writes the INI from closeEvent.
    void loadSettings();
    void saveSettings();
    [[nodiscard]] QString settings_path() const;
    QString last_style_path_;   // most recently loaded style/MIDI file

    void onPlaybackFinished();

signals:
    // Emitted when all startup loading (devices, SoundFont, last style)
    // has finished — the splash's fade-out waits for this.
    void startup_ready();

private:
    // Disconnect whichever MIDI output backend is active.
    void disconnectActiveOutput();

    // ----- MIDI I/O backends -----
    midi::WinMidiOutput         win_midi_out_;
    fluidsynth::FluidSynthOutput fluid_midi_out_;
    midi::WinMidiInput          midi_in_;
    prs::VoiceRemap             remap_;
    // The authoritative 16-channel state. Declared BEFORE controller_ so it
    // is destroyed AFTER the controller (whose playback thread uses it).
    chmodel::ChannelModel       model_;
    playback::PlaybackController controller_;

    // Points to whichever backend is currently connected (or nullptr).
    prs::MidiSink* active_midi_out_ = nullptr;

    bool win_midi_open_   = false;
    bool fluid_open_      = false;
    bool midi_in_open_    = false;

    // ----- Widgets: MIDI output -----
    QRadioButton* radio_winmidi_       = nullptr;
    QRadioButton* radio_fluidsynth_    = nullptr;
    QComboBox*    midi_out_combo_      = nullptr;
    QPushButton*  midi_out_connect_    = nullptr;
    QLabel*       midi_out_status_     = nullptr;
    QLineEdit*    soundfont_edit_      = nullptr;
    QPushButton*  soundfont_browse_    = nullptr;
    QLabel*       fluid_status_        = nullptr;
    // ----- Generic overlay notice (see showOverlay / closeOverlay) -----
    QLabel*       overlay_label_       = nullptr;  // lazily created
    QTimer*       overlay_timer_       = nullptr;  // auto-hide timer for the overlay
    bool          fluid_loading_       = false;    // open() running on the worker thread

    // ----- Startup splash status queue (see showSplashStatus) -----
    // Each status line displays on the splash for at least 1 second;
    // lines arriving faster than that are queued FIFO and shown in
    // order. startup_ready (the fade-out release) is held until the
    // queue has fully drained.
    QTimer*     splash_msg_timer_      = nullptr;  // single-shot chain scheduler
    QStringList splash_msg_queue_;                 // pending lines, in order
    qint64      splash_msg_shown_at_   = 0;        // when the current line appeared (ms epoch)
    bool        splash_has_current_    = false;    // a line is currently on the splash
    bool        splash_ready_on_drain_ = false;    // emit startup_ready when queue drains
    QSlider*      gain_slider_         = nullptr;
    QCheckBox*    reverb_check_        = nullptr;
    QCheckBox*    chorus_check_        = nullptr;
    QCheckBox*    montage_check_       = nullptr;
    QCheckBox*    patch_mega_check_    = nullptr;  // "Patch MegaVoices" toggle

    // ----- Widgets: remap profile picker -----
    QComboBox*   remap_combo_         = nullptr;  // lists .remap files in remaps folder
    QPushButton* remap_browse_        = nullptr;  // load .remap from elsewhere
    QPushButton* remap_reload_        = nullptr;  // re-scan remaps folder
    QLabel*      remap_status_        = nullptr;  // shows "name — N entries"
    // Cached list of remap file paths parallel to remap_combo_ items.
    // Item 0 is always "<no remap>" with an empty path.
    std::vector<std::filesystem::path> remap_paths_;
    // Folder scanned at startup. Defaults to
    //   <Documents>/PSRstudio/remaps  (Qt: QStandardPaths::DocumentsLocation)
    // The user can override via Browse... but the combobox always shows
    // the contents of this folder.
    std::filesystem::path remaps_folder_;

    // ----- Widgets: MIDI input -----
    QComboBox*   midi_in_combo_    = nullptr;
    QPushButton* midi_in_connect_  = nullptr;
    QLabel*      midi_in_status_   = nullptr;

    // ----- Widgets: file / status -----
    QPushButton* load_button_      = nullptr;
    QLabel*      status_label_     = nullptr;

    // ----- Widgets: channel control (in the Mixer window) -----
    // Exactly 16 strips, one per OUTPUT MIDI channel, built ONCE at
    // startup by buildMixerStrips() and never rebuilt (a style load, remap
    // change or MONTAGE toggle only changes what the widgets display).
    // The strips hold widgets ONLY — all state lives in ChannelModel.
    struct ChannelStrip {
        QWidget*     strip_widget = nullptr;  // the column widget
        QLabel*      label    = nullptr;  // voice name (click = voice selector)
        QWidget*     vlabel   = nullptr;  // vertical label: "Ch N: part" (VerticalLabel)
        QSlider*     volume   = nullptr;  // vertical, 0-127
        QLabel*      vol_val  = nullptr;  // "64"
        QPushButton* mute_btn = nullptr;  // "M" toggle
        QPushButton* solo_btn = nullptr;  // "S" toggle
        QDial*       reverb   = nullptr;  // 0-127
        QLabel*      rev_val  = nullptr;
        QDial*       chorus   = nullptr;  // 0-127
        QLabel*      cho_val  = nullptr;
        QDial*       pan      = nullptr;  // 0-127, 64=center
        QLabel*      pan_val  = nullptr;
    };
    QWidget*                   mixer_window_     = nullptr;  // overlay panel (child of this window)
    QWidget*                   mixer_header_      = nullptr;  // title bar + close button
    QWidget*                   mixer_container_   = nullptr;  // holds the 16 strips
    QHBoxLayout*               mixer_layout_      = nullptr;  // lays the strips out edge to edge
    QPushButton*               mixer_button_      = nullptr;  // in the transport row
    std::array<ChannelStrip, chmodel::kChannels> channel_strips_{};
    QTimer*                    mixer_timer_       = nullptr;  // 200 ms model -> widgets refresh
    std::uint32_t              mixer_last_version_ = 0;       // model version last shown
    bool                       mixer_ever_refreshed_ = false;
    int                        mixer_strip_inner_w_ = 70;     // px available for a strip's label text
    // Part names (from the style's CASM) per OUTPUT channel; "" = none.
    std::array<std::string, chmodel::kChannels> part_names_{};

    // ----- Voice selector data -----
    struct VoiceEntry {
        std::string name;
        int msb;
        int lsb;
        int pc;      // 0-127 (raw MIDI Program Change value, same as on the wire)
        std::string category;
    };
    std::vector<VoiceEntry> voice_list_;
    // Address (msb<<16 | lsb<<8 | pc) -> voice_list_ indices, in CSV
    // order. Built once after the CSV loads (rebuild_voice_index) so
    // voiceIndexFor() is a hash lookup instead of a linear scan of the
    // whole list per channel per refresh.
    std::unordered_map<std::uint32_t, std::vector<int>> voice_by_addr_;
    // The voice list entry the user last picked for each channel (or -1).
    // Only used to tell apart voices that share the same MSB/LSB/PC in
    // voices.csv; the channel's real voice always comes from the model.
    std::array<int, chmodel::kChannels> picked_voice_idx_ = [] {
        std::array<int, chmodel::kChannels> a{};
        a.fill(-1);
        return a;
    }();

    // ----- Voice selector window -----
    QWidget*     voice_window_          = nullptr;
    QLabel*       voice_title_label_       = nullptr;
    QLabel*       voice_path_label_        = nullptr;
    QLabel*       voice_page_label_        = nullptr;   // "4/7"
    QGridLayout*  voice_category_grid_     = nullptr;   // 2-column category buttons
    QGridLayout*  voice_grid_              = nullptr;   // 2 cols x 5 rows of voices
    QHBoxLayout*  voice_page_tabs_layout_  = nullptr;   // P1..P5 tabs
    QPushButton*  voice_prev_btn_          = nullptr;
    QPushButton*  voice_next_btn_          = nullptr;
    std::vector<QPushButton*> voice_category_buttons_;
    QStringList   voice_category_names_;                // parallel to the buttons
    QString       voice_current_category_;
    std::vector<int> voice_category_indices_;           // voice_list_ indices in the current category
    int           voice_page_ = 0;
    QLineEdit*    voice_search_edit_       = nullptr;
    int           voice_selected_idx_      = -1;   // voice_list_ index in use by the strip (highlighted)
    bool          voice_in_search_         = false;
    int           voice_page_before_search_ = 0;
    int          voice_strip_idx_     = -1;  // which strip we're selecting for

    // ----- Widgets: section buttons -----
    QPushButton* intro_buttons_[3]  = {};
    QPushButton* main_buttons_[4]   = {};
    QPushButton* break_button_      = nullptr;
    QPushButton* ending_buttons_[3] = {};

    // ----- Widgets: transport -----
    QPushButton* autofill_button_  = nullptr;
    QPushButton* play_button_      = nullptr;
    QSpinBox*    tempo_spin_       = nullptr;
    QSlider*     master_volume_slider_ = nullptr;  // global master volume (0-100)
    QLabel*      master_volume_label_  = nullptr;   // shows "Master: 100"
    QPushButton* split_button_     = nullptr;          // Split Point picker
    bool         split_capture_armed_ = false;         // waiting for a key
    QComboBox*   chord_mode_combo_  = nullptr;          // chord recognition mode
    // The chosen recognition mode — kept here so re-opening the input
    // port (onConnectMidiIn) restores the user's selection instead of
    // the hardcoded default.
    chord::InputMode chord_mode_ = chord::InputMode::Fingered;


    // ----- Widgets: display -----
    QLabel* chord_label_   = nullptr;
    QLabel* section_label_ = nullptr;
    QLabel* measure_label_ = nullptr;
    QLabel* beat_label_    = nullptr;

    QTimer* poll_timer_ = nullptr;
    QTimer* chord_timer_ = nullptr;   // 5 ms: expires the chord-engine window

    // ----- Custom title bar -----
    QWidget*     title_bar_        = nullptr;
    QPushButton* minimize_button_  = nullptr;
    QPushButton* maximize_button_  = nullptr;
    QPushButton* close_button_     = nullptr;
    bool         is_maximized_custom_ = true;
    QPoint       drag_start_pos_;
    bool         dragging_ = false;

    bool suppress_tempo_signal_ = false;
};
