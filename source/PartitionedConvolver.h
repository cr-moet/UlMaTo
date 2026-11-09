#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>

//==============================================================================
// Uniformly partitioned overlap-save convolution for one channel.
//
// The filter is passed as partition spectra (see makeSpectra), so new filters can be
// prepared on another thread. When the filter changes, the block is calculated with
// the old and the new filter from the same input history and cross-faded, so filter
// changes cause no clicks - however often they happen.
//==============================================================================
class PartitionedConvolver final
{
public:
    PartitionedConvolver() = default;

    void prepare (int newBlockSize, int filterLength)
    {
        blockSize     = newBlockSize;
        fftSize       = 2 * blockSize;
        numPartitions = (filterLength + blockSize - 1) / blockSize;
        spectrumSize  = fftSize + 2;   // fftSize / 2 + 1 complex values

        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (fftSize)));
        history.assign ((size_t) (numPartitions * spectrumSize), 0.0f);
        previous.assign ((size_t) blockSize, 0.0f);
        work.assign ((size_t) fftSize * 2, 0.0f);
        outA.assign ((size_t) fftSize * 2, 0.0f);
        outB.assign ((size_t) fftSize * 2, 0.0f);
        position = 0;
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (previous.begin(), previous.end(), 0.0f);
        position = 0;
    }

    int getSpectraSize() const noexcept  { return numPartitions * spectrumSize; }

    // Impulse response (filterLength samples) -> partition spectra; call on any thread
    // with its own FFT (size 2 * blockSize) and scratch buffer
    static void makeSpectra (const float* ir, int length, int blockSize, juce::dsp::FFT& partitionFft,
                             std::vector<float>& scratch, std::vector<float>& spectra)
    {
        const int fftSize = 2 * blockSize, spectrumSize = fftSize + 2;
        const int partitions = (length + blockSize - 1) / blockSize;
        scratch.resize ((size_t) fftSize * 2);
        spectra.resize ((size_t) (partitions * spectrumSize));

        for (int p = 0; p < partitions; ++p)
        {
            std::fill (scratch.begin(), scratch.end(), 0.0f);
            const int n = juce::jmin (blockSize, length - p * blockSize);
            std::copy (ir + p * blockSize, ir + p * blockSize + n, scratch.begin());
            partitionFft.performRealOnlyForwardTransform (scratch.data(), true);
            std::copy (scratch.begin(), scratch.begin() + spectrumSize, spectra.begin() + p * spectrumSize);
        }
    }

    // Filters exactly one block (blockSize samples) in place. With 'next' the output
    // fades from 'filter' to 'next' over the block.
    void process (float* data, const float* filter, const float* next) noexcept
    {
        // Spectrum of [previous block, this block] into the frequency-domain delay line
        std::copy (previous.begin(), previous.end(), work.begin());
        std::copy (data, data + blockSize, work.begin() + blockSize);
        std::fill (work.begin() + fftSize, work.end(), 0.0f);
        fft->performRealOnlyForwardTransform (work.data(), true);
        std::copy (work.begin(), work.begin() + spectrumSize, history.begin() + position * spectrumSize);
        std::copy (data, data + blockSize, previous.begin());

        convolve (filter, outA);
        const float* a = outA.data() + blockSize;   // overlap-save: the second half is valid

        if (next == nullptr)
            std::copy (a, a + blockSize, data);
        else
        {
            convolve (next, outB);
            const float* b = outB.data() + blockSize;
            const float step = 1.0f / (float) blockSize;
            for (int i = 0; i < blockSize; ++i)
            {
                const float w = (float) (i + 1) * step;
                data[i] = a[i] + w * (b[i] - a[i]);
            }
        }

        position = (position + 1) % numPartitions;
    }

private:
    // Sum of (input spectrum of partition p) x (filter partition p), then inverse FFT
    void convolve (const float* filter, std::vector<float>& out) noexcept
    {
        std::fill (out.begin(), out.end(), 0.0f);

        for (int p = 0; p < numPartitions; ++p)
        {
            const float* x = history.data() + ((position - p + numPartitions) % numPartitions) * spectrumSize;
            const float* h = filter + p * spectrumSize;
            for (int k = 0; k < spectrumSize; k += 2)
            {
                out[(size_t) k]     += x[k] * h[k]     - x[k + 1] * h[k + 1];
                out[(size_t) k + 1] += x[k] * h[k + 1] + x[k + 1] * h[k];
            }
        }

        fft->performRealOnlyInverseTransform (out.data());
    }

    int blockSize = 512, fftSize = 1024, numPartitions = 1, spectrumSize = 1026, position = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> history, previous, work, outA, outB;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PartitionedConvolver)
};
