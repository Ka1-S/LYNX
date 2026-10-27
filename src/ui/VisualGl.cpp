#include "ui/VisualGl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>

#include "platform/CrashGuard.h"
#include "platform/Paths.h"
#include "ui/VisualEditors.h"

#include "ui/VisualGlShaders.inc"

namespace lynx
{
using namespace juce::gl;
namespace vs = visualfx::scene;
using visualfx::Frame;

// ---------------------------------------------------------------------------------------------------------------
// Settings and the crash note
// ---------------------------------------------------------------------------------------------------------------
namespace visualgl
{
namespace
{
int setting = -1;
std::atomic<bool> failed { false };
juce::String failure;
std::atomic<int> activeViews { 0 };

void viewStarted()
{
    if (activeViews++ == 0)
        crashguard::note (startNote);
}

void viewStopped()
{
    if (--activeViews == 0)
        crashguard::note (stopNote);
}
} // namespace

bool enabled()
{
    // (LYNX_VISUALIZER_GPU=0: the 2D renderer for this run, without changing the saved choice)
    static const bool forcedOff = juce::SystemStats::getEnvironmentVariable ("LYNX_VISUALIZER_GPU", {}).trim() == "0";
    if (forcedOff)
        return false;
    if (setting < 0)
        setting = platform::settingsFile ("visualizer-gpu.txt").loadFileAsString().trim() == "0" ? 0 : 1;
    return setting != 0;
}

void setEnabled (bool on)
{
    setting = on ? 1 : 0;
    platform::settingsFile ("visualizer-gpu.txt").replaceWithText (on ? "1" : "0");
    if (on)
        failed = false; // (the user asks for it again: try once more)
}

bool failedThisSession() { return failed.load(); }

void markFailed (const juce::String& why)
{
    failed = true;
    failure = why;
}

juce::String failureReason() { return failure; }

void useClassicRendering (juce::Component& topLevel)
{
    topLevel.getProperties().set ("lynxGdi", true);
   #if JUCE_WINDOWS
    if (auto* peer = topLevel.getPeer())
        if (const auto engines = peer->getAvailableRenderingEngines(); engines.size() > 1 && peer->getCurrentRenderingEngine() != 0)
            peer->setCurrentRenderingEngine (0);
   #endif
}
} // namespace visualgl

// ---------------------------------------------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------------------------------------------
namespace
{
// the live picture is drawn at most 1280 x 720 inside and stretched to the window (full screen too): the shader layers'
// cost grows with the pixels, and a quarter of a 4K screen's pixels keeps even an integrated GPU cool (the video
// export draws its own size off-screen, as fast as the encoder takes the frames)
constexpr int maxLiveWidth = 1280, maxLiveHeight = 720;

struct Program
{
    std::unique_ptr<juce::OpenGLShaderProgram> program;
    GLint position = -1;
};

const char* effectSource (vs::LayerType type, int effect, juce::String& prefix)
{
    using T = vs::LayerType;
    switch (type)
    {
        case T::canvas:
        {
            static const char* list[] = { glsl::canvasPlasma, glsl::canvasTunnel, glsl::canvasCells, glsl::canvasKaleido,
                                          glsl::canvasClouds, glsl::canvasWarp, glsl::canvasElectric, glsl::canvasRays };
            return list[juce::jlimit (0, (int) std::size (list) - 1, effect)];
        }
        case T::terrain:
        {
            prefix = glsl::terrainCommon;
            static const char* list[] = { glsl::terrainMountains, glsl::terrainHills, glsl::terrainRoad };
            return list[juce::jlimit (0, (int) std::size (list) - 1, effect)];
        }
        case T::object3d:
        {
            prefix = glsl::objectCommon;
            static const char* list[] = { glsl::objectBlob, glsl::objectTorus, glsl::objectCubes, glsl::objectRings };
            return list[juce::jlimit (0, (int) std::size (list) - 1, effect)];
        }
        case T::feedback:
        {
            static const char* list[] = { glsl::feedbackZoom, glsl::feedbackSpin, glsl::feedbackKaleido, glsl::feedbackWarp,
                                          glsl::feedbackEcho, glsl::feedbackDrift };
            return list[juce::jlimit (0, (int) std::size (list) - 1, effect)];
        }
        case T::postprocess:
        {
            // bloom (0) and blur (1) are multi-pass (see Impl::blur)
            static const char* list[] = { nullptr, nullptr, glsl::postRgbShift, glsl::postPixelate, glsl::postDots,
                                          glsl::postScanlines, glsl::postVignette, glsl::postColor, glsl::postKaleido,
                                          glsl::postMirror, glsl::postEdges, glsl::postShake, glsl::postGlitch,
                                          glsl::postGrain, glsl::postInvert };
            return list[juce::jlimit (0, (int) std::size (list) - 1, effect)];
        }
        default: return nullptr;
    }
}
} // namespace

struct GlSceneRenderer::Impl
{
    juce::OpenGLContext* context = nullptr;
    juce::String error;
    GLuint quad = 0, specTex = 0, histTex = 0;
    std::map<int, Program> programs; // key: type * 100 + effect; special keys below
    std::map<int, bool> broken;      // shaders that did not compile (that layer is skipped)
    enum Special { composite = 9000, finalPass, copy, down, blurPass, bloomMix, blurMix };
    juce::OpenGLFrameBuffer accum[2], layer, out, blurA, blurB;
    std::map<std::size_t, std::unique_ptr<juce::OpenGLFrameBuffer>> trails; // feedback layers' own pictures
    int cur = 0, w = 0, h = 0;
    SceneRenderer flat; // the 2D layers (through JUCE's OpenGL renderer)
    std::vector<float> phase;
    std::array<std::uint8_t, 64 * 4> specBytes {};
    std::vector<std::uint8_t> histRowBytes = std::vector<std::uint8_t> (64 * 4);
    int histRow = 0;
    bool timing = false, hasFrame = false;
    double gpuMs = 0.0;

