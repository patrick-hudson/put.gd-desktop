#include "record/Sources.h"
#include "capture/Png.h"
#include "common/Log.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace putgd::record {
using wic::Com;
// The WinRT interfaces are implemented by Windows, never here: declared outside an
// anonymous namespace, or GCC's devirtualizer sees no implementation and turns
// every call through them into unreachable code (-O2 and up).
namespace winrt {
// ---- Windows.Graphics.Capture ABI (windows.graphics.capture.idl), the parts used ----
struct SizeInt32 { int32_t width, height; };
struct TimeSpan { int64_t duration; }; // 100-ns units
constexpr int pixelFormatBgra8 = 87;   // DirectXPixelFormat::B8G8R8A8UIntNormalized
const IID iidItemInterop{0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
const IID iidItem{0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
const IID iidPoolStatics2{0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
const IID iidSessionStatics{0x2224a540, 0x5974, 0x49aa, {0xb2, 0x32, 0x08, 0x82, 0x53, 0x6f, 0x4c, 0xb5}};
const IID iidSession2{0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
const IID iidSession3{0xf2cdd966, 0x22ae, 0x5ea1, {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
const IID iidClosable{0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
const IID iidDirect3DDevice{0xa37624ab, 0x8d5f, 0x4650, {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};
const IID iidDxgiAccess{0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
struct IItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR, REFIID, void**) = 0;
};
struct IItem : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(INT64) = 0;
};
struct IFrame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(TimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32*) = 0;
};
struct ISession : IInspectable { virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0; };
struct IFramePool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable* device, int format, INT32 buffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IFrame**) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IItem*, ISession**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable**) = 0;
};
struct IFramePoolStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable* device, int format, INT32 buffers, SizeInt32 size, IFramePool**) = 0;
};
struct ISessionStatics : IInspectable { virtual HRESULT STDMETHODCALLTYPE IsSupported(boolean*) = 0; };
struct ISessionFlag : IInspectable { // IGraphicsCaptureSession2 (cursor) and 3 (border) share this shape
    virtual HRESULT STDMETHODCALLTYPE get_Value(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Value(boolean) = 0;
};
struct IClosable : IInspectable { virtual HRESULT STDMETHODCALLTYPE Close() = 0; };
struct IDxgiAccess : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID, void**) = 0; };

}
using namespace winrt;
namespace {
long long frequency() { static const long long f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }(); return f; }
std::string hex(HRESULT hr) { char b[16]; wsprintfA(b, "0x%08lX", static_cast<unsigned long>(hr)); return b; }

struct Api {
    bool loaded = false, ok = false;
    HMODULE d3d11 = nullptr, dxgi = nullptr, combase = nullptr;
    PFN_D3D11_CREATE_DEVICE createDevice = nullptr;
    HRESULT(WINAPI* createFactory)(REFIID, void**) = nullptr;
    HRESULT(WINAPI* winrtDevice)(IDXGIDevice*, IInspectable**) = nullptr;
    HRESULT(WINAPI* activationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* createString)(const wchar_t*, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* deleteString)(HSTRING) = nullptr;
};
template <class F> void resolve(HMODULE module, const char* name, F& out) {
    out = module ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}
