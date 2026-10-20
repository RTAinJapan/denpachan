#include "filter.h"

#include <avrt.h>

#include <algorithm>
#include <climits>
#include <new>
#include <vector>

#include "denpachan/win_util.h"
#include "frame_source.h"
#include "guids.h"
#include "module.h"

namespace denpachan::vcam {

namespace {

constexpr wchar_t kPinName[] = L"Capture";

// ---- IEnumPins (ピンは 1 本だけ) ----

class EnumPinsImpl final : public IEnumPins {
public:
    EnumPinsImpl(VcamFilter* filter, ULONG pos) : filter_(filter), pos_(pos) { filter_->AddRef(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (cPins > 1 && !pcFetched) return E_INVALIDARG;
        ULONG n = 0;
        while (n < cPins && pos_ < 1) {
            ppPins[n] = filter_->pin();
            ppPins[n]->AddRef();
            ++n;
            ++pos_;
        }
        if (pcFetched) *pcFetched = n;
        return n == cPins ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cPins) override {
        pos_ += cPins;
        return pos_ <= 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) EnumPinsImpl(filter_, pos_);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~EnumPinsImpl() { filter_->Release(); }

    ModuleRef module_ref_;
    std::atomic<ULONG> ref_{1};
    VcamFilter* filter_;
    ULONG pos_;
};

// ---- IEnumMediaTypes ----

class EnumMediaTypesImpl final : public IEnumMediaTypes {
public:
    EnumMediaTypesImpl(std::vector<OutputFormat> formats, ULONG pos) : formats_(std::move(formats)), pos_(pos) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        if (cMediaTypes > 1 && !pcFetched) return E_INVALIDARG;
        ULONG n = 0;
        while (n < cMediaTypes && pos_ < formats_.size()) {
            AM_MEDIA_TYPE* pmt = create_media_type(formats_[pos_]);
            if (!pmt) break;
            ppMediaTypes[n++] = pmt;
            ++pos_;
        }
        if (pcFetched) *pcFetched = n;
        return n == cMediaTypes ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) override {
        pos_ += c;
        return pos_ <= formats_.size() ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) EnumMediaTypesImpl(formats_, pos_);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~EnumMediaTypesImpl() = default;

    ModuleRef module_ref_;
    std::atomic<ULONG> ref_{1};
    std::vector<OutputFormat> formats_;
    ULONG pos_;
};

GUID subtype_of(PixelFormat pix) { return pix == PixelFormat::NV12 ? MEDIASUBTYPE_NV12 : MEDIASUBTYPE_YUY2; }

// current を先頭にした提示フォーマット一覧
std::vector<OutputFormat> ordered_formats(const OutputFormat& current) {
    std::vector<OutputFormat> v{current};
    for (const auto& f : supported_formats())
        if (!(f == current)) v.push_back(f);
    return v;
}

}  // namespace

// ============================================================================
// VcamFilter
// ============================================================================

HRESULT VcamFilter::Create(int cam, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    VcamFilter* f = nullptr;
    try {
        f = new VcamFilter(cam);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

VcamFilter::VcamFilter(int cam) : cam_(cam), name_(cam_name(cam)) {
    ++g_module_refs;
    pin_ = std::make_unique<VcamPin>(this);
}

VcamFilter::~VcamFilter() {
    if (state() != State_Stopped) pin_->Inactive();
    pin_.reset();
    if (clock_) clock_->Release();
    --g_module_refs;
}

STDMETHODIMP VcamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) VcamFilter::AddRef() { return ++ref_; }

STDMETHODIMP_(ULONG) VcamFilter::Release() {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP VcamFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    *pClassID = kCamClsid[cam_ - 1];
    return S_OK;
}

STDMETHODIMP VcamFilter::Stop() {
    std::lock_guard lk(lock_);
    if (state() != State_Stopped) {
        pin_->Inactive();
        state_ = State_Stopped;
    }
    return S_OK;
}

STDMETHODIMP VcamFilter::Pause() {
    std::lock_guard lk(lock_);
    if (state() == State_Stopped) {
        const HRESULT hr = pin_->Active();
        if (FAILED(hr)) return hr;
    }
    state_ = State_Paused;
    return S_OK;
}

STDMETHODIMP VcamFilter::Run(REFERENCE_TIME tStart) {
    std::lock_guard lk(lock_);
    if (state() == State_Stopped) {
        const HRESULT hr = Pause();
        if (FAILED(hr)) return hr;
    }
    start_time_ = tStart;
    run_qpc_ = qpc_now();
    state_ = State_Running;
    return S_OK;
}

STDMETHODIMP VcamFilter::GetState(DWORD, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    *State = state();
    // ライブソースは Paused 中にデータを出さない
    return *State == State_Paused ? VFW_S_CANT_CUE : S_OK;
}

STDMETHODIMP VcamFilter::SetSyncSource(IReferenceClock* pClock) {
    std::lock_guard lk(clock_lock_);
    if (pClock) pClock->AddRef();
    if (clock_) clock_->Release();
    clock_ = pClock;
    return S_OK;
}

STDMETHODIMP VcamFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    std::lock_guard lk(clock_lock_);
    *pClock = clock_;
    if (clock_) clock_->AddRef();
    return S_OK;
}

REFERENCE_TIME VcamFilter::stream_time() {
    IReferenceClock* clock = nullptr;
    {
        std::lock_guard lk(clock_lock_);
        clock = clock_;
        if (clock) clock->AddRef();
    }
    REFERENCE_TIME t = 0;
    if (clock) {
        REFERENCE_TIME now = 0;
        clock->GetTime(&now);
        clock->Release();
        t = now - start_time_.load();
    } else {
        t = qpc_to_us(qpc_now() - run_qpc_.load()) * 10;
    }
    return std::max<REFERENCE_TIME>(t, 0);
}

STDMETHODIMP VcamFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) EnumPinsImpl(this, 0);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VcamFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!Id || !ppPin) return E_POINTER;
    if (wcscmp(Id, kPinName) != 0) {
        *ppPin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    *ppPin = pin_.get();
    pin_->AddRef();
    return S_OK;
}

