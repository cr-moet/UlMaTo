#pragma once

#include "Spectrum.h"

//==============================================================================
// Live analysis and the linear-phase mid/side match EQ.
//
// Audio thread  - process(): hands the input to the engine thread and applies the
//                 current EQ (two FFT convolutions, mid and side).
// Engine thread - tracks the long-term spectrum of the input (time constant a few
//                 seconds), compares it with the reference, and designs new FIR
//                 filters several times per second. New filters are handed to the
//                 audio thread without locking; the convolution cross-fades to them.
//
// Matching mid and side separately also matches the stereo width and the phase
// correlation per band, which follow from the side/mid ratio.
//==============================================================================
class MatchEngine final : private juce::Thread
{
public:
    static constexpr float  maxCorrectionDb  = 12.0f;   // limit per band
    static constexpr double trackingSeconds  = 3.0;     // time constant of the live spectrum
    static constexpr double updateSeconds    = 0.25;    // filter update interval

    MatchEngine();
    ~MatchEngine() override;

    // Called while the audio is stopped (prepareToPlay / releaseResources)
    void prepare (double sampleRate, int maxBlockSize);
    void release();

    // Linear-phase filters delay the signal by half their length
    int getLatencySamples() const noexcept   { return firLength / 2; }

    // Audio thread. amount = 0 ... 1 (0 = no correction, signal only delayed)
    void process (juce::AudioBuffer<float>& buffer, float amount) noexcept;

    // Reference profile (from the analysis of the reference song); any thread except audio
    void setReference (const Spectrum::Profile& reference);

    // For the display: live spectrum and the correction curves currently applied (dB)
    struct State
    {
        Spectrum::Profile live;
        Spectrum::Curve mid {}, side {};
    };
    State getState() const;

private:
    void run() override;
    void readInput();
    void updateFilters();
    void designFilter (const Spectrum::Curve& gainDb, juce::AudioBuffer<float>& ir);

    double sampleRate = 48000.0;
    int firLength = 4096;

    // Audio thread -> engine thread
    static constexpr int fifoSize = 1 << 16;
    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> fifoBuffer { 2, fifoSize };
    std::atomic<float> currentAmount { 0.0f };

    // Engine thread: live analysis and filter design
    Spectrum::Analyser analyser;
    juce::AudioBuffer<float> frame { 2, Spectrum::Analyser::size };   // last 'size' samples
    int newSamples = 0;
    double frameDecay = 1.0;
    std::unique_ptr<juce::dsp::FFT> designFft;
    std::vector<float> designData, designWindow;
    Spectrum::Curve appliedMid {}, appliedSide {};
    bool filtersValid = false;
    std::atomic<bool> referenceChanged { false };

    // Engine thread -> audio thread: new filters (moved, never copied on the audio thread)
    juce::SpinLock pendingLock;
    juce::AudioBuffer<float> pendingMid, pendingSide;
    bool pendingReady = false;

    // Audio thread: mid/side convolution
    juce::dsp::ConvolutionMessageQueue convolutionQueue;
    juce::dsp::Convolution midFilter  { juce::dsp::Convolution::NonUniform { 512 }, convolutionQueue };
    juce::dsp::Convolution sideFilter { juce::dsp::Convolution::NonUniform { 512 }, convolutionQueue };
    juce::AudioBuffer<float> midBuffer, sideBuffer;
    bool prepared = false;

    // Shared with the GUI / message thread
    mutable juce::CriticalSection stateLock;
    Spectrum::Profile reference;
    State state;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatchEngine)
};
