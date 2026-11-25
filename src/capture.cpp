#include "catalyst/bridge.hpp"
#include <algorithm>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
namespace catalyst {
Frame capture_window(HWND window) {
  using namespace winrt;
  using namespace winrt::Windows::Graphics::Capture;
  using namespace winrt::Windows::Graphics::DirectX;
  using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment {
      ~Apartment() {
        winrt::uninit_apartment();
      }
    } apartment;
    if (!IsWindow(window) || IsIconic(window))
      throw ToolError("Restore the Catalyst window before rendering an image");
    if (!GraphicsCaptureSession::IsSupported())
      throw ToolError("Windows Graphics Capture is unavailable");
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                    device.put(), nullptr, context.put()));
    auto dxgi = device.as<IDXGIDevice>();
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    auto direct3d = inspectable.as<IDirect3DDevice>();
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForWindow(window, guid_of<GraphicsCaptureItem>(), put_abi(item)));
    auto size = item.Size();
    if (size.Width < 1 || size.Height < 1 || size.Width > 8192 || size.Height > 8192)
      throw ToolError("Unsupported capture dimensions");
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        direct3d, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    auto session = pool.CreateCaptureSession(item);
    session.IsCursorCaptureEnabled(false);
    struct CaptureState {
      handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      std::mutex mutex;
      Direct3D11CaptureFrame captured{nullptr};
    };
    auto state = std::make_shared<CaptureState>();
    auto token = pool.FrameArrived([state](const Direct3D11CaptureFramePool &sender, const auto &) {
      try {
        auto frame = sender.TryGetNextFrame();
        std::lock_guard lock(state->mutex);
        if (!state->captured) {
          state->captured = frame;
          SetEvent(state->ready.get());
        }
      } catch (const winrt::hresult_error &) {
        SetEvent(state->ready.get());
      }
    });
    session.StartCapture();
    const auto wait = WaitForSingleObject(state->ready.get(), 5000);
    pool.FrameArrived(token);
    if (wait != WAIT_OBJECT_0) {
      session.Close();
      pool.Close();
      throw ToolError("Timed out waiting for a rendered Catalyst frame");
    }
    Direct3D11CaptureFrame frame{nullptr};
    {
      std::lock_guard lock(state->mutex);
      frame = state->captured;
    }
    if (!frame)
      throw ToolError("Window capture did not produce a frame");
    auto access =
        frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    com_ptr<ID3D11Texture2D> staging;
    check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
    context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    Frame result;
    result.width = static_cast<int>(std::min<UINT>(desc.Width, static_cast<UINT>(size.Width)));
    result.height = static_cast<int>(std::min<UINT>(desc.Height, static_cast<UINT>(size.Height)));
    result.bgra.resize(static_cast<size_t>(result.width) * result.height * 4);
    for (int y = 0; y < result.height; ++y)
      std::memcpy(result.bgra.data() + static_cast<size_t>(y) * result.width * 4,
                  static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                  static_cast<size_t>(result.width) * 4);
    context->Unmap(staging.get(), 0);
    frame.Close();
    session.Close();
    pool.Close();
    // Window capture does not guarantee an opaque alpha channel.
    for (size_t i = 3; i < result.bgra.size(); i += 4)
      result.bgra[i] = 255;
    return result;
  } catch (const winrt::hresult_error &error) {
    throw ToolError("Window capture failed: " + winrt::to_string(error.message()));
  }
}

namespace {
// Editor chrome is dark, nearly neutral grey; the preview's sky and ground are bright or saturated.
bool scene_pixel(const uint8_t *p) {
  const int b = p[0], g = p[1], r = p[2];
  const int high = std::max({r, g, b}), low = std::min({r, g, b});
  return high > 95 || high - low > 70;
}
// Longest run of indices where keep(i) holds, bridging gaps of up to max_gap indices.
template <class Keep> std::pair<int, int> longest_run(int count, int max_gap, Keep keep) {
  int best_begin = 0, best_end = 0, begin = -1, last = -1;
  for (int i = 0; i <= count; ++i) {
    const bool ok = i < count && keep(i);
    if (ok) {
      if (begin < 0 || i - last - 1 > max_gap)
        begin = i;
      last = i;
      if (last + 1 - begin > best_end - best_begin) {
        best_begin = begin;
        best_end = last + 1;
      }
    }
  }
  return {best_begin, best_end};
}
} // namespace

