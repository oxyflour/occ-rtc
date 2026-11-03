#include "Session.hxx"
#include "Encoder.hxx"
#include <windows.h>
#include <tcl.h>
#include <Draw_Interpretor.hxx>
#include <ViewerTest.hxx>
#include <ViewerTest_DoubleMapOfInteractiveAndName.hxx>
#include <V3d_View.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_CView.hxx>
#include <AIS_InteractiveContext.hxx>
#include <Image_PixMap.hxx>
#include <WNT_Window.hxx>
#include <Standard_Failure.hxx>
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

Standard_IMPORT ViewerTest_DoubleMapOfInteractiveAndName& GetMapOfAIS();
namespace drawrtc {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
std::string randomToken() {
  std::random_device random;
  const char* hex = "0123456789abcdef";
  std::string token;
  for (int i = 0; i < 32; ++i) { unsigned n = random(); token += hex[n & 15]; }
  return token;
}
std::pair<int,int> dimensions(int w, int h) {
  if (w < 1 || h < 1 || w > 16384 || h > 16384) throw std::runtime_error("Size must be in 1..16384");
  double scale = std::min({1.0, 1920.0 / w, 1080.0 / h});
  return {std::max(2, static_cast<int>(w * scale) & ~1), std::max(2, static_cast<int>(h * scale) & ~1)};
}
template<class Channel> void send(const std::shared_ptr<Channel>& channel, const json& message) {
  try { if (channel && channel->isOpen()) channel->send(message.dump(-1, ' ', false, json::error_handler_t::replace)); }
  catch (const std::exception&) {}
}
std::filesystem::path moduleDirectory() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&moduleDirectory), &module);
  wchar_t path[32768];
  DWORD length = GetModuleFileNameW(module, path, 32768);
  if (!length || length == 32768) throw std::runtime_error("Cannot locate TKDrawRTC.dll");
  return std::filesystem::path(std::wstring(path, length)).parent_path();
}
struct Capture {
  std::string output;
  Tcl_Channel channel = nullptr, previous = nullptr;
  Tcl_Interp* interp = nullptr;
  int standard;
  std::string encoding, buffering, translation;
  static int write(ClientData data, const char* bytes, int size, int*) {
    auto& text = static_cast<Capture*>(data)->output;
    // Output is bounded independently of command execution.
    if (text.size() < 1024 * 1024) text.append(bytes, std::min<size_t>(size, 1024 * 1024 - text.size()));
    return size;
  }
  static const Tcl_ChannelType* type() {
    static Tcl_ChannelType value = [] {
      Tcl_ChannelType t{}; t.typeName = "drawrtc-capture"; t.version = TCL_CHANNEL_VERSION_5;
      t.closeProc = [](ClientData, Tcl_Interp*) { return 0; };
      t.outputProc = write;
      t.watchProc = [](ClientData, int) {};
      t.getHandleProc = [](ClientData, int, ClientData*) { return TCL_ERROR; };
      return t;
    }();
    return &value;
  }
  Capture(Tcl_Interp* ip, int which) : interp(ip), standard(which) {
    previous = Tcl_GetStdChannel(which);
    if (!previous) throw std::runtime_error("Tcl standard channel is unavailable");
    Tcl_Flush(previous); Tcl_RegisterChannel(nullptr, previous);
    auto option=[&](const char* name) {
      Tcl_DString value; Tcl_DStringInit(&value);
      Tcl_GetChannelOption(interp,previous,name,&value);
      std::string result=Tcl_DStringValue(&value); Tcl_DStringFree(&value); return result;
    };
    encoding=option("-encoding"); buffering=option("-buffering"); translation=option("-translation");
    channel=Tcl_StackChannel(interp,type(),this,TCL_WRITABLE,previous);
    if (!channel) { Tcl_UnregisterChannel(nullptr,previous); throw std::runtime_error("Cannot stack Tcl output capture"); }
    Tcl_SetChannelOption(interp, channel, "-encoding", "utf-8");
    Tcl_SetChannelOption(interp, channel, "-translation", "lf");
    Tcl_SetChannelOption(interp, channel, "-buffering", "none");
  }
  ~Capture() {
    Tcl_Flush(channel);
    Tcl_UnstackChannel(interp,channel);
    Tcl_SetChannelOption(interp,previous,"-encoding",encoding.c_str());
    Tcl_SetChannelOption(interp,previous,"-translation",translation.c_str());
    Tcl_SetChannelOption(interp,previous,"-buffering",buffering.c_str());
    Tcl_UnregisterChannel(nullptr,previous);
  }
};
struct Peer {
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track> video;
  std::shared_ptr<rtc::DataChannel> control;
  std::shared_ptr<rtc::WebSocket> socket;
};
struct Request { std::weak_ptr<Peer> peer; json message; };
struct Runtime : std::enable_shared_from_this<Runtime> {
  Options options;
  std::atomic<bool> stopping{false}, keyframe{true};
  std::atomic<int> requestedBitrate{0};
  std::atomic<uint64_t> captured{0}, encoded{0}, dropped{0}, bytes{0}, commands{0};
  std::mutex peerMutex, queueMutex, frameMutex, statusMutex, socketMutex, connectionMutex;
  std::vector<std::shared_ptr<rtc::WebSocket>> sockets;
  std::shared_ptr<Peer> peer;
  std::deque<Request> requests;
  std::optional<Frame> latestFrame;
  std::condition_variable frameReady;
  std::thread encoderThread, httpThread;
  httplib::Server http;
  std::unique_ptr<rtc::WebSocketServer> signaling;
  std::string lastError;
  json cachedStatus;
  const Clock::time_point started = Clock::now();
  explicit Runtime(Options o) : options(std::move(o)) {}
  std::shared_ptr<Peer> getPeer() { std::lock_guard lock(peerMutex); return peer; }
  void error(const std::string& message) {
    { std::lock_guard lock(statusMutex); lastError = message; }
    if (auto p = getPeer()) send(p->control, json{{"type","error"},{"message",message}});
  }
  json stats() {
    std::lock_guard lock(statusMutex);
    json value = cachedStatus;
    value["running"] = !stopping;
    value["captured"] = captured.load(); value["encoded"] = encoded.load();
    value["dropped"] = dropped.load(); value["bytes"] = bytes.load();
    value["commands"] = commands.load(); value["lastError"] = lastError;
    value["uptimeSeconds"] = std::chrono::duration<double>(Clock::now() - started).count();
    value["encoder"] = "Windows Media Foundation H.264 (software)";
    return value;
  }
  void enqueue(std::weak_ptr<Peer> source, const std::string& text) {
    try {
      if (text.size() > 65536) throw std::runtime_error("Control message exceeds 64 KiB");
      json message = json::parse(text);
      if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) throw std::runtime_error("Expected object with type");
      if (stopping) return;
      std::lock_guard lock(queueMutex);
      if (requests.size() >= 256) throw std::runtime_error("Command queue full");
      requests.push_back({source, std::move(message)});
    } catch (const std::exception& e) {
      if (auto p = source.lock()) send(p->control, json{{"type","error"},{"message",e.what()}});
    }
  }
  void connect(const std::shared_ptr<rtc::WebSocket>& socket) {
    std::lock_guard connectionLock(connectionMutex);
    if (stopping) { socket->close(); return; }
    if (socket->path().value_or("") != "/?token=" + options.token) { socket->close(); return; }
    auto previous = getPeer();
    if (previous && previous->socket->isOpen()) {
      send(socket, json{{"type","error"},{"message","A browser is already connected"}}); socket->close(); return;
    }
    if (previous) previous->pc->close();
    auto p = std::make_shared<Peer>(); p->socket = socket;
    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    if (options.bind != "0.0.0.0" && options.bind != "::") config.bindAddress = options.bind;
    config.portRangeBegin = 50000; config.portRangeEnd = 50100;
    config.maxMessageSize = 65536;
    p->pc = std::make_shared<rtc::PeerConnection>(config);
    std::weak_ptr<Runtime> weak = shared_from_this();
    std::weak_ptr<Peer> wp = p;
    p->pc->onGatheringStateChange([wp](rtc::PeerConnection::GatheringState state) {
      if (state != rtc::PeerConnection::GatheringState::Complete) return;
      if (auto current = wp.lock()) {
        auto desc = current->pc->localDescription();
        if (desc) send(current->socket, json{{"type",desc->typeString()},{"sdp",std::string(*desc)}});
      }
    });
    p->pc->onStateChange([weak,wp](rtc::PeerConnection::State state) {
      if (auto runtime = weak.lock()) {
        if (auto current = wp.lock()) send(current->socket, json{{"type","state"},{"state",static_cast<int>(state)}});
        if (state == rtc::PeerConnection::State::Connected) runtime->keyframe = true;
      }
    });
    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(102); media.addSSRC(42, "drawrtc", "drawrtc", "view");
    p->video = p->pc->addTrack(media);
    auto configRtp = std::make_shared<rtc::RtpPacketizationConfig>(42u, "drawrtc", uint8_t(102), rtc::H264RtpPacketizer::ClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, configRtp);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(configRtp));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak] { if (auto r = weak.lock()) r->keyframe = true; }));
    packetizer->addToChain(std::make_shared<rtc::RembHandler>([weak](unsigned int b) {
      if (auto r = weak.lock()) r->requestedBitrate = std::clamp(static_cast<int>(std::min(b,100000000u)), 200000, r->options.bitrate);
    }));
    p->video->setMediaHandler(packetizer);
    p->video->onOpen([weak] { if (auto r = weak.lock()) r->keyframe = true; });
    p->control = p->pc->createDataChannel("control");
    p->control->onMessage([weak,wp](rtc::message_variant data) {
      if (auto text = std::get_if<std::string>(&data)) if (auto r = weak.lock()) r->enqueue(wp, *text);
    });
    p->control->onOpen([weak,wp] {
      if (auto r = weak.lock()) if (auto current = wp.lock()) send(current->control, json{{"type","status"},{"data",r->stats()}});
    });
    socket->onMessage([weak,wp](rtc::message_variant data) {
      try {
        auto text = std::get_if<std::string>(&data);
        auto current = wp.lock(); if (!text || !current) return;
        auto msg = json::parse(*text);
        if (msg.value("type","") == "hello") current->pc->setLocalDescription();
        else if (msg.value("type","") == "answer") current->pc->setRemoteDescription(rtc::Description(msg.at("sdp").get<std::string>(), "answer"));
      } catch (const std::exception& e) { if (auto r = weak.lock()) r->error(e.what()); }
    });
    socket->onClosed([wp] { if (auto current = wp.lock()) current->pc->close(); });
    // Publish only once video/control are initialized: the Tcl and encoder threads
    // can read this pointer immediately after publication.
    { std::lock_guard lock(peerMutex); peer = p; }
    send(socket, json{{"type","ready"}});
  }
  void encodeLoop() {
    try {
      std::unique_ptr<Encoder> encoder;
      int w = 0, h = 0;
      while (!stopping) {
        Frame frame;
        {
          std::unique_lock lock(frameMutex);
          frameReady.wait(lock, [this] { return stopping || latestFrame.has_value(); });
          if (stopping) break;
          frame = std::move(*latestFrame); latestFrame.reset();
        }
        if (!encoder || w != frame.width || h != frame.height) {
          encoder.reset();
          encoder = std::make_unique<Encoder>(frame.width, frame.height, options.fps, options.bitrate);
          w = frame.width; h = frame.height; keyframe = true;
        }
        if (int bitrate = requestedBitrate.exchange(0)) encoder->setBitrate(bitrate);
        auto frames = encoder->encode(frame, keyframe.exchange(false));
        auto p = getPeer();
        for (const auto& encodedFrame : frames) {
          if (!p || !p->video->isOpen() || stopping) continue;
          try {
            if (p->video->bufferedAmount()>1024*1024) { ++dropped; keyframe=true; continue; }
            p->video->sendFrame(reinterpret_cast<const rtc::byte*>(encodedFrame.annexB.data()), encodedFrame.annexB.size(),
              rtc::FrameInfo(std::chrono::duration<double>(encodedFrame.timestampUs / 1000000.0)));
            ++encoded; bytes += encodedFrame.annexB.size();
          } catch (const std::exception&) {
            // A track may close between isOpen() and sendFrame() during reconnect.
            ++dropped; keyframe=true;
          }
        }
      }
    } catch (const std::exception& e) { error(e.what()); }
  }
  void start() {
    auto weak = weak_from_this();
    const auto htmlPath = moduleDirectory() / "web" / "index.html";
    std::ifstream input(htmlPath, std::ios::binary);
    if (!input) throw std::runtime_error("web/index.html is missing beside TKDrawRTC.dll");
    std::string html((std::istreambuf_iterator<char>(input)), {});
    http.set_payload_max_length(65536);
    http.Get("/", [html](const httplib::Request&, httplib::Response& response) {
      response.set_header("Cache-Control", "no-store");
      response.set_header("Referrer-Policy", "no-referrer");
      response.set_content(html, "text/html; charset=utf-8");
    });
    http.Get("/api/status", [weak](const httplib::Request& request, httplib::Response& response) {
      if (auto r = weak.lock()) {
        if (request.get_header_value("Authorization") != "Bearer " + r->options.token) { response.status = 401; return; }
        response.set_content(r->stats().dump(), "application/json");
      }
    });
    if (!http.bind_to_port(options.bind, options.port)) throw std::runtime_error("Cannot bind HTTP port");
    rtc::WebSocketServerConfiguration ws;
    ws.port = static_cast<uint16_t>(options.port + 1); ws.bindAddress = options.bind;
    ws.maxMessageSize = 65536;
    signaling = std::make_unique<rtc::WebSocketServer>(ws);
    signaling->onClient([weak](std::shared_ptr<rtc::WebSocket> socket) {
      auto r=weak.lock(); if (!r || r->stopping) { socket->close(); return; }
      {
        std::lock_guard lock(r->socketMutex);
        auto& sockets=r->sockets;
        sockets.erase(std::remove_if(sockets.begin(),sockets.end(),[](const auto& s) { return s->isClosed(); }),sockets.end());
        if (sockets.size()>=8) { socket->close(); return; }
        sockets.push_back(socket);
      }
      std::weak_ptr<rtc::WebSocket> ws=socket;
      // onClient runs before the HTTP upgrade; path() is only populated onOpen.
      socket->onOpen([weak,ws] {
        auto socket=ws.lock(); if (!socket) return;
        try { if (auto r=weak.lock()) r->connect(socket); else socket->close(); }
        catch (const std::exception& e) { send(socket,json{{"type","error"},{"message",e.what()}}); socket->close(); }
      });
    });
    encoderThread = std::thread([this] { encodeLoop(); });
    httpThread = std::thread([this] { http.listen_after_bind(); });
  }
  void stop() {
    if (stopping.exchange(true)) return;
    http.stop();
    if (signaling) signaling->stop();
    // Wait for any connection initialization to finish before draining workers.
    { std::lock_guard connectionLock(connectionMutex); }
    frameReady.notify_all();
    if (encoderThread.joinable()) encoderThread.join();
    if (httpThread.joinable()) httpThread.join();
    auto p = getPeer();
    if (p) { p->socket->close(); p->pc->close(); }
    std::vector<std::shared_ptr<rtc::WebSocket>> pending;
    { std::lock_guard lock(socketMutex); pending.swap(sockets); }
    for (auto& socket:pending) socket->close();
    { std::lock_guard lock(peerMutex); peer.reset(); }
    { std::lock_guard lock(queueMutex); requests.clear(); }
    { std::lock_guard lock(frameMutex); latestFrame.reset(); }
    signaling.reset();
  }
  ~Runtime() { stop(); }
};
}
struct Session::Impl {
  Draw_Interpretor* di = nullptr;
  Tcl_TimerToken timer = nullptr;
  Handle(V3d_View) view;
  int width = 1280, height = 720;
  int lastX = 0, lastY = 0;
  Clock::time_point lastStatus = Clock::now();
  std::shared_ptr<Runtime> runtime;
  json viewState() const {
    const bool hasView=!view.IsNull() && !view->View()->IsRemoved();
    json state{{"width",width},{"height",height},{"hidden",true},{"initialized",hasView}};
    if (hasView) {
      auto camera = view->Camera(); auto eye = camera->Eye(); auto at = camera->Center(); auto up = camera->Up();
      state["camera"] = {{"eye",{eye.X(),eye.Y(),eye.Z()}},{"center",{at.X(),at.Y(),at.Z()}},{"up",{up.X(),up.Y(),up.Z()}},{"scale",camera->Scale()}};
    }
    return state;
  }
  void resize(int w, int h) {
    auto size = dimensions(w,h); width = size.first; height = size.second;
    if (view.IsNull()) return;
    auto window = Handle(WNT_Window)::DownCast(view->Window());
    if (window.IsNull()) throw std::runtime_error("Expected a Windows DRAW view");
    window->SetPos(0, 0, width, height);
    view->MustBeResized(); view->Camera()->SetAspect(static_cast<double>(width)/height);
    view->Invalidate();
    if (runtime) runtime->keyframe = true;
  }
  void command(const std::shared_ptr<Peer>& p, const json& message) {
    const auto script = message.at("script").get<std::string>();
    if (script.size() > 60000) throw std::runtime_error("Tcl script exceeds 60000 bytes");
    json response{{"type","result"},{"id",message.value("id",json(nullptr))}};
    Tcl_Interp* interp = di->Interp();
    Tcl_InterpState state = Tcl_SaveInterpState(interp, TCL_OK);
    {
      Capture standardOutput(interp,TCL_STDOUT), standardError(interp,TCL_STDERR);
      const int code = Tcl_EvalEx(interp, script.data(), static_cast<int>(script.size()), TCL_EVAL_GLOBAL);
      response["code"] = code;
      response["result"] = Tcl_GetStringResult(interp);
      response["stdout"] = standardOutput.output; response["stderr"] = standardError.output;
      if (code != TCL_OK) {
        const char* info = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        response["errorInfo"] = info ? info : "";
      }
    }
    Tcl_RestoreInterpState(interp,state);
    send(p->control,response);
  }
  void handle(const Request& request) {
    auto p = request.peer.lock(); if (!p) return;
    const auto& m = request.message;
    auto type = m.at("type").get<std::string>();
    if (type == "command") { command(p,m); return; }
    if (type == "ping") { send(p->control,json{{"type","pong"},{"id",m.value("id",json(nullptr))}}); return; }
    if (view.IsNull() || view->View()->IsRemoved()) throw std::runtime_error("Call rtcinit first");
    if (type == "resize") {
      resize(m.at("width").get<int>(),m.at("height").get<int>());
      send(p->control,json{{"type","resize"},{"width",width},{"height",height}});
    } else if (type == "fit") view->FitAll();
    else if (type == "wheel") {
      double delta = m.at("delta").get<double>();
      if (!std::isfinite(delta)) throw std::runtime_error("Invalid wheel delta");
      view->SetZoom(std::exp(-std::clamp(delta,-500.0,500.0)*0.002));
    } else if (type == "pointer") {
      double u = m.at("x").get<double>(), v = m.at("y").get<double>();
      if (!std::isfinite(u) || !std::isfinite(v)) throw std::runtime_error("Invalid pointer coordinate");
      int x = std::clamp(static_cast<int>(std::clamp(u,0.0,1.0) * width),0,width-1);
      int y = std::clamp(static_cast<int>(std::clamp(v,0.0,1.0) * height),0,height-1);
      auto action = m.at("action").get<std::string>();
      if (action == "down") { view->StartRotation(x,y); lastX=x; lastY=y; }
      else if (action == "rotate") { view->Rotation(x,y); lastX=x; lastY=y; }
      else if (action == "pan") { view->Pan(x-lastX,lastY-y); lastX=x; lastY=y; }
      else if (action == "hover" || action == "pick") {
        auto context = ViewerTest::GetAISContext();
        context->MoveTo(x,y,view,Standard_False);
        if (action == "pick") {
          context->SelectDetected(m.value("add",false) ? AIS_SelectionScheme_XOR : AIS_SelectionScheme_Replace);
          json selected = json::array();
          for (context->InitSelected(); context->MoreSelected(); context->NextSelected()) {
            auto object = context->SelectedInteractive();
            if (GetMapOfAIS().IsBound1(object)) selected.push_back(GetMapOfAIS().Find1(object).ToCString());
          }
          send(p->control,json{{"type","selection"},{"names",selected},{"count",context->NbSelected()}});
        }
      } else throw std::runtime_error("Unknown pointer action");
    } else throw std::runtime_error("Unknown control message type");
  }
  void tick() {
    auto r = runtime; if (!r || r->stopping) return;
    std::deque<Request> batch;
    { std::lock_guard lock(r->queueMutex); for (int i = 0; i < 32 && !r->requests.empty(); ++i) { batch.push_back(std::move(r->requests.front())); r->requests.pop_front(); } }
    for (const auto& request : batch) {
      if (r->stopping) break;
      try { handle(request); ++r->commands; }
      catch (const Standard_Failure& e) { if (auto p = request.peer.lock()) send(p->control,json{{"type","error"},{"id",request.message.value("id",json(nullptr))},{"message",e.GetMessageString()}}); }
      catch (const std::exception& e) { if (auto p = request.peer.lock()) send(p->control,json{{"type","error"},{"id",request.message.value("id",json(nullptr))},{"message",e.what()}}); }
    }
    if (r->stopping) return;
    { std::lock_guard lock(r->statusMutex); r->cachedStatus = viewState(); }
    auto p = r->getPeer();
    if (!view.IsNull() && !view->View()->IsRemoved() && p && p->video->isOpen()) {
      Image_PixMap image;
      if (!image.InitZero(Image_Format_BGRA, width, height) || !view->ToPixMap(image,width,height,Graphic3d_BT_RGBA)) throw std::runtime_error("Offscreen pixel read failed");
      Frame frame; frame.width=width; frame.height=height;
      frame.timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-r->started).count();
      frame.bgra.resize(static_cast<size_t>(width)*height*4);
      for (int row=0; row<height; ++row) memcpy(frame.bgra.data()+static_cast<size_t>(row)*width*4,image.Row(row),static_cast<size_t>(width)*4);
      { std::lock_guard lock(r->frameMutex); if (r->latestFrame) ++r->dropped; r->latestFrame = std::move(frame); }
      ++r->captured; r->frameReady.notify_one();
    }
    if (Clock::now()-lastStatus > std::chrono::seconds(1)) {
      lastStatus=Clock::now(); if (p) send(p->control,json{{"type","status"},{"data",r->stats()}});
    }
  }
  static void timerCallback(ClientData data) {
    auto self = static_cast<Impl*>(data); self->timer=nullptr;
    const auto started=Clock::now();
    auto r = self->runtime;
    try { self->tick(); }
    catch (const Standard_Failure& e) { if (r) r->error(e.GetMessageString()); }
    catch (const std::exception& e) { if (r) r->error(e.what()); }
    const auto elapsed=static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count());
    if (self->runtime && !self->runtime->stopping && !self->timer) self->timer=Tcl_CreateTimerHandler(std::max(1,1000/self->runtime->options.fps-elapsed),timerCallback,self);
  }
};
Session::Session() : impl(std::make_unique<Impl>()) {}
Session& Session::instance() { static Session* session = new Session; return *session; }
void Session::attach(Draw_Interpretor& di) {
  if (impl->di) return;
  impl->di=&di;
  rtc::InitLogger(rtc::LogLevel::Warning);
  Tcl_CreateExitHandler([](ClientData) { Session::instance().stop(); },nullptr);
}
void Session::init(int width,int height) {
  if (!impl->view.IsNull() && impl->view->View()->IsRemoved()) impl->view.Nullify();
  if (impl->view.IsNull()) {
    ViewerTest_VinitParams params;
    auto size=dimensions(width,height);
    params.ViewName="DrawRTC/Viewer/View"; params.Size.SetValues(size.first,size.second); params.IsVirtual=true;
    ViewerTest::ViewerInit(params);
    impl->view=ViewerTest::CurrentView();
    if (impl->view.IsNull()) throw std::runtime_error("Failed to create hidden DRAW view");
    impl->view->SetShadingModel(Graphic3d_TypeOfShadingModel_Phong);
    impl->view->SetBgGradientColors(Quantity_Color(0.075,0.09,0.12,Quantity_TOC_sRGB),
      Quantity_Color(0.025,0.032,0.045,Quantity_TOC_sRGB),Aspect_GFM_VER,Standard_False);
  } else ViewerTest::ActivateView(impl->view,Standard_False);
  impl->resize(width,height);
}
std::string Session::start(Options options) {
  if (impl->runtime) throw std::runtime_error("Streaming is already running; call rtcstop first");
  if (options.port<1024 || options.port>65534 || options.fps<1 || options.fps>60 || options.bitrate<200000 || options.bitrate>50000000)
    throw std::runtime_error("port:1024..65534, fps:1..60, bitrate:200000..50000000");
  if (options.token.empty()) options.token=randomToken();
  if (options.token.size()<16 || options.token.size()>128 || options.token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("Token must be 16..128 ASCII letters, digits, underscores or hyphens");
  if (impl->view.IsNull()) init(1280,720);
  auto r=std::make_shared<Runtime>(std::move(options));
  r->cachedStatus=impl->viewState();
  r->start(); impl->runtime=r;
  impl->timer=Tcl_CreateTimerHandler(1,Impl::timerCallback,impl.get());
  const auto host=r->options.bind=="0.0.0.0" ? "127.0.0.1" : r->options.bind;
  return "http://"+host+":"+std::to_string(r->options.port)+"/?token="+r->options.token;
}
void Session::stop() {
  if (impl->timer) { Tcl_DeleteTimerHandler(impl->timer); impl->timer=nullptr; }
  auto r=std::move(impl->runtime); if (r) r->stop();
}
std::string Session::status() const {
  if (impl->runtime) {
    auto status=impl->runtime->stats(); status.update(impl->viewState());
    return status.dump();
  }
  auto status=impl->viewState(); status["running"]=false; return status.dump();
}
}