// Loaded once by the first source on the recorder thread; never unloaded.
Api& api() {
    static Api a;
    if (a.loaded) return a;
    a.loaded = true;
    a.d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    a.dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    a.combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    resolve(a.d3d11, "D3D11CreateDevice", a.createDevice);
    resolve(a.d3d11, "CreateDirect3D11DeviceFromDXGIDevice", a.winrtDevice);
    resolve(a.dxgi, "CreateDXGIFactory1", a.createFactory);
    resolve(a.combase, "RoGetActivationFactory", a.activationFactory);
    resolve(a.combase, "WindowsCreateString", a.createString);
    resolve(a.combase, "WindowsDeleteString", a.deleteString);
    a.ok = a.createDevice && a.createFactory;
    return a;
}
// RoGetActivationFactory for a runtime class name.
HRESULT factory(const wchar_t* name, REFIID iid, void** out) {
    auto& a = api();
    if (!a.activationFactory || !a.createString || !a.deleteString) return E_NOTIMPL;
    HSTRING s = nullptr;
    HRESULT hr = a.createString(name, UINT32(wcslen(name)), &s);
    if (FAILED(hr)) return hr;
    hr = a.activationFactory(s, iid, out);
    a.deleteString(s);
    return hr;
}
Error deviceFor(IDXGIAdapter* adapter, Com<ID3D11Device>& device, Com<ID3D11DeviceContext>& context) {
    auto& a = api();
    if (!a.ok) return {"recording_unsupported", "Direct3D 11 is not available", {}};
    const HRESULT hr = a.createDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    if (FAILED(hr)) return {"recording_unsupported", "No Direct3D 11 device", {{"hresult", hex(hr)}}};
    return {};
}
// A CPU-readable copy target.
bool staging(ID3D11Device* device, int width, int height, Com<ID3D11Texture2D>& out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(width); d.Height = UINT(height); d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, out.put()));
}
// Rows of a mapped staging texture into `pixels` (width x height, BGRX).
bool readBack(ID3D11DeviceContext* context, ID3D11Texture2D* texture, int width, int height, int copyWidth, int copyHeight, std::vector<uint8_t>& pixels) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(texture, 0, D3D11_MAP_READ, 0, &m))) return false;
    const size_t stride = size_t(width) * 4, row = size_t(copyWidth) * 4;
    for (int y = 0; y < height; ++y) {
        uint8_t* to = pixels.data() + stride * size_t(y);
        if (y < copyHeight) { std::memcpy(to, static_cast<const uint8_t*>(m.pData) + size_t(m.RowPitch) * size_t(y), row); std::memset(to + row, 0, stride - row); }
        else std::memset(to, 0, stride);
    }
    context->Unmap(texture, 0);
    return true;
}

// Waits on a high-resolution timer until `deadline`, or until `wake` is set (false).
class Waiter {
public:
    Waiter() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~Waiter() { if (timer_) CloseHandle(timer_); }
    bool until(long long deadline, HANDLE wake) {
        const long long due = deadline - now100ns();
        if (due <= 0) return true;
        if (!timer_) { Sleep(DWORD(due / 10000)); return true; }
        LARGE_INTEGER relative; relative.QuadPart = -due;
        SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE);
        HANDLE handles[2] = {timer_, wake};
        return WaitForMultipleObjects(wake ? 2 : 1, handles, FALSE, INFINITE) == WAIT_OBJECT_0;
    }
private:
    HANDLE timer_ = nullptr;
};

// ---- GDI ----------------------------------------------------------------------
class Gdi final : public Source {
public:
    Error open(const grab::Rect& area) {
        area_ = area;
        screen_ = GetDC(nullptr);
        if (!screen_ || !image_.create(area.w, area.h)) return {"recording_failed", "Cannot open the screen for recording", {}};
        return {};
    }
    ~Gdi() override { if (screen_) ReleaseDC(nullptr, screen_); }
    // The screen as it is now.
    bool read() {
        if (!BitBlt(image_.dc(), 0, 0, area_.w, area_.h, screen_, area_.x, area_.y, SRCCOPY | CAPTUREBLT)) return false;
        GdiFlush();
        return true;
    }
    // Read at the deadline, and timed at it: the picture stands for that instant.
    Error next(long long deadline, HANDLE wake, bool& got, long long& present) override {
        got = false;
        if (!waiter_.until(deadline, wake)) return {};
        if (!read()) return {"recording_failed", "The screen could not be read", {}};
        got = true; present = deadline;
        return {};
    }
    const uint8_t* bits() const override { return image_.bits(); }
    size_t stride() const override { return image_.stride(); }
    std::string name() const override { return "gdi"; }
private:
    grab::Rect area_;
    HDC screen_ = nullptr;
    grab::Image image_;
    Waiter waiter_;
};

