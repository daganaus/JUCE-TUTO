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

namespace CDBurnerHelpers
{
    IDiscRecorder* enumCDBurners (StringArray* list, int indexToOpen, IDiscMaster** master)
    {
        CoInitialize (0);

        IDiscMaster* dm;
        IDiscRecorder* result = nullptr;

        if (SUCCEEDED (CoCreateInstance (CLSID_MSDiscMasterObj, 0,
                                         CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                                         IID_IDiscMaster,
                                         (void**) &dm)))
        {
            if (SUCCEEDED (dm->Open()))
            {
                IEnumDiscRecorders* drEnum = nullptr;

                if (SUCCEEDED (dm->EnumDiscRecorders (&drEnum)))
                {
                    IDiscRecorder* dr = nullptr;
                    DWORD dummy;
                    int index = 0;

                    while (drEnum->Next (1, &dr, &dummy) == S_OK)
                    {
                        if (indexToOpen == index)
                        {
                            result = dr;
                            break;
                        }
                        else if (list != nullptr)
                        {
                            BSTR path;

                            if (SUCCEEDED (dr->GetPath (&path)))
                                list->add ((const WCHAR*) path);
                        }

                        ++index;
                        dr->Release();
                    }

                    drEnum->Release();
                }

                if (master == 0)
                    dm->Close();
            }

            if (master != nullptr)
                *master = dm;
            else
                dm->Release();
        }

        return result;
    }
}

//==============================================================================
class AudioCDBurner::Pimpl  : public ComBaseClassHelper<IDiscMasterProgressEvents>,
                              public Timer
{
public:
    Pimpl (AudioCDBurner& owner_, IDiscMaster* discMaster_, IDiscRecorder* discRecorder_)
      : owner (owner_), discMaster (discMaster_), discRecorder (discRecorder_), redbook (0),
        listener (0), progress (0), shouldCancel (false)
    {
        HRESULT hr = discMaster->SetActiveDiscMasterFormat (IID_IRedbookDiscMaster, (void**) &redbook);
        jassert (SUCCEEDED (hr));
        hr = discMaster->SetActiveDiscRecorder (discRecorder);
        //jassert (SUCCEEDED (hr));

        lastState = getDiskState();
        startTimer (2000);
    }

    void releaseObjects()
    {
        discRecorder->Close();
        if (redbook != nullptr)
            redbook->Release();
        discRecorder->Release();
        discMaster->Release();
        Release();
    }

    JUCE_COMRESULT QueryCancel (boolean* pbCancel) override
    {
        if (listener != nullptr && ! shouldCancel)
            shouldCancel = listener->audioCDBurnProgress (progress);

        *pbCancel = shouldCancel;

        return S_OK;
    }

    JUCE_COMRESULT NotifyBlockProgress (long nCompleted, long nTotal) override
    {
        progress = nCompleted / (float) nTotal;
        shouldCancel = listener != nullptr && listener->audioCDBurnProgress (progress);

        return E_NOTIMPL;
    }

    JUCE_COMRESULT NotifyPnPActivity (void)                                            override  { return E_NOTIMPL; }
    JUCE_COMRESULT NotifyAddProgress (long /*nCompletedSteps*/, long /*nTotalSteps*/)  override  { return E_NOTIMPL; }
    JUCE_COMRESULT NotifyTrackProgress (long /*nCurrentTrack*/, long /*nTotalTracks*/) override  { return E_NOTIMPL; }
    JUCE_COMRESULT NotifyPreparingBurn (long /*nEstimatedSeconds*/)                    override  { return E_NOTIMPL; }
    JUCE_COMRESULT NotifyClosingDisc (long /*nEstimatedSeconds*/)                      override  { return E_NOTIMPL; }
    JUCE_COMRESULT NotifyBurnComplete (HRESULT /*status*/)                             override  { return E_NOTIMPL; }
    JUCE_COMRESULT NotifyEraseComplete (HRESULT /*status*/)                            override  { return E_NOTIMPL; }

    class ScopedDiscOpener
    {
    public:
        ScopedDiscOpener (Pimpl& p) : pimpl (p) { pimpl.discRecorder->OpenExclusive(); }
        ~ScopedDiscOpener()                     { pimpl.discRecorder->Close(); }

    private:
        Pimpl& pimpl;

        JUCE_DECLARE_NON_COPYABLE (ScopedDiscOpener)
    };

    DiskState getDiskState()
    {
        const ScopedDiscOpener opener (*this);

        long type, flags;
        HRESULT hr = discRecorder->QueryMediaType (&type, &flags);

        if (FAILED (hr))
            return unknown;

        if (type != 0 && (flags & MEDIA_WRITABLE) != 0)
            return writableDiskPresent;

        if (type == 0)
            return noDisc;

        return readOnlyDiskPresent;
    }

    int getIntProperty (const wchar_t* name, const int defaultReturn) const
    {
        std::wstring copy { name };

        ComSmartPtr<IPropertyStorage> prop;
        if (FAILED (discRecorder->GetRecorderProperties (prop.resetAndGetPointerAddress())))
            return defaultReturn;

        PROPSPEC iPropSpec;
        iPropSpec.ulKind = PRSPEC_LPWSTR;
        iPropSpec.lpwstr = copy.data();

        PROPVARIANT iPropVariant;
        return FAILED (prop->ReadMultiple (1, &iPropSpec, &iPropVariant))
                   ? defaultReturn : (int) iPropVariant.lVal;
    }

    bool setIntProperty (const wchar_t* name, const int value) const
    {
        std::wstring copy { name };

        ComSmartPtr<IPropertyStorage> prop;
        if (FAILED (discRecorder->GetRecorderProperties (prop.resetAndGetPointerAddress())))
            return false;

        PROPSPEC iPropSpec;
        iPropSpec.ulKind = PRSPEC_LPWSTR;
        iPropSpec.lpwstr = copy.data();

        PROPVARIANT iPropVariant;
        if (FAILED (prop->ReadMultiple (1, &iPropSpec, &iPropVariant)))
            return false;

        iPropVariant.lVal = (long) value;
        return SUCCEEDED (prop->WriteMultiple (1, &iPropSpec, &iPropVariant, iPropVariant.vt))
                && SUCCEEDED (discRecorder->SetRecorderProperties (prop));
    }

    void timerCallback() override
    {
        const DiskState state = getDiskState();

        if (state != lastState)
        {
            lastState = state;
            owner.sendChangeMessage();
        }
    }

    AudioCDBurner& owner;
    DiskState lastState;
    IDiscMaster* discMaster;
    IDiscRecorder* discRecorder;
    IRedbookDiscMaster* redbook;
    AudioCDBurner::BurnProgressListener* listener;
    float progress;
    bool shouldCancel;
};

