#include <iostream>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <thread>
#include <queue>
#include <mutex>
#include <chrono>

#include "portaudio.h"
#include "absl/types/span.h"
#include "lyra/lyra_encoder.h"
#include "lyra/lyra_config.h"

// Platform-specific socket headers
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using chromemedia::codec::LyraEncoder;
using chromemedia::codec::GetBitrate;

constexpr int kSampleRate = 16000;
constexpr int kNumChannels = 1;
constexpr int kBitrate = 3200; // 3.2 kbps, Lyra V2's lowest bitrate
constexpr int kFramesPerBuffer = kSampleRate / 50; // 320 frames for 20ms

std::queue<std::vector<uint8_t>> g_encoded_packets_queue;
std::mutex g_queue_mutex;
bool g_finished = false;


static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long frameCount,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags, void* userData) {
  auto* encoder = reinterpret_cast<LyraEncoder*>(userData);
  const auto* in = reinterpret_cast<const int16_t*>(inputBuffer);

  std::optional<std::vector<uint8_t>> encoded = encoder->Encode(absl::MakeConstSpan(in, frameCount));
  if(encoded.has_value()) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_encoded_packets_queue.push(encoded.value());
  }
  return paContinue;
}

// 网络线程函数
// 从队列中取出数据包并通过UDP发送
void network_thread_func(const std::string& server_ip, int port) {
  #ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return;
    }
#endif
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Could not create socket.\n";
        return;
    }
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr);
    std::cout << "Network thread started. Sending to " << server_ip << ":" << port << std::endl;

    while(!g_finished) {
      std::vector<uint8_t> packet_to_send;
      {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if(!g_encoded_packets_queue.empty()) {
          packet_to_send = g_encoded_packets_queue.front();
          g_encoded_packets_queue.pop();
        }
      }

      if(!packet_to_send.empty()) {
        sendto(sock, reinterpret_cast<const char*>(packet_to_send.data()), packet_to_send.size(),
                             0, (const struct sockaddr*)&server_address, sizeof(server_address));
      } else {
        // 等待一会儿
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      
    }
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    std::cout << "Network thread finished.\n";
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
      std::cerr << "Usage: " << argv[0] << " <server_ip> <port>\n";
      return 1;
  }
  const std::string server_ip = argv[1];
  const int port = std::stoi(argv[2]);
  const std::string model_path = "lyra/model_coeffs";

  // 1. 初始化编码器
  auto encoder = LyraEncoder::Create(kSampleRate, kNumChannels, kBitrate, false, model_path);
  if(!encoder) {
    std::cerr << "Failed to create Lyra encoder.\n";
    return 1;
  }
  
  // 2. 启动网络线程
  std::thread network_thread(network_thread_func, server_ip, port);

  // 3. 初始化 PortAudio
  Pa_Initialize();
  PaStream* stream;
    PaStreamParameters inputParameters{};
    inputParameters.device = Pa_GetDefaultInputDevice();
    inputParameters.channelCount = kNumChannels;
    inputParameters.sampleFormat = paInt16;
    inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
    
    Pa_OpenStream(&stream, &inputParameters, nullptr /* no output */,
                  kSampleRate, kFramesPerBuffer, paClipOff, audioCallback, encoder.get());

    Pa_StartStream(stream);
    std::cout << "Recording started... Press Enter to stop.\n";
    std::cin.get();

    // 4. 清理
    g_finished = true;
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    network_thread.join();

    std::cout << "Sender finished.\n";
    return 0;
}