    bool make (int key, const juce::String& fragment)
    {
        Program p;
        p.program = std::make_unique<juce::OpenGLShaderProgram> (*context);
        if (! p.program->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (glsl::vertex))
            || ! p.program->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragment)) || ! p.program->link())
        {
            error = "shader " + juce::String (key) + ": " + p.program->getLastError();
            DBG (error);
            broken[key] = true;
            return false;
        }
        p.position = glGetAttribLocation (p.program->getProgramID(), "position");
        programs[key] = std::move (p);
        return true;
    }

    Program* get (int key, const std::function<juce::String()>& source)
    {
        if (auto it = programs.find (key); it != programs.end())
            return &it->second;
        if (broken.count (key) != 0)
            return nullptr;
        return make (key, source()) ? &programs[key] : nullptr;
    }

    Program* effectProgram (vs::LayerType type, int effect)
    {
        const auto key = (int) type * 100 + effect;
        return get (key, [type, effect] {
            juce::String prefix;
            const auto* body = effectSource (type, effect, prefix);
            if (body == nullptr)
                return juce::String();
            if (type == vs::LayerType::feedback)
                return juce::String (glsl::common) + body + glsl::feedbackMain;
            return juce::String (glsl::common) + prefix + body;
        });
    }

    Program* special (Special s)
    {
        return get ((int) s, [s] {
            switch (s)
            {
                case composite: return juce::String (glsl::composite);
                case finalPass: return juce::String (glsl::finalPass);
                case copy: return juce::String (glsl::copy);
                case down: return juce::String (glsl::common) + glsl::postDown;
                case blurPass: return juce::String (glsl::common) + glsl::postBlurPass;
                case bloomMix: return juce::String (glsl::common) + glsl::postBloomMix;
                case blurMix: return juce::String (glsl::common) + glsl::postBlurMix;
            }
            return juce::String();
        });
    }

    bool create (juce::OpenGLContext& c)
    {
        context = &c;
        if (! c.areShadersAvailable())
        {
            error = "OpenGL shaders are not available";
            return false;
        }
        const GLfloat corners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
        glGenBuffers (1, &quad);
        glBindBuffer (GL_ARRAY_BUFFER, quad);
        glBufferData (GL_ARRAY_BUFFER, sizeof (corners), corners, GL_STATIC_DRAW);
        glBindBuffer (GL_ARRAY_BUFFER, 0);
        auto texture = [] (GLuint& id, int tw, int th, GLint wrapT) {
            glGenTextures (1, &id);
            glBindTexture (GL_TEXTURE_2D, id);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
            std::vector<std::uint8_t> zeros ((std::size_t) (tw * th * 4), 0);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
        };
        texture (specTex, 64, 1, GL_CLAMP_TO_EDGE);
        texture (histTex, 64, 128, GL_REPEAT);
        glBindTexture (GL_TEXTURE_2D, 0);
        // the programs every frame needs (a failure here = no GPU path)
        for (auto s : { composite, finalPass, copy })
            if (special (s) == nullptr)
                return false;
        return glGetError() == GL_NO_ERROR || true;
    }

    void release()
    {
        programs.clear();
        broken.clear();
        for (auto& f : accum)
            f.release();
        for (auto* f : { &layer, &out, &blurA, &blurB })
            f->release();
        trails.clear();
        if (quad != 0)
            glDeleteBuffers (1, &quad);
        if (specTex != 0)
            glDeleteTextures (1, &specTex);
        if (histTex != 0)
            glDeleteTextures (1, &histTex);
        quad = specTex = histTex = 0;
        w = h = 0;
        hasFrame = false;
        flat.reset();
    }

    bool resize (int width, int height)
    {
        if (width == w && height == h)
            return true;
        w = width;
        h = height;
        trails.clear();
        bool ok = true;
        for (auto& f : accum)
            ok = ok && f.initialise (*context, w, h);
        ok = ok && layer.initialise (*context, w, h) && out.initialise (*context, w, h);
        ok = ok && blurA.initialise (*context, juce::jmax (1, w / 2), juce::jmax (1, h / 2)) && blurB.initialise (*context, juce::jmax (1, w / 2), juce::jmax (1, h / 2));
        if (! ok)
            error = "could not make the " + juce::String (w) + " x " + juce::String (h) + " frame buffers";
        return ok;
    }

    // ---- drawing helpers ----
    void bindTarget (juce::OpenGLFrameBuffer& f)
    {
        glBindFramebuffer (GL_FRAMEBUFFER, f.getFrameBufferID());
        glViewport (0, 0, f.getWidth(), f.getHeight());
    }

    void bindTexture (int unit, GLuint id)
    {
        glActiveTexture ((GLenum) (GL_TEXTURE0 + unit));
        glBindTexture (GL_TEXTURE_2D, id);
    }

    void drawQuad (Program& p)
    {
        glBindBuffer (GL_ARRAY_BUFFER, quad);
        if (p.position >= 0)
        {
            glVertexAttribPointer ((GLuint) p.position, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray ((GLuint) p.position);
        }
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        if (p.position >= 0)
            glDisableVertexAttribArray ((GLuint) p.position);
        glBindBuffer (GL_ARRAY_BUFFER, 0);
    }

    void resetState()
    {
        glDisable (GL_BLEND);
        glDisable (GL_DEPTH_TEST);
        glDisable (GL_SCISSOR_TEST);
        glDisable (GL_STENCIL_TEST);
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram (0);
    }

    static juce::Colour hued (std::uint32_t argb, float hueShift)
    {
        auto c = juce::Colour (argb);
        return hueShift > 0.001f ? c.withRotatedHue (hueShift) : c;
    }

    void setCommon (juce::OpenGLShaderProgram& p, const vs::Layer& L, std::size_t index, const Frame& f, int tw, int th)
    {
        p.setUniform ("uSrc", 0);
        p.setUniform ("uPrev", 1);
        p.setUniform ("uSpec", 2);
        p.setUniform ("uHist", 3);
        p.setUniform ("uRes", (GLfloat) tw, (GLfloat) th);
        p.setUniform ("uTime", index < phase.size() ? phase[index] : 0.0f);
        p.setUniform ("uAbs", (GLfloat) std::fmod (f.time, 10000.0));
        p.setUniform ("uLevel", f.level);
        p.setUniform ("uBass", f.bass);
        p.setUniform ("uMid", f.mid);
        p.setUniform ("uHigh", f.high);
        p.setUniform ("uBeat", f.beat);
        p.setUniform ("uBand", f.bandValue (L.band));
        p.setUniform ("uReact", L.react);
        p.setUniform ("uSpeed", L.speed);
        p.setUniform ("uAmount", L.amount);
        p.setUniform ("uSize", L.size);
        p.setUniform ("uCount", (GLfloat) L.count);
        p.setUniform ("uOpacity", L.opacity);
        p.setUniform ("uHistRow", (GLfloat) histRow);
        // the shaders measure y up (GL); the scene's y goes down
        p.setUniform ("uPos", L.x, 1.0f - L.y);
        const auto hue = f.params[visualfx::visualizer::hueShift];
        const auto a = hued (L.colourA, hue), b = hued (L.colourB, hue);
        p.setUniform ("uColA", a.getFloatRed(), a.getFloatGreen(), a.getFloatBlue(), a.getFloatAlpha());
        p.setUniform ("uColB", b.getFloatRed(), b.getFloatGreen(), b.getFloatBlue(), b.getFloatAlpha());
        p.setUniform ("uDir", 0.0f, 0.0f);
    }

    void bindAnalysisTextures()
    {
        bindTexture (2, specTex);
        bindTexture (3, histTex);
    }

    // a pass of `program` from the current picture into the other accumulation buffer
    void passInto (juce::OpenGLFrameBuffer& target, Program& p, GLuint src, GLuint prev)
    {
        bindTarget (target);
        p.program->use();
        bindTexture (0, src);
        bindTexture (1, prev);
        bindAnalysisTextures();
        drawQuad (p);
    }

    void swapInto (Program& p, const vs::Layer& L, std::size_t index, const Frame& f, GLuint prev)
    {
        auto& next = accum[1 - cur];
        p.program->use();
        setCommon (*p.program, L, index, f, w, h);
        passInto (next, p, accum[cur].getTextureID(), prev);
        cur = 1 - cur;
    }

    void composeLayer (const vs::Layer& L, float opacity)
    {
        auto* p = special (composite);
        if (p == nullptr)
            return;
        bindTarget (accum[1 - cur]);
        p->program->use();
        p->program->setUniform ("uSrc", 0);
        p->program->setUniform ("uLayer", 4);
        p->program->setUniform ("uOpacity", opacity);
        p->program->setUniform ("uMode", (GLfloat) (int) L.blend);
        bindTexture (0, accum[cur].getTextureID());
        bindTexture (4, layer.getTextureID());
        drawQuad (*p);
        cur = 1 - cur;
    }

    // ---- per frame ----
    void advance (const vs::Scene& s, const Frame& f)
    {
        flat.advance (s, f, (float) w / (float) juce::jmax (1, h));
        const auto speedKnob = (float) std::pow (4.0, (double) f.params[visualfx::visualizer::speed] * 2.0 - 1.0);
        phase.resize (s.layers.size(), 0.0f);
        for (std::size_t i = 0; i < s.layers.size(); ++i)
        {
            const auto& L = s.layers[i];
            const auto rate = (0.15f + L.speed * 1.85f) * speedKnob * (1.0f + 0.5f * f.bandValue (L.band) * L.react);
            phase[i] = std::fmod (phase[i] + (float) f.dt * rate, 3600.0f);
        }
        for (int b = 0; b < 64; ++b)
        {
            const auto v = (std::uint8_t) juce::jlimit (0, 255, (int) std::lround (f.spectrum[(std::size_t) b] * 255.0f));
            for (int k = 0; k < 4; ++k)
                specBytes[(std::size_t) (b * 4 + k)] = v;
        }
        glBindTexture (GL_TEXTURE_2D, specTex);
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, specBytes.data());
        histRow = (histRow + 1) % 128;
        glBindTexture (GL_TEXTURE_2D, histTex);
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, histRow, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, specBytes.data());
        glBindTexture (GL_TEXTURE_2D, 0);
    }

    void drawFlat (juce::OpenGLFrameBuffer& target, const vs::Scene& s, std::size_t from, std::size_t to, const Frame& f)
    {
        {
            auto g2 = juce::createOpenGLGraphicsContext (*context, target);
            juce::Graphics g (*g2);
            for (auto i = from; i < to; ++i)
                flat.drawLayer (g, juce::Rectangle<float> ((float) w, (float) h), s, i, f);
        }
        resetState();
    }

    void blur (const vs::Layer& L, std::size_t index, const Frame& f, bool bloom)
    {
        auto* dp = special (down);
        auto* bp = special (blurPass);
        auto* mp = special (bloom ? bloomMix : blurMix);
        if (dp == nullptr || bp == nullptr || mp == nullptr)
            return;
        const auto bw = blurA.getWidth(), bh = blurA.getHeight();
        dp->program->use();
        setCommon (*dp->program, L, index, f, bw, bh);
        dp->program->setUniform ("uThreshold", bloom ? juce::jlimit (0.0f, 0.95f, 0.85f - juce::jlimit (0, 512, L.count) / 512.0f * 0.8f) : -1.0f);
        passInto (blurA, *dp, accum[cur].getTextureID(), 0);
        const auto radius = (0.5f + L.size * 4.0f) * (1.0f + (bloom ? 0.0f : f.bandValue (L.band) * L.react));
        const auto passes = bloom ? 2 : (L.size > 0.5f ? 3 : 2);
        bp->program->use();
        setCommon (*bp->program, L, index, f, bw, bh);
        for (int k = 0; k < passes; ++k)
        {
            bp->program->use();
            bp->program->setUniform ("uDir", radius / (float) bw, 0.0f);
            passInto (blurB, *bp, blurA.getTextureID(), 0);
            bp->program->use();
            bp->program->setUniform ("uDir", 0.0f, radius / (float) bh);
            passInto (blurA, *bp, blurB.getTextureID(), 0);
        }
        swapInto (*mp, L, index, f, blurA.getTextureID());
    }

    void render (const vs::Scene& s, const Frame& f, int width, int height)
    {
        if (context == nullptr || ! resize (width, height))
            return;
        if (timing)
            glFinish();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        resetState();
        advance (s, f);
        cur = 0;
        bindTarget (accum[0]);
        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);

        std::size_t i = 0;
        while (i < s.layers.size())
        {
            const auto& L = s.layers[i];
            if (! L.enabled || L.opacity <= 0.0f)
            {
                ++i;
                continue;
            }
            if (! vs::isShaderType (L.type))
            {
                if (L.blend == vs::Blend::normal)
                {
                    // the 2D layers in a row draw straight into the picture (JUCE blends them normally)
                    auto end = i + 1;
                    while (end < s.layers.size() && ! vs::isShaderType (s.layers[end].type) && s.layers[end].blend == vs::Blend::normal)
                        ++end;
                    drawFlat (accum[cur], s, i, end, f);
                    i = end;
                    continue;
                }
                layer.makeCurrentRenderingTarget();
                glViewport (0, 0, w, h);
                glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
                glClear (GL_COLOR_BUFFER_BIT);
                layer.releaseAsRenderingTarget();
                drawFlat (layer, s, i, i + 1, f);
                composeLayer (L, 1.0f); // (the layer drew its own opacity)
                ++i;
                continue;
            }
            switch (L.type)
            {
                case vs::LayerType::canvas:
                case vs::LayerType::terrain:
                case vs::LayerType::object3d:
                {
                    auto* p = effectProgram (L.type, L.effect);
                    if (p == nullptr)
                        break;
                    p->program->use();
                    setCommon (*p->program, L, i, f, w, h);
                    if (L.blend == vs::Blend::normal || L.blend == vs::Blend::add)
                    {
                        // over the picture with GL's own blending (premultiplied colour)
                        bindTarget (accum[cur]);
                        glEnable (GL_BLEND);
                        glBlendFunc (GL_ONE, L.blend == vs::Blend::add ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                        bindTexture (0, 0); // (these shaders do not read the picture below: no texture bound to the target)
                        bindAnalysisTextures();
                        drawQuad (*p);
                        glDisable (GL_BLEND);
                    }
                    else
                    {
                        p->program->setUniform ("uOpacity", 1.0f);
                        bindTarget (layer);
                        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
                        glClear (GL_COLOR_BUFFER_BIT);
                        bindAnalysisTextures();
                        drawQuad (*p);
                        composeLayer (L, L.opacity);
                    }
                    break;
                }
                case vs::LayerType::feedback:
                {
                    auto* p = effectProgram (L.type, L.effect);
                    if (p == nullptr)
                        break;
                    auto& trail = trails[i];
                    if (trail == nullptr)
                    {
                        trail = std::make_unique<juce::OpenGLFrameBuffer>();
                        if (! trail->initialise (*context, w, h))
                            break;
                        bindTarget (*trail);
                        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
                        glClear (GL_COLOR_BUFFER_BIT);
                    }
                    swapInto (*p, L, i, f, trail->getTextureID());
                    // keep this frame for the next one
                    if (auto* c = special (copy))
                    {
                        bindTarget (*trail);
                        c->program->use();
                        c->program->setUniform ("uSrc", 0);
                        bindTexture (0, accum[cur].getTextureID());
                        drawQuad (*c);
                    }
                    break;
                }
                case vs::LayerType::postprocess:
                {
                    if (L.effect == 0 || L.effect == 1)
                    {
                        blur (L, i, f, L.effect == 0);
                        break;
                    }
                    if (auto* p = effectProgram (L.type, L.effect))
                        swapInto (*p, L, i, f, 0);
                    break;
                }
                default: break;
            }
            ++i;
        }

        // zoom and brightness (the global knobs)
        if (auto* p = special (finalPass))
        {
            bindTarget (out);
            p->program->use();
            p->program->setUniform ("uSrc", 0);
            p->program->setUniform ("uZoom", 0.5f + f.params[visualfx::visualizer::zoom]);
            p->program->setUniform ("uBright", juce::jlimit (0.0f, 1.0f, f.params[visualfx::visualizer::brightness]));
            bindTexture (0, accum[cur].getTextureID());
            drawQuad (*p);
        }
        bindTexture (0, 0);
        glActiveTexture (GL_TEXTURE0);
        glBindFramebuffer (GL_FRAMEBUFFER, context->getFrameBufferID());
        hasFrame = true;
        if (timing)
        {
            glFinish();
            gpuMs = juce::Time::getMillisecondCounterHiRes() - t0;
        }
    }

    void present (juce::Rectangle<int> viewport)
    {
        auto* p = special (copy);
        if (p == nullptr || ! hasFrame)
            return;
        glBindFramebuffer (GL_FRAMEBUFFER, context->getFrameBufferID());
        glViewport (viewport.getX(), viewport.getY(), viewport.getWidth(), viewport.getHeight());
        resetState();
        p->program->use();
        p->program->setUniform ("uSrc", 0);
        bindTexture (0, out.getTextureID());
        drawQuad (*p);
        bindTexture (0, 0);
        glUseProgram (0);
    }

    bool read (std::uint8_t* bgra)
    {
        if (! hasFrame || w <= 0 || h <= 0)
            return false;
        std::vector<std::uint8_t> rows ((std::size_t) w * (std::size_t) h * 4);
        glBindFramebuffer (GL_FRAMEBUFFER, out.getFrameBufferID());
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glReadPixels (0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, rows.data());
        glBindFramebuffer (GL_FRAMEBUFFER, context->getFrameBufferID());
        const auto stride = (std::size_t) w * 4;
        for (int y = 0; y < h; ++y) // GL's first row is the bottom one
            std::memcpy (bgra + (std::size_t) y * stride, rows.data() + (std::size_t) (h - 1 - y) * stride, stride);
        return glGetError() == GL_NO_ERROR || true;
    }
};

