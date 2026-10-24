#include "cableloom.hpp"
static VOID TimerCallback(PEX_TIMER timer, PVOID context) {
    UNREFERENCED_PARAMETER(timer); static_cast<Adapter*>(context)->tick();
}
Adapter::Adapter() { KeInitializeSpinLock(&lock); queue_.reset(delay_); }
Adapter::~Adapter() { stop(); if (interface_.Buffer) RtlFreeUnicodeString(&interface_); }
NTSTATUS Adapter::start(PDEVICE_OBJECT device) {
    PDEVICE_OBJECT pdo = nullptr;
    NTSTATUS status = PcGetPhysicalDeviceObject(device, &pdo);
    if (!NT_SUCCESS(status)) return status;
    WCHAR location[32]{}; ULONG bytes = 0;
    status = IoGetDeviceProperty(pdo, DevicePropertyLocationInformation, sizeof(location) - sizeof(WCHAR), location, &bytes);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING text; RtlInitUnicodeString(&text, location);
    status = RtlUnicodeStringToInteger(&text, 10, &cable_id);
    if (!NT_SUCCESS(status) || cable_id < 1 || cable_id > CL_MAX_CABLES) return STATUS_DEVICE_CONFIGURATION_ERROR;
    timer_ = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
    if (!timer_) return STATUS_INSUFFICIENT_RESOURCES;
    UNICODE_STRING control; RtlInitUnicodeString(&control, L"Control");
    status = IoRegisterDeviceInterface(pdo, &GUID_DEVINTERFACE_CABLELOOM, &control, &interface_);
    if (!NT_SUCCESS(status)) return status;
    // The interface is enabled only after all audio subdevices have been registered.
    return STATUS_SUCCESS;
}
NTSTATUS Adapter::enable() { return IoSetDeviceInterfaceState(&interface_, TRUE); }
void Adapter::stop() {
    KIRQL irql;
    KeAcquireSpinLock(&lock, &irql);
    if (stopped_) { KeReleaseSpinLock(&lock, irql); return; }
    stopped_ = true;
    for (auto s : streams) if (s) s->state = KSSTATE_STOP;
    auto timer = timer_;
    timer_ = nullptr;
    KeReleaseSpinLock(&lock, irql);
    if (interface_.Buffer) IoSetDeviceInterfaceState(&interface_, FALSE);
    // PASSIVE_LEVEL; drains the callback before the adapter or its buffers can be freed.
    if (timer) ExDeleteTimer(timer, TRUE, TRUE, nullptr);
}
NTSTATUS Adapter::state(Stream* s, KSSTATE next) {
    if (next < KSSTATE_STOP || next > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    KIRQL irql; KeAcquireSpinLock(&lock, &irql);
    if (stopped_) {
        s->state = KSSTATE_STOP;
        KeReleaseSpinLock(&lock, irql);
        return next == KSSTATE_STOP ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }
    if (next == KSSTATE_RUN && (!s->buffer || !s->frames)) { KeReleaseSpinLock(&lock, irql); return STATUS_INVALID_DEVICE_STATE; }
    bool was_running = false;
    for (auto current : streams) if (current && current->state == KSSTATE_RUN) was_running = true;
    bool change = (s->state == KSSTATE_RUN) != (next == KSSTATE_RUN);
    s->state = next;
    if (next == KSSTATE_STOP) s->position = 0;
    // Flush queued audio at a RUN boundary; never replay a previous session.
    if (change) queue_.reset(delay_);
    bool running = false;
    for (auto current : streams) if (current && current->state == KSSTATE_RUN) running = true;
    if (running && !was_running) {
        LARGE_INTEGER frequency;
        auto now = KeQueryPerformanceCounter(&frequency);
        clock_.reset(now.QuadPart, frequency.QuadPart);
        ExSetTimer(timer_, -10000, 10000, nullptr);
    } else if (!running && was_running) ExCancelTimer(timer_, nullptr);
    KeReleaseSpinLock(&lock, irql);
    return STATUS_SUCCESS;
}
void Adapter::tick() {
    KIRQL irql; KeAcquireSpinLock(&lock, &irql);
    if (stopped_) { KeReleaseSpinLock(&lock, irql); return; }
    auto render = streams[0], capture = streams[1];
    bool r = render && render->state == KSSTATE_RUN && render->buffer;
    bool c = capture && capture->state == KSSTATE_RUN && capture->buffer;
    if (!r && !c) { KeReleaseSpinLock(&lock, irql); return; }
    ++stats_.timer_ticks;
    uint64_t elapsed = clock_.advance(KeQueryPerformanceCounter(nullptr).QuadPart);
    uint64_t rp = r ? render->position : 0, cp = c ? capture->position : 0;
    // Bound DPC work and avoid rereading DMA buffers after an entire wrap.
    ULONG limit = 480;
    if (r && render->frames < limit) limit = render->frames;
    if (c && capture->frames < limit) limit = capture->frames;
    if (elapsed > limit) {
        ++stats_.discontinuities;
        queue_.reset(delay_);
        if (r) { render->position += elapsed; stats_.render_frames += elapsed; }
        if (c) {
            RtlZeroMemory(capture->buffer, capture->frames * capture->frame_bytes);
            capture->position += elapsed; stats_.capture_frames += elapsed; stats_.underrun_frames += elapsed;
        }
    } else {
        for (uint64_t i = 0; i < elapsed; ++i) {
            if (r) {
                auto frame = cableloom::decode(render->buffer + (render->position % render->frames) * render->frame_bytes, render->bits);
                if (queue_.push(frame)) ++stats_.overrun_frames;
                ++render->position; ++stats_.render_frames;
            }
            if (c) {
                cableloom::Frame frame{};
                if (!queue_.pull(frame)) ++stats_.underrun_frames;
                cableloom::encode(capture->buffer + (capture->position % capture->frames) * capture->frame_bytes, capture->bits, frame);
                ++capture->position; ++stats_.capture_frames;
            }
        }
    }
    if (r) render->notify(rp);
    if (c) capture->notify(cp);
    KeReleaseSpinLock(&lock, irql);
}
void Adapter::status(ClStatus* output) {
    KIRQL irql; KeAcquireSpinLock(&lock, &irql);
    *output = stats_;
    output->version = CL_PROTOCOL_VERSION; output->size = sizeof(*output);
    output->cable_id = cable_id; output->delay_frames = delay_;
    output->sample_rate = CL_SAMPLE_RATE; output->channels = CL_CHANNELS; output->bits = streams[0] ? streams[0]->bits : (streams[1] ? streams[1]->bits : 32);
    output->running = 0;
    for (ULONG i = 0; i < 2; ++i) if (streams[i] && streams[i]->state == KSSTATE_RUN) output->running |= 1u << i;
    KeReleaseSpinLock(&lock, irql);
}
NTSTATUS Adapter::delay(ULONG frames) {
    if (frames < CL_MIN_DELAY || frames > CL_MAX_DELAY) return STATUS_INVALID_PARAMETER;
    KIRQL irql; KeAcquireSpinLock(&lock, &irql);
    for (auto s : streams) if (s && s->state == KSSTATE_RUN) { KeReleaseSpinLock(&lock, irql); return STATUS_DEVICE_BUSY; }
    delay_ = frames; queue_.reset(delay_);
    KeReleaseSpinLock(&lock, irql); return STATUS_SUCCESS;
}
