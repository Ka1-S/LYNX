#include "ui/Splash.h"

#include "core/StartupSound.h"
#include "engine/PluginHost.h"
#include "ui/I18n.h"
#include "ui/SplashData.h"
#include "ui/Theme.h"

namespace lynx
{
namespace
{
double clamp01 (double x) { return juce::jlimit (0.0, 1.0, x); }
double easeOutCubic (double u) { return 1.0 - std::pow (1.0 - clamp01 (u), 3.0); }

double easeOutBack (double u)
{
    u = clamp01 (u);
    constexpr double c1 = 1.5, c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow (u - 1.0, 3.0) + c1 * std::pow (u - 1.0, 2.0);
}

// a ball dropped on the floor: 0 = up, 1 = resting; it touches the floor at u = 1/2.75, 2/2.75, 2.5/2.75, 1
double easeOutBounce (double u)
{
    u = clamp01 (u);
    constexpr double n1 = 7.5625, d1 = 2.75;

    if (u < 1.0 / d1)
        return n1 * u * u;
    auto arc = [] (double v, double centre, double floor) { return n1 * (v - centre) * (v - centre) + floor; };
    if (u < 2.0 / d1)
        return arc (u, 1.5 / d1, 0.75);
    if (u < 2.5 / d1)
        return arc (u, 2.25 / d1, 0.9375);
    return arc (u, 2.625 / d1, 0.984375);
}

// the drop lasts so that its first touch is the sound's landing "pop"
constexpr double dropTime = startup::impactTime * 2.75;
constexpr double touches[] = { dropTime / 2.75, dropTime * 2.0 / 2.75, dropTime * 2.5 / 2.75 };
constexpr double touchStrength[] = { 1.0, 0.35, 0.12 };

double impactAt (double t)
{
    double k = 0.0;
    for (int i = 0; i < 3; ++i)
        if (t >= touches[i])
            k += touchStrength[i] * std::exp (-(t - touches[i]) / 0.07);
    return k;
}
} // namespace

// --- options --------------------------------------------------------------------------------------

StartupOptions StartupOptions::load()
{
    StartupOptions options;
    juce::StringArray lines;
    lines.addLines (PluginHost::settingsFolder().getChildFile ("ui-options.txt").loadFileAsString());

    for (const auto& line : lines)
    {
        const auto key = line.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto on = line.fromFirstOccurrenceOf ("=", false, false).trim() != "0";

        if (key == "splash")
            options.splash = on;
        else if (key == "startupSound")
            options.sound = on;
    }

    return options;
}

std::shared_ptr<const SampleData> makeStartupSample()
{
    auto sound = startup::makeSound (48000.0);
    auto sample = std::make_shared<SampleData>();
    sample->left = std::move (sound.left);
    sample->right = std::move (sound.right);
    sample->sampleRate = sound.sampleRate;
    return sample;
}

// --- the splash -----------------------------------------------------------------------------------

SplashScreen::SplashScreen()
    : pepper (juce::ImageCache::getFromMemory (splashdata::pepperPng, (int) sizeof (splashdata::pepperPng)))
{
    setOpaque (false);
    setSize (width, height);
    status = tr ("Starting the audio engine...");
}

SplashScreen::~SplashScreen()
{
    stopTimer();
}

void SplashScreen::showOnDesktop()
{
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        setCentrePosition (display->userArea.getCentre());

    addToDesktop (0); // no title bar, no taskbar entry
    setAlwaysOnTop (true);
    setVisible (true);
    toFront (false);
    startTimerHz (60);
}

void SplashScreen::startClock()
{
    clockStartMs = juce::Time::getMillisecondCounterHiRes();
    time = 0.0;
}

double SplashScreen::remainingDuration (double sinceProcessStart, double landing)
{
    return juce::jmax (landing, minimumDuration - juce::jmax (0.0, sinceProcessStart));
}

double SplashScreen::getElapsed() const
{
    return clockStartMs < 0.0 ? 0.0 : (juce::Time::getMillisecondCounterHiRes() - clockStartMs) / 1000.0;
}

void SplashScreen::setStatus (const juce::String& text)
{
    status = text;
    repaint();
}

void SplashScreen::finish (std::function<void()> whenGone)
{
    onGone = std::move (whenGone);
    fadeStartMs = juce::Time::getMillisecondCounterHiRes();

    if (! isTimerRunning())
        startTimerHz (60);
}

void SplashScreen::setTimeForSnapshot (double seconds)
{
    time = seconds;
    repaint();
}

void SplashScreen::timerCallback()
{
    time = getElapsed();

    if (fadeStartMs >= 0.0)
    {
        const auto f = (juce::Time::getMillisecondCounterHiRes() - fadeStartMs) / 1000.0 / fadeOutTime;
        setAlpha ((float) (1.0 - clamp01 (f)));

        if (f >= 1.0)
        {
            stopTimer();
            setVisible (false);
            removeFromDesktop();

            if (auto gone = std::move (onGone))
                juce::MessageManager::callAsync (std::move (gone)); // may delete this splash
            return;
        }
    }

    repaint();
}

void SplashScreen::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto t = time;
    const auto card = getLocalBounds().toFloat().reduced (0.5f);
    juce::Path cardShape;
    cardShape.addRoundedRectangle (card, 14.0f);

