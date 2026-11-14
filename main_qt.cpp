// main_qt.cpp — Qt application entry point.
//
// Startup order (designed so the splash is the FIRST thing on screen):
//   1. Show the splash immediately — full-screen, opaque black — and
//      repaint() it before anything heavy runs.
//   2. The logo fade-in starts at the first event-loop tick, while the
//      event loop is still completely free (no heavy work has run), so
//      the animation is smooth from the first frames.
//   3. When the fade-in COMPLETES, the MainWindow is constructed and its
//      saved setup runs (device connects, SoundFont load, last-style
//      autoload) — all behind the fully-visible logo, mapping the window
//      full-screen UNDER the still-opaque splash so nothing flashes.
//   4. When ALL loading is done the window emits startup_ready(), which
//      releases the splash's fade-out (never before the 3 s hold).
//   5. At fade-out start the main window is raised/activated while the
//      splash still covers the screen, so no other window can take its
//      place; the splash closes when fully transparent.
#include "mainwindow.h"

#include <QApplication>
#include <QTimer>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

// The startup splash while it is on screen; nullptr afterwards. The
// main window uses it to route loading messages ("Loading Soundfont...")
// ONTO the splash, where they are visible, instead of an overlay that
// is hidden underneath it.
SplashScreen* g_startup_splash = nullptr;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

#ifdef _WIN32
    // Raise Windows timer resolution to 1ms for precise MIDI timing,
    // matching the behavior of the entry point.
    timeBeginPeriod(1);
#endif

    // 1. Splash first — within milliseconds of process start.
    auto* splash = new SplashScreen(QStringLiteral(":/resources/splash_logo.png"),
                                    /*fadeInMs=*/1000, /*holdMs=*/3000, /*fadeOutMs=*/1000);
    g_startup_splash = splash;
    splash->show();
    splash->repaint();   // force an immediate paint (black + logo)

    // 2. Heavy startup runs behind the splash. `window` is filled in by
    //    the deferred lambda below.
    MainWindow* window = nullptr;

    // Foreground hand-off happens at the START of the splash's fade-out,
    // not after it: while the (still top-most) splash covers the screen,
    // the main window is raised, activated AND synchronously repainted —
    // so the opacity fade reveals an already-rendered window. (The
    // window has been fully covered since it was shown, so without the
    // forced repaint the compositor may not have a frame of it yet and
    // the fade would show the desktop before the window pops in.)
    // (Activating only at finished() races: during the 1 s fade the
    // splash stops being the foreground window and another app's window
    // can take PSRstudio's place.)
    QObject::connect(splash, &SplashScreen::revealUnderneath, [splash, &window]() {
        if (window) {
            window->showFullScreen();   // re-assert fullscreen (no-op if already)
            window->raise();
            window->activateWindow();
            window->repaint();          // force a synchronous paint NOW
        }
    });

    QObject::connect(splash, &SplashScreen::finished, [splash, &window]() {
        if (window) {
            window->raise();
            window->activateWindow();
        }
        g_startup_splash = nullptr;     // splash is going away; stop routing to it
        splash->close();
        splash->deleteLater();
    });

    // 3. The logo fade-in starts at the FIRST event-loop tick — nothing
    //    heavy has run yet, so the animation plays smoothly from the
    //    moment the black splash appears. (Starting it after the window
    //    construction instead left the screen black for the whole
    //    constructor: device enumeration, voices.csv, mixer build.)
    QTimer::singleShot(0, splash, [splash]() {
        splash->start_fade_in();
    });

    // 4. ALL heavy work waits for the fade-in to complete: the window is
    //    constructed and its saved setup (devices, SoundFont, last style)
    //    runs behind the fully-visible logo. startup_ready() then
    //    releases the splash's fade-out (still gated by the 3 s hold).
    //    The heavy load is DEFERRED one event-loop tick: running it
    //    directly inside this signal handler meant a multi-second
    //    SoundFont load (with its own nested event loop) executed
    //    re-entrantly inside the fade-in animation's finished() emission
    //    — fragile, and the source of the broken startup fade-out.
    QObject::connect(splash, &SplashScreen::fade_in_finished,
                     [splash, &window]() {
        window = new MainWindow();
        window->showFullScreen();

        QObject::connect(window, &MainWindow::startup_ready,
                         splash, &SplashScreen::notify_ready);
        QTimer::singleShot(0, window, [window]() {
            window->begin_startup_load();
        });
    });

    const int result = app.exec();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return result;
}
