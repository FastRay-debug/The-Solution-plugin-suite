#pragma once
#include "Vendor.h"

#include <juce_dsp/juce_dsp.h>
#include <RTNeural/RTNeural.h>
#include <memory>
#include <sstream>

// Neural "Character" stage: runs an RTNeural model (e.g. a capture of a tape machine, tube preamp
// or console channel) on the module's processed signal. Models are RTNeural / Keras-style JSON
// files in a shared Models folder:
//   Windows: %APPDATA%\Billet Audio\The Solution\Models
//   macOS:   ~/Library/Application Support/Billet Audio/The Solution/Models
// Loading happens off the audio thread; the audio thread swaps models without blocking or freeing.
namespace SolutionDSP
{
    class NeuralCharacter
    {
    public:
        struct Model
        {
            std::unique_ptr<RTNeural::Model<float>> channel[2];
            juce::String name;
            double trainedSampleRate = 0.0;     // 0 = unknown
        };

        static juce::File getModelsFolder()
        {
            return SolutionVendor::userFolder ("Models");
        }

        static juce::StringArray listModels()
        {
            juce::StringArray names;
            for (const auto& f : getModelsFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
                names.add (f.getFileNameWithoutExtension());
            names.sort (true);
            return names;
        }

        // Message/background thread. Mono-in/mono-out models only (one instance per channel).
        static std::unique_ptr<Model> load (const juce::File& file, juce::String& error)
        {
            const auto text = file.loadFileAsString();
            if (text.isEmpty()) { error = "Couldn't read " + file.getFileName(); return {}; }
            auto m = std::make_unique<Model>();
            m->name = file.getFileNameWithoutExtension();
            try
            {
                for (auto& ch : m->channel)
                {
                    std::stringstream ss (text.toStdString());
                    auto json = nlohmann::json::parse (ss);
                    if (json.contains ("samplerate")) m->trainedSampleRate = json["samplerate"].get<double>();
                    if (json.contains ("sample_rate")) m->trainedSampleRate = json["sample_rate"].get<double>();
                    ch = RTNeural::json_parser::parseJson<float> (json, false);
                    if (ch == nullptr) { error = file.getFileName() + " isn't a supported RTNeural model"; return {}; }
                    if (ch->getInSize() != 1 || ch->getOutSize() != 1) { error = file.getFileName() + " must take 1 input and give 1 output (mono audio model)"; return {}; }
                    ch->reset();
                }
            }
            catch (const std::exception& e)
            {
                error = file.getFileName() + ": " + juce::String (e.what());
                return {};
            }
            return m;
        }

        // Message thread: queue a new model (nullptr = no model). Never blocks the audio thread.
        void setModel (std::unique_ptr<Model> newModel)
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            pending = std::move (newModel);
            hasPending = true;
        }

        // Message thread: free a model the audio thread has finished with.
        void housekeeping()
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            if (! hasPending.load()) pending.reset();
        }

        juce::String getModelName() const { const juce::SpinLock::ScopedLockType sl (lock); return liveName; }
        double getTrainedSampleRate() const { return trainedRate.load(); }

        // Audio thread.
        void process (float* l, float* r, int numSamples, float driveDb, float amount)
        {
            if (hasPending.load (std::memory_order_acquire))
            {
                const juce::SpinLock::ScopedTryLockType sl (lock);
                if (sl.isLocked())
                {
                    std::swap (live, pending);          // the old model is freed later on the message thread
                    hasPending = false;
                    liveName = live != nullptr ? live->name : juce::String();
                    trainedRate = live != nullptr ? live->trainedSampleRate : 0.0;
                    if (live != nullptr) for (auto& ch : live->channel) ch->reset();
                }
            }
            if (live == nullptr || amount <= 0.0005f) return;
            const float drive = juce::Decibels::decibelsToGain (driveDb);
            float* chans[2] = { l, r };
            for (int c = 0; c < 2; ++c)
            {
                auto& model = *live->channel[c];
                auto* d = chans[c];
                for (int i = 0; i < numSamples; ++i)
                {
                    const float in = d[i] * drive;
                    const float y = model.forward (&in);
                    d[i] = d[i] + amount * (y - d[i]);
                }
            }
        }

    private:
        mutable juce::SpinLock lock;
        std::unique_ptr<Model> live, pending;
        std::atomic<bool> hasPending { false };
        std::atomic<double> trainedRate { 0.0 };
        juce::String liveName;
    };
}