GlSceneRenderer::GlSceneRenderer() : impl (std::make_unique<Impl>()) {}
GlSceneRenderer::~GlSceneRenderer() = default;

bool GlSceneRenderer::create (juce::OpenGLContext& context) { return impl->create (context); }
void GlSceneRenderer::release() { impl->release(); }
void GlSceneRenderer::render (const vs::Scene& scene, const Frame& frame, int width, int height) { impl->render (scene, frame, width, height); }
void GlSceneRenderer::present (juce::Rectangle<int> viewport, int) { impl->present (viewport); }
bool GlSceneRenderer::read (std::uint8_t* bgra) { return impl->read (bgra); }
int GlSceneRenderer::width() const noexcept { return impl->w; }
int GlSceneRenderer::height() const noexcept { return impl->h; }
const juce::String& GlSceneRenderer::error() const noexcept { return impl->error; }
void GlSceneRenderer::setTiming (bool on) noexcept { impl->timing = on; }
double GlSceneRenderer::lastGpuMs() const noexcept { return impl->gpuMs; }

void GlSceneRenderer::reset()
{
    impl->flat.reset();
    impl->trails.clear();
    impl->phase.clear();
    impl->hasFrame = false;
}

// ---------------------------------------------------------------------------------------------------------------
// The live view
// ---------------------------------------------------------------------------------------------------------------
GlSceneView::GlSceneView (Producer p, int framesPerSecond, bool fullScreen) : produce (std::move (p)), full (fullScreen), fps (framesPerSecond)
{
    setOpaque (true);
    context.setRenderer (this);
    context.setComponentPaintingEnabled (false);
    context.setContinuousRepainting (false);
    visualgl::viewStarted();
    context.attachTo (*this);
    attachedMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (juce::jlimit (1, 60, fps));
}