bool detect_viewport(const Frame &frame, Rect &area) {
  const int w = frame.width, h = frame.height;
  if (w < 64 || h < 64)
    return false;
  const auto pixel = [&](int x, int y) { return frame.bgra.data() + (static_cast<size_t>(y) * w + x) * 4; };
  std::vector<int> columns(w, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      columns[x] += scene_pixel(pixel(x, y));
  // The 90th percentile column lies inside the viewport whenever it spans more than a tenth of the window.
  auto sorted = columns;
  std::nth_element(sorted.begin(), sorted.begin() + w * 9 / 10, sorted.end());
  const int reference = sorted[w * 9 / 10];
  if (reference < h / 8)
    return false;
  // Gaps are bridged because dark particles can hide the sky behind them.
  auto [x0, x1] = longest_run(w, 40, [&](int x) { return columns[x] * 2 >= reference; });
  if (x1 - x0 < 160)
    return false;
  auto [y0, y1] = longest_run(h, 40, [&](int y) {
    int count = 0;
    for (int x = x0; x < x1; ++x)
      count += scene_pixel(pixel(x, y));
    return count * 2 >= x1 - x0;
  });
  if (y1 - y0 < 160)
    return false;
  area = {x0, y0, x1 - x0, y1 - y0};
  return true;
}

Frame crop(const Frame &frame, const Rect &area) {
  const int x0 = std::clamp(area.x, 0, frame.width), y0 = std::clamp(area.y, 0, frame.height);
  const int x1 = std::clamp(area.x + area.width, x0, frame.width);
  const int y1 = std::clamp(area.y + area.height, y0, frame.height);
  if (x1 - x0 < 1 || y1 - y0 < 1)
    throw ToolError("The capture region lies outside the Catalyst window");
  Frame out{x1 - x0, y1 - y0, {}};
  out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y)
    std::memcpy(out.bgra.data() + static_cast<size_t>(y) * out.width * 4,
                frame.bgra.data() + (static_cast<size_t>(y0 + y) * frame.width + x0) * 4,
                static_cast<size_t>(out.width) * 4);
  return out;
}

Frame compose_row(const std::vector<Frame> &frames, int gap) {
  if (frames.size() == 1)
    return frames.front();
  Frame out;
  for (const auto &f : frames) {
    out.width += f.width;
    out.height = std::max(out.height, f.height);
  }
  out.width += gap * static_cast<int>(frames.size() - 1);
  out.bgra.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
  for (size_t i = 3; i < out.bgra.size(); i += 4)
    out.bgra[i] = 255;
  int left = 0;
  for (const auto &f : frames) {
    for (int y = 0; y < f.height; ++y)
      std::memcpy(out.bgra.data() + (static_cast<size_t>(y) * out.width + left) * 4,
                  f.bgra.data() + static_cast<size_t>(y) * f.width * 4, static_cast<size_t>(f.width) * 4);
    left += f.width + gap;
  }
  return out;
}

namespace {
Frame downscale(const Frame &frame, int max_width) {
  if (max_width <= 0 || frame.width <= max_width)
    return frame;
  const double scale = double(frame.width) / max_width;
  Frame out{max_width, std::max(1, static_cast<int>(frame.height / scale + 0.5)), {}};
  out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y) {
    const int sy0 = static_cast<int>(y * scale), sy1 = std::max(sy0 + 1, static_cast<int>((y + 1) * scale));
    for (int x = 0; x < out.width; ++x) {
      const int sx0 = static_cast<int>(x * scale), sx1 = std::max(sx0 + 1, static_cast<int>((x + 1) * scale));
      unsigned sum[4]{}, count = 0;
      for (int sy = sy0; sy < std::min(sy1, frame.height); ++sy)
        for (int sx = sx0; sx < std::min(sx1, frame.width); ++sx) {
          const auto *p = frame.bgra.data() + (static_cast<size_t>(sy) * frame.width + sx) * 4;
          for (int c = 0; c < 4; ++c)
            sum[c] += p[c];
          ++count;
        }
      auto *q = out.bgra.data() + (static_cast<size_t>(y) * out.width + x) * 4;
      for (int c = 0; c < 4; ++c)
        q[c] = static_cast<uint8_t>(count ? sum[c] / count : 0);
    }
  }
  return out;
}
} // namespace

std::string encode_png(const Frame &source, int max_width, int &out_width, int &out_height) {
  using namespace winrt;
  const auto frame = downscale(source, max_width);
  out_width = frame.width;
  out_height = frame.height;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment {
      ~Apartment() {
        winrt::uninit_apartment();
      }
    } apartment;
    com_ptr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put())));
    com_ptr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    com_ptr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> output;
    com_ptr<IPropertyBag2> properties;
    check_hresult(encoder->CreateNewFrame(output.put(), properties.put()));
    check_hresult(output->Initialize(properties.get()));
    check_hresult(output->SetSize(frame.width, frame.height));
    auto format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(output->SetPixelFormat(&format));
    const UINT stride = static_cast<UINT>(frame.width) * 4;
    check_hresult(output->WritePixels(frame.height, stride, stride * frame.height,
                                      const_cast<BYTE *>(frame.bgra.data())));
    check_hresult(output->Commit());
    check_hresult(encoder->Commit());
    STATSTG stat{};
    check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > 32 * 1024 * 1024)
      throw ToolError("Captured PNG is too large");
    LARGE_INTEGER start{};
    check_hresult(stream->Seek(start, STREAM_SEEK_SET, nullptr));
    std::string png(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
    ULONG read{};
    check_hresult(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read));
    png.resize(read);
    return png;
  } catch (const winrt::hresult_error &error) {
    throw ToolError("PNG encoding failed: " + winrt::to_string(error.message()));
  }
}
} // namespace catalyst
