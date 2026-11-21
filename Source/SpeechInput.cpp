#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <sapi.h>
 #pragma comment (lib, "sapi.lib")
 #pragma comment (lib, "ole32.lib")
#endif
#include "SpeechInput.h"

namespace SolutionVoice
{
SpeechInput::SpeechInput() : juce::Thread ("Solution Voice") {}
SpeechInput::~SpeechInput() { alive->store (false); stop(); }

void SpeechInput::setVocabulary (const juce::StringArray& v)
{
    const juce::ScopedLock sl (lock);
    if (v != words) { words = v; vocabChanged = true; }
}

bool SpeechInput::start (const juce::StringArray& vocabulary, juce::String& whyNot)
{
   #if defined (_WIN32)
    if (isThreadRunning()) { setVocabulary (vocabulary); return true; }
    { const juce::ScopedLock sl (lock); words = vocabulary; }
    started.reset(); startError.clear();
    startThread();
    started.wait (5000);
    if (startError.isNotEmpty()) { whyNot = startError; stopThread (2000); return false; }
    return true;
   #else
    juce::ignoreUnused (vocabulary);
    whyNot = "Voice control uses Windows speech recognition; the Mac version is coming.";
    return false;
   #endif
}

void SpeechInput::stop() { stopThread (3000); }

#if defined (_WIN32)
namespace
{
    template <typename T> struct Com { T* p = nullptr; ~Com() { if (p) p->Release(); } T* operator->() const { return p; } T** operator&() { return &p; } explicit operator bool() const { return p != nullptr; } };

    bool defaultToken (const wchar_t* category, ISpObjectToken** out)
    {
        Com<ISpObjectTokenCategory> cat;
        if (FAILED (CoCreateInstance (CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void**) &cat))) return false;
        if (FAILED (cat->SetId (category, FALSE))) return false;
        WCHAR* id = nullptr;
        if (FAILED (cat->GetDefaultTokenId (&id)) || id == nullptr) return false;
        ISpObjectToken* tok = nullptr;
        const bool ok = SUCCEEDED (CoCreateInstance (CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken, (void**) &tok))
                        && SUCCEEDED (tok->SetId (nullptr, id, FALSE));
        CoTaskMemFree (id);
        if (! ok) { if (tok) tok->Release(); return false; }
        *out = tok; return true;
    }

    // Grammar: up to 10 words, each any word of our vocabulary. Order is left to the parser.
    bool buildGrammar (ISpRecoGrammar* g, const juce::StringArray& vocab)
    {
        SPSTATEHANDLE top = nullptr, word = nullptr;
        if (FAILED (g->GetRule (L"command", 1, SPRAF_TopLevel | SPRAF_Active | SPRAF_Dynamic, TRUE, &top))) return false;
        if (FAILED (g->GetRule (L"word", 2, SPRAF_Dynamic, TRUE, &word))) return false;
        g->ClearRule (top); g->ClearRule (word);
        for (auto& w : vocab)
            g->AddWordTransition (word, nullptr, w.toWideCharPointer(), L" ", SPWT_LEXICAL, 1.0f, nullptr);
        constexpr int maxWords = 10;
        SPSTATEHANDLE prev = top;
        for (int i = 0; i < maxWords; ++i)
        {
            SPSTATEHANDLE next = nullptr;
            if (i < maxWords - 1) g->CreateNewState (top, &next);
            g->AddRuleTransition (prev, next, word, 1.0f, nullptr);             // next == nullptr ends the phrase
            if (next != nullptr) g->AddWordTransition (next, nullptr, nullptr, nullptr, SPWT_LEXICAL, 1.0f, nullptr);   // may stop here
            prev = next;
            if (prev == nullptr) break;
        }
        return SUCCEEDED (g->Commit (0)) && SUCCEEDED (g->SetRuleState (nullptr, nullptr, SPRS_ACTIVE));
    }
}