    // card: dark, a touch lighter at the top
    g.setGradientFill (juce::ColourGradient (colour::bg3, 0.0f, 0.0f, colour::bg0, 0.0f, card.getBottom(), false));
    g.fillPath (cardShape);

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (cardShape);

    // the pepper's resting place, and its drop
    const auto pepperHeight = 212.0f;
    const auto pepperWidth = pepperHeight * (float) pepper.getWidth() / (float) juce::jmax (1, pepper.getHeight());
    const auto restX = 168.0f, restBottom = 270.0f;
    const auto fall = 300.0f;
    const auto lift = (float) (1.0 - easeOutBounce (t / dropTime)) * fall;
    const auto impact = (float) impactAt (t);
    const auto settled = (float) easeOutCubic ((t - touches[0]) / 0.5);

    // warm glow behind it: wakes up with the first touch and stays
    {
        const auto glow = juce::jlimit (0.0f, 0.5f, (t < touches[0] ? 0.05f * (float) clamp01 (t / touches[0]) : 0.05f + 0.13f * settled) + 0.2f * impact);
        juce::ColourGradient grad (colour::accent.withAlpha (glow), restX, restBottom - 90.0f, colour::accent.withAlpha (0.0f), restX + 210.0f,
                                   restBottom - 90.0f, true);
        g.setGradientFill (grad);
        g.fillRect (card);
    }

    // a ripple on the floor at the first touch
    if (t > touches[0] && t < touches[0] + 0.7)
    {
        const auto s = (float) ((t - touches[0]) / 0.7);
        const auto r = 24.0f + 150.0f * (float) easeOutCubic (s);
        g.setColour (colour::accent.withAlpha (0.4f * (1.0f - s)));
        g.drawEllipse (restX - r, restBottom - r * 0.16f, 2.0f * r, r * 0.32f, 1.5f);
    }

    // shadow: grows and darkens as the pepper comes down
    {
        const auto near = 1.0f - juce::jlimit (0.0f, 1.0f, lift / fall);
        const auto w = 92.0f * (0.35f + 0.65f * near) * (1.0f + 0.15f * impact);
        g.setColour (juce::Colours::black.withAlpha (0.55f * near * (float) clamp01 (t / 0.15)));
        g.fillEllipse (restX - w / 2.0f, restBottom - 7.0f, w, 12.0f);
    }

    // the pepper: fades in, falls, squashes a little on each touch and settles with a slight tilt
    if (pepper.isValid() && t > 0.0)
    {
        const auto angle = juce::degreesToRadians ((float) (-12.0 - 26.0 * (1.0 - easeOutBack (t / 0.95))));
        const auto squashY = 1.0f - 0.10f * impact, squashX = 1.0f + 0.06f * impact;
        const auto transform = juce::AffineTransform::scale (pepperWidth / (float) pepper.getWidth() * squashX,
                                                             pepperHeight / (float) pepper.getHeight() * squashY)
                                   .translated (-pepperWidth * squashX / 2.0f, -pepperHeight * squashY)
                                   .rotated (angle)
                                   .translated (restX, restBottom - lift);
        g.setOpacity ((float) clamp01 (t / 0.18));
        g.drawImageTransformed (pepper, transform, false);
        g.setOpacity (1.0f);
    }