GlSceneView::~GlSceneView()
{
    stopTimer();
    context.detach();
    visualgl::viewStopped();
}

void GlSceneView::setOverlayText (const juce::String& text)
{
    const std::lock_guard<std::mutex> g (lock);
    overlay = text;
    context.triggerRepaint();
}

void GlSceneView::restart()
{
    const std::lock_guard<std::mutex> g (lock);
    resetPending = true;
}

void GlSceneView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black); // (only while the context is not up)
}

void GlSceneView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick)
        onDoubleClick();
}

void GlSceneView::mouseDown (const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

juce::Rectangle<int> GlSceneView::frameArea (const vs::Scene& s, juce::Rectangle<int> bounds) const
{
    const auto aspect = (double) s.width / (double) juce::jmax (1, s.height);
    auto area = bounds.toDouble();
    auto frame = area.getWidth() / juce::jmax (1.0, area.getHeight()) > aspect ? area.withSizeKeepingCentre (area.getHeight() * aspect, area.getHeight())
                                                                              : area.withSizeKeepingCentre (area.getWidth(), area.getWidth() / aspect);
    return frame.toNearestInt();
}

void GlSceneView::timerCallback()
{
    if (failed.load())
        return;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (! ready.load() && now - attachedMs > 5000.0)
    {
        // no context after five seconds (no OpenGL here): the editor goes back to the 2D picture
        failed = true;
        stopTimer();
        visualgl::markFailed ("no OpenGL context");
        if (onFailed)
            onFailed ("no OpenGL context");
        return;
    }
    if (! ready.load() || ! isShowing())
        return;
    Frame f;
    vs::Scene s;
    if (! produce || ! produce (f, s))
        return;
    {
        const std::lock_guard<std::mutex> g (lock);
        pendingFrame = f;
        pendingScene = std::move (s);
        havePending = true;
    }
    context.triggerRepaint();
}

void GlSceneView::newOpenGLContextCreated()
{
    context.setSwapInterval (1);
    if (! renderer.create (context))
    {
        failed = true;
        const auto why = renderer.error();
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<GlSceneView> (this), why] {
            visualgl::markFailed (why);
            if (safe != nullptr && safe->onFailed)
                safe->onFailed (why);
        });
        return;
    }
    ready = true;
}

