#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Presets for The Solution Suite.
//  * Factory presets are built in and also appear in the DAW's own preset/program menu.
//  * User presets are .solpreset files (XML) in one shared folder, so every module sees them:
//      Windows: %APPDATA%\Billet Audio\The Solution\Presets
//      macOS:   ~/Library/Application Support/Billet Audio/The Solution/Presets
//  * A preset stores every parameter; anything missing (e.g. parameters added in a later
//    version) falls back to its default, so old presets keep working.
// All methods run on the message thread.
class PresetManager
{
public:
    struct Info { juce::String name, tag; bool factory = false; };

    explicit PresetManager (juce::AudioProcessorValueTreeState& state,
                            juce::File folder = getDefaultFolder());

    static juce::File getDefaultFolder();
    static constexpr const char* fileExtension = ".solpreset";

    juce::Array<Info> getAllPresets() const;          // factory first, then user presets A-Z
    int getNumFactoryPresets() const;
    juce::String getFactoryPresetName (int index) const;

    bool loadPreset (const juce::String& name);       // factory or user
    bool loadFactoryPreset (int index);
    bool savePreset (const juce::String& name, const juce::String& tag);
    bool deletePreset (const juce::String& name);     // user presets only

    juce::String getCurrentPresetName() const { return currentName; }
    const juce::File& getFolder() const { return folder; }

private:
    struct Factory { const char* name; const char* tag; std::vector<std::pair<const char*, float>> values; };
    static const std::vector<Factory>& factoryPresets();

    void applyValues (const juce::NamedValueSet& values, const juce::String& presetName);
    juce::File fileFor (const juce::String& name) const;

    juce::AudioProcessorValueTreeState& apvts;
    juce::File folder;
    juce::String currentName;
};
