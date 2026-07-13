#pragma once
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>  // WAVEFORMATEX (excluded by WIN32_LEAN_AND_MEAN otherwise)
#include <propsys.h>
#include <propvarutil.h>  // PROPVARIANT, PropVariantInit/Clear
#include <windows.h>
#define INITGUID
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName
#undef INITGUID

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../audio_backend.hpp"

namespace audio_device_manager {

namespace detail {

/// @brief  Smart Pointer handeling reference counting for COM objects
/// @tparam T COM object
template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  explicit ComPtr(T* ptr) : ptr_(ptr) {}  // adopts an already-held reference
  ~ComPtr() { this->reset(); }

  ComPtr(const ComPtr&)            = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      this->reset();
      this->ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }

  T** put() {
    this->reset();
    return &this->ptr_;
  }

  T* get() const { return this->ptr_; }
  T* operator->() const { return this->ptr_; }
  explicit operator bool() const { return this->ptr_ != nullptr; }

  void reset() {
    if (this->ptr_) {
      this->ptr_->Release();
      this->ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

// IPolicyConfig is an undocumented COM interface used to change the system default
// audio endpoint. There is no public WASAPI API for this: IMMDeviceEnumerator
// only lets you *read* the default endpoint (GetDefaultAudioEndpoint), and
// Microsoft has never shipped a documented setter. Every mainstream tool that
// changes the Windows default device from code or CLI (NirSoft's SoundVolumeView,
// EarTrumpet, audioswitch, svcl, ...) goes through this same private interface,
// which backs the "Set as Default Device" option in the Windows Sound control
// panel itself.
//
// Definition and GUIDs (Windows 7 and later) reproduced from:
//   https://github.com/tartakynov/audioswitch/blob/master/IPolicyConfig.h
// Further links:
//   https://learn.microsoft.com/en-us/answers/questions/259576/app-volume-and-device-preferences-api-dll-interfac
//   https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/audio-policy-manager-sample-application/
// (that last one is Microsoft's own audio *policy* sample; it does not cover
// SetDefaultEndpoint, but is a useful reference for the surrounding APIs)
//
// This interface is not part of the public ABI and might change in a future
// Windows release without notice. Every call site here treats
// CoCreateInstance/QueryInterface failure as a normal, recoverable
// `CommandStatus::Unsupported` result rather than assuming success.

struct IPolicyConfig : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**)                       = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**)               = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR)                                  = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*)      = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64)           = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64)                        = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*)              = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*)              = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole)        = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT)                         = 0;
};

// COM identifiers defined as plain GUID literals due to missing header

inline constexpr CLSID kPolicyConfigClientClsid = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
inline constexpr IID kPolicyConfigIid           = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

inline std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
  return wide;
}

inline std::string wide_to_utf8(const wchar_t* wide) {
  if (!wide) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string utf8(static_cast<size_t>(size - 1), '\0');  // -1: exclude the null terminator WideCharToMultiByte counts
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
  return utf8;
}

inline std::string hresult_to_string(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

// Endpoint add/remove/state/default-device notifications. Registered once
// against the enumerator; not endpoint-specific.
// https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient
class NotificationClient : public IMMNotificationClient {
 public:
  explicit NotificationClient(std::function<void()> on_change) : on_change_(std::move(on_change)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *ppv = this;
      this->AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++this->ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --this->ref_count_;
    if (count == 0) delete this;
    return count;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
    this->notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
    this->notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
    this->notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
    this->notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    this->notify();
    return S_OK;
  }

 private:
  void notify() {
    if (this->on_change_) this->on_change_();
  }
  std::function<void()> on_change_;
  std::atomic<ULONG> ref_count_{1};
};

// Per-endpoint volume/mute change notifications. IMMNotificationClient does
// NOT cover these: volume/mute live behind IAudioEndpointVolume, not the
// device property store, so they need their own callback registered per
// device via IAudioEndpointVolume::RegisterControlChangeNotify.
// https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nn-endpointvolume-iaudioendpointvolumecallback
class VolumeCallback : public IAudioEndpointVolumeCallback {
 public:
  explicit VolumeCallback(std::function<void()> on_change) : on_change_(std::move(on_change)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
      *ppv = this;
      this->AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++this->ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --this->ref_count_;
    if (count == 0) delete this;
    return count;
  }

  HRESULT STDMETHODCALLTYPE OnNotify(AUDIO_VOLUME_NOTIFICATION_DATA*) override {
    if (this->on_change_) this->on_change_();
    return S_OK;
  }

 private:
  std::function<void()> on_change_;
  std::atomic<ULONG> ref_count_{1};
};

// One VolumeSubscription lives per currently-known endpoint. Keeps the activated
// IAudioEndpointVolume alive (required for the callback registration to stay
// valid) alongside the callback object itself, so both can be unregistered
// and torn down together when the device disappears or the backend shuts down.
struct VolumeSubscription {
  ComPtr<IAudioEndpointVolume> endpoint_volume;
  ComPtr<VolumeCallback> callback;
};

}  // namespace detail