void GlSceneView::renderOpenGL()
{
    if (failed.load())
        return;
    Frame f;
    bool fresh = false;
    juce::String text;
    {
        const std::lock_guard<std::mutex> g (lock);
        if (resetPending)
        {
            renderer.reset();
            resetPending = false;
        }
        if (havePending)
        {
            f = pendingFrame;
            drawnScene = pendingScene;
            havePending = false;
            fresh = true;
        }
        text = overlay;
    }
    const auto scale = context.getRenderingScale();
    const auto fullW = juce::roundToInt (getWidth() * scale), fullH = juce::roundToInt (getHeight() * scale);
    if (fullW <= 0 || fullH <= 0)
        return;
    auto area = frameArea (drawnScene, { 0, 0, fullW, fullH });
    if (fresh && area.getWidth() > 8 && area.getHeight() > 8)
    {
        // drawn at most maxLiveWidth x maxLiveHeight inside, stretched to the window
        auto rw = area.getWidth(), rh = area.getHeight();
        const auto shrink = juce::jmin (1.0, (double) maxLiveWidth / rw, (double) maxLiveHeight / rh);
        rw = juce::jmax (16, juce::roundToInt (rw * shrink));
        rh = juce::jmax (16, juce::roundToInt (rh * shrink));
        renderer.setTiming (timing.load());
        renderer.render (drawnScene, f, rw, rh);
        gpuMsLast = renderer.lastGpuMs();
    }
    glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());
    glViewport (0, 0, fullW, fullH);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    // (GL's viewport origin is the bottom-left corner)
    renderer.present ({ area.getX(), fullH - area.getBottom(), area.getWidth(), area.getHeight() }, fullH);
    if (text.isNotEmpty())
    {
        if (auto g2 = juce::createOpenGLGraphicsContext (context, fullW, fullH))
        {
            juce::Graphics g (*g2);
            g.addTransform (juce::AffineTransform::scale ((float) scale));
            auto box = juce::Rectangle<float> ((float) getWidth(), 50.0f).withSizeKeepingCentre (440.0f, 30.0f).translated (0.0f, 10.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRoundedRectangle (box, 6.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText (text, box, juce::Justification::centred);
        }
    }
}

void GlSceneView::openGLContextClosing()
{
    renderer.release();
    ready = false;
}

juce::Image GlSceneView::snapshot()
{
    if (! ready.load())
        return {};
    struct Shot
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        int w = 0, h = 0;
        std::vector<std::uint8_t> pixels;
    };
    auto shot = std::make_shared<Shot>();
    context.executeOnGLThread ([this, shot] (juce::OpenGLContext&) {
        const auto w = renderer.width(), h = renderer.height();
        std::vector<std::uint8_t> px ((std::size_t) juce::jmax (0, w * h * 4));
        const auto ok = w > 0 && h > 0 && renderer.read (px.data());
        const std::lock_guard<std::mutex> g (shot->m);
        if (ok)
        {
            shot->w = w;
            shot->h = h;
            shot->pixels = std::move (px);
        }
        shot->done = true;
        shot->cv.notify_all();
    }, false);
    std::unique_lock<std::mutex> g (shot->m);
    // (the message thread waits: the GL thread never needs it, component painting is off)
    shot->cv.wait_for (g, std::chrono::milliseconds (2000), [&] { return shot->done; });
    if (shot->w <= 0)
        return {};
    juce::Image image (juce::Image::ARGB, shot->w, shot->h, false, juce::SoftwareImageType());
    const juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < shot->h; ++y)
    {
        auto* line = data.getLinePointer (y);
        std::memcpy (line, shot->pixels.data() + (std::size_t) y * (std::size_t) shot->w * 4, (std::size_t) shot->w * 4);
        for (int x = 0; x < shot->w; ++x)
            line[x * 4 + 3] = 255;
    }
    return image;
}

