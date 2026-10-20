#pragma once
// DirectShow ソースフィルタ (baseclasses を使わない最小実装)。
//
// VcamFilter (IBaseFilter) が出力ピン VcamPin を 1 本持つ。
// ピンの参照カウントはフィルタに委譲し、フィルタと同じ寿命にする。
//
// スレッド:
// - グラフ操作 (Connect / Stop / Pause / Run など) は lock_ で直列化する。
// - ストリーミングスレッドは lock_ を取らない (Stop が lock_ を持ったままスレッドを join するため)。
//   状態や開始時刻は atomic、参照クロックは clock_lock_ で保護する。

#include <dshow.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "formats.h"

namespace denpachan::vcam {

class VcamPin;
class FrameSource;

class VcamFilter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
    static HRESULT Create(int cam, REFIID riid, void** ppv);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

    int cam() const { return cam_; }
    std::recursive_mutex& lock() { return lock_; }
    FILTER_STATE state() const { return FILTER_STATE(state_.load()); }
    IFilterGraph* graph() const { return graph_; }
    VcamPin* pin() const { return pin_.get(); }

    // ストリーミングスレッド用: 現在のストリーム時刻 (100ns 単位)
    REFERENCE_TIME stream_time();

private:
    explicit VcamFilter(int cam);
    ~VcamFilter();

    std::atomic<ULONG> ref_{1};
    const int cam_;
    std::recursive_mutex lock_;
    std::atomic<int> state_{State_Stopped};
    std::wstring name_;
    IFilterGraph* graph_ = nullptr;  // DirectShow の規約で AddRef しない

    std::mutex clock_lock_;
    IReferenceClock* clock_ = nullptr;
    std::atomic<REFERENCE_TIME> start_time_{0};
    std::atomic<int64_t> run_qpc_{0};  // 参照クロックがないときの基準

    std::unique_ptr<VcamPin> pin_;
};

class VcamPin final : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    explicit VcamPin(VcamFilter* filter);
    ~VcamPin();

    // IUnknown (フィルタに委譲)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData,
                     DWORD cbPropData) override;
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData,
                     DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // フィルタの状態遷移から呼ぶ (lock_ 保持中)
    HRESULT Active();
    HRESULT Inactive();

    // 接続中ならその形式、未接続なら SetFormat で選ばれた形式 (なければ既定)
    OutputFormat current_format() const { return format_; }

private:
    HRESULT try_connect(IPin* receive_pin, const OutputFormat& f);
    HRESULT decide_allocator();
    void release_connection();
    void streaming_thread();

    VcamFilter* const filter_;

    IPin* connected_ = nullptr;
    IMemInputPin* input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE mt_{};
    OutputFormat format_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::unique_ptr<FrameSource> source_;
};

}  // namespace denpachan::vcam
