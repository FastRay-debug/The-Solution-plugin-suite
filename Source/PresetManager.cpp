#include "PresetManager.h"
#include "Vendor.h"

namespace
{
    constexpr int presetFormatVersion = 1;
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, juce::File presetFolder)
    : apvts (state), folder (std::move (presetFolder)) {}

juce::File PresetManager::getDefaultFolder()
{
    return SolutionVendor::userFolder ("Presets");
}

const std::vector<PresetManager::Factory>& PresetManager::factoryPresets()
{
    // Values are in real units (degrees, dB, Hz, 0-1 amounts). Unlisted parameters use defaults.
    static const std::vector<Factory> list {
        { "Init",                    "#Master",    {} },
        { "-0.5 dBFS Master Glue",   "#Master",    { { "stage_depth", 0.28f }, { "stage_width", 1.05f }, { "stage_ms_mid", 1.2f } } },
        { "Car Check - '69 Lincoln", "#Master",    { { "room_profile", 2.0f }, { "room_match_mix", 1.0f } } },
        { "Club Check",              "#Master",    { { "room_profile", 3.0f }, { "room_match_mix", 1.0f } } },
        { "Mastering Room Check",    "#Master",    { { "room_profile", 1.0f }, { "room_match_mix", 1.0f } } },
        { "Lincoln Sub Warmth",      "#SubBass",   { { "stage_depth", 0.48f }, { "stage_anchor_hz", 85.0f }, { "stage_width", 0.9f } } },
        { "Mono-Safe Sub",           "#SubBass",   { { "stage_depth", 0.4f }, { "stage_anchor_hz", 120.0f }, { "stage_width", 0.8f }, { "clipper_shape", 1.0f } } },
        { "Pantheon Wide Hall",      "#Cinematic", { { "stage_depth", 0.35f }, { "stage_width", 1.45f }, { "room_match_mix", 0.42f }, { "room_profile", 1.0f } } },
        { "Wide Chorus Lift",        "#Cinematic", { { "stage_width", 1.5f }, { "stage_ms_side", 2.5f }, { "ultra_wide", 0.4f } } },
        { "Overhead Air",            "#Cinematic", { { "stage_elevation", 45.0f }, { "stage_depth", 0.15f } } },
    };
    return list;
}

int PresetManager::getNumFactoryPresets() const { return (int) factoryPresets().size(); }

juce::String PresetManager::getFactoryPresetName (int index) const
{
    return juce::isPositiveAndBelow (index, getNumFactoryPresets()) ? juce::String (factoryPresets()[(size_t) index].name) : juce::String();
}

juce::File PresetManager::fileFor (const juce::String& name) const
{
    return folder.getChildFile (juce::File::createLegalFileName (name.trim()) + fileExtension);
}

juce::Array<PresetManager::Info> PresetManager::getAllPresets() const
{
    juce::Array<Info> result;
    for (const auto& f : factoryPresets())
        result.add ({ f.name, f.tag, true });

    auto files = folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    files.sort();
    for (const auto& file : files)
        if (auto xml = juce::XmlDocument::parse (file))
            if (xml->hasTagName ("SolutionPreset"))
                result.add ({ xml->getStringAttribute ("name", file.getFileNameWithoutExtension()),
                              xml->getStringAttribute ("tag", "#User"), false });
    return result;
}

void PresetManager::applyValues (const juce::NamedValueSet& values, const juce::String& presetName)
{
    apvts.undoManager->beginNewTransaction ("Load preset " + presetName);
    for (auto* p : apvts.processor.getParameters())
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (param == nullptr)
            continue;
        const auto* v = values.getVarPointer (param->getParameterID());
        const float norm = v != nullptr ? param->convertTo0to1 ((float) *v) : param->getDefaultValue();
        param->beginChangeGesture();
        param->setValueNotifyingHost (norm);
        param->endChangeGesture();
    }
    currentName = presetName;
}

bool PresetManager::loadFactoryPreset (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumFactoryPresets()))
        return false;
    const auto& f = factoryPresets()[(size_t) index];
    juce::NamedValueSet values;
    for (const auto& [id, value] : f.values)
        values.set (id, value);
    applyValues (values, f.name);
    return true;
}

bool PresetManager::loadPreset (const juce::String& name)
{
    for (int i = 0; i < getNumFactoryPresets(); ++i)
        if (name == factoryPresets()[(size_t) i].name)
            return loadFactoryPreset (i);

    auto xml = juce::XmlDocument::parse (fileFor (name));
    if (xml == nullptr || ! xml->hasTagName ("SolutionPreset"))
        return false;

    juce::NamedValueSet values;
    for (auto* p : xml->getChildWithTagNameIterator ("PARAM"))
        values.set (p->getStringAttribute ("id"), p->getDoubleAttribute ("value"));
    applyValues (values, xml->getStringAttribute ("name", name));
    return true;
}

bool PresetManager::savePreset (const juce::String& name, const juce::String& tag)
{
    const auto clean = name.trim().substring (0, 64);
    if (clean.isEmpty())
        return false;
    for (const auto& f : factoryPresets())
        if (clean == f.name)
            return false;   // never shadow a factory preset

    juce::XmlElement xml ("SolutionPreset");
    xml.setAttribute ("name", clean);
    xml.setAttribute ("tag", tag.isNotEmpty() ? tag : juce::String ("#User"));
    xml.setAttribute ("formatVersion", presetFormatVersion);
   #ifdef JucePlugin_VersionString
    xml.setAttribute ("pluginVersion", JucePlugin_VersionString);
   #endif
    for (auto* p : apvts.processor.getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            auto* e = xml.createNewChildElement ("PARAM");
            e->setAttribute ("id", param->getParameterID());
            e->setAttribute ("value", (double) param->convertFrom0to1 (param->getValue()));
        }

    if (! folder.createDirectory())
        return false;
    if (! xml.writeTo (fileFor (clean)))
        return false;
    currentName = clean;
    return true;
}

bool PresetManager::deletePreset (const juce::String& name)
{
    for (const auto& f : factoryPresets())
        if (name == f.name)
            return false;
    const auto file = fileFor (name);
    return file.existsAsFile() && file.deleteFile();
}
