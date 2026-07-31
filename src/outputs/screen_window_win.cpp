// The Windows screen output: Win32 for the window, D3D11 for the picture.
//
// NEVER RUN. This compiles against the Windows SDK and has never been executed,
// let alone put on a projector — see docs/04-verification.md. Treat every claim
// below as intent, not as evidence.
//
// The structure differs from the macOS backend on purpose. A Win32 window
// belongs to the thread that created it and only lives while that thread pumps
// messages, so rather than borrowing the main thread the way AppKit forces,
// this owns a thread outright: it creates the window, runs the pump, and
// renders. Present(1, 0) blocks until the next vertical blank, so that same
// loop is also the pacing — no display-link equivalent is needed.

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "diag/diag.h"
#include "outputs/screen_frame_ring.h"
#include "outputs/screen_geometry.h"
#include "outputs/screen_window.h"

namespace weblinked {
namespace {

constexpr int kRingSlots = ScreenFrameRing::kSlots;

struct Uniforms {
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float padding[2] = {0.0f, 0.0f};  // HLSL constant buffers round up to 16 bytes.
};

const char* const kShaderSource = R"HLSL(
cbuffer Uniforms : register(b0) { float2 scale; float2 padding; };
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

struct VertexOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

VertexOut vertexMain(uint vid : SV_VertexID) {
  float2 pos = float2(float((vid << 1) & 2), float(vid & 2));
  VertexOut output;
  output.position = float4(pos * 2.0 - 1.0, 0.0, 1.0);
  // D3D texture space is already top-left origin, but clip space is not, so the
  // same vertical flip as the Metal backend applies here.
  float2 uv = float2(pos.x, 1.0 - pos.y);
  output.uv = (uv - 0.5) * scale + 0.5;
  return output;
}

float4 pixelMain(VertexOut input) : SV_TARGET {
  if (input.uv.x < 0.0 || input.uv.x > 1.0 || input.uv.y < 0.0 || input.uv.y > 1.0) {
    return float4(0.0, 0.0, 0.0, 1.0);
  }
  return float4(frameTexture.Sample(frameSampler, input.uv).rgb, 1.0);
}
)HLSL";

/// Collected by EnumDisplayMonitors, in the order --screen indexes them.
struct MonitorEntry {
  HMONITOR handle = nullptr;
  RECT bounds{};
  std::string name;
  bool primary = false;
  double refreshHz = 0;
};

BOOL CALLBACK monitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
  auto* entries = reinterpret_cast<std::vector<MonitorEntry>*>(userData);

  MONITORINFOEXA info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoA(monitor, &info) == 0) {
    return TRUE;
  }

  MonitorEntry entry;
  entry.handle = monitor;
  entry.bounds = info.rcMonitor;
  entry.name = info.szDevice;
  entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

  DEVMODEA mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != 0) {
    entry.refreshHz = mode.dmDisplayFrequency;
  }

  entries->push_back(std::move(entry));
  return TRUE;
}

std::vector<MonitorEntry> collectMonitors() {
  std::vector<MonitorEntry> entries;
  EnumDisplayMonitors(nullptr, nullptr, &monitorProc,
                      reinterpret_cast<LPARAM>(&entries));
  // The primary monitor goes first so that --screen=0 means what an operator
  // expects; EnumDisplayMonitors makes no promise about order.
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].primary && i != 0) {
      std::swap(entries[0], entries[i]);
      break;
    }
  }
  return entries;
}


template <typename T>
void releaseCom(T*& pointer) {
  if (pointer != nullptr) {
    pointer->Release();
    pointer = nullptr;
  }
}

class WinScreenWindow final : public ScreenWindow {
 public:
  ~WinScreenWindow() override { WinScreenWindow::close(); }

  bool open(const VideoFormat& format, int display, ScreenScaling scaling,
            std::string& error) override;
  void close() override;
  void present(const VideoFrame& frame) override;

  int64_t presentedCount() const override { return ring_.presentedCount(); }
  int64_t droppedCount() const override { return ring_.droppedCount(); }
  std::string describe() const override { return description_; }

 private:
  /// Everything below runs on thread_ and nowhere else.
  void threadMain(VideoFormat format, int display, ScreenScaling scaling);
  bool buildDevice(int display, const RECT& bounds, std::string& error);
  void teardownDevice();
  void drawOnce();

  ScreenFrameRing ring_;
  std::thread thread_;
  std::atomic<bool> quit_{false};

  // Handshake so open() can report a failure the operator can act on rather
  // than returning true and leaving a black display behind.
  std::mutex startMutex_;
  std::condition_variable startSignal_;
  bool started_ = false;
  bool startOk_ = false;
  std::string startError_;

  std::string description_ = "Direct3D 11";
  int frameWidth_ = 0;
  int frameHeight_ = 0;

  HWND window_ = nullptr;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  IDXGISwapChain1* swapChain_ = nullptr;
  ID3D11RenderTargetView* renderTarget_ = nullptr;
  ID3D11VertexShader* vertexShader_ = nullptr;
  ID3D11PixelShader* pixelShader_ = nullptr;
  ID3D11SamplerState* sampler_ = nullptr;
  ID3D11Buffer* uniformBuffer_ = nullptr;
  ID3D11Texture2D* texture_ = nullptr;
  ID3D11ShaderResourceView* textureView_ = nullptr;

  Uniforms uniforms_;

  /// Staging rows, one buffer per ring slot. D3D11 has no equivalent of
  /// aliasing a texture onto a mapped buffer, so unlike the Metal backend this
  /// costs a second copy at map time. Acceptable, and measured nowhere.
  std::vector<uint8_t> slots_[kRingSlots];
};

bool WinScreenWindow::open(const VideoFormat& format, int display,
                           ScreenScaling scaling, std::string& error) {
  if (thread_.joinable()) {
    return true;
  }
  frameWidth_ = format.width;
  frameHeight_ = format.height;
  for (auto& slot : slots_) {
    slot.assign(static_cast<size_t>(format.width) * format.height * 4, 0);
  }

  quit_.store(false);
  started_ = false;
  startOk_ = false;
  startError_.clear();

  thread_ = std::thread(&WinScreenWindow::threadMain, this, format, display, scaling);

  std::unique_lock<std::mutex> lock(startMutex_);
  startSignal_.wait(lock, [this] { return started_; });
  if (!startOk_) {
    error = startError_;
    lock.unlock();
    close();
    return false;
  }
  return true;
}

void WinScreenWindow::close() {
  ring_.shutdown();
  quit_.store(true);
  if (window_ != nullptr) {
    // Wakes the pump; the thread owns the window and does the destroying.
    PostMessageW(window_, WM_CLOSE, 0, 0);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  window_ = nullptr;
}

void WinScreenWindow::present(const VideoFrame& frame) {
  if (frame.pixelFormat() != PixelFormat::kBGRA) {
    return;
  }
  if (frame.format().width != frameWidth_ || frame.format().height != frameHeight_) {
    return;  // A paint from before a raster change; the slots are the old size.
  }
  const int slot = ring_.claim();
  if (slot < 0) {
    return;
  }
  const size_t rowBytes = static_cast<size_t>(frameWidth_) * 4;
  const uint8_t* source = frame.data();
  uint8_t* destination = slots_[slot].data();
  for (int y = 0; y < frameHeight_; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * rowBytes,
                source + static_cast<size_t>(y) * frame.rowBytes(), rowBytes);
  }
  ring_.publish(slot);
}

LRESULT CALLBACK screenWindowProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
  if (message == WM_CLOSE) {
    DestroyWindow(window);
    return 0;
  }
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

