/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

MPESynthesiser::MPESynthesiser()
{
}

MPESynthesiser::MPESynthesiser (MPEInstrument& mpeInstrument)
    : MPESynthesiserBase (mpeInstrument)
{
}

MPESynthesiser::~MPESynthesiser()
{
}

//==============================================================================
void MPESynthesiser::startVoice (MPESynthesiserVoice* voice, MPENote noteToStart)
{
    jassert (voice != nullptr);

    voice->currentlyPlayingNote = noteToStart;
    voice->noteOnTime = lastNoteOnCounter++;
    voice->noteStarted();
}

void MPESynthesiser::stopVoice (MPESynthesiserVoice* voice, MPENote noteToStop, bool allowTailOff)
{
    jassert (voice != nullptr);

    voice->currentlyPlayingNote = noteToStop;
    voice->noteStopped (allowTailOff);
}

//==============================================================================
void MPESynthesiser::noteAdded (MPENote newNote)
{
    const ScopedLock sl (voicesLock);

    if (auto* voice = findFreeVoice (newNote, shouldStealVoices))
        startVoice (voice, newNote);
}

void MPESynthesiser::notePressureChanged (MPENote changedNote)
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (voice->isCurrentlyPlayingNote (changedNote))
        {
            voice->currentlyPlayingNote = changedNote;
            voice->notePressureChanged();
        }
    }
}

void MPESynthesiser::notePitchbendChanged (MPENote changedNote)
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (voice->isCurrentlyPlayingNote (changedNote))
        {
            voice->currentlyPlayingNote = changedNote;
            voice->notePitchbendChanged();
        }
    }
}

void MPESynthesiser::noteTimbreChanged (MPENote changedNote)
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (voice->isCurrentlyPlayingNote (changedNote))
        {
            voice->currentlyPlayingNote = changedNote;
            voice->noteTimbreChanged();
        }
    }
}

void MPESynthesiser::noteKeyStateChanged (MPENote changedNote)
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (voice->isCurrentlyPlayingNote (changedNote))
        {
            voice->currentlyPlayingNote = changedNote;
            voice->noteKeyStateChanged();
        }
    }
}

void MPESynthesiser::noteReleased (MPENote finishedNote)
{
    const ScopedLock sl (voicesLock);

    for (auto i = voices.size(); --i >= 0;)
    {
        auto* voice = voices.getUnchecked (i);

        if (voice->isCurrentlyPlayingNote (finishedNote))
            stopVoice (voice, finishedNote, true);
    }
}

void MPESynthesiser::setCurrentPlaybackSampleRate (const double newRate)
{
    MPESynthesiserBase::setCurrentPlaybackSampleRate (newRate);

    const ScopedLock sl (voicesLock);

    turnOffAllVoices (false);

    for (auto i = voices.size(); --i >= 0;)
        voices.getUnchecked (i)->setCurrentSampleRate (newRate);
}

void MPESynthesiser::handleMidiEvent (const MidiMessage& m)
{
    if (m.isController())
        handleController (m.getChannel(), m.getControllerNumber(), m.getControllerValue());
    else if (m.isProgramChange())
        handleProgramChange (m.getChannel(), m.getProgramChangeNumber());

    MPESynthesiserBase::handleMidiEvent (m);
}

MPESynthesiserVoice* MPESynthesiser::findFreeVoice (MPENote noteToFindVoiceFor, bool stealIfNoneAvailable) const
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (! voice->isActive())
            return voice;
    }

    if (stealIfNoneAvailable)
        return findVoiceToSteal (noteToFindVoiceFor);

    return nullptr;
}

