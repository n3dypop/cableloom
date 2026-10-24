#pragma once
#include <ntddk.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>
#include "core.hpp"
#include "../include/protocol.h"

extern const GUID GUID_DEVINTERFACE_CABLELOOM;
class Stream;
class Adapter;
#define CL_UNKNOWN \
    STDMETHODIMP QueryInterface(REFIID iid, PVOID* object) override; \
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); } \
    STDMETHODIMP_(ULONG) Release() override { LONG n = InterlockedDecrement(&refs_); if (!n) delete this; return static_cast<ULONG>(n); }
class Adapter {
public:
    Adapter();
    ~Adapter();
    void add_ref() { InterlockedIncrement(&refs_); }
    void release() { if (!InterlockedDecrement(&refs_)) delete this; }
    NTSTATUS start(PDEVICE_OBJECT device);
    NTSTATUS enable();
    void stop();
    NTSTATUS state(Stream* stream, KSSTATE state);
    void tick();
    void status(ClStatus* result);
    NTSTATUS delay(ULONG frames);
    KSPIN_LOCK lock;
    Stream* streams[2]{};
    ULONG cable_id = 0;
private:
    LONG refs_ = 1;
    bool stopped_ = false;
    PEX_TIMER timer_ = nullptr;
    UNICODE_STRING interface_{};
    cableloom::DelayLine queue_;
    cableloom::FrameClock clock_;
    ULONG delay_ = 96;
    ClStatus stats_{};
};
class Stream final : public IMiniportWaveRTStreamNotification {
public:
    Stream(Adapter* adapter, PPORTWAVERTSTREAM port, bool capture, ULONG bits);
    ~Stream();
    CL_UNKNOWN
    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    NTSTATUS allocate(ULONG notifications, ULONG requested, PMDL* mdl, ULONG* actual, ULONG* offset, MEMORY_CACHING_TYPE* cache);
    // All following fields are protected by adapter->lock.
    Adapter* adapter;
    PPORTWAVERTSTREAM port;
    bool capture;
    KSSTATE state = KSSTATE_STOP;
    PMDL mdl = nullptr;
    BYTE* buffer = nullptr;
    ULONG bits = 32, frame_bytes = 8;
    ULONG frames = 0, notifications = 0;
    uint64_t position = 0;
    PKEVENT events[8]{};
    void notify(uint64_t previous);
private:
    LONG refs_ = 1;
};
class Wave final : public IMiniportWaveRT {
public:
    Wave(Adapter* adapter, bool capture);
    ~Wave();
    CL_UNKNOWN
    IMP_IMiniportWaveRT;
private:
    LONG refs_ = 1;
    Adapter* adapter_;
    bool capture_;
};
class Topology final : public IMiniportTopology {
public:
    explicit Topology(bool capture) : capture_(capture) {}
    CL_UNKNOWN
    IMP_IMiniportTopology;
private:
    LONG refs_ = 1;
    bool capture_;
};
PCFILTER_DESCRIPTOR* WaveDescriptor(bool capture);
PCFILTER_DESCRIPTOR* TopologyDescriptor(bool capture);
bool ValidFormat(PKSDATAFORMAT format);
