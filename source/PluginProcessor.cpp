#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier fileProperty ("referenceFile"), startProperty ("rangeStart"),
                           endProperty ("rangeEnd"), profileProperty ("referenceProfile");

    // Reference profile <-> text (stored with the project, so no new analysis is needed)
    juce::String profileToString (const Spectrum::Profile& p)
    {
        if (! p.valid)
            return {};

        juce::MemoryOutputStream out;
        for (const auto* curve : { &p.left, &p.right, &p.mid, &p.side, &p.correlation })
            out.write (curve->data(), sizeof (Spectrum::Curve));
        return out.getMemoryBlock().toBase64Encoding();
    }

    Spectrum::Profile profileFromString (const juce::String& text)
    {
        Spectrum::Profile p;
        juce::MemoryBlock block;
        if (block.fromBase64Encoding (text) && block.getSize() == 5 * sizeof (Spectrum::Curve))
        {
            auto* src = static_cast<const char*> (block.getData());
            for (auto* curve : { &p.left, &p.right, &p.mid, &p.side, &p.correlation })
            {
                std::memcpy (curve->data(), src, sizeof (Spectrum::Curve));
                src += sizeof (Spectrum::Curve);
            }
            p.valid = true;
        }
        return p;
    }
}

//==============================================================================
UlMaToProcessor::UlMaToProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "UlMaTo", createParameterLayout())
{
    formatManager.registerBasicFormats();
    amountParam   = apvts.getRawParameterValue (ParamIDs::amount);
    responseParam = apvts.getRawParameterValue (ParamIDs::response);
}

UlMaToProcessor::~UlMaToProcessor()
{
    shuttingDown = true;   // ends a running analysis early
    analysisPool.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout UlMaToProcessor::createParameterLayout()
{
    // Response: time constant of the live analysis (short = follows the music quickly)
    juce::NormalisableRange<float> responseRange (0.2f, 5.0f, 0.01f);
    responseRange.setSkewForCentre (1.0f);

    return { std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::amount, 1 }, "Match",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
                 juce::AudioParameterFloatAttributes().withLabel ("%")
                     .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 0) + " %"; })),
             std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::response, 1 }, "Response", responseRange, 1.0f,
                 juce::AudioParameterFloatAttributes().withLabel ("s")
                     .withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 1.0f ? 2 : 1) + " s"; })) };
}

bool UlMaToProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && layouts.getMainInputChannelSet() == out;
}

void UlMaToProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (engine.getLatencySamples());
}

void UlMaToProcessor::releaseResources()
{
    engine.release();
}

void UlMaToProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    engine.process (buffer, amountParam->load() * 0.01f, responseParam->load());
}

//==============================================================================
void UlMaToProcessor::setReferenceFile (const juce::File& file)
{
    apvts.state.setProperty (fileProperty, file.getFullPathName(), nullptr);
    setReferenceRange (0.0, 0.0);   // 0 ... 0 = whole song
}

juce::File UlMaToProcessor::getReferenceFile() const
{
    const auto path = apvts.state.getProperty (fileProperty).toString();
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

void UlMaToProcessor::setReferenceRange (double startSeconds, double endSeconds)
{
    apvts.state.setProperty (startProperty, startSeconds, nullptr);
    apvts.state.setProperty (endProperty, endSeconds, nullptr);
}

juce::Range<double> UlMaToProcessor::getReferenceRange() const
{
    return { (double) apvts.state.getProperty (startProperty, 0.0),
             (double) apvts.state.getProperty (endProperty, 0.0) };
}

Spectrum::Profile UlMaToProcessor::getReferenceProfile() const
{
    const juce::ScopedLock lock (referenceLock);
    return referenceProfile;
}

void UlMaToProcessor::setReferenceProfile (const Spectrum::Profile& profile)
{
    {
        const juce::ScopedLock lock (referenceLock);
        referenceProfile = profile;
    }
    engine.setReference (profile);
    ++referenceVersion;
}

// Reads the selected range of the reference song frame by frame (the song is never
// loaded completely) and calculates its profile on the analysis thread
void UlMaToProcessor::analyseReference()
{
    const auto file  = getReferenceFile();
    const auto range = getReferenceRange();
    if (analysing.load() || ! file.existsAsFile())
        return;

    analysing = true;
    analysisProgress = 0.0f;

    analysisPool.addJob ([this, file, range]
    {
        if (std::unique_ptr<juce::AudioFormatReader> reader { formatManager.createReaderFor (file) })
        {
            constexpr int size = Spectrum::Analyser::size, hop = Spectrum::Analyser::hop;
            const double sr = reader->sampleRate;
            const auto length = reader->lengthInSamples;

            auto start = juce::jlimit ((juce::int64) 0, length, (juce::int64) (range.getStart() * sr));
            auto end   = juce::jlimit ((juce::int64) 0, length, (juce::int64) (range.getEnd() * sr));
            if (end - start < hop)   // no (useful) selection: whole song
                std::tie (start, end) = std::pair { (juce::int64) 0, length };

            auto analyser = std::make_unique<Spectrum::Analyser>();
            juce::AudioBuffer<float> frame (2, size);
            frame.clear();

            for (auto pos = start; pos < end && ! shuttingDown.load(); pos += hop)
            {
                for (int ch = 0; ch < 2; ++ch)   // keep the newest samples (75 % overlap, regions overlap: memmove)
                    std::memmove (frame.getWritePointer (ch), frame.getReadPointer (ch, hop), sizeof (float) * (size_t) (size - hop));

                const auto count = (int) juce::jmin ((juce::int64) hop, end - pos);
                frame.clear (size - hop, hop);
                reader->read (&frame, size - hop, count, pos, true, true);   // mono: both channels
                analyser->addFrame (frame.getReadPointer (0), frame.getReadPointer (1));

                analysisProgress = (float) ((double) (pos - start) / (double) (end - start));
            }

            if (analyser->hasData())
                setReferenceProfile (analyser->getProfile (sr));
        }

        analysisProgress = 1.0f;
        analysing = false;
    });
}

//==============================================================================
juce::AudioProcessorEditor* UlMaToProcessor::createEditor()
{
    return new UlMaToEditor (*this);
}

void UlMaToProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (profileProperty, profileToString (getReferenceProfile()), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void UlMaToProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            const auto profile = profileFromString (state.getProperty (profileProperty).toString());
            state.removeProperty (profileProperty, nullptr);
            apvts.replaceState (state);

            if (profile.valid)
                setReferenceProfile (profile);
        }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new UlMaToProcessor();
}
