#define INITGUID
#include "cableloom.hpp"
extern void InitializeDescriptors();
const GUID GUID_DEVINTERFACE_CABLELOOM = {0x1678cb4e,0x9a71,0x4c0f,{0xba,0xe5,0x12,0xef,0x00,0xcb,0x71,0x28}};
static constexpr ULONG tag = 'moLC';
void* __cdecl operator new(size_t bytes) { return ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, tag); }
void __cdecl operator delete(void* memory) noexcept { if (memory) ExFreePoolWithTag(memory, tag); }
void __cdecl operator delete(void* memory, size_t) noexcept { if (memory) ExFreePoolWithTag(memory, tag); }
struct Extension {
    ULONG_PTR portcls[PORT_CLASS_DEVICE_EXTENSION_SIZE / sizeof(ULONG_PTR)];
    Adapter* adapter;
};
static KMUTEX control_mutex;
static void ControlLock() { KeWaitForSingleObject(&control_mutex, Executive, KernelMode, FALSE, nullptr); }
static void ControlUnlock() { KeReleaseMutex(&control_mutex, FALSE); }
static UCHAR control_marker;
static PDRIVER_UNLOAD port_unload;
static NTSTATUS Finish(PIRP irp, NTSTATUS status, ULONG_PTR count = 0) {
    irp->IoStatus.Status = status; irp->IoStatus.Information = count;
    IoCompleteRequest(irp, IO_NO_INCREMENT); return status;
}
static NTSTATUS RegisterPort(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources,
    PWSTR name, REFCLSID clsid, PUNKNOWN miniport, PPORT* result) {
    *result = nullptr;
    PPORT port = nullptr;
    NTSTATUS status = PcNewPort(&port, clsid);
    if (NT_SUCCESS(status)) status = port->Init(device, irp, miniport, nullptr, resources);
    if (NT_SUCCESS(status)) status = PcRegisterSubdevice(device, name, port);
    if (NT_SUCCESS(status)) *result = port;
    else if (port) port->Release();
    return status;
}
static NTSTATUS StartDevice(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources) {
    auto extension = static_cast<Extension*>(device->DeviceExtension);
    if (extension->adapter) return STATUS_INVALID_DEVICE_STATE;
    auto adapter = new Adapter();
    if (!adapter) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = adapter->start(device);
    PPORT ports[4]{};
    PUNKNOWN miniports[4]{};
    if (NT_SUCCESS(status)) {
        miniports[0] = static_cast<IMiniportWaveRT*>(new Wave(adapter, false));
        miniports[1] = static_cast<IMiniportWaveRT*>(new Wave(adapter, true));
        miniports[2] = static_cast<IMiniportTopology*>(new Topology(false));
        miniports[3] = static_cast<IMiniportTopology*>(new Topology(true));
        for (auto m : miniports) if (!m) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    WCHAR render_wave[] = L"RenderWave";
    WCHAR capture_wave[] = L"CaptureWave";
    WCHAR render_topology[] = L"RenderTopology";
    WCHAR capture_topology[] = L"CaptureTopology";
    PWSTR names[] = { render_wave, capture_wave, render_topology, capture_topology };
    for (ULONG i = 0; i < 4 && NT_SUCCESS(status); ++i)
        status = RegisterPort(device, irp, resources, names[i], i < 2 ? CLSID_PortWaveRT : CLSID_PortTopology, miniports[i], &ports[i]);
    if (NT_SUCCESS(status)) status = PcRegisterPhysicalConnection(device, ports[0], 1, ports[2], 0);
    if (NT_SUCCESS(status)) status = PcRegisterPhysicalConnection(device, ports[3], 1, ports[1], 0);
    for (auto m : miniports) if (m) m->Release();
    for (auto p : ports) if (p) p->Release();
    if (NT_SUCCESS(status)) {
        ControlLock();
        extension->adapter = adapter;
        status = adapter->enable();
        if (!NT_SUCCESS(status)) extension->adapter = nullptr;
        ControlUnlock();
    }
    if (!NT_SUCCESS(status)) { adapter->stop(); adapter->release(); }
    return status;
}
static NTSTATUS AddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo) {
    return PcAddAdapterDevice(driver, pdo, StartDevice, 4, sizeof(Extension));
}
static NTSTATUS Pnp(PDEVICE_OBJECT device, PIRP irp) {
    UCHAR minor = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
    if (minor == IRP_MN_STOP_DEVICE || minor == IRP_MN_REMOVE_DEVICE || minor == IRP_MN_SURPRISE_REMOVAL) {
        ControlLock();
        auto extension = static_cast<Extension*>(device->DeviceExtension);
        auto adapter = extension->adapter;
        extension->adapter = nullptr;
        if (adapter) { adapter->stop(); adapter->release(); }
        ControlUnlock();
    }
    return PcDispatchIrp(device, irp);
}
static NTSTATUS CreateClose(PDEVICE_OBJECT device, PIRP irp) {
    auto stack = IoGetCurrentIrpStackLocation(irp);
    auto file = stack->FileObject;
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        UNICODE_STRING name; RtlInitUnicodeString(&name, L"\\Control");
        if (!RtlEqualUnicodeString(&file->FileName, &name, TRUE)) return PcDispatchIrp(device, irp);
        ControlLock();
        auto adapter = static_cast<Extension*>(device->DeviceExtension)->adapter;
        if (adapter) { adapter->add_ref(); file->FsContext = adapter; file->FsContext2 = &control_marker; }
        ControlUnlock();
        return Finish(irp, adapter ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY, adapter ? FILE_OPENED : 0);
    }
    if (file->FsContext2 != &control_marker) return PcDispatchIrp(device, irp);
    if (stack->MajorFunction == IRP_MJ_CLOSE) {
        auto adapter = static_cast<Adapter*>(file->FsContext);
        file->FsContext = file->FsContext2 = nullptr;
        if (adapter) adapter->release();
    }
    return Finish(irp, STATUS_SUCCESS);
}
static NTSTATUS Control(PDEVICE_OBJECT device, PIRP irp) {
    auto stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    if (stack->FileObject->FsContext2 != &control_marker) return PcDispatchIrp(device, irp);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return Finish(irp, STATUS_INVALID_DEVICE_STATE);
    ULONG input = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output = stack->Parameters.DeviceIoControl.OutputBufferLength;
    auto buffer = irp->AssociatedIrp.SystemBuffer;
    if (!buffer || input < sizeof(ClRequest)) return Finish(irp, STATUS_INVALID_PARAMETER);
    auto header = *static_cast<ClRequest*>(buffer);
    if (header.version != CL_PROTOCOL_VERSION || header.size != input) return Finish(irp, STATUS_REVISION_MISMATCH);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR count = 0;
    ControlLock();
    auto adapter = static_cast<Extension*>(device->DeviceExtension)->adapter;
    if (!adapter || adapter != stack->FileObject->FsContext) status = STATUS_DEVICE_NOT_READY;
    else if (code == CL_IOCTL_STATUS) {
        if (input != sizeof(ClRequest)) status = STATUS_INVALID_PARAMETER;
        else if (output < sizeof(ClStatus)) status = STATUS_BUFFER_TOO_SMALL;
        else { adapter->status(static_cast<ClStatus*>(buffer)); count = sizeof(ClStatus); status = STATUS_SUCCESS; }
    } else if (code == CL_IOCTL_DELAY) {
        if (input != sizeof(ClDelay) || output || static_cast<ClDelay*>(buffer)->reserved)
            status = STATUS_INVALID_PARAMETER;
        else status = adapter->delay(static_cast<ClDelay*>(buffer)->frames);
    }
    ControlUnlock();
    return Finish(irp, status, count);
}
static VOID Unload(PDRIVER_OBJECT driver) { if (port_unload) port_unload(driver); }
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    KeInitializeMutex(&control_mutex, 0);
    InitializeDescriptors();
    NTSTATUS status = PcInitializeAdapterDriver(driver, path, AddDevice);
    if (!NT_SUCCESS(status)) return status;
    port_unload = driver->DriverUnload;
    driver->DriverUnload = Unload;
    driver->MajorFunction[IRP_MJ_PNP] = Pnp;
    driver->MajorFunction[IRP_MJ_CREATE] = CreateClose;
    driver->MajorFunction[IRP_MJ_CLEANUP] = CreateClose;
    driver->MajorFunction[IRP_MJ_CLOSE] = CreateClose;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Control;
    return STATUS_SUCCESS;
}