void WinScreenWindow::threadMain(VideoFormat format, int display,
                                 ScreenScaling scaling) {
  const auto signalStart = [this](bool ok, std::string message) {
    std::lock_guard<std::mutex> lock(startMutex_);
    started_ = true;
    startOk_ = ok;
    startError_ = std::move(message);
    startSignal_.notify_all();
  };

  const auto monitors = collectMonitors();
  if (display < 0 || static_cast<size_t>(display) >= monitors.size()) {
    signalStart(false, "display " + std::to_string(display) + " does not exist (" +
                           std::to_string(monitors.size()) + " attached)");
    return;
  }
  const RECT bounds = monitors[static_cast<size_t>(display)].bounds;

  static const wchar_t* kClassName = L"WebLinkedScreenOutput";
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = &screenWindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.lpszClassName = kClassName;
  // Re-registering the same class is harmless and returns 0; a second screen
  // output in one process must not fail on it.
  RegisterClassExW(&windowClass);

  window_ = CreateWindowExW(
      // Topmost so it sits over the shell; NOACTIVATE and TRANSPARENT together
      // keep it from stealing focus or swallowing clicks, matching the macOS
      // backend's setIgnoresMouseEvents:.
      WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClassName,
      L"WebLinked", WS_POPUP, bounds.left, bounds.top, bounds.right - bounds.left,
      bounds.bottom - bounds.top, nullptr, nullptr, windowClass.hInstance, nullptr);
  if (window_ == nullptr) {
    signalStart(false, "could not create the output window");
    return;
  }

  std::string error;
  if (!buildDevice(display, bounds, error)) {
    DestroyWindow(window_);
    window_ = nullptr;
    signalStart(false, error);
    return;
  }

  const ScreenScale scale = screenScaleFor(
      format.width, format.height, static_cast<int>(bounds.right - bounds.left),
      static_cast<int>(bounds.bottom - bounds.top), scaling);
  uniforms_.scaleX = scale.x;
  uniforms_.scaleY = scale.y;

  ShowWindow(window_, SW_SHOWNOACTIVATE);
  ring_.reset();
  signalStart(true, {});

  MSG message;
  while (!quit_.load()) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        quit_.store(true);
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (quit_.load()) {
      break;
    }
    // Present(1, 0) inside drawOnce blocks until the vertical blank, so this
    // loop is paced by the display and does not spin.
    drawOnce();
  }

  teardownDevice();
  if (window_ != nullptr) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
}

bool WinScreenWindow::buildDevice(int display, const RECT& bounds,
                                  std::string& error) {
  (void)display;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL featureLevel{};
  HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     flags, nullptr, 0, D3D11_SDK_VERSION, &device_,
                                     &featureLevel, &context_);
  if (FAILED(result)) {
    error = "no Direct3D 11 device available";
    return false;
  }

  IDXGIDevice* dxgiDevice = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice),
                                     reinterpret_cast<void**>(&dxgiDevice))) ||
      FAILED(dxgiDevice->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                reinterpret_cast<void**>(&factory)))) {
    releaseCom(factory);
    releaseCom(adapter);
    releaseCom(dxgiDevice);
    error = "could not reach the DXGI factory";
    return false;
  }

  DXGI_ADAPTER_DESC adapterDescription{};
  if (SUCCEEDED(adapter->GetDesc(&adapterDescription))) {
    char name[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, adapterDescription.Description, -1, name,
                        sizeof(name) - 1, nullptr, nullptr);
    description_ = std::string("Direct3D 11, ") + name;
  }

  DXGI_SWAP_CHAIN_DESC1 swapDescription{};
  swapDescription.Width = static_cast<UINT>(bounds.right - bounds.left);
  swapDescription.Height = static_cast<UINT>(bounds.bottom - bounds.top);
  swapDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swapDescription.SampleDesc.Count = 1;
  swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapDescription.BufferCount = 2;
  swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  result = factory->CreateSwapChainForHwnd(device_, window_, &swapDescription,
                                           nullptr, nullptr, &swapChain_);
  releaseCom(factory);
  releaseCom(adapter);
  releaseCom(dxgiDevice);
  if (FAILED(result)) {
    error = "could not create the swap chain";
    return false;
  }

  ID3D11Texture2D* backBuffer = nullptr;
  if (FAILED(swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                   reinterpret_cast<void**>(&backBuffer)))) {
    error = "could not reach the swap chain back buffer";
    return false;
  }
  result = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
  releaseCom(backBuffer);
  if (FAILED(result)) {
    error = "could not create the render target view";
    return false;
  }

  ID3DBlob* vertexBlob = nullptr;
  ID3DBlob* pixelBlob = nullptr;
  ID3DBlob* compileErrors = nullptr;
  if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr,
                        nullptr, "vertexMain", "vs_4_0", 0, 0, &vertexBlob,
                        &compileErrors))) {
    error = "vertex shader would not compile";
    releaseCom(compileErrors);
    return false;
  }
  if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr,
                        nullptr, "pixelMain", "ps_4_0", 0, 0, &pixelBlob,
                        &compileErrors))) {
    error = "pixel shader would not compile";
    releaseCom(vertexBlob);
    releaseCom(compileErrors);
    return false;
  }
  device_->CreateVertexShader(vertexBlob->GetBufferPointer(),
                              vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
  device_->CreatePixelShader(pixelBlob->GetBufferPointer(),
                             pixelBlob->GetBufferSize(), nullptr, &pixelShader_);
  releaseCom(vertexBlob);
  releaseCom(pixelBlob);

  D3D11_TEXTURE2D_DESC textureDescription{};
  textureDescription.Width = static_cast<UINT>(frameWidth_);
  textureDescription.Height = static_cast<UINT>(frameHeight_);
  textureDescription.MipLevels = 1;
  textureDescription.ArraySize = 1;
  textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  textureDescription.SampleDesc.Count = 1;
  textureDescription.Usage = D3D11_USAGE_DYNAMIC;
  textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateTexture2D(&textureDescription, nullptr, &texture_)) ||
      FAILED(device_->CreateShaderResourceView(texture_, nullptr, &textureView_))) {
    error = "could not create the frame texture";
    return false;
  }

  D3D11_SAMPLER_DESC samplerDescription{};
  samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  device_->CreateSamplerState(&samplerDescription, &sampler_);

  D3D11_BUFFER_DESC uniformDescription{};
  uniformDescription.ByteWidth = sizeof(Uniforms);
  uniformDescription.Usage = D3D11_USAGE_DYNAMIC;
  uniformDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  uniformDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device_->CreateBuffer(&uniformDescription, nullptr, &uniformBuffer_);

  return true;
}

