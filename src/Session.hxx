#pragma once
#include <memory>
#include <string>
class Draw_Interpretor;
namespace drawrtc {
struct Options {
  std::string bind = "127.0.0.1";
  std::string token;
  int port = 8080;
  int fps = 30;
  int bitrate = 4000000;
};
class Session {
public:
  static Session& instance();
  void attach(Draw_Interpretor& interpreter);
  void init(int width, int height);
  std::string start(Options options);
  void stop();
  std::string status() const;
private:
  Session();
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