// ---------------------------------------------------------------------------------------------------------------
// The video export's GPU
// ---------------------------------------------------------------------------------------------------------------
GlFrameExporter::GlFrameExporter()
{
    setOpaque (true);
    context.setRenderer (this);
    context.setComponentPaintingEnabled (false);
    context.setContinuousRepainting (false);
    visualgl::viewStarted();
    context.attachTo (*this);
}

GlFrameExporter::~GlFrameExporter()
{
    closing = true;
    context.detach();
    visualgl::viewStopped();
}

bool GlFrameExporter::waitReady (int ms)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + ms;
    while (state.load() == 0 && juce::Time::getMillisecondCounterHiRes() < until)
        juce::Thread::sleep (20);
    return state.load() == 1;
}

void GlFrameExporter::reset()
{
    if (state.load() != 1)
        return;
    context.executeOnGLThread ([this] (juce::OpenGLContext&) { renderer.reset(); }, false);
}

bool GlFrameExporter::renderFrame (const vs::Scene& scene, const Frame& frame, int width, int height, std::uint8_t* bgra)
{
    if (state.load() != 1 || closing.load())
        return false;
    struct Job
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, ok = false, abandoned = false;
        vs::Scene scene;
        Frame frame;
        std::vector<std::uint8_t> pixels;
    };
    auto job = std::make_shared<Job>();
    job->scene = scene;
    job->frame = frame;
    context.executeOnGLThread ([this, job, width, height] (juce::OpenGLContext&) {
        {
            const std::lock_guard<std::mutex> g (job->m);
            if (job->abandoned)
                return;
        }
        renderer.render (job->scene, job->frame, width, height);
        std::vector<std::uint8_t> px ((std::size_t) width * (std::size_t) height * 4);
        const auto ok = renderer.width() == width && renderer.height() == height && renderer.read (px.data());
        const std::lock_guard<std::mutex> g (job->m);
        job->ok = ok;
        job->pixels = std::move (px);
        job->done = true;
        job->cv.notify_all();
    }, false);
    std::unique_lock<std::mutex> g (job->m);
    if (! job->cv.wait_for (g, std::chrono::milliseconds (15000), [&] { return job->done; }))
    {
        job->abandoned = true;
        error = "the GPU did not answer for 15 s";
        return false;
    }
    if (! job->ok)
    {
        error = renderer.error().isNotEmpty() ? renderer.error() : juce::String ("the GPU could not draw the frame");
        return false;
    }
    std::memcpy (bgra, job->pixels.data(), job->pixels.size());
    return true;
}