void WinScreenWindow::teardownDevice() {
  releaseCom(textureView_);
  releaseCom(texture_);
  releaseCom(uniformBuffer_);
  releaseCom(sampler_);
  releaseCom(pixelShader_);
  releaseCom(vertexShader_);
  releaseCom(renderTarget_);
  releaseCom(swapChain_);
  releaseCom(context_);
  releaseCom(device_);
}

void WinScreenWindow::drawOnce() {
  const int slot = ring_.acquire();
  if (slot < 0) {
    // Nothing published yet. Still presents, so the window is black rather than
    // showing whatever the desktop had there, and still blocks on the vblank.
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(renderTarget_, black);
    swapChain_->Present(1, 0);
    return;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(context_->Map(texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    const size_t rowBytes = static_cast<size_t>(frameWidth_) * 4;
    const uint8_t* source = slots_[slot].data();
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < frameHeight_; ++y) {
      std::memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch,
                  source + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    context_->Unmap(texture_, 0);
  }

  if (SUCCEEDED(context_->Map(uniformBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped))) {
    std::memcpy(mapped.pData, &uniforms_, sizeof(uniforms_));
    context_->Unmap(uniformBuffer_, 0);
  }

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  context_->ClearRenderTargetView(renderTarget_, black);
  context_->OMSetRenderTargets(1, &renderTarget_, nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertexShader_, nullptr, 0);
  context_->VSSetConstantBuffers(0, 1, &uniformBuffer_);
  context_->PSSetShader(pixelShader_, nullptr, 0);
  context_->PSSetShaderResources(0, 1, &textureView_);
  context_->PSSetSamplers(0, 1, &sampler_);
  context_->Draw(3, 0);

  const HRESULT result = swapChain_->Present(1, 0);
  ring_.release(SUCCEEDED(result));
}

}  // namespace

std::unique_ptr<ScreenWindow> createScreenWindow() {
  return std::make_unique<WinScreenWindow>();
}

std::vector<DisplayInfo> enumerateDisplays() {
  std::vector<DisplayInfo> displays;
  const auto monitors = collectMonitors();
  for (size_t i = 0; i < monitors.size(); ++i) {
    DisplayInfo info;
    info.index = static_cast<int>(i);
    info.name = monitors[i].name;
    info.width = static_cast<int>(monitors[i].bounds.right - monitors[i].bounds.left);
    info.height = static_cast<int>(monitors[i].bounds.bottom - monitors[i].bounds.top);
    info.refreshHz = monitors[i].refreshHz;
    info.primary = monitors[i].primary;
    displays.push_back(std::move(info));
  }
  return displays;
}

}  // namespace weblinked