void SpeechInput::run()
{
    if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) { startError = "Couldn't start Windows speech services."; started.signal(); return; }
    {
        Com<ISpRecognizer> rec; Com<ISpRecoContext> ctx; Com<ISpRecoGrammar> grammar;
        ISpObjectToken* engine = nullptr; ISpObjectToken* mic = nullptr;
        auto fail = [&] (const char* why) { startError = why; started.signal(); };
        if (FAILED (CoCreateInstance (CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer, (void**) &rec))) { fail ("Windows speech recognition isn't installed."); }
        else if (! defaultToken (SPCAT_RECOGNIZERS, &engine) || FAILED (rec->SetRecognizer (engine))) { fail ("No Windows speech recognition language is installed (Settings > Time & language > Speech)."); }
        else if (! defaultToken (SPCAT_AUDIOIN, &mic) || FAILED (rec->SetInput (mic, TRUE))) { fail ("No microphone found - check Windows sound settings and mic privacy permissions."); }
        else if (FAILED (rec->CreateRecoContext (&ctx)) || FAILED (ctx->SetNotifyWin32Event())
                 || FAILED (ctx->SetInterest (SPFEI (SPEI_RECOGNITION), SPFEI (SPEI_RECOGNITION)))
                 || FAILED (ctx->CreateGrammar (1, &grammar))) { fail ("Couldn't start the speech recogniser."); }
        else
        {
            juce::StringArray vocab; { const juce::ScopedLock sl (lock); vocab = words; vocabChanged = false; }
            if (! buildGrammar (grammar.p, vocab)) fail ("Couldn't load the voice command vocabulary.");
            else
            {
                rec->SetRecoState (SPRST_ACTIVE);
                started.signal();
                const HANDLE ev = ctx->GetNotifyEventHandle();
                while (! threadShouldExit())
                {
                    if (vocabChanged.exchange (false))
                    {
                        { const juce::ScopedLock sl (lock); vocab = words; }
                        grammar->SetRuleState (nullptr, nullptr, SPRS_INACTIVE);
                        buildGrammar (grammar.p, vocab);
                    }
                    if (WaitForSingleObject (ev, 100) != WAIT_OBJECT_0) continue;
                    SPEVENT e {}; ULONG fetched = 0;
                    while (ctx->GetEvents (1, &e, &fetched) == S_OK && fetched == 1)
                    {
                        if (e.eEventId == SPEI_RECOGNITION && e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam != 0)
                        {
                            auto* result = reinterpret_cast<ISpRecoResult*> (e.lParam);
                            WCHAR* text = nullptr; float confidence = 0.0f;
                            SPPHRASE* phrase = nullptr;
                            if (SUCCEEDED (result->GetPhrase (&phrase)) && phrase != nullptr) { confidence = phrase->Rule.SREngineConfidence; CoTaskMemFree (phrase); }
                            if (SUCCEEDED (result->GetText ((ULONG) SP_GETWHOLEPHRASE, (ULONG) SP_GETWHOLEPHRASE, TRUE, &text, nullptr)) && text != nullptr)
                            {
                                const juce::String heard (text); CoTaskMemFree (text);
                                juce::MessageManager::callAsync ([this, heard, confidence, alive = alive] { if (alive->load() && onPhrase) onPhrase (heard, juce::jlimit (0.0f, 1.0f, confidence)); });
                            }
                        }
                        if (e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam != 0) reinterpret_cast<IUnknown*> (e.lParam)->Release();
                        else if ((e.elParamType == SPET_LPARAM_IS_POINTER || e.elParamType == SPET_LPARAM_IS_STRING) && e.lParam != 0) CoTaskMemFree ((void*) e.lParam);
                        e = {}; fetched = 0;
                    }
                }
                rec->SetRecoState (SPRST_INACTIVE);
            }
        }
        if (engine) engine->Release();
        if (mic) mic->Release();
    }
    CoUninitialize();
}
#else
void SpeechInput::run() {}
#endif
}