    // "LYNX SHARE" (v0.1.20 rename): "LYNX" big, "SHARE" under it in the accent colour, spaced out to the same
    // width; the letters slide in one after another
    {
        const juce::String word ("LYNX"), word2 ("SHARE");
        const juce::Font big (juce::FontOptions (78.0f, juce::Font::bold));
        const juce::Font small (juce::FontOptions (30.0f, juce::Font::bold));
        const auto left = 300.0f;
        const auto baseline = 150.0f, baseline2 = 188.0f;

        auto lynxWidth = 0.0f;
        for (int i = 0; i < word.length(); ++i)
            lynxWidth += juce::GlyphArrangement::getStringWidth (big, word.substring (i, i + 1)) + 3.0f;
        lynxWidth -= 3.0f;
        auto shareWidth = 0.0f;
        for (int i = 0; i < word2.length(); ++i)
            shareWidth += juce::GlyphArrangement::getStringWidth (small, word2.substring (i, i + 1));
        const auto tracking = juce::jmax (2.0f, (lynxWidth - shareWidth) / (float) juce::jmax (1, word2.length() - 1));

        const auto drawWord = [&] (const juce::String& w, const juce::Font& f, float y, float gap, double start, juce::Colour c)
        {
            g.setFont (f);
            auto x = left;
            for (int i = 0; i < w.length(); ++i)
            {
                const auto letter = w.substring (i, i + 1);
                const auto p = (float) easeOutCubic ((t - start - 0.075 * i) / 0.30);
                if (p > 0.0f)
                {
                    g.setColour (c.withMultipliedAlpha (p));
                    g.drawSingleLineText (letter, juce::roundToInt (x + (1.0f - p) * 26.0f), juce::roundToInt (y));
                }
                x += juce::GlyphArrangement::getStringWidth (f, letter) + gap;
            }
        };
        drawWord (word, big, baseline, 3.0f, 0.50, colour::textPrimary);
        drawWord (word2, small, baseline2, tracking, 0.68, colour::accent);

        // an accent underline that grows after the words
        const auto line = (float) easeOutCubic ((t - 0.95) / 0.35);
        if (line > 0.0f)
        {
            g.setColour (colour::accent);
            g.fillRoundedRectangle (302.0f, baseline2 + 12.0f, 54.0f * line, 4.0f, 2.0f);
        }

        const auto info = (float) easeOutCubic ((t - 1.05) / 0.35);
        if (info > 0.0f)
        {
            g.setColour (colour::textSecondary.withAlpha (info));
            g.setFont (font (type::title));
            g.drawText (tr ("Digital audio workstation"), 302, (int) baseline2 + 24, 240, 22, juce::Justification::centredLeft);
            g.setColour (colour::textDisabled.withAlpha (info));
            g.setFont (font (type::small));
            g.drawText (tr ("Version {0}", { JUCE_APPLICATION_VERSION_STRING }), 302, (int) baseline2 + 48, 240, 18, juce::Justification::centredLeft);
        }
    }

    // status and progress along the bottom
    {
        const auto bar = juce::Rectangle<float> (24.0f, card.getBottom() - 20.0f, card.getWidth() - 48.0f, 3.0f);
        g.setColour (colour::bg4);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (colour::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) easeOutCubic (juce::jmin (1.0, t / juce::jmax (0.1, barDuration)))), 1.5f);
        g.setColour (colour::textDisabled);
        g.setFont (font (type::small));
        g.drawText (status, juce::Rectangle<float> (24.0f, bar.getY() - 24.0f, card.getWidth() - 48.0f, 18.0f), juce::Justification::centredLeft);
    }

    // hairline border on top of everything
    g.setColour (colour::borderStrong);
    g.strokePath (cardShape, juce::PathStrokeType (1.0f));
}
} // namespace lynx