MPESynthesiserVoice* MPESynthesiser::findVoiceToSteal (MPENote noteToStealVoiceFor) const
{
    // This voice-stealing algorithm applies the following heuristics:
    // - Re-use the oldest notes first
    // - Protect the lowest & topmost notes, even if sustained, but not if they've been released.


    // apparently you are trying to render audio without having any voices...
    jassert (voices.size() > 0);

    // These are the voices we want to protect (ie: only steal if unavoidable)
    MPESynthesiserVoice* low = nullptr; // Lowest sounding note, might be sustained, but NOT in release phase
    MPESynthesiserVoice* top = nullptr; // Highest sounding note, might be sustained, but NOT in release phase

    // All major OSes use double-locking so this will be lock- and wait-free as long as stealLock is not
    // contended. This is always the case if you do not call findVoiceToSteal on multiple threads at
    // the same time.
    const ScopedLock sl (stealLock);

    // this is a list of voices we can steal, sorted by how long they've been running
    usableVoicesToStealArray.clear();

    for (auto* voice : voices)
    {
        jassert (voice->isActive()); // We wouldn't be here otherwise

        usableVoicesToStealArray.add (voice);

        // NB: Using a functor rather than a lambda here due to scare-stories about
        // compilers generating code containing heap allocations..
        struct Sorter
        {
            bool operator() (const MPESynthesiserVoice* a, const MPESynthesiserVoice* b) const noexcept { return a->noteOnTime < b->noteOnTime; }
        };

        std::sort (usableVoicesToStealArray.begin(), usableVoicesToStealArray.end(), Sorter());

        if (! voice->isPlayingButReleased()) // Don't protect released notes
        {
            auto noteNumber = voice->getCurrentlyPlayingNote().initialNote;

            if (low == nullptr || noteNumber < low->getCurrentlyPlayingNote().initialNote)
                low = voice;

            if (top == nullptr || noteNumber > top->getCurrentlyPlayingNote().initialNote)
                top = voice;
        }
    }

    // Eliminate pathological cases (ie: only 1 note playing): we always give precedence to the lowest note(s)
    if (top == low)
        top = nullptr;

    // If we want to re-use the voice to trigger a new note,
    // then The oldest note that's playing the same note number is ideal.
    if (noteToStealVoiceFor.isValid())
        for (auto* voice : usableVoicesToStealArray)
            if (voice->getCurrentlyPlayingNote().initialNote == noteToStealVoiceFor.initialNote)
                return voice;

    // Oldest voice that has been released (no finger on it and not held by sustain pedal)
    for (auto* voice : usableVoicesToStealArray)
        if (voice != low && voice != top && voice->isPlayingButReleased())
            return voice;

    // Oldest voice that doesn't have a finger on it:
    for (auto* voice : usableVoicesToStealArray)
        if (voice != low && voice != top
             && voice->getCurrentlyPlayingNote().keyState != MPENote::keyDown
             && voice->getCurrentlyPlayingNote().keyState != MPENote::keyDownAndSustained)
            return voice;

    // Oldest voice that isn't protected
    for (auto* voice : usableVoicesToStealArray)
        if (voice != low && voice != top)
            return voice;

    // We've only got "protected" voices now: lowest note takes priority
    jassert (low != nullptr);

    // Duophonic synth: give priority to the bass note:
    if (top != nullptr)
        return top;

    return low;
}

//==============================================================================
void MPESynthesiser::addVoice (MPESynthesiserVoice* const newVoice)
{
    {
        const ScopedLock sl (voicesLock);
        newVoice->setCurrentSampleRate (getSampleRate());
        voices.add (newVoice);
    }

    {
        const ScopedLock sl (stealLock);
        usableVoicesToStealArray.ensureStorageAllocated (voices.size() + 1);
    }
}

void MPESynthesiser::clearVoices()
{
    const ScopedLock sl (voicesLock);
    voices.clear();
}

MPESynthesiserVoice* MPESynthesiser::getVoice (const int index) const
{
    const ScopedLock sl (voicesLock);
    return voices [index];
}

void MPESynthesiser::removeVoice (const int index)
{
    const ScopedLock sl (voicesLock);
    voices.remove (index);
}

void MPESynthesiser::reduceNumVoices (const int newNumVoices)
{
    // we can't possibly get to a negative number of voices...
    jassert (newNumVoices >= 0);

    const ScopedLock sl (voicesLock);

    while (voices.size() > newNumVoices)
    {
        if (auto* voice = findFreeVoice ({}, true))
            voices.removeObject (voice);
        else
            voices.remove (0); // if there's no voice to steal, kill the oldest voice
    }
}

void MPESynthesiser::turnOffAllVoices (bool allowTailOff)
{
    {
        const ScopedLock sl (voicesLock);

        // first turn off all voices (it's more efficient to do this immediately
        // rather than to go through the MPEInstrument for this).
        for (auto* voice : voices)
        {
            voice->currentlyPlayingNote.noteOffVelocity = MPEValue::from7BitInt (64); // some reasonable number
            voice->currentlyPlayingNote.keyState = MPENote::off;

            voice->noteStopped (allowTailOff);
        }
    }

    // finally make sure the MPE Instrument also doesn't have any notes anymore.
    instrument.releaseAllNotes();
}

//==============================================================================
void MPESynthesiser::renderNextSubBlock (AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (voice->isActive())
            voice->renderNextBlock (buffer, startSample, numSamples);
    }
}

void MPESynthesiser::renderNextSubBlock (AudioBuffer<double>& buffer, int startSample, int numSamples)
{
    const ScopedLock sl (voicesLock);

    for (auto* voice : voices)
    {
        if (voice->isActive())
            voice->renderNextBlock (buffer, startSample, numSamples);
    }
}

} // namespace juce
