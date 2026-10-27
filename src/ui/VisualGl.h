#pragma once

// The Visualizer's GPU path (round 165e, docs/analise/REA_rodada165e.md): the scene drawn with OpenGL shaders — the
// 2D layers through JUCE's OpenGL renderer, plus the shader layers ZGameEditor's categories inspired (Canvas, Terrain,
// 3D objects, Feedback, Post-process) and per-layer blend modes. It runs on JUCE's OpenGL thread of its own component;
// the audio analysis stays on the message thread (the tap is read there, as the 2D preview does) and is handed over
// once per frame. Never on the audio thread.
//
// Care for the graphics driver (a GPU crash once took this machine down): frames are only drawn when the message
// thread hands a new one over (30 a second in the editor, 60 in full screen, a third of that in silence; never
// continuous), with vsync; the live picture is drawn at most 1280 x 720 inside; ray-marching loops have fixed small
// step counts. A session note marks the GPU path as active, so a crash inside the driver turns it off at the next
// start (Main.cpp). Without OpenGL (or after a failure) the editor uses the 2D renderer.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include <juce_opengl/juce_opengl.h>

#include "core/VisualFx.h"

namespace lynx
{
namespace visualgl
{
// the user's choice (Visualizer › GPU; saved in visualizer-gpu.txt, default on)
bool enabled();
void setEnabled (bool on);
// a context could not be made or its shaders failed in this run: the 2D renderer from now on (until restart)
bool failedThisSession();
void markFailed (const juce::String& why);
juce::String failureReason();
// the session note Main.cpp reads after a crash ("visualizer GPU: start" without a "stop")
constexpr const char* startNote = "visualizer GPU: start";
constexpr const char* stopNote = "visualizer GPU: stop";
// the window holding a GL child draws itself with GDI on Windows (as plugin windows do since round 157a: a Direct2D
// parent is where an Intel OpenGL driver crashed)
void useClassicRendering (juce::Component& topLevel);
} // namespace visualgl

// The GPU scene renderer. Every call on the OpenGL thread with the context current.
class GlSceneRenderer
{
public:
    GlSceneRenderer();
    ~GlSceneRenderer();

    bool create (juce::OpenGLContext& context); // false: unusable (error())
    void release();
    // moves the animated layers on to `frame` and draws the scene into the renderer's own w x h target
    void render (const visualfx::scene::Scene& scene, const visualfx::Frame& frame, int width, int height);
    // the last rendered picture into the current framebuffer's `viewport` (GL pixels, origin bottom-left)
    void present (juce::Rectangle<int> viewport, int targetHeight);
    // the last rendered picture as BGRA rows, top row first (w * h * 4 bytes)
    bool read (std::uint8_t* bgra);
    void reset(); // forget animation state (particles, trails, history)
    int width() const noexcept;
    int height() const noexcept;
    const juce::String& error() const noexcept;
    // GPU time of the last render (glFinish-timed) when timing is on (--visualtest)
    void setTiming (bool on) noexcept;
    double lastGpuMs() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// The live picture (editor preview and full screen). The message thread hands it frames; it draws them on the GL thread.
class GlSceneView final : public juce::Component,
                          private juce::OpenGLRenderer,
                          private juce::Timer
{
public:
    // `produce` (message thread, at `fps`): fills the frame and the scene to draw; returns false when nothing changed
    // enough to draw (e.g. silence, a third of the rate)
    using Producer = std::function<bool (visualfx::Frame&, visualfx::scene::Scene&)>;
    GlSceneView (Producer produce, int fps, bool fullScreen = false);
    ~GlSceneView() override;

    std::function<void()> onDoubleClick;
    std::function<void (const juce::String&)> onFailed; // message thread: the view cannot draw (use the 2D one)
    std::function<void()> onClick;
    void setOverlayText (const juce::String& text); // full screen: a hint drawn over the picture
    void restart();                                  // forget animation state
    bool isReady() const noexcept { return ready.load(); }
    // the last picture (blocks up to 2 s; empty on failure) — --visualtest
    juce::Image snapshot();
    double gpuMs() const noexcept { return gpuMsLast.load(); }
    void setTiming (bool on) noexcept { timing = on; }

    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void paint (juce::Graphics&) override;

private:
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;
    juce::Rectangle<int> frameArea (const visualfx::scene::Scene& s, juce::Rectangle<int> bounds) const;

    juce::OpenGLContext context;
    GlSceneRenderer renderer;
    Producer produce;
    bool full = false;
    int fps = 30;

    std::mutex lock; // message thread ↔ GL thread hand-over (never the audio thread)
    visualfx::Frame pendingFrame;
    visualfx::scene::Scene pendingScene, drawnScene;
    bool havePending = false, resetPending = false;
    juce::String overlay;
    std::atomic<bool> ready { false }, failed { false }, timing { false };
    std::atomic<double> gpuMsLast { 0.0 };
    double attachedMs = 0.0;
    std::unique_ptr<juce::LowLevelGraphicsContext> overlayContext;
};

// The video export's GPU: a small thumbnail of the frames being exported (in the export's progress window) whose
// context draws each frame of the video off-screen at the export's size.
class GlFrameExporter final : public juce::Component,
                              private juce::OpenGLRenderer
{
public:
    GlFrameExporter();
    ~GlFrameExporter() override;
    // any thread but the message thread: waits until the context works (false = no GPU: draw with the 2D renderer)
    bool waitReady (int ms);
    // any thread but the message thread: draws the next frame and reads it back (BGRA, top row first)
    bool renderFrame (const visualfx::scene::Scene& scene, const visualfx::Frame& frame, int width, int height, std::uint8_t* bgra);
    void reset();
    juce::String error;

private:
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    juce::OpenGLContext context;
    GlSceneRenderer renderer;
    std::atomic<int> state { 0 }; // 0 waiting, 1 ready, −1 failed
    std::atomic<bool> closing { false };
};
} // namespace lynx
