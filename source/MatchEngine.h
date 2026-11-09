#pragma once

#include "Spectrum.h"
#include "PartitionedConvolver.h"

//==============================================================================
// Live analysis and the linear-phase mid/side match EQ.
//
// Audio thread  - process(): hands input and output to the engine thread and applies
//                 the current EQ (partitioned convolution, mid and side).
// Engine thread - tracks the long-term spectrum of the input, compares it with the
//                 reference and designs new FIR filters every 100 ms. New filters are
//                 handed over without locks and cross-faded without clicks.
//
// Matching mid and side separately also matches the stereo width and the phase
// correlation per band, which follow from the side/mid ratio.
//==============================================================================
class MatchEngine final : private juce::Thread
{
public:
    static constexpr float  maxCorrectionDb = 12.0f;   // limit per band
    static constexpr int    blockSize       = 512;     // internal processing block
    static constexpr double updateSeconds   = 0.1;     // filter update interval

    MatchEngine();
    ~MatchEngine() override;

    // Called while the audio is stopped (prepareToPlay / releaseResources)
    void prepare (double sampleRate, int maxBlockSize);
    void release();

    // Internal block + linear-phase filters (half their length)
    int getLatencySamples() const noexcept   { return blockSize + firLength / 2; }

    // Audio thread. amount = 0 ... 1 (0 = no correction, signal only delayed),
    // response = time constant of the live analysis in seconds
    void process (juce::AudioBuffer<float>& buffer, float amount, float response) noexcept;

    // Reference profile (from the analysis of the reference song); any thread except audio
    void setReference (const Spectrum::Profile& reference);

    // For the display: spectrum of the processed signal
    Spectrum::Profile getOutputProfile() const;

private:
    //==========================================================================
    // Collects one stereo signal from the audio thread into overlapping frames
    struct Tap
    {
        static constexpr int fifoSize = 1 << 16;
        juce::AbstractFifo fifo { fifoSize };
        juce::AudioBuffer<float> fifoBuffer { 2, fifoSize };
        juce::AudioBuffer<float> frame { 2, Spectrum::Analyser::size };
        int newSamples = 0;
        Spectrum::Analyser analyser;

        void reset();
        void push (const float* l, const float* r, int numSamples) noexcept;   // audio thread
        void read (double decay);                                              // engine thread
    };

    // Filter spectra for mid and side; two slots: one in use by the audio thread,
    // the other filled by the engine thread
    struct FilterSet
    {
        std::vector<float> mid, side;
    };

    void run() override;
    void updateFilters();
    void designFilter (const Spectrum::Curve& gainDb, std::vector<float>& spectra);

    double sampleRate = 48000.0;
    int firLength = 4096;

    std::atomic<float> currentAmount { 0.0f }, currentResponse { 1.0f };
    Tap input, output;

    // Engine thread: filter design
    std::unique_ptr<juce::dsp::FFT> designFft, partitionFft;
    std::vector<float> designData, designWindow, designIr, partitionScratch;
    Spectrum::Curve appliedMid {}, appliedSide {};
    bool filtersValid = false;
    std::atomic<bool> referenceChanged { false };

    // Hand-over: the engine writes slot [1 - active] while newFilter is false,
    // the audio thread switches (cross-fade) and clears newFilter
    std::array<FilterSet, 2> filters;
    std::atomic<int> activeFilter { 0 };
    std::atomic<bool> newFilter { false };

    // Audio thread: block buffering and convolution
    PartitionedConvolver midConvolver, sideConvolver;
    std::vector<float> midBlock, sideBlock, midOut, sideOut;
    int blockPosition = 0;
    bool prepared = false;

    // Shared with the GUI / message thread
    mutable juce::CriticalSection stateLock;
    Spectrum::Profile reference, outputProfile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatchEngine)
};
