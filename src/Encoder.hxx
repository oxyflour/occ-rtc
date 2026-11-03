#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace drawrtc {
struct Frame {
  int width = 0, height = 0;
  uint64_t timestampUs = 0;
  std::vector<uint8_t> bgra;
};
struct EncodedFrame {
  uint64_t timestampUs = 0;
  std::vector<uint8_t> annexB;
};
class Encoder {
public:
  Encoder(int width, int height, int fps, int bitrate);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  std::vector<EncodedFrame> encode(const Frame& frame, bool keyframe);
  void setBitrate(int bitrate);
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
