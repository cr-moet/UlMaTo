#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier fileProperty ("referenceFile"), startProperty ("rangeStart"),
                           endProperty ("rangeEnd"), profileProperty ("referenceProfile"),
                           learnedProperty ("learnedProfile"), referenceDynamicsProperty ("referenceDynamics"),
                           learnedDynamicsProperty ("learnedDynamics"), orderProperty ("stageOrder"),
                           dynamicsFirstProperty ("dynamicsFirst");   // older versions

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

    // Dynamics profile <-> text: the histograms (centre and range follow from them)
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
    for (size_t b = 0; b < tapeParams.size(); ++b)
        tapeParams[b] = apvts.getRawParameterValue (ParamIDs::tape[b]);
    wowParam         = apvts.getRawParameterValue (ParamIDs::wow);
    flutterParam     = apvts.getRawParameterValue (ParamIDs::flutter);
    crackleParam     = apvts.getRawParameterValue (ParamIDs::crackle);
    tubeParam        = apvts.getRawParameterValue (ParamIDs::tube);
    transformerParam = apvts.getRawParameterValue (ParamIDs::transformer);
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

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto addPercent = [&] (const char* id, const juce::String& name, float defaultValue)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name,
                                                                 juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), defaultValue, percent));
    };

    layout.add (std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::amount, 1 }, "Match",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent),
             std::make_unique<juce::AudioParameterFloat> (
                 juce::ParameterID { ParamIDs::response, 1 }, "Response", responseRange, 1.0f,
                 juce::AudioParameterFloatAttributes().withLabel ("s")
                     .withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 1.0f ? 2 : 1) + " s"; })),
             std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::bypass, 1 }, "Bypass", false));

    addPercent (ParamIDs::dynamics, "Dynamics Match", 50.0f);
    for (int b = 0; b < Dynamics::numBands; ++b)
        addPercent (ParamIDs::tape[b], juce::String ("Tape ") + Dynamics::bandNames[b], 0.0f);
    addPercent (ParamIDs::wow,         "Wow",         0.0f);
    addPercent (ParamIDs::flutter,     "Flutter",     0.0f);
    addPercent (ParamIDs::crackle,     "Vinyl Crackle", 0.0f);
    addPercent (ParamIDs::tube,        "Tube",        0.0f);
    addPercent (ParamIDs::transformer, "Transformer", 0.0f);

    // Graphic EQ
    const auto hz = juce::AudioParameterFloatAttributes().withLabel ("Hz")
                        .withStringFromValueFunction ([] (float v, int) { return v < 1000.0f ? juce::String (v, 0) + " Hz" : juce::String (v / 1000.0f, 2) + " kHz"; });
    const auto db = juce::AudioParameterFloatAttributes().withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; });
    juce::NormalisableRange<float> freqRange (20.0f, 20000.0f, 0.1f);
    freqRange.setSkewForCentre (632.0f);   // log-like: the centre of the knob range is the geometric middle
    juce::NormalisableRange<float> qRange (0.2f, 8.0f, 0.01f);
    qRange.setSkewForCentre (1.0f);
    for (int b = 0; b < Tone::numBands; ++b)
    {
        const auto n = juce::String (b + 1);
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::eqFreq[b], 1 }, "EQ " + n + " Freq", freqRange, Tone::eqDefaultFreq[b], hz));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::eqGain[b], 1 }, "EQ " + n + " Gain",
                                                                 juce::NormalisableRange<float> (-18.0f, 18.0f, 0.01f), 0.0f, db));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::eqQ[b], 1 }, "EQ " + n + " Q", qRange,
                                                                 Tone::eqTypes[b] == Tone::EqType::bell ? 1.0f : 0.71f));
    }

    // PhaseQ: left and right
    const auto degrees = juce::AudioParameterFloatAttributes().withLabel ("deg")
                             .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 0) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0")); });
    for (int n = 0; n < Tone::numPhaseNodes; ++n)
    {
        const auto i = "Phase " + juce::String (n + 1);
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::phaseOn[n], 1 }, i + " On", false));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamIDs::phaseSide[n], 1 }, i + " Channel", juce::StringArray { "Left", "Right" }, 0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamIDs::phaseShape[n], 1 }, i + " Shape", juce::StringArray { "Bell", "Low Shelf", "High Shelf" }, 0));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::phaseBypass[n], 1 }, i + " Bypass", false));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::phaseFreq[n], 1 }, i + " Freq", freqRange, 1000.0f, hz));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::phaseAmount[n], 1 }, i + " Amount",
                                                                 juce::NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f, degrees));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::phaseQ[n], 1 }, i + " Q", qRange, 1.0f,
                                                                 juce::AudioParameterFloatAttributes()
                                                                     .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); })));
    }

    // Limiter
    juce::NormalisableRange<float> releaseRange (1.0f, 1000.0f, 0.1f);
    releaseRange.setSkewForCentre (100.0f);
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::limiterOn, 1 }, "Limiter On", true));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::limiterTruePeak, 1 }, "Limiter True Peak", true));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::limiterInput, 1 }, "Limiter Input",
                                                             juce::NormalisableRange<float> (0.0f, 24.0f, 0.01f), 0.0f, db));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::limiterCeiling, 1 }, "Limiter Ceiling",
                                                             juce::NormalisableRange<float> (-6.0f, 0.0f, 0.01f), -0.3f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dBFS")
                                                                 .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2) + " dBFS"; })));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ParamIDs::limiterRelease, 1 }, "Limiter Release", releaseRange, 100.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("ms")
                                                                 .withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + " ms"; })));
    addPercent (ParamIDs::limiterLink, "Limiter Link", 100.0f);

    static constexpr const char* stageNames[] { "EQ Match", "Dynamics", "Analog Heat", "PhaseQ", "Graphic EQ", "Limiter" };
    for (int stage = 0; stage < MatchEngine::numStages; ++stage)
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamIDs::stageBypass[stage], 1 },
                                                                juce::String (stageNames[stage]) + " Bypass", false));
    return layout;
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

    const auto stages = readStageSettings();

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
        engine.process (chunk, amountParam->load() * 0.01f, responseParam->load(), dynamicsParam->load() * 0.01f, stages);

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

