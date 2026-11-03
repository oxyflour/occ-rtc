#include "Encoder.hxx"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace drawrtc {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char* action) {
  if (FAILED(hr)) {
    char message[160];
    snprintf(message, sizeof(message), "%s: HRESULT 0x%08lX", action, static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
  }
}
uint8_t clamp(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }
// BT.601 limited-range NV12. Rows in Frame are always top-down.
std::vector<uint8_t> nv12(const Frame& frame) {
  const int w = frame.width, h = frame.height;
  std::vector<uint8_t> result(static_cast<size_t>(w) * h * 3 / 2);
  auto* yPlane = result.data();
  auto* uvPlane = yPlane + w * h;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const auto* p = &frame.bgra[(static_cast<size_t>(y) * w + x) * 4];
      yPlane[y * w + x] = clamp(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16);
    }
  }
  for (int y = 0; y < h; y += 2) {
    for (int x = 0; x < w; x += 2) {
      int r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        const auto* p = &frame.bgra[(static_cast<size_t>(y + dy) * w + x + dx) * 4];
        b += p[0]; g += p[1]; r += p[2];
      }
      r /= 4; g /= 4; b /= 4;
      const int offset = (y / 2) * w + x;
      uvPlane[offset] = clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
      uvPlane[offset + 1] = clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
  }
  return result;
}
void setU32(ICodecAPI* api, const GUID& guid, ULONG value) {
  VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = value;
  api->SetValue(&guid, &v);
}
}
struct Encoder::Impl {
  ComPtr<IMFTransform> transform;
  ComPtr<ICodecAPI> api;
  int width, height, fps;
  bool comInitialized = false, mfInitialized = false;
  std::vector<uint8_t> header;
  Impl(int w, int h, int rate, int bitrate) : width(w), height(h), fps(rate) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized = SUCCEEDED(hr);
    if (hr != RPC_E_CHANGED_MODE) check(hr, "CoInitializeEx");
    check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
    mfInitialized = true;
    check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&transform)), "Create H.264 encoder");
    check(transform.As(&api), "H.264 ICodecAPI");
    VARIANT low; VariantInit(&low); low.vt = VT_BOOL; low.boolVal = VARIANT_TRUE;
    api->SetValue(&CODECAPI_AVLowLatencyMode, &low);
    setU32(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    setU32(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    setU32(api.Get(), CODECAPI_AVEncMPVGOPSize, rate * 2);
    ComPtr<IMFMediaType> output;
    check(MFCreateMediaType(&output), "Create output type");
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, rate, 1);
    MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    check(transform->SetOutputType(0, output.Get(), 0), "Set H.264 output type");
    ComPtr<IMFMediaType> input;
    check(MFCreateMediaType(&input), "Create input type");
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, rate, 1);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    check(transform->SetInputType(0, input.Get(), 0), "Set NV12 input type");
    UINT32 headerSize = 0;
    if (SUCCEEDED(output->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerSize))) {
      header.resize(headerSize);
      output->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), headerSize, nullptr);
    }
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin encoder stream");
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start encoder stream");
  }
  ~Impl() {
    if (transform) transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    api.Reset(); transform.Reset();
    if (mfInitialized) MFShutdown();
    if (comInitialized) CoUninitialize();
  }
};
Encoder::Encoder(int w, int h, int fps, int bitrate) : impl(std::make_unique<Impl>(w,h,fps,bitrate)) {}
Encoder::~Encoder() = default;
void Encoder::setBitrate(int bitrate) { setU32(impl->api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate); }
std::vector<EncodedFrame> Encoder::encode(const Frame& frame, bool keyframe) {
  if (frame.width != impl->width || frame.height != impl->height) throw std::runtime_error("Encoder/frame size mismatch");
  if (keyframe) setU32(impl->api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
  auto pixels = nv12(frame);
  ComPtr<IMFMediaBuffer> buffer;
  check(MFCreateMemoryBuffer(static_cast<DWORD>(pixels.size()), &buffer), "Create input buffer");
  BYTE* bytes = nullptr;
  check(buffer->Lock(&bytes, nullptr, nullptr), "Lock input buffer");
  memcpy(bytes, pixels.data(), pixels.size());
  buffer->Unlock(); buffer->SetCurrentLength(static_cast<DWORD>(pixels.size()));
  ComPtr<IMFSample> input;
  MFCreateSample(&input); input->AddBuffer(buffer.Get());
  input->SetSampleTime(static_cast<LONGLONG>(frame.timestampUs * 10));
  input->SetSampleDuration(10000000 / impl->fps);
  check(impl->transform->ProcessInput(0, input.Get(), 0), "Encode input");
  std::vector<EncodedFrame> frames;
  for (;;) {
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    check(impl->transform->GetOutputStreamInfo(0, &streamInfo), "Encoder output info");
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> out;
    MFCreateSample(&sample);
    MFCreateMemoryBuffer(std::max<DWORD>(streamInfo.cbSize, impl->width * impl->height * 2), &out);
    sample->AddBuffer(out.Get());
    MFT_OUTPUT_DATA_BUFFER data{}; data.pSample = sample.Get();
    DWORD status = 0;
    HRESULT hr = impl->transform->ProcessOutput(0, 1, &data, &status);
    if (data.pEvents) data.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
    check(hr, "Read encoded output");
    ComPtr<IMFMediaBuffer> contiguous;
    check(sample->ConvertToContiguousBuffer(&contiguous), "Get H.264 data");
    DWORD length = 0;
    check(contiguous->Lock(&bytes, nullptr, &length), "Lock H.264 buffer");
    EncodedFrame encoded;
    LONGLONG time = static_cast<LONGLONG>(frame.timestampUs * 10);
    sample->GetSampleTime(&time); encoded.timestampUs = time / 10;
    UINT32 clean = 0; sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    // MF emits Annex B. Repeating SPS/PPS on keyframes supports reconnect and resizing.
    if (clean && !impl->header.empty()) encoded.annexB = impl->header;
    encoded.annexB.insert(encoded.annexB.end(), bytes, bytes + length);
    contiguous->Unlock();
    frames.push_back(std::move(encoded));
  }
  return frames;
}
}
