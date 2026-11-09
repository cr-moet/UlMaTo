#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>

//==============================================================================
// Stereo 2x2 filter matrix with uniformly partitioned overlap-save convolution:
//
//     out L = a * in L + b * in R
//     out R = c * in L + d * in R
//
// This covers left/right EQ, phase between the channels and mid/side EQ in one
// stage. Each input is transformed once per block, the products are summed in the
// frequency domain, and each output needs a single inverse FFT.
//
// Filters are passed as partition spectra (see makeSpectra), so they can be prepared
// on another thread. A filter change is calculated with the old and the new filters
// from the same input history and cross-faded over one block - no clicks.
//==============================================================================
class MatrixConvolver final
{
public:
    struct Filters { std::vector<float> a, b, c, d; };   // partition spectra

    MatrixConvolver() = default;

    void prepare (int newBlockSize, int filterLength)
    {
        blockSize     = newBlockSize;
        fftSize       = 2 * blockSize;
        numPartitions = (filterLength + blockSize - 1) / blockSize;
        spectrumSize  = fftSize + 2;   // fftSize / 2 + 1 complex values

        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (fftSize)));
        for (auto* h : { &historyL, &historyR })
            h->assign ((size_t) (numPartitions * spectrumSize), 0.0f);
        for (auto* v : { &previousL, &previousR })
            v->assign ((size_t) blockSize, 0.0f);
        for (auto* v : { &work, &outL, &outR, &nextL, &nextR })
            v->assign ((size_t) fftSize * 2, 0.0f);
        position = 0;
    }

    // Impulse response (length samples) -> partition spectra; call on any thread
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

    // Filters exactly one block (blockSize samples) of both channels in place.
    // With 'next' the output fades from 'filters' to 'next' over the block.
    void process (float* left, float* right, const Filters& filters, const Filters* next) noexcept
    {
        addToHistory (left,  previousL, historyL);
        addToHistory (right, previousR, historyR);

        render (filters, outL, outR);
        const float* l = outL.data() + blockSize;   // overlap-save: the second half is valid
        const float* r = outR.data() + blockSize;

        if (next == nullptr)
        {
            std::copy (l, l + blockSize, left);
            std::copy (r, r + blockSize, right);
        }
        else
        {
            render (*next, nextL, nextR);
            const float* nl = nextL.data() + blockSize;
            const float* nr = nextR.data() + blockSize;
            const float step = 1.0f / (float) blockSize;
            for (int i = 0; i < blockSize; ++i)
            {
                const float w = (float) (i + 1) * step;
                left[i]  = l[i] + w * (nl[i] - l[i]);
                right[i] = r[i] + w * (nr[i] - r[i]);
            }
        }

        position = (position + 1) % numPartitions;
    }

private:
    // Spectrum of [previous block, this block] into the frequency-domain delay line
    void addToHistory (const float* data, std::vector<float>& previous, std::vector<float>& history) noexcept
    {
        std::copy (previous.begin(), previous.end(), work.begin());
        std::copy (data, data + blockSize, work.begin() + blockSize);
        std::fill (work.begin() + fftSize, work.end(), 0.0f);
        fft->performRealOnlyForwardTransform (work.data(), true);
        std::copy (work.begin(), work.begin() + spectrumSize, history.begin() + position * spectrumSize);
        std::copy (data, data + blockSize, previous.begin());
    }

    // Sum over all partitions of (input spectra) x (filter matrix), then inverse FFTs
    void render (const Filters& f, std::vector<float>& yL, std::vector<float>& yR) noexcept
    {
        std::fill (yL.begin(), yL.end(), 0.0f);
        std::fill (yR.begin(), yR.end(), 0.0f);

        for (int p = 0; p < numPartitions; ++p)
        {
            const int slot = ((position - p + numPartitions) % numPartitions) * spectrumSize;
            const int part = p * spectrumSize;
            const float* xl = historyL.data() + slot;
            const float* xr = historyR.data() + slot;
            const float *a = f.a.data() + part, *b = f.b.data() + part, *c = f.c.data() + part, *d = f.d.data() + part;

            for (int k = 0; k < spectrumSize; k += 2)
            {
                const float lr = xl[k], li = xl[k + 1], rr = xr[k], ri = xr[k + 1];
                yL[(size_t) k]     += lr * a[k] - li * a[k + 1] + rr * b[k] - ri * b[k + 1];
                yL[(size_t) k + 1] += lr * a[k + 1] + li * a[k] + rr * b[k + 1] + ri * b[k];
                yR[(size_t) k]     += lr * c[k] - li * c[k + 1] + rr * d[k] - ri * d[k + 1];
                yR[(size_t) k + 1] += lr * c[k + 1] + li * c[k] + rr * d[k + 1] + ri * d[k];
            }
        }

        fft->performRealOnlyInverseTransform (yL.data());
        fft->performRealOnlyInverseTransform (yR.data());
    }

    int blockSize = 512, fftSize = 1024, numPartitions = 1, spectrumSize = 1026, position = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> historyL, historyR, previousL, previousR, work, outL, outR, nextL, nextR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatrixConvolver)
};
