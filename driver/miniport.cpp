#include "cableloom.hpp"
static KSDATARANGE_AUDIO audio[] = {
    {{sizeof(KSDATARANGE_AUDIO),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},2,16,16,48000,48000},
    {{sizeof(KSDATARANGE_AUDIO),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},2,24,24,48000,48000},
    {{sizeof(KSDATARANGE_AUDIO),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},2,32,32,48000,48000}
};
static KSDATARANGE analog = { sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE) };
static PKSDATARANGE audio_ranges[] = { reinterpret_cast<PKSDATARANGE>(&audio[0]), reinterpret_cast<PKSDATARANGE>(&audio[1]), reinterpret_cast<PKSDATARANGE>(&audio[2]) };
static PKSDATARANGE analog_ranges[] = { &analog };
static PCPIN_DESCRIPTOR pins[4][2]{};
static PCCONNECTION_DESCRIPTOR connections[4]{};
static PCFILTER_DESCRIPTOR filters[4]{};
void InitializeDescriptors() {
    for (ULONG i = 0; i != 4; ++i) {
        bool wave = i < 2, capture = (i & 1) != 0;
        for (ULONG p = 0; p != 2; ++p) {
            bool streaming = wave && p == (capture ? 1u : 0u);
            auto& pin = pins[i][p];
            pin.MaxGlobalInstanceCount = streaming ? 1 : 0;
            pin.MaxFilterInstanceCount = streaming ? 1 : 0;
            pin.MinFilterInstanceCount = 0;
            pin.KsPinDescriptor.DataRangesCount = streaming ? 3 : 1;
            pin.KsPinDescriptor.DataRanges = streaming ? audio_ranges : analog_ranges;
            pin.KsPinDescriptor.DataFlow = p == 0 ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
            pin.KsPinDescriptor.Communication = streaming ? KSPIN_COMMUNICATION_SINK : KSPIN_COMMUNICATION_NONE;
            pin.KsPinDescriptor.Category = (wave || p != (capture ? 0u : 1u)) ? &KSCATEGORY_AUDIO :
                (capture ? &KSNODETYPE_MICROPHONE : &KSNODETYPE_SPEAKER);
        }
        connections[i] = { PCFILTER_NODE, 0, PCFILTER_NODE, 1 };
        auto& f = filters[i];
        f.PinSize = sizeof(PCPIN_DESCRIPTOR); f.PinCount = 2; f.Pins = pins[i];
        f.NodeSize = sizeof(PCNODE_DESCRIPTOR);
        f.ConnectionCount = 1; f.Connections = &connections[i];
    }
}
PCFILTER_DESCRIPTOR* WaveDescriptor(bool capture) { return &filters[capture ? 1 : 0]; }
PCFILTER_DESCRIPTOR* TopologyDescriptor(bool capture) { return &filters[capture ? 3 : 2]; }
bool ValidFormat(PKSDATAFORMAT data) {
    if (!data || data->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(data->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(data->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(data->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return false;
    auto f = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(data);
    auto& w = f->WaveFormatEx;
    if (w.nChannels != 2 || w.nSamplesPerSec != 48000 || (w.wBitsPerSample != 16 && w.wBitsPerSample != 24 && w.wBitsPerSample != 32) ||
        w.nBlockAlign != w.wBitsPerSample / 8 * 2 || w.nAvgBytesPerSec != 48000u * w.nBlockAlign) return false;
    if (w.wFormatTag == WAVE_FORMAT_PCM) return w.cbSize == 0;
    if (w.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        data->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) || w.cbSize != 22) return false;
    auto e = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(data);
    return e->WaveFormatExt.Samples.wValidBitsPerSample == w.wBitsPerSample &&
        e->WaveFormatExt.dwChannelMask == KSAUDIO_SPEAKER_STEREO &&
        IsEqualGUIDAligned(e->WaveFormatExt.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}
static NTSTATUS Intersect(ULONG pin, bool streaming, PKSDATARANGE client, PKSDATARANGE matching, ULONG length, PVOID output, PULONG result) {
    if (!result || !client || !matching || pin > 1) return STATUS_INVALID_PARAMETER;
    if (!streaming) return STATUS_NOT_IMPLEMENTED;
    if (!IsEqualGUIDAligned(client->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
        !IsEqualGUIDAligned(client->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD)) return STATUS_NO_MATCH;
    if ((!IsEqualGUIDAligned(client->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
         !IsEqualGUIDAligned(client->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)) ||
        !IsEqualGUIDAligned(client->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return STATUS_NO_MATCH;
    if (client->FormatSize < sizeof(KSDATARANGE_AUDIO)) return STATUS_NO_MATCH;
    auto range = reinterpret_cast<PKSDATARANGE_AUDIO>(client);
    if (range->MaximumChannels < 2 ||
        range->MinimumSampleFrequency > 48000 || range->MaximumSampleFrequency < 48000) return STATUS_NO_MATCH;
    ULONG bits = reinterpret_cast<PKSDATARANGE_AUDIO>(matching)->MinimumBitsPerSample;
    if (bits < range->MinimumBitsPerSample || bits > range->MaximumBitsPerSample) return STATUS_NO_MATCH;
    *result = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    if (!length) return STATUS_BUFFER_OVERFLOW;
    if (length < *result) return STATUS_BUFFER_TOO_SMALL;
    if (!output) return STATUS_INVALID_PARAMETER;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE format{};
    format.DataFormat.FormatSize = sizeof(format);
    format.DataFormat.SampleSize = bits / 8 * 2;
    format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    format.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    auto& w = format.WaveFormatExt.Format;
    w.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w.nChannels = 2; w.nSamplesPerSec = 48000;
    w.nBlockAlign = static_cast<WORD>(bits / 8 * 2); w.nAvgBytesPerSec = 48000 * w.nBlockAlign; w.wBitsPerSample = static_cast<WORD>(bits); w.cbSize = 22;
    format.WaveFormatExt.Samples.wValidBitsPerSample = static_cast<WORD>(bits);
    format.WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    format.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    RtlCopyMemory(output, &format, sizeof(format));
    return STATUS_SUCCESS;
}
Wave::Wave(Adapter* adapter, bool input) : adapter_(adapter), capture_(input) { adapter_->add_ref(); }
Wave::~Wave() { adapter_->release(); }
STDMETHODIMP Wave::QueryInterface(REFIID iid, PVOID* out) {
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (IsEqualGUIDAligned(iid, IID_IUnknown) || IsEqualGUIDAligned(iid, IID_IMiniport) ||
        IsEqualGUIDAligned(iid, IID_IMiniportWaveRT)) {
        *out = static_cast<IMiniportWaveRT*>(this); AddRef(); return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}
STDMETHODIMP Wave::GetDescription(PPCFILTER_DESCRIPTOR* out) {
    if (!out) return STATUS_INVALID_PARAMETER; *out = WaveDescriptor(capture_); return STATUS_SUCCESS;
}
STDMETHODIMP Wave::DataRangeIntersection(ULONG pin, PKSDATARANGE client, PKSDATARANGE matching, ULONG length, PVOID output, PULONG result) {
    return Intersect(pin, pin == (capture_ ? 1u : 0u), client, matching, length, output, result);
}
STDMETHODIMP Wave::Init(PUNKNOWN unknown, PRESOURCELIST resources, PPORTWAVERT port) {
    UNREFERENCED_PARAMETER(unknown); UNREFERENCED_PARAMETER(resources); UNREFERENCED_PARAMETER(port); return STATUS_SUCCESS;
}
STDMETHODIMP Wave::GetDeviceDescription(PDEVICE_DESCRIPTION out) {
    if (!out) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(out, sizeof(*out)); out->Version = DEVICE_DESCRIPTION_VERSION;
    out->Master = TRUE; out->ScatterGather = TRUE; out->Dma32BitAddresses = TRUE;
    out->InterfaceType = Internal; out->MaximumLength = 1024 * 1024;
    return STATUS_SUCCESS;
}
STDMETHODIMP Wave::NewStream(PMINIPORTWAVERTSTREAM* out, PPORTWAVERTSTREAM port, ULONG pin, BOOLEAN capture, PKSDATAFORMAT format) {
    if (!out || !port) return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (!!capture != capture_ || pin != (capture_ ? 1u : 0u) || !ValidFormat(format)) return STATUS_NO_MATCH;
    ULONG bits = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(format)->WaveFormatEx.wBitsPerSample;
    auto s = new Stream(adapter_, port, capture_, bits);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    KIRQL irql; KeAcquireSpinLock(&adapter_->lock, &irql);
    auto other = adapter_->streams[capture_ ? 0 : 1];
    bool busy = adapter_->streams[capture_ ? 1 : 0] != nullptr || (other && other->bits != bits);
    if (!busy) adapter_->streams[capture_ ? 1 : 0] = s;
    KeReleaseSpinLock(&adapter_->lock, irql);
    if (busy) { s->Release(); return STATUS_DEVICE_BUSY; }
    *out = s; return STATUS_SUCCESS;
}
STDMETHODIMP Topology::QueryInterface(REFIID iid, PVOID* out) {
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (IsEqualGUIDAligned(iid, IID_IUnknown) || IsEqualGUIDAligned(iid, IID_IMiniport) ||
        IsEqualGUIDAligned(iid, IID_IMiniportTopology)) {
        *out = static_cast<IMiniportTopology*>(this); AddRef(); return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}
STDMETHODIMP Topology::GetDescription(PPCFILTER_DESCRIPTOR* out) {
    if (!out) return STATUS_INVALID_PARAMETER; *out = TopologyDescriptor(capture_); return STATUS_SUCCESS;
}
STDMETHODIMP Topology::DataRangeIntersection(ULONG pin, PKSDATARANGE client, PKSDATARANGE matching, ULONG length, PVOID output, PULONG result) {
    UNREFERENCED_PARAMETER(pin); UNREFERENCED_PARAMETER(client); UNREFERENCED_PARAMETER(matching);
    UNREFERENCED_PARAMETER(length); UNREFERENCED_PARAMETER(output); UNREFERENCED_PARAMETER(result);
    return STATUS_NOT_IMPLEMENTED;
}
STDMETHODIMP Topology::Init(PUNKNOWN unknown, PRESOURCELIST resources, PPORTTOPOLOGY port) {
    UNREFERENCED_PARAMETER(unknown); UNREFERENCED_PARAMETER(resources); UNREFERENCED_PARAMETER(port); return STATUS_SUCCESS;
}