MatchEngine::StageSettings UlMaToProcessor::readStageSettings() const
{
    MatchEngine::StageSettings s;
    for (size_t b = 0; b < tapeParams.size(); ++b)
        s.analog.tape[b] = tapeParams[b]->load() * 0.01f;
    s.analog.wow         = wowParam->load() * 0.01f;
    s.analog.flutter     = flutterParam->load() * 0.01f;
    s.analog.crackle     = crackleParam->load() * 0.01f;
    s.analog.tube        = tubeParam->load() * 0.01f;
    s.analog.transformer = transformerParam->load() * 0.01f;

    const auto value = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    for (size_t b = 0; b < (size_t) Tone::numBands; ++b)
    {
        s.eq.freq[b] = value (ParamIDs::eqFreq[b]);
        s.eq.gain[b] = value (ParamIDs::eqGain[b]);
        s.eq.q[b]    = value (ParamIDs::eqQ[b]);
    }
    for (size_t n = 0; n < (size_t) Tone::numPhaseNodes; ++n)
    {
        auto& node   = s.phase.node[n];
        node.on      = value (ParamIDs::phaseOn[n]) > 0.5f && value (ParamIDs::phaseBypass[n]) < 0.5f;
        node.channel = value (ParamIDs::phaseSide[n]) > 0.5f ? 1 : 0;
        node.shape   = (Tone::PhaseShape) juce::roundToInt (value (ParamIDs::phaseShape[n]));
        node.freq    = value (ParamIDs::phaseFreq[n]);
        node.amount  = value (ParamIDs::phaseAmount[n]);
        node.q       = value (ParamIDs::phaseQ[n]);
    }
    s.phase.valid = true;

    s.limiter.on        = value (ParamIDs::limiterOn) > 0.5f;
    s.limiter.truePeak  = value (ParamIDs::limiterTruePeak) > 0.5f;
    s.limiter.inputDb   = value (ParamIDs::limiterInput);
    s.limiter.ceilingDb = value (ParamIDs::limiterCeiling);
    s.limiter.releaseMs = value (ParamIDs::limiterRelease);
    s.limiter.link      = value (ParamIDs::limiterLink) * 0.01f;
    for (size_t stage = 0; stage < s.bypass.size(); ++stage)
        s.bypass[stage] = value (ParamIDs::stageBypass[stage]) > 0.5f;
    return s;
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

void UlMaToProcessor::setStageOrder (const MatchEngine::Order& order)
{
    juce::String text;
    for (int stage : order)
        text << stage;
    apvts.state.setProperty (orderProperty, text, nullptr);
    engine.setStageOrder (order);
}

// Stored as e.g. "012345". Older versions: three stages ("012", the new ones follow) or
// only "dynamics first"
MatchEngine::Order UlMaToProcessor::getStageOrder() const
{
    auto text = apvts.state.getProperty (orderProperty).toString();
    if (text.length() == 3)
        text << "345";
    else if (text.isEmpty() && (bool) apvts.state.getProperty (dynamicsFirstProperty, false))
        text = "102345";

    MatchEngine::Order order {};
    std::array<bool, MatchEngine::numStages> used {};
    if (text.length() == MatchEngine::numStages)
    {
        bool valid = true;
        for (int i = 0; i < MatchEngine::numStages; ++i)
        {
            const int stage = (int) (text[i] - '0');
            valid = valid && stage >= 0 && stage < MatchEngine::numStages && ! used[(size_t) stage];
            if (valid)
            {
                used[(size_t) stage] = true;
                order[(size_t) i] = stage;
            }
        }
        if (valid)
            return order;
    }
    return MatchEngine::defaultOrder();
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
            engine.setStageOrder (getStageOrder());

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
