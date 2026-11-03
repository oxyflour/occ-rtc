#include "Session.hxx"
#include <Draw_Interpretor.hxx>
#include <Draw_PluginMacro.hxx>
#include <Standard_Failure.hxx>
#include <stdexcept>
#include <string>

namespace {
int number(const char* text) {
  size_t end = 0;
  int n = std::stoi(text, &end);
  if (end != std::string(text).size()) throw std::runtime_error("Invalid integer");
  return n;
}
int command(Draw_Interpretor& di, int argc, const char** argv) {
  try {
    auto& session = drawrtc::Session::instance();
    const std::string name = argv[0];
    if (name == "rtcinit") {
      int width = 1280, height = 720;
      for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::runtime_error("Expected option value");
        const std::string option = argv[i];
        if (option == "-width") width = number(argv[i+1]);
        else if (option == "-height") height = number(argv[i+1]);
        else if (option == "-hidden" && number(argv[i+1]) == 1) {}
        else throw std::runtime_error("Unknown rtcinit option (only -hidden 1 is supported)");
      }
      session.init(width, height);
      di << session.status().c_str();
    } else if (name == "rtcstart") {
      drawrtc::Options options;
      for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::runtime_error("Expected option value");
        const std::string option = argv[i];
        if (option == "-bind") options.bind = argv[i+1];
        else if (option == "-port") options.port = number(argv[i+1]);
        else if (option == "-fps") options.fps = number(argv[i+1]);
        else if (option == "-bitrate") options.bitrate = number(argv[i+1]);
        else if (option == "-token") options.token = argv[i+1];
        else throw std::runtime_error("Unknown rtcstart option");
      }
      di << session.start(std::move(options)).c_str();
    } else if (name == "rtcstatus" && argc == 1) di << session.status().c_str();
    else if (name == "rtcstop" && argc == 1) { session.stop(); di << "stopped"; }
    else throw std::runtime_error("Invalid arguments; use help rtcinit / rtcstart / rtcstatus / rtcstop");
    return 0;
  } catch (const Standard_Failure& e) {
    di << "OCCT: " << e.GetMessageString();
  } catch (const std::exception& e) { di << e.what(); }
  return 1;
}
}
class DrawRTC {
public:
  static void Factory(Draw_Interpretor& di) {
    drawrtc::Session::instance().attach(di);
    di.Add("rtcinit", "rtcinit [-hidden 1] [-width 1280] [-height 720]: create hidden streaming view", __FILE__, command, "DRAWRTC");
    di.Add("rtcstart", "rtcstart [-bind 127.0.0.1] [-port 8080] [-fps 30] [-bitrate 4000000] [-token TOKEN]: start HTTP and WebRTC; signaling uses port+1", __FILE__, command, "DRAWRTC");
    di.Add("rtcstatus", "rtcstatus: JSON status and streaming statistics", __FILE__, command, "DRAWRTC");
    di.Add("rtcstop", "rtcstop: stop streaming and release network/encoder resources; keep DRAW view", __FILE__, command, "DRAWRTC");
  }
};
DPLUGIN(DrawRTC)