// ---- Desktop Duplication --------------------------------------------------------
// The output whose desktop rectangle holds all of `area`, with its adapter.
Error findOutput(const grab::Rect& area, Com<IDXGIAdapter1>& adapter, Com<IDXGIOutput1>& output, RECT& desktop) {
    auto& a = api();
    if (!a.ok) return {"recording_unsupported", "DXGI is not available", {}};
    Com<IDXGIFactory1> factory;
    if (FAILED(a.createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())))) return {"recording_unsupported", "DXGI is not available", {}};
    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, candidate.put()) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT j = 0;; ++j) {
            Com<IDXGIOutput> plain;
            if (candidate->EnumOutputs(j, plain.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            if (FAILED(plain->GetDesc(&d)) || !d.AttachedToDesktop) continue;
            const auto r = grab::fromRECT(d.DesktopCoordinates);
            if (!(grab::intersect(r, area) == area)) continue;
            if (d.Rotation != DXGI_MODE_ROTATION_IDENTITY && d.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) return {"recording_unsupported", "The output is rotated", {}};
            if (FAILED(plain->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(output.put())))) return {"recording_unsupported", "DXGI 1.2 is not available", {}};
            adapter = std::move(candidate); desktop = d.DesktopCoordinates;
            return {};
        }
    }
    return {"recording_unsupported", "The area spans more than one output", {}};
}

class Duplication final : public Source {
public:
    Error open(const grab::Rect& area) {
        area_ = area;
        Com<IDXGIAdapter1> adapter;
        if (auto e = findOutput(area, adapter, output_, desktop_)) return e;
        if (auto e = deviceFor(adapter.get(), device_, context_)) return e;
        if (auto e = duplicate()) return e;
        if (!staging(device_.get(), area.w, area.h, staging_)) return {"recording_unsupported", "Cannot create a copy texture", {}};
        pixels_.assign(size_t(area.w) * area.h * 4, 0);
        // The first picture comes from GDI, so a still screen that presents nothing
        // still has one; later ones come with the compositor's present times.
        if (auto e = gdi_.open(area)) return e;
        seeded_ = gdi_.read();
        if (seeded_) std::memcpy(pixels_.data(), gdi_.bits(), pixels_.size());
        return {};
    }
    Error next(long long deadline, HANDLE wake, bool& got, long long& present) override {
        got = false;
        if (seeded_) { seeded_ = false; got = true; present = now100ns(); return {}; }
        if (!duplication_) {
            // Lost (a mode change, the secure desktop): try again every 250 ms and read
            // the screen with GDI meanwhile, so the clip keeps time.
            if (now100ns() - lostAt_ >= 2500000) { lostAt_ = now100ns(); if (!duplicate()) log::info("record", "Desktop duplication restored", {}); }
            if (!duplication_) {
                bool read = false;
                if (auto e = gdi_.next(deadline, wake, read, present)) { (void)e; return {}; } // the secure desktop cannot be read either: hold the picture
                if (read) { std::memcpy(pixels_.data(), gdi_.bits(), pixels_.size()); got = true; }
                return {};
            }
        }
        // AcquireNextFrame cannot also wait on `wake`: at most 20 ms per call.
        const long long left = deadline - now100ns();
        const UINT timeout = left <= 0 ? 0 : UINT(std::min<long long>(20, (left + 9999) / 10000));
        DXGI_OUTDUPL_FRAME_INFO info{};
        Com<IDXGIResource> resource;
        const HRESULT hr = duplication_->AcquireNextFrame(timeout, &info, resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return {};
        if (FAILED(hr)) {
            log::warning("record", "Desktop duplication lost", {{"hresult", hex(hr)}});
            duplication_.reset(); lostAt_ = now100ns();
            return {};
        }
        // Only the pointer moved: no new picture.
        if (info.LastPresentTime.QuadPart != 0) {
            Com<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put())))) {
                const D3D11_BOX box{UINT(area_.x - desktop_.left), UINT(area_.y - desktop_.top), 0, UINT(area_.x - desktop_.left + area_.w), UINT(area_.y - desktop_.top + area_.h), 1};
                context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, texture.get(), 0, &box);
                if (readBack(context_.get(), staging_.get(), area_.w, area_.h, area_.w, area_.h, pixels_)) { got = true; present = hundredNs(info.LastPresentTime.QuadPart); }
            }
        }
        resource.reset();
        duplication_->ReleaseFrame();
        return {};
    }
    const uint8_t* bits() const override { return pixels_.data(); }
    size_t stride() const override { return size_t(area_.w) * 4; }
    std::string name() const override { return "dxgi"; }