STDMETHODIMP VcamFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    std::lock_guard lk(lock_);
    wcsncpy_s(pInfo->achName, name_.c_str(), _TRUNCATE);
    pInfo->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

STDMETHODIMP VcamFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    std::lock_guard lk(lock_);
    graph_ = pGraph;
    name_ = pName ? std::wstring(pName) : cam_name(cam_);
    return S_OK;
}

STDMETHODIMP VcamFilter::QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }

STDMETHODIMP_(ULONG) VcamFilter::GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

// ============================================================================
// VcamPin
// ============================================================================

VcamPin::VcamPin(VcamFilter* filter) : filter_(filter), format_(supported_formats().front()) {}

VcamPin::~VcamPin() {
    Inactive();
    release_connection();
}

STDMETHODIMP VcamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) VcamPin::AddRef() { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) VcamPin::Release() { return filter_->Release(); }

STDMETHODIMP VcamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    std::lock_guard lk(filter_->lock());
    if (connected_) return VFW_E_ALREADY_CONNECTED;
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;

    std::vector<OutputFormat> candidates;
    if (pmt && pmt->formattype != GUID_NULL && pmt->pbFormat) {
        // 完全に指定された型: それ以外ではつながない
        OutputFormat f;
        if (!media_type_to_format(*pmt, f)) return VFW_E_TYPE_NOT_ACCEPTED;
        candidates.push_back(f);
    } else {
        for (const auto& f : ordered_formats(format_)) {
            if (pmt && pmt->majortype != GUID_NULL && pmt->majortype != MEDIATYPE_Video) continue;
            if (pmt && pmt->subtype != GUID_NULL && pmt->subtype != subtype_of(f.pix)) continue;
            candidates.push_back(f);
        }
    }

    for (const auto& f : candidates) {
        if (SUCCEEDED(try_connect(pReceivePin, f))) return S_OK;
    }
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT VcamPin::try_connect(IPin* receive_pin, const OutputFormat& f) {
    AM_MEDIA_TYPE mt;
    HRESULT hr = format_to_media_type(f, mt);
    if (FAILED(hr)) return hr;
    hr = receive_pin->ReceiveConnection(this, &mt);
    if (FAILED(hr)) {
        free_media_type(mt);
        return hr;
    }

    const OutputFormat previous = format_;
    connected_ = receive_pin;
    connected_->AddRef();
    mt_ = mt;  // 所有権を移す
    format_ = f;

    hr = receive_pin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input_));
    if (SUCCEEDED(hr)) hr = decide_allocator();
    if (FAILED(hr)) {
        receive_pin->Disconnect();
        release_connection();
        format_ = previous;
    }
    return hr;
}