//==============================================================================
AudioCDBurner::AudioCDBurner (const int deviceIndex)
{
    IDiscMaster* discMaster = nullptr;
    IDiscRecorder* discRecorder = CDBurnerHelpers::enumCDBurners (0, deviceIndex, &discMaster);

    if (discRecorder != nullptr)
        pimpl.reset (new Pimpl (*this, discMaster, discRecorder));
}

AudioCDBurner::~AudioCDBurner()
{
    if (pimpl != nullptr)
        pimpl.release()->releaseObjects();
}

StringArray AudioCDBurner::findAvailableDevices()
{
    StringArray devs;
    CDBurnerHelpers::enumCDBurners (&devs, -1, 0);
    return devs;
}

AudioCDBurner* AudioCDBurner::openDevice (const int deviceIndex)
{
    std::unique_ptr<AudioCDBurner> b (new AudioCDBurner (deviceIndex));

    if (b->pimpl == 0)
        b = nullptr;

    return b.release();
}

AudioCDBurner::DiskState AudioCDBurner::getDiskState() const
{
    return pimpl->getDiskState();
}

bool AudioCDBurner::isDiskPresent() const
{
    return getDiskState() == writableDiskPresent;
}

bool AudioCDBurner::openTray()
{
    const Pimpl::ScopedDiscOpener opener (*pimpl);
    return SUCCEEDED (pimpl->discRecorder->Eject());
}

AudioCDBurner::DiskState AudioCDBurner::waitUntilStateChange (int timeOutMilliseconds)
{
    const int64 timeout = Time::currentTimeMillis() + timeOutMilliseconds;
    DiskState oldState = getDiskState();
    DiskState newState = oldState;

    while (newState == oldState && Time::currentTimeMillis() < timeout)
    {
        newState = getDiskState();
        Thread::sleep (jmin (250, (int) (timeout - Time::currentTimeMillis())));
    }

    return newState;
}