private:
    Error duplicate() {
        duplication_.reset();
        const HRESULT hr = output_->DuplicateOutput(device_.get(), duplication_.put());
        if (FAILED(hr)) return {"recording_unsupported", "Desktop duplication is not available on this output", {{"hresult", hex(hr)}}};
        return {};
    }
    grab::Rect area_;
    RECT desktop_{};
    Com<IDXGIOutput1> output_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutputDuplication> duplication_;
    Com<ID3D11Texture2D> staging_;
    std::vector<uint8_t> pixels_;
    Gdi gdi_;
    bool seeded_ = false;
    long long lostAt_ = 0;
};

// ---- Windows.Graphics.Capture --------------------------------------------------------
void closeObject(IInspectable* object) {
    if (!object) return;
    Com<IClosable> closable;
    if (SUCCEEDED(object->QueryInterface(iidClosable, reinterpret_cast<void**>(closable.put())))) closable->Close();
}
Error captureItem(HWND window, Com<IItem>& item) {
    Com<IItemInterop> interop;
    HRESULT hr = factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", iidItemInterop, reinterpret_cast<void**>(interop.put()));
    if (FAILED(hr)) return {"recording_unsupported", "Window capture is not available on this Windows", {{"hresult", hex(hr)}}};
    hr = interop->CreateForWindow(window, iidItem, reinterpret_cast<void**>(item.put()));
    if (FAILED(hr)) return {"recording_target", "This window cannot be captured", {{"hresult", hex(hr)}}};
    return {};
}

class WindowCapture final : public Source {
public:
    ~WindowCapture() override {
        if (session_) closeObject(session_.get());
        if (pool_) closeObject(pool_.get());
    }
    Error open(HWND window, int width, int height) {
        window_ = window; width_ = width; height_ = height;
        if (auto e = captureItem(window, item_)) return e;
        if (FAILED(item_->get_Size(&size_)) || size_.width < 1 || size_.height < 1) return {"recording_target", "This window has no size to capture", {}};
        if (auto e = deviceFor(nullptr, device_, context_)) return e;
        Com<IDXGIDevice> dxgi;
        Com<IInspectable> inspectable;
        if (!api().winrtDevice || FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))) ||
            FAILED(api().winrtDevice(dxgi.get(), inspectable.put())) || FAILED(inspectable->QueryInterface(iidDirect3DDevice, reinterpret_cast<void**>(winrtDevice_.put()))))
            return {"recording_unsupported", "Window capture cannot use Direct3D here", {}};
        Com<IFramePoolStatics2> statics;
        HRESULT hr = factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", iidPoolStatics2, reinterpret_cast<void**>(statics.put()));
        if (SUCCEEDED(hr)) hr = statics->CreateFreeThreaded(winrtDevice_.get(), pixelFormatBgra8, 3, size_, pool_.put());
        if (SUCCEEDED(hr)) hr = pool_->CreateCaptureSession(item_.get(), session_.put());
        if (FAILED(hr)) return {"recording_unsupported", "Window capture could not start", {{"hresult", hex(hr)}}};
        // No pointer in the clip (as on the screen paths), and no yellow border where Windows allows it.
        Com<ISessionFlag> flag;
        if (SUCCEEDED(session_->QueryInterface(iidSession2, reinterpret_cast<void**>(flag.put())))) flag->put_Value(false);
        if (SUCCEEDED(session_->QueryInterface(iidSession3, reinterpret_cast<void**>(flag.put())))) flag->put_Value(false);
        if (!staging(device_.get(), width, height, staging_)) return {"recording_unsupported", "Cannot create a copy texture", {}};
        pixels_.assign(size_t(width) * height * 4, 0);
        hr = session_->StartCapture();
        if (FAILED(hr)) return {"recording_unsupported", "Window capture could not start", {{"hresult", hex(hr)}}};
        return {};
    }
    Error next(long long deadline, HANDLE wake, bool& got, long long& present) override {
        got = false;
        while (true) {
            IFrame* frame = nullptr;
            IFramePool* pool = pool_.get();
            const HRESULT hr = pool->TryGetNextFrame(&frame);
            if (SUCCEEDED(hr) && frame) { take(frame, got, present); closeObject(frame); frame->Release(); return {}; }
            if (!IsWindow(window_)) { ended_ = true; return {}; }
            const long long now = now100ns();
            if (now >= deadline) return {};
            // Frames are polled: about every 2 ms, so none waits long in the pool.
            if (!waiter_.until(std::min(deadline, now + 20000), wake)) return {};
        }
    }
    const uint8_t* bits() const override { return pixels_.data(); }
    size_t stride() const override { return size_t(width_) * 4; }
    std::string name() const override { return "wgc"; }
    bool ended() const override { return ended_; }
