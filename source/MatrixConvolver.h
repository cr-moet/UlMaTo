#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

//==============================================================================
// Stereo 2x2 filter matrix with uniformly partitioned overlap-save convolution:
//
//     out L = a * in L + b * in R
//     out R = c * in L + d * in R
//
// This covers left/right EQ, phase between the channels and mid/side EQ in one stage.
// Both channels share one complex FFT per block (left in the real part, right in the
// imaginary part), the products are summed in the frequency domain and both outputs come
// from one inverse FFT.
//
// Filters are passed as partition spectra (see makeSpectra), so they can be prepared on
// another thread. A filter change is calculated with the old and the new filters from the
// same input history and cross-faded over one block - no clicks.
//
// Shortcuts that give exactly the same result:
//   identity filters (a = d = delayed impulse, b = c = 0): only a delay, no FFT at all; the
//   spectra of the input are rebuilt from the kept samples when a real filter comes,
//   diagonal filters (b = c = 0): the cross products are skipped.
//==============================================================================
class MatrixConvolver final
{
public:
    struct Filters
    {
        std::vector<float> a, b, c, d;   // partition spectra
        bool identity = false;           // only a delay of 'delay' samples
        bool diagonal = false;           // b and c are zero
        int delay = 0;
    };

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
        for (auto* v : { &yL, &yR })
            v->assign ((size_t) spectrumSize, 0.0f);
        for (auto* v : { &timeIn, &timeOut, &freq })
            v->assign ((size_t) fftSize, {});
        for (auto* v : { &outL, &outR, &fadeL, &fadeR })
            v->assign ((size_t) blockSize, 0.0f);

        // The last numPartitions + 1 blocks of the input (for the delay and to rebuild the history)
        ringLength = (numPartitions + 1) * blockSize;
        ringL.assign ((size_t) ringLength, 0.0f);
        ringR.assign ((size_t) ringLength, 0.0f);
        ringPosition = 0;
        position = 0;
        historyValid = true;
    }

    // Impulse response (length samples) -> partition spectra; call on any thread with its
    // own FFT (size 2 * blockSize) and scratch buffer
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
        std::copy (left,  left  + blockSize, ringL.begin() + ringPosition);
        std::copy (right, right + blockSize, ringR.begin() + ringPosition);

        const bool needSpectra = ! filters.identity || (next != nullptr && ! next->identity);
        if (needSpectra)
        {
            if (! historyValid)
                rebuildHistory();
            addToHistory (position, ringPosition);
        }
        else
            historyValid = false;

        output (filters, outL, outR);
        if (next == nullptr)
        {
            std::copy (outL.begin(), outL.end(), left);
            std::copy (outR.begin(), outR.end(), right);
        }
        else
        {
            output (*next, fadeL, fadeR);
            const float step = 1.0f / (float) blockSize;
            for (int i = 0; i < blockSize; ++i)
            {
                const float w = (float) (i + 1) * step;
                left[i]  = outL[(size_t) i] + w * (fadeL[(size_t) i] - outL[(size_t) i]);
                right[i] = outR[(size_t) i] + w * (fadeR[(size_t) i] - outR[(size_t) i]);
            }
        }

        position = (position + 1) % numPartitions;
        ringPosition = (ringPosition + blockSize) % ringLength;
    }