void GlFrameExporter::newOpenGLContextCreated()
{
    context.setSwapInterval (0); // (the export runs as fast as the encoder takes the frames)
    if (! renderer.create (context))
    {
        error = renderer.error();
        state = -1;
        return;
    }
    state = 1;
}

void GlFrameExporter::renderOpenGL()
{
    // the thumbnail: the last exported frame drawn small (JUCE runs a frame's job just before this)
    const auto scale = context.getRenderingScale();
    const auto tw = juce::roundToInt (getWidth() * scale), th = juce::roundToInt (getHeight() * scale);
    if (tw <= 0 || th <= 0)
        return;
    glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());
    glViewport (0, 0, tw, th);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    const auto fw = renderer.width(), fh = renderer.height();
    if (fw <= 0 || fh <= 0)
        return;
    const auto aspect = (double) fw / (double) fh;
    const auto area = ((double) tw / th > aspect ? juce::Rectangle<double> (0, 0, th * aspect, th) : juce::Rectangle<double> (0, 0, tw, tw / aspect))
                          .withCentre ({ tw * 0.5, th * 0.5 })
                          .toNearestInt();
    renderer.present ({ area.getX(), th - area.getBottom(), area.getWidth(), area.getHeight() }, th);
}

void GlFrameExporter::openGLContextClosing()
{
    renderer.release();
    if (state.load() == 1)
        state = -1;
}
} // namespace lynx
