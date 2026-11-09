#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier fileProperty ("referenceFile"), startProperty ("rangeStart"),
                           endProperty ("rangeEnd"), profileProperty ("referenceProfile"),
                           learnedProperty ("learnedProfile"), referenceDynamicsProperty ("referenceDynamics"),
                           learnedDynamicsProperty ("learnedDynamics"), dynamicsFirstProperty ("dynamicsFirst");

    constexpr int dryBlockSize = 1024;   // bypass is handled in chunks of this size

    // Reference profile <-> text (stored with the project, so no new analysis is needed)
    juce::String profileToString (const Spectrum::Profile& p)
    {
        if (! p.valid)
            return {};

        juce::MemoryOutputStream out;
        for (const auto* curve : { &p.left, &p.right, &p.mid, &p.side, &p.crossRe, &p.crossIm })
            out.write (curve->data(), sizeof (Spectrum::Curve));
        return out.getMemoryBlock().toBase64Encoding();
    }

    Spectrum::Profile profileFromString (const juce::String& text)
    {
        Spectrum::Profile p;
        juce::MemoryBlock block;
        if (! block.fromBase64Encoding (text) || block.getSize() != 6 * sizeof (Spectrum::Curve))
            return p;

        auto* src = static_cast<const char*> (block.getData());
        for (auto* curve : { &p.left, &p.right, &p.mid, &p.side, &p.crossRe, &p.crossIm })
        {
            std::memcpy (curve->data(), src, sizeof (Spectrum::Curve));
            src += sizeof (Spectrum::Curve);
        }
        p.valid = true;
        return p;
    }

    // Dynamics profile <-> text: the histograms (median and range follow from them)
    juce::String dynamicsToString (const Dynamics::Profile& p)
    {
        return p.valid ? juce::MemoryBlock (p.histogram.data(), sizeof (p.histogram)).toBase64Encoding() : juce::String();
    }

    Dynamics::Profile dynamicsFromString (const juce::String& text)
    {
        Dynamics::Profile p;
        juce::MemoryBlock block;
        if (block.fromBase64Encoding (text) && block.getSize() == sizeof (p.histogram))
        {
            std::memcpy (p.histogram.data(), block.getData(), sizeof (p.histogram));
            p.evaluate();
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
    dynamicsParam = apvts.getRawParameterValue (ParamIDs::dynamics);
    bypassParam   = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::bypass));
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

    const auto percent = juce::AudioParameterFloatAttributes().withLabel ("%")
                             .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 0) + " %"; });

    return { std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::amount, 1 }, "Match",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent),
             std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::response, 1 }, "Response", responseRange, 1.0f,
                 juce::AudioParameterFloatAttributes().withLabel ("s")
                     .withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 1.0f ? 2 : 1) + " s"; })),
             std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::dynamics, 1 }, "Dynamics Match",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent),
             std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::bypass, 1 }, "Bypass", false) };
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
    const int latency = engine.getLatencySamples();
    setLatencySamples (latency);

    dryDelay.setSize (2, latency);
    dryDelay.clear();
    dryBlock.setSize (2, dryBlockSize);
    dryPosition = 0;
    bypassMix.reset (sampleRate, 0.02);
    bypassMix.setCurrentAndTargetValue (bypassParam->get() ? 1.0f : 0.0f);
}

void UlMaToProcessor::releaseResources()
{
    engine.release();
}

void UlMaToProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    bypassMix.setTargetValue (bypassParam->get() ? 1.0f : 0.0f);

    // In chunks: keep the delayed input, process, mix (the engine keeps running when
    // bypassed, so switching back is seamless)
    for (int start = 0; start < buffer.getNumSamples(); start += dryBlockSize)
    {
        const int count = juce::jmin (dryBlockSize, buffer.getNumSamples() - start);
        const int latency = dryDelay.getNumSamples();

        for (int i = 0; i < count; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float& slot = dryDelay.getWritePointer (ch)[dryPosition];
                dryBlock.setSample (ch, i, slot);
                slot = buffer.getSample (ch, start + i);
            }
            dryPosition = (dryPosition + 1) % latency;
        }

        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, count);
        engine.process (chunk, amountParam->load() * 0.01f, responseParam->load(), dynamicsParam->load() * 0.01f);

        if (! bypassMix.isSmoothing() && bypassMix.getCurrentValue() == 0.0f)
            continue;

        for (int i = 0; i < count; ++i)
        {
            const float mix = bypassMix.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* out = chunk.getWritePointer (ch, i);
                *out += mix * (dryBlock.getSample (ch, i) - *out);
            }
        }
    }
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

void UlMaToProcessor::setDynamicsFirst (bool first)
{
    apvts.state.setProperty (dynamicsFirstProperty, first, nullptr);
    engine.setDynamicsFirst (first);
}

bool UlMaToProcessor::isDynamicsFirst() const
{
    return (bool) apvts.state.getProperty (dynamicsFirstProperty, false);
}

Spectrum::Profile UlMaToProcessor::getReferenceProfile() const
{
    const juce::ScopedLock lock (referenceLock);
    return referenceProfile;
}

Dynamics::Profile UlMaToProcessor::getReferenceDynamics() const
{
    const juce::ScopedLock lock (referenceLock);
    return referenceDynamics;
}

void UlMaToProcessor::setReferenceProfile (const Spectrum::Profile& profile, const Dynamics::Profile& dynamics)
{
    {
        const juce::ScopedLock lock (referenceLock);
        referenceProfile  = profile;
        referenceDynamics = dynamics;
    }
    engine.setReference (profile);
    engine.setReferenceDynamics (dynamics);
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
            auto dynamics = std::make_unique<Dynamics::Analyser>();
            dynamics->prepare (sr);
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
                dynamics->process (frame.getReadPointer (0, size - hop), frame.getReadPointer (1, size - hop), count);

                analysisProgress = (float) ((double) (pos - start) / (double) (end - start));
            }

            if (analyser->hasData())
                setReferenceProfile (analyser->getProfile (sr), dynamics->getProfile());
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
    state.setProperty (learnedProperty, profileToString (engine.getLearned()), nullptr);
    state.setProperty (referenceDynamicsProperty, dynamicsToString (getReferenceDynamics()), nullptr);
    state.setProperty (learnedDynamicsProperty, dynamicsToString (engine.getLearnedDynamics()), nullptr);

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
            const auto learned = profileFromString (state.getProperty (learnedProperty).toString());
            const auto dynamics = dynamicsFromString (state.getProperty (referenceDynamicsProperty).toString());
            const auto learnedDynamics = dynamicsFromString (state.getProperty (learnedDynamicsProperty).toString());
            for (const auto& id : { profileProperty, learnedProperty, referenceDynamicsProperty, learnedDynamicsProperty })
                state.removeProperty (id, nullptr);
            apvts.replaceState (state);
            engine.setDynamicsFirst (isDynamicsFirst());

            if (profile.valid)
                setReferenceProfile (profile, dynamics);   // saved by an older version: no dynamics until analysed again
            if (learnedDynamics.valid)
                engine.setLearnedDynamics (learnedDynamics);
            if (learned.valid)
                engine.setLearned (learned);
        }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new UlMaToProcessor();
}