Array<int> AudioCDBurner::getAvailableWriteSpeeds() const
{
    Array<int> results;
    const int maxSpeed = pimpl->getIntProperty (L"MaxWriteSpeed", 1);
    const int speeds[] = { 1, 2, 4, 8, 12, 16, 20, 24, 32, 40, 64, 80 };

    for (int i = 0; i < numElementsInArray (speeds); ++i)
        if (speeds[i] <= maxSpeed)
            results.add (speeds[i]);

    results.addIfNotAlreadyThere (maxSpeed);
    return results;
}

bool AudioCDBurner::setBufferUnderrunProtection (const bool shouldBeEnabled)
{
    if (pimpl->getIntProperty (L"BufferUnderrunFreeCapable", 0) == 0)
        return false;

    pimpl->setIntProperty (L"EnableBufferUnderrunFree", shouldBeEnabled ? -1 : 0);
    return pimpl->getIntProperty (L"EnableBufferUnderrunFree", 0) != 0;
}

int AudioCDBurner::getNumAvailableAudioBlocks() const
{
    long blocksFree = 0;
    pimpl->redbook->GetAvailableAudioTrackBlocks (&blocksFree);
    return blocksFree;
}

String AudioCDBurner::burn (AudioCDBurner::BurnProgressListener* listener, bool ejectDiscAfterwards,
                            bool performFakeBurnForTesting, int writeSpeed)
{
    pimpl->setIntProperty (L"WriteSpeed", writeSpeed > 0 ? writeSpeed : -1);

    pimpl->listener = listener;
    pimpl->progress = 0;
    pimpl->shouldCancel = false;

    UINT_PTR cookie;
    HRESULT hr = pimpl->discMaster->ProgressAdvise ((AudioCDBurner::Pimpl*) pimpl.get(), &cookie);

    hr = pimpl->discMaster->RecordDisc (performFakeBurnForTesting,
                                        ejectDiscAfterwards);

    String error;
    if (hr != S_OK)
    {
        const char* e = "Couldn't open or write to the CD device";

        if (hr == IMAPI_E_USERABORT)
            e = "User cancelled the write operation";
        else if (hr == IMAPI_E_MEDIUM_NOTPRESENT || hr == IMAPI_E_TRACKOPEN)
            e = "No Disk present";

        error = e;
    }

    pimpl->discMaster->ProgressUnadvise (cookie);
    pimpl->listener = 0;

    return error;
}

bool AudioCDBurner::addAudioTrack (AudioSource* audioSource, int numSamples)
{
    if (audioSource == 0)
        return false;

    std::unique_ptr<AudioSource> source (audioSource);

    long bytesPerBlock;
    HRESULT hr = pimpl->redbook->GetAudioBlockSize (&bytesPerBlock);

    const int samplesPerBlock = bytesPerBlock / 4;
    bool ok = true;

    hr = pimpl->redbook->CreateAudioTrack ((long) numSamples / (bytesPerBlock * 4));

    HeapBlock<byte> buffer (bytesPerBlock);
    AudioBuffer<float> sourceBuffer (2, samplesPerBlock);
    int samplesDone = 0;

    source->prepareToPlay (samplesPerBlock, 44100.0);

    while (ok)
    {
        {
            AudioSourceChannelInfo info (&sourceBuffer, 0, samplesPerBlock);
            sourceBuffer.clear();

            source->getNextAudioBlock (info);
        }

        buffer.clear (bytesPerBlock);

        AudioData::interleaveSamples (AudioData::NonInterleavedSource<AudioData::Float32, AudioData::NativeEndian> { sourceBuffer.getArrayOfReadPointers(), 2 },
                                      AudioData::InterleavedDest<AudioData::Int16, AudioData::LittleEndian>        { reinterpret_cast<uint16*> (buffer.get()), 2 },
                                      samplesPerBlock);

        hr = pimpl->redbook->AddAudioTrackBlocks (buffer, bytesPerBlock);

        if (FAILED (hr))
            ok = false;

        samplesDone += samplesPerBlock;

        if (samplesDone >= numSamples)
            break;
    }

    hr = pimpl->redbook->CloseAudioTrack();
    return ok && hr == S_OK;
}

} // namespace juce