class WasapiBackend : public AudioBackend {
 public:
  WasapiBackend() : WasapiBackend(std::make_shared<bool>(false)) {}

  ~WasapiBackend() override {
    this->poll_worker_.stop();

    std::promise<void> released;
    auto released_future = released.get_future();
    this->worker_.post([this, &released] {
      if (this->enumerator_ && this->notification_client_) {
        this->enumerator_->UnregisterEndpointNotificationCallback(this->notification_client_.get());
      }
      this->notification_client_.reset();

      for (auto& [id, subscription] : this->volume_subscriptions_) {
        subscription.endpoint_volume->UnregisterControlChangeNotify(subscription.callback.get());
      }
      this->volume_subscriptions_.clear();

      this->enumerator_.reset();
      released.set_value();
    });
    released_future.wait();
  }

  AudioBackendType type() const override { return AudioBackendType::Wasapi; }
  bool available() const override { return this->available_; }

 private:
  CommandResult handle_set_volume(const std::string& device_id, float volume) override {
    auto device = this->find_device(device_id);
    if (!device) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};

    detail::ComPtr<IAudioEndpointVolume> endpoint_volume;
    HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(endpoint_volume.put()));
    if (FAILED(hr)) return {CommandStatus::BackendError, detail::hresult_to_string(hr)};

    hr = endpoint_volume->SetMasterVolumeLevelScalar(std::clamp(volume, 0.f, 1.f), nullptr);
    if (FAILED(hr)) return {CommandStatus::BackendError, detail::hresult_to_string(hr)};

    return {};
  }

  CommandResult handle_set_mute(const std::string& device_id, bool muted) override {
    auto device = this->find_device(device_id);
    if (!device) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};

    detail::ComPtr<IAudioEndpointVolume> endpoint_volume;
    HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(endpoint_volume.put()));
    if (FAILED(hr)) return {CommandStatus::BackendError, detail::hresult_to_string(hr)};

    hr = endpoint_volume->SetMute(muted, nullptr);
    if (FAILED(hr)) return {CommandStatus::BackendError, detail::hresult_to_string(hr)};

    return {};
  }

  CommandResult handle_set_default(const std::string& device_id) override {
    auto device = this->find_device(device_id);
    if (!device) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};

    detail::ComPtr<detail::IPolicyConfig> policy_config;
    HRESULT hr =
        CoCreateInstance(detail::kPolicyConfigClientClsid, nullptr, CLSCTX_ALL, detail::kPolicyConfigIid, reinterpret_cast<void**>(policy_config.put()));
    if (FAILED(hr)) return {CommandStatus::Unsupported, "IPolicyConfig unavailable: " + detail::hresult_to_string(hr)};

    std::wstring wide_id = detail::utf8_to_wide(device_id);
    // set default for all roles
    for (ERole role : {eConsole, eMultimedia, eCommunications}) {
      hr = policy_config->SetDefaultEndpoint(wide_id.c_str(), role);
      if (FAILED(hr)) return {CommandStatus::BackendError, detail::hresult_to_string(hr)};
    }

    return {};
  }

  CommandResult handle_refresh() override {
    this->refresh_devices();
    return {};
  }

  explicit WasapiBackend(std::shared_ptr<bool> com_ok)
      : AudioBackend(
            "WASAPI", BackendFeature::All, [com_ok] { *com_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); },
            [com_ok] {
              if (*com_ok) CoUninitialize();
            }) {
    this->try_init();
  }

  void try_init() {
    std::promise<bool> probe;
    auto probe_future = probe.get_future();
    this->worker_.post([this, &probe] {
      detail::ComPtr<IMMDeviceEnumerator> enumerator;
      HRESULT hr =
          CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put()));
      bool ok = SUCCEEDED(hr);
      if (ok) {
        this->enumerator_ = std::move(enumerator);

        // push notifications for device add/remove/state/default-endpoint changes
        this->notification_client_ = detail::ComPtr<detail::NotificationClient>(new detail::NotificationClient([this] { this->request_refresh(); }));
        this->enumerator_->RegisterEndpointNotificationCallback(this->notification_client_.get());
      }
      probe.set_value(ok);
    });
    this->available_ = probe_future.get();
  }

  void refresh_devices() {
    if (!this->enumerator_) return;
    std::vector<DeviceSnapshot> snapshots;
    std::unordered_set<std::string> seen_ids;
    this->enumerate_flow(eRender, DeviceType::Output, snapshots, seen_ids);
    this->enumerate_flow(eCapture, DeviceType::Input, snapshots, seen_ids);

    // drop volume-change subscriptions for endpoints that vanished
    for (auto it = this->volume_subscriptions_.begin(); it != this->volume_subscriptions_.end();) {
      if (seen_ids.count(it->first)) {
        ++it;
      } else {
        it->second.endpoint_volume->UnregisterControlChangeNotify(it->second.callback.get());
        it = this->volume_subscriptions_.erase(it);
      }
    }

    this->push_update_event(std::move(snapshots));
  }

  void enumerate_flow(EDataFlow flow, DeviceType type, std::vector<DeviceSnapshot>& out, std::unordered_set<std::string>& seen_ids) {
    std::string default_id;
    {
      detail::ComPtr<IMMDevice> default_device;
      if (SUCCEEDED(this->enumerator_->GetDefaultAudioEndpoint(flow, eConsole, default_device.put()))) {
        default_id = this->device_id_of(default_device.get());
      }
    }

    detail::ComPtr<IMMDeviceCollection> collection;
    if (FAILED(this->enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) return;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      detail::ComPtr<IMMDevice> device;
      if (FAILED(collection->Item(i, device.put()))) continue;

      std::string id = this->device_id_of(device.get());
      if (id.empty()) continue;
      seen_ids.insert(id);

      DeviceSnapshot snap;
      snap.backend_device_id = id;
      snap.name              = this->device_name_of(device.get());
      snap.type              = type;
      snap.is_default        = (id == default_id);
      snap.min_level         = 0.f;
      snap.max_level         = 1.f;

      // each endpoint gets its own IAudioEndpointVolumeCallback, keep both
      // callback and IAudioEndpointVolume alive in volume_subscriptions_
      // for as long as the device is around
      IAudioEndpointVolume* endpoint_volume = nullptr;
      auto existing_subscription            = this->volume_subscriptions_.find(id);
      if (existing_subscription != this->volume_subscriptions_.end()) {
        endpoint_volume = existing_subscription->second.endpoint_volume.get();
      } else {
        detail::ComPtr<IAudioEndpointVolume> activated;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(activated.put())))) {
          auto callback = detail::ComPtr<detail::VolumeCallback>(new detail::VolumeCallback([this] { this->request_refresh(); }));
          activated->RegisterControlChangeNotify(callback.get());
          endpoint_volume = activated.get();
          this->volume_subscriptions_.emplace(id, detail::VolumeSubscription{std::move(activated), std::move(callback)});
        }
      }

      if (endpoint_volume) {
        float level = 0.f;
        BOOL muted  = FALSE;
        if (SUCCEEDED(endpoint_volume->GetMasterVolumeLevelScalar(&level))) snap.volume = level;
        if (SUCCEEDED(endpoint_volume->GetMute(&muted))) snap.muted = (muted != FALSE);
      }

      out.push_back(std::move(snap));
    }
  }

  std::string device_id_of(IMMDevice* device) {
    if (!device) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::string result = detail::wide_to_utf8(id);
    CoTaskMemFree(id);
    return result;
  }

  std::string device_name_of(IMMDevice* device) {
    detail::ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) return {};

    PROPVARIANT prop;
    PropVariantInit(&prop);
    std::string name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &prop)) && prop.vt == VT_LPWSTR && prop.pwszVal) {
      name = detail::wide_to_utf8(prop.pwszVal);
    }
    PropVariantClear(&prop);
    return name;
  }

  detail::ComPtr<IMMDevice> find_device(const std::string& device_id) {
    detail::ComPtr<IMMDevice> device;
    if (!this->enumerator_) return device;
    std::wstring wide_id = detail::utf8_to_wide(device_id);
    this->enumerator_->GetDevice(wide_id.c_str(), device.put());
    return device;
  }

  bool available_ = false;
  detail::ComPtr<IMMDeviceEnumerator> enumerator_;
  detail::ComPtr<detail::NotificationClient> notification_client_;
  std::unordered_map<std::string, detail::VolumeSubscription> volume_subscriptions_;
};

}  // namespace audio_device_manager
#endif
