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
#include "lyra/lyra_decoder.h"
#include "lyra/lyra_config.h"

// Platform-specific socket headers
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

using chromemedia::codec::LyraDecoder;

// --- 配置常量 ---
constexpr int kSampleRate = 16000;
constexpr int kNumChannels = 1;
constexpr int kFramesPerBuffer = kSampleRate / 50; // 320 frames for 20ms

// --- 线程安全的数据队列 ---
std::queue<std::vector<uint8_t>> g_jitter_buffer; // Encoded packets
std::mutex g_jitter_mutex;
std::queue<int16_t> g_pcm_buffer; // Decoded samples
std::mutex g_pcm_mutex;
bool g_finished = false;
int g_socket_handle = -1;

// --- 网络线程函数 ---
// 接收 UDP 包并放入抖动缓冲器
void network_thread_func(int port) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n"; return;
    }
#endif

    g_socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket_handle < 0) {
        std::cerr << "Could not create socket.\n"; return;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    if (bind(g_socket_handle, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        std::cerr << "Bind failed.\n"; return;
    }

    std::cout << "Network thread started. Listening on port " << port << std::endl;
    while(!g_finished) {
        std::vector<uint8_t> buffer(256);
        int bytes_received = recvfrom(g_socket_handle, reinterpret_cast<char*>(buffer.data()),
                                    buffer.size(), 0, nullptr, nullptr);
        if(bytes_received > 0) {
            buffer.resize(bytes_received);
            std::lock_guard<std::mutex> lock(g_jitter_mutex);
            g_jitter_buffer.push(buffer);
        }
    }
#ifdef _WIN32
    closesocket(g_socket_handle);
    WSACleanup();
#else
    close(g_socket_handle);
#endif
    std::cout << "Network thread finished.\n";
}

// --- 解码线程函数 ---
// 从抖动缓冲器取出数据，解码后放入 PCM 缓冲
void decoder_thread_func(LyraDecoder* decoder) {
    while(!g_finished) {
        std::vector<uint8_t> encoded_packet;
        {
            std::lock_guard<std::mutex> lock(g_jitter_mutex);
            if(!g_jitter_buffer.empty()) {
                encoded_packet = g_jitter_buffer.front();
                g_jitter_buffer.pop();
            }
        }
        if(!encoded_packet.empty()) {
            if(decoder->SetEncodedPacket(encoded_packet)) {
                auto decoded = decoder->DecodeSamples(kFramesPerBuffer);
                if(decoded.has_value()) {
                    std::lock_guard<std::mutex> lock(g_pcm_mutex);
                    for(int16_t sample : decoded.value()) {
                        g_pcm_buffer.push(sample);
                    }
                }
            }
        }else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::cout << "Decoder thread finished.\n";
}

// --- PortAudio 回调函数 ---
// 从 PCM 缓冲中取出数据播放
static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long frameCount,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    auto* out = reinterpret_cast<int16_t*>(outputBuffer);
    std::lock_guard<std::mutex> lock(g_pcm_mutex);
    
    for (int i = 0; i < frameCount; ++i) {
        if (!g_pcm_buffer.empty()) {
            out[i] = g_pcm_buffer.front();
            g_pcm_buffer.pop();
        } else {
            // Underrun: Play silence if no data is available
            out[i] = 0;
        }
    }
    return paContinue;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <listen_port>\n";
        return 1;
    }
    const int port = std::stoi(argv[1]);
    const std::string model_path = "lyra/model_coeffs";

    // 1. 初始化Lyra解码器
    auto decoder = LyraDecoder::Create(kSampleRate, kNumChannels, model_path);
    if (!decoder) {
        std::cerr << "Failed to create Lyra decoder.\n"; return 1;
    }

    // 2. 启动网络和解码线程
    std::thread network_thread(network_thread_func, port);
    std::thread decoder_thread(decoder_thread_func, decoder.get());

        // 3. 初始化 PortAudio (仅输出)
    Pa_Initialize();
    PaStream* stream;
    PaStreamParameters outputParameters{};
    outputParameters.device = Pa_GetDefaultOutputDevice();
    outputParameters.channelCount = kNumChannels;
    outputParameters.sampleFormat = paInt16;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
    
    Pa_OpenStream(&stream, nullptr /* no input */, &outputParameters,
                  kSampleRate, kFramesPerBuffer, paClipOff, audioCallback, nullptr);

    Pa_StartStream(stream);
    std::cout << "Playback started... Press Enter to stop.\n";
    std::cin.get();
    
    // 4. 清理
    g_finished = true;
    
    // Unblock the network thread's recvfrom call by closing the socket
    // This must be done before joining the thread.
#ifdef _WIN32
    closesocket(g_socket_handle);
#else
    shutdown(g_socket_handle, SHUT_RDWR); // A more forceful way to unblock
#endif

    network_thread.join();
    decoder_thread.join();
    
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    
    std::cout << "Receiver finished.\n";
    return 0;

}