HRESULT VcamPin::decide_allocator() {
    const long needed = long(format_.image_bytes());
    ALLOCATOR_PROPERTIES req{};
    input_->GetAllocatorRequirements(&req);

    ALLOCATOR_PROPERTIES props{};
    props.cBuffers = std::max(req.cBuffers, 4L);
    props.cbBuffer = std::max(req.cbBuffer, needed);
    props.cbAlign = std::max(req.cbAlign, 1L);
    props.cbPrefix = req.cbPrefix;

    auto try_alloc = [&](IMemAllocator* a) -> HRESULT {
        ALLOCATOR_PROPERTIES actual{};
        HRESULT hr = a->SetProperties(&props, &actual);
        if (FAILED(hr)) return hr;
        if (actual.cbBuffer < needed) return E_FAIL;
        return input_->NotifyAllocator(a, FALSE);
    };

    IMemAllocator* a = nullptr;
    if (SUCCEEDED(input_->GetAllocator(&a))) {
        if (SUCCEEDED(try_alloc(a))) {
            allocator_ = a;
            return S_OK;
        }
        a->Release();
        a = nullptr;
    }
    HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&a));
    if (FAILED(hr)) return hr;
    hr = try_alloc(a);
    if (FAILED(hr)) {
        a->Release();
        return hr;
    }
    allocator_ = a;
    return S_OK;
}

void VcamPin::release_connection() {
    if (allocator_) allocator_->Release();
    if (input_) input_->Release();
    if (connected_) connected_->Release();
    allocator_ = nullptr;
    input_ = nullptr;
    connected_ = nullptr;
    free_media_type(mt_);
    mt_ = AM_MEDIA_TYPE{};
}

STDMETHODIMP VcamPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }

STDMETHODIMP VcamPin::Disconnect() {
    std::lock_guard lk(filter_->lock());
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
    if (!connected_) return S_FALSE;
    release_connection();
    return S_OK;
}

STDMETHODIMP VcamPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    std::lock_guard lk(filter_->lock());
    *pPin = connected_;
    if (!connected_) return VFW_E_NOT_CONNECTED;
    connected_->AddRef();
    return S_OK;
}

STDMETHODIMP VcamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard lk(filter_->lock());
    if (!connected_) {
        *pmt = AM_MEDIA_TYPE{};
        return VFW_E_NOT_CONNECTED;
    }
    return copy_media_type(*pmt, mt_);
}

STDMETHODIMP VcamPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = filter_;
    filter_->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcsncpy_s(pInfo->achName, kPinName, _TRUNCATE);
    return S_OK;
}

STDMETHODIMP VcamPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP VcamPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinName)));
    if (!*Id) return E_OUTOFMEMORY;
    memcpy(*Id, kPinName, sizeof(kPinName));
    return S_OK;
}

STDMETHODIMP VcamPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    OutputFormat f;
    return media_type_to_format(*pmt, f) ? S_OK : S_FALSE;
}

STDMETHODIMP VcamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::lock_guard lk(filter_->lock());
    try {
        *ppEnum = new EnumMediaTypesImpl(ordered_formats(format_), 0);
    } catch (...) {
        *ppEnum = nullptr;
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

STDMETHODIMP VcamPin::QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP VcamPin::EndOfStream() { return E_UNEXPECTED; }
STDMETHODIMP VcamPin::BeginFlush() { return E_UNEXPECTED; }
STDMETHODIMP VcamPin::EndFlush() { return E_UNEXPECTED; }
STDMETHODIMP VcamPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// ---- IAMStreamConfig ----

STDMETHODIMP VcamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    std::unique_lock lk(filter_->lock());
    OutputFormat f = supported_formats().front();
    if (pmt && !media_type_to_format(*pmt, f)) return VFW_E_INVALIDMEDIATYPE;
    if (!connected_) {
        format_ = f;
        return S_OK;
    }
    if (f == format_) return S_OK;
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;

    // 接続中: 相手が受け入れるならつなぎ直す
    AM_MEDIA_TYPE mt;
    if (FAILED(format_to_media_type(f, mt))) return E_OUTOFMEMORY;
    const HRESULT accept = connected_->QueryAccept(&mt);
    free_media_type(mt);
    if (accept != S_OK) return VFW_E_INVALIDMEDIATYPE;
    IFilterGraph* graph = filter_->graph();
    if (!graph) return VFW_E_NOT_IN_GRAPH;
    format_ = f;
    lk.unlock();  // Reconnect は Disconnect / Connect を呼び返してくる
    return graph->Reconnect(this);
}

STDMETHODIMP VcamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    std::lock_guard lk(filter_->lock());
    *ppmt = create_media_type(format_);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VcamPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = int(supported_formats().size());
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP VcamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    const auto& formats = supported_formats();
    if (iIndex < 0 || iIndex >= int(formats.size())) return S_FALSE;
    const auto& f = formats[iIndex];
    *ppmt = create_media_type(f);
    if (!*ppmt) return E_OUTOFMEMORY;

    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    *caps = VIDEO_STREAM_CONFIG_CAPS{};
    const SIZE size{LONG(f.fmt.width), LONG(f.fmt.height)};
    caps->guid = FORMAT_VideoInfo;
    caps->InputSize = size;
    caps->MinCroppingSize = size;
    caps->MaxCroppingSize = size;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->CropAlignX = 1;
    caps->CropAlignY = 1;
    caps->MinOutputSize = size;
    caps->MaxOutputSize = size;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = f.frame_duration();
    caps->MaxFrameInterval = f.frame_duration();
    const uint64_t bps = uint64_t(f.image_bytes()) * 8 * f.fmt.fps_num / f.fmt.fps_den;
    caps->MinBitsPerSecond = LONG(std::min<uint64_t>(bps, LONG_MAX));
    caps->MaxBitsPerSecond = caps->MinBitsPerSecond;
    return S_OK;
}

// ---- IKsPropertySet (ピンカテゴリ = キャプチャ) ----

STDMETHODIMP VcamPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }

STDMETHODIMP VcamPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (!pPropData) return pcbReturned ? S_OK : E_POINTER;
    if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP VcamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// ---- ストリーミング ----

HRESULT VcamPin::Active() {
    if (!connected_) return S_OK;
    HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) return hr;
    try {
        source_ = std::make_unique<FrameSource>(filter_->cam(), format_);
        stop_ = false;
        thread_ = std::thread(&VcamPin::streaming_thread, this);
    } catch (...) {
        source_.reset();
        allocator_->Decommit();
        return E_FAIL;
    }
    return S_OK;
}

HRESULT VcamPin::Inactive() {
    stop_ = true;
    if (source_) source_->interrupt();
    if (allocator_) allocator_->Decommit();  // GetBuffer の待ちを解除
    if (thread_.joinable()) thread_.join();
    source_.reset();
    return S_OK;
}

void VcamPin::streaming_thread() {
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);

    try {
        const REFERENCE_TIME duration = format_.frame_duration();
        const long bytes = long(format_.image_bytes());
        bool first = true;

        while (!stop_) {
            if (filter_->state() != State_Running) {
                Sleep(5);
                continue;
            }
            IMediaSample* sample = nullptr;
            if (FAILED(allocator_->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample) {
                if (!stop_) Sleep(5);
                continue;
            }

            HRESULT hr = S_OK;
            BYTE* data = nullptr;
            if (SUCCEEDED(sample->GetPointer(&data)) && sample->GetSize() >= bytes) {
                source_->fill(data);
                if (!stop_) {
                    REFERENCE_TIME start = filter_->stream_time();
                    REFERENCE_TIME end = start + duration;
                    sample->SetTime(&start, &end);
                    sample->SetActualDataLength(bytes);
                    sample->SetSyncPoint(TRUE);
                    sample->SetDiscontinuity(first ? TRUE : FALSE);
                    sample->SetPreroll(FALSE);
                    hr = input_->Receive(sample);
                    first = false;
                }
            }
            sample->Release();

            if (hr != S_OK) {
                // 下流がこれ以上受け取らない (S_FALSE / エラー)。次の Stop まで待つ。
                while (!stop_) Sleep(10);
            }
        }
    } catch (...) {
        // ホストプロセス (OBS) を巻き込まないよう、例外は外に出さない
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace denpachan::vcam