private:
    void take(IFrame* frame, bool& got, long long& present) {
        SizeInt32 content{}; TimeSpan time{};
        frame->get_ContentSize(&content);
        frame->get_SystemRelativeTime(&time);
        Com<IInspectable> surface;
        Com<IDxgiAccess> access;
        Com<ID3D11Texture2D> texture;
        if (SUCCEEDED(frame->get_Surface(surface.put())) && SUCCEEDED(surface->QueryInterface(iidDxgiAccess, reinterpret_cast<void**>(access.put()))) &&
            SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put())))) {
            const int w = std::min({content.width, size_.width, width_}), h = std::min({content.height, size_.height, height_});
            if (w > 0 && h > 0) {
                const D3D11_BOX box{0, 0, 0, UINT(w), UINT(h), 1};
                context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, texture.get(), 0, &box);
                if (readBack(context_.get(), staging_.get(), width_, height_, w, h, pixels_)) { got = true; present = time.duration; }
            }
        }
        // The window changed size: later frames come at the new size.
        if (content.width > 0 && content.height > 0 && (content.width != size_.width || content.height != size_.height)) {
            size_ = content;
            pool_->Recreate(winrtDevice_.get(), pixelFormatBgra8, 3, size_);
        }
    }
    HWND window_ = nullptr;
    int width_ = 0, height_ = 0;
    SizeInt32 size_{};
    Com<IItem> item_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IInspectable> winrtDevice_;
    Com<IFramePool> pool_;
    Com<ISession> session_;
    Com<ID3D11Texture2D> staging_;
    std::vector<uint8_t> pixels_;
    Waiter waiter_;
    bool ended_ = false;
};
}

long long hundredNs(long long ticks) { const long long f = frequency(); return ticks / f * 10000000 + ticks % f * 10000000 / f; }
long long now100ns() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return hundredNs(v.QuadPart); }

Error openScreenSource(const grab::Rect& area, std::unique_ptr<Source>& out, std::string& note) {
    auto duplication = std::make_unique<Duplication>();
    const auto e = duplication->open(area);
    if (!e) { out = std::move(duplication); return {}; }
    note = e.message;
    duplication.reset();
    auto gdi = std::make_unique<Gdi>();
    if (auto g = gdi->open(area)) return g;
    out = std::move(gdi);
    return {};
}
Error openWindowSource(HWND window, int width, int height, std::unique_ptr<Source>& out) {
    auto capture = std::make_unique<WindowCapture>();
    if (auto e = capture->open(window, width, height)) return e;
    out = std::move(capture);
    return {};
}
bool windowCaptureAvailable(std::string& reason) {
    Com<ISessionStatics> statics;
    boolean supported = false;
    if (FAILED(factory(L"Windows.Graphics.Capture.GraphicsCaptureSession", iidSessionStatics, reinterpret_cast<void**>(statics.put()))) || FAILED(statics->IsSupported(&supported)) || !supported) {
        reason = "Window capture (Windows.Graphics.Capture) is not available on this Windows";
        return false;
    }
    if (!api().winrtDevice) { reason = "Window capture cannot use Direct3D here"; return false; }
    return true;
}
bool duplicationAvailable(const grab::Rect& area, std::string& reason) {
    Duplication probe;
    if (auto e = probe.open(area)) { reason = e.message; return false; }
    return true;
}
Error windowCaptureSize(HWND window, int& width, int& height) {
    Com<IItem> item;
    if (auto e = captureItem(window, item)) return e;
    SizeInt32 size{};
    if (FAILED(item->get_Size(&size)) || size.width < 1 || size.height < 1) return {"recording_target", "This window has no size to capture", {}};
    width = size.width; height = size.height;
    return {};
}
}
