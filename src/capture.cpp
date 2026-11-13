#include "catalyst/bridge.hpp"
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
Json capture_window(HWND window, const Json &) {
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
    struct Unmap {
      ID3D11DeviceContext *context;
      ID3D11Resource *resource;
      ~Unmap() {
        context->Unmap(resource, 0);
      }
    } unmap{context.get(), staging.get()};
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
    check_hresult(output->SetSize(size.Width, size.Height));
    auto format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(output->SetPixelFormat(&format));
    check_hresult(output->WritePixels(size.Height, mapped.RowPitch, mapped.RowPitch * size.Height,
                                      static_cast<BYTE *>(mapped.pData)));
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
    frame.Close();
    session.Close();
    pool.Close();
    return {{"width", size.Width}, {"height", size.Height}, {"png_base64", base64(png)}};
  } catch (const winrt::hresult_error &error) {
    throw ToolError("Window capture failed: " + winrt::to_string(error.message()));
  }
}
} // namespace catalyst