private:
    using Complex = std::complex<float>;

    // One block of the filtered output
    void output (const Filters& f, std::vector<float>& l, std::vector<float>& r) noexcept
    {
        if (f.identity)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto slot = (size_t) ((ringPosition + i - f.delay + ringLength * 2) % ringLength);
                l[(size_t) i] = ringL[slot];
                r[(size_t) i] = ringR[slot];
            }
        }
        else
            render (f, l, r);
    }

    // Spectra of [previous block, block at blockStart] (from the ring) into history slot
    // 'slot': one complex FFT of left + i right, then split into the two real spectra
    void addToHistory (int slot, int blockStart) noexcept
    {
        const int previous = (blockStart - blockSize + ringLength) % ringLength;
        for (int i = 0; i < blockSize; ++i)
        {
            timeIn[(size_t) i]               = { ringL[(size_t) (previous + i)],   ringR[(size_t) (previous + i)] };
            timeIn[(size_t) (blockSize + i)] = { ringL[(size_t) (blockStart + i)], ringR[(size_t) (blockStart + i)] };
        }
        fft->perform (timeIn.data(), freq.data(), false);

        float* hl = historyL.data() + slot * spectrumSize;
        float* hr = historyR.data() + slot * spectrumSize;
        for (int k = 0; k <= blockSize; ++k)
        {
            const Complex z = freq[(size_t) k], w = std::conj (freq[(size_t) ((fftSize - k) % fftSize)]);
            // left = (z + w) / 2, right = (z - w) / 2i
            hl[2 * k]     = 0.5f * (z.real() + w.real());
            hl[2 * k + 1] = 0.5f * (z.imag() + w.imag());
            hr[2 * k]     = 0.5f * (z.imag() - w.imag());
            hr[2 * k + 1] = -0.5f * (z.real() - w.real());
        }
    }

    // After a stretch with identity filters: the spectra of the last blocks again
    void rebuildHistory() noexcept
    {
        for (int p = 1; p < numPartitions; ++p)   // slot 'position' is filled right after
        {
            const int slot = (position - p + numPartitions) % numPartitions;
            addToHistory (slot, (ringPosition - p * blockSize + ringLength * 2) % ringLength);
        }
        historyValid = true;
    }

    // Sum over all partitions of (input spectra) x (filter matrix), one inverse FFT
    void render (const Filters& f, std::vector<float>& l, std::vector<float>& r) noexcept
    {
        std::fill (yL.begin(), yL.end(), 0.0f);
        std::fill (yR.begin(), yR.end(), 0.0f);
        float* outLeft = yL.data();
        float* outRight = yR.data();

        for (int p = 0; p < numPartitions; ++p)
        {
            const int slot = ((position - p + numPartitions) % numPartitions) * spectrumSize;
            const int part = p * spectrumSize;
            const float* xl = historyL.data() + slot;
            const float* xr = historyR.data() + slot;
            const float *a = f.a.data() + part, *d = f.d.data() + part;

            if (f.diagonal)
            {
                for (int k = 0; k < spectrumSize; k += 2)
                {
                    outLeft[k]      += xl[k] * a[k] - xl[k + 1] * a[k + 1];
                    outLeft[k + 1]  += xl[k] * a[k + 1] + xl[k + 1] * a[k];
                    outRight[k]     += xr[k] * d[k] - xr[k + 1] * d[k + 1];
                    outRight[k + 1] += xr[k] * d[k + 1] + xr[k + 1] * d[k];
                }
            }
            else
            {
                const float *b = f.b.data() + part, *c = f.c.data() + part;
                for (int k = 0; k < spectrumSize; k += 2)
                {
                    const float lr = xl[k], li = xl[k + 1], rr = xr[k], ri = xr[k + 1];
                    outLeft[k]      += lr * a[k] - li * a[k + 1] + rr * b[k] - ri * b[k + 1];
                    outLeft[k + 1]  += lr * a[k + 1] + li * a[k] + rr * b[k + 1] + ri * b[k];
                    outRight[k]     += lr * c[k] - li * c[k + 1] + rr * d[k] - ri * d[k + 1];
                    outRight[k + 1] += lr * c[k + 1] + li * c[k] + rr * d[k + 1] + ri * d[k];
                }
            }
        }

        // Both outputs from one inverse FFT: left + i right (hermitian halves mirrored)
        for (int k = 0; k <= blockSize; ++k)
        {
            const Complex left (outLeft[2 * k], outLeft[2 * k + 1]), right (outRight[2 * k], outRight[2 * k + 1]);
            freq[(size_t) k] = { left.real() - right.imag(), left.imag() + right.real() };   // left + i right
            if (k > 0 && k < blockSize)
                freq[(size_t) (fftSize - k)] = { left.real() + right.imag(), right.real() - left.imag() };   // conj (left) + i conj (right)
        }
        fft->perform (freq.data(), timeOut.data(), true);

        for (int i = 0; i < blockSize; ++i)   // overlap-save: the second half is valid
        {
            l[(size_t) i] = timeOut[(size_t) (blockSize + i)].real();
            r[(size_t) i] = timeOut[(size_t) (blockSize + i)].imag();
        }
    }

    int blockSize = 512, fftSize = 1024, numPartitions = 1, spectrumSize = 1026, position = 0;
    int ringLength = 1024, ringPosition = 0;
    bool historyValid = true;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> historyL, historyR, yL, yR, outL, outR, fadeL, fadeR, ringL, ringR;
    std::vector<Complex> timeIn, timeOut, freq;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatrixConvolver)
};
