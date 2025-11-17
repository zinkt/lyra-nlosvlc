#include <iostream>
#include <vector>
#include <optional>
#include <memory>
#include <string>
#include <algorithm> // For std::copy and std::fill

#include "portaudio.h"
#include "absl/types/span.h" // Needed for absl::MakeConstSpan

#include "lyra/lyra_encoder.h"
#include "lyra/lyra_decoder.h"
#include "lyra/lyra_config.h"

using chromemedia::codec::LyraDecoder;
using chromemedia::codec::LyraEncoder;
using chromemedia::codec::GetBitrate;

// --- 音频和 Lyra 配置常量 ---
// Lyra V1 支持 8, 16, 32, 48 kHz 采样率
// Lyra V2 支持 16, 32, 48 kHz 采样率
// 推荐使用 16kHz 以获得最佳性能
constexpr int kSampleRate = 16000; 
// Lyra 设计为单声道
constexpr int kNumChannels = 1;      
// 支持的比特率: 3200, 6000, 9200 bps
// GetBitrate() 函数需要一个量化比特数，这里我们直接使用目标比特率
constexpr int kBitrate = 3200; 
// Lyra V2 的帧大小是 20ms (对于 16kHz 就是 320 采样点)
constexpr int kFramesPerBuffer = kSampleRate / 50; // 16000 / 50 = 320

// --- 全局编码器和解码器 ---
// 在回调函数中需要访问它们，因此设为全局变量（在更复杂的应用中应避免）
std::unique_ptr<LyraEncoder> encoder;
std::unique_ptr<LyraDecoder> decoder;

// --- PortAudio 错误检查宏 ---
// 简化 PortAudio 函数的错误检查
#define PA_CHECK(err) \
    do { \
        if (err != paNoError) { \
            std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl; \
            goto error; \
        } \
    } while (0)

// --- PortAudio 回调函数 ---
// 这是核心部分，PortAudio 会在需要音频数据时自动调用这个函数
static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long frameCount,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    const int16_t* in = reinterpret_cast<const int16_t*>(inputBuffer);
    int16_t* out = reinterpret_cast<int16_t*>(outputBuffer);

    // 1. 编码
    std::optional<std::vector<uint8_t>> encoded = encoder->Encode(
        absl::MakeConstSpan(in, frameCount));

    if (encoded.has_value()) {
        // 2. 解码
        if (decoder->SetEncodedPacket(encoded.value())) {
            std::optional<std::vector<int16_t>> decoded =
                decoder->DecodeSamples(frameCount);

            if (decoded.has_value()) {
                // 3. 输出 (修正之处)
                // 使用点运算符 . 而不是箭头运算符 ->
                std::copy(decoded.value().begin(), decoded.value().end(), out);
            } else {
                std::cerr << "解码失败！" << std::endl;
                std::fill(out, out + frameCount * kNumChannels, 0); 
            }
        } else {
             std::cerr << "设置解码器数据包失败！" << std::endl;
             std::fill(out, out + frameCount * kNumChannels, 0); 
        }
    } else {
        std::cerr << "编码失败！" << std::endl;
        std::fill(out, out + frameCount * kNumChannels, 0); 
    }

    return paContinue;
}
int main() {
    // 此路径必须指向包含 Lyra 模型权重文件 (例如 soundstream_encoder.tflite) 的目录。
    std::string model_path = "lyra/model_coeffs"; 

    std::cout << "正在初始化 Lyra 编解码器..." << std::endl;
    std::cout << "模型路径: " << model_path << std::endl;

    // 创建 Lyra 编码器和解码器
    encoder = LyraEncoder::Create(kSampleRate, kNumChannels, kBitrate, 
                                  false, model_path);
    decoder = LyraDecoder::Create(kSampleRate, kNumChannels, model_path);

    if (!encoder || !decoder) {
        std::cerr << "创建 Lyra 编解码器失败。请检查模型路径是否正确。" << std::endl;
        return 1;
    }
    std::cout << "Lyra 编解码器初始化成功。" << std::endl;

    PaStream* stream;
    PaError err;

    std::cout << "正在初始化 PortAudio..." << std::endl;
    PA_CHECK(Pa_Initialize());

    // 配置输入设备（麦克风）
    PaStreamParameters inputParameters;
    inputParameters.device = Pa_GetDefaultInputDevice();
    if (inputParameters.device == paNoDevice) {
        std::cerr << "错误: 未找到默认输入设备。" << std::endl;
        goto error;
    }
    inputParameters.channelCount = kNumChannels;
    inputParameters.sampleFormat = paInt16;
    inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = nullptr;

    // 配置输出设备（扬声器）
    PaStreamParameters outputParameters;
    outputParameters.device = Pa_GetDefaultOutputDevice();
     if (outputParameters.device == paNoDevice) {
        std::cerr << "错误: 未找到默认输出设备。" << std::endl;
        goto error;
    }
    outputParameters.channelCount = kNumChannels;
    outputParameters.sampleFormat = paInt16;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = nullptr;

    std::cout << "正在打开音频流..." << std::endl;
    // 打开一个双向音频流
    PA_CHECK(Pa_OpenStream(
        &stream,
        &inputParameters,
        &outputParameters,
        kSampleRate,
        kFramesPerBuffer,
        paClipOff,      // 不裁剪超出范围的样本
        audioCallback,
        nullptr));      // 没有用户数据传递给回调函数

    std::cout << "正在启动音频流... 请对着麦克风说话。" << std::endl;
    PA_CHECK(Pa_StartStream(stream));

    std::cout << "\n音频流已激活。你听到的声音是经过 Lyra 编解码器处理后的声音。\n" << std::endl;
    std::cout << "按回车键停止..." << std::endl;
    std::cin.get(); // 等待用户输入

    std::cout << "正在停止音频流..." << std::endl;
    PA_CHECK(Pa_StopStream(stream));
    PA_CHECK(Pa_CloseStream(stream));

    Pa_Terminate();
    std::cout << "程序结束。" << std::endl;
    return 0;

error:
    Pa_Terminate();
    std::cerr << "\n程序因错误而终止。" << std::endl;
    return 1;
}