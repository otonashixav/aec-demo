#include <iostream>
#include <vector>
#include <csignal>
#include <atomic>
#include <portaudio.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

// Use std::atomic to prevent data races across standard execution and the handler
std::atomic<bool> shutdown_requested(false);

// The signal handler function signature must match: void(int)
void handle_sigint(int signal_num) {
    if (signal_num == SIGINT) {
        // Only perform lock-free atomic assignments or async-signal-safe actions here
        shutdown_requested.store(true); 
    }
}

// Audio parameters
const int SAMPLE_RATE = 16000;
const int FRAME_SIZE = 160;       // 10ms at 16kHz
const int FILTER_LENGTH = FRAME_SIZE * 10; // 100ms echo tail

int main() {
    // Register the custom handler for SIGINT
    std::signal(SIGINT, handle_sigint);
    std::cout << "Initializing PortAudio..." << std::endl;
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio init error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }
    std::cout << "PortAudio initialized successfully!" << std::endl;

    // Checking available devices
    int numDevices = Pa_GetDeviceCount();
    std::cout << "Number of audio devices: " << numDevices << std::endl;

    if (numDevices < 0) {
        std::cerr << "Error getting device count: " << Pa_GetErrorText(numDevices) << std::endl;
        Pa_Terminate();
        return 1;
    }

    // Open default input streams for mic and system (works if loopback device)
    PaStream *micStream, *refStream, *outStream;
    PaStreamParameters inParams, refParams, outParams;

    PaDeviceIndex inputDevice = Pa_GetDefaultInputDevice();
    PaDeviceIndex outputDevice = Pa_GetDefaultOutputDevice();
    
    std::cout << "Default input device: " << inputDevice << std::endl;
    std::cout << "Default output device: " << outputDevice << std::endl;

    if (inputDevice == paNoDevice) {
        std::cerr << "No default input device available!" << std::endl;
        Pa_Terminate();
        return 1;
    }

    if (outputDevice == paNoDevice) {
        std::cerr << "No default output device available!" << std::endl;
        Pa_Terminate();
        return 1;
    }

    inParams.device = inputDevice;
    inParams.channelCount = 1;
    inParams.sampleFormat = paInt16;
    inParams.suggestedLatency = Pa_GetDeviceInfo(inParams.device)->defaultLowInputLatency;
    inParams.hostApiSpecificStreamInfo = nullptr;

    // For system audio, select loopback or WASAPI loopback on Windows
    refParams = inParams;
    // TODO: user may need to select a specific loopback device for system audio

    outParams.device = outputDevice;
    outParams.channelCount = 1;
    outParams.sampleFormat = paInt16;
    outParams.suggestedLatency = Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    std::cout << "Opening audio streams..." << std::endl;
    
    err = Pa_OpenStream(&micStream, &inParams, nullptr, SAMPLE_RATE, FRAME_SIZE, paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "Error opening mic stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_Terminate();
        return 1;
    }
    
    err = Pa_OpenStream(&refStream, &refParams, nullptr, SAMPLE_RATE, FRAME_SIZE, paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "Error opening reference stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(micStream);
        Pa_Terminate();
        return 1;
    }
    
    err = Pa_OpenStream(&outStream, nullptr, &outParams, SAMPLE_RATE, FRAME_SIZE, paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "Error opening output stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(micStream);
        Pa_CloseStream(refStream);
        Pa_Terminate();
        return 1;
    }

    std::cout << "Starting audio streams..." << std::endl;
    
    err = Pa_StartStream(micStream);
    if (err != paNoError) {
        std::cerr << "Error starting mic stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(micStream);
        Pa_CloseStream(refStream);
        Pa_CloseStream(outStream);
        Pa_Terminate();
        return 1;
    }
    
    err = Pa_StartStream(refStream);
    if (err != paNoError) {
        std::cerr << "Error starting reference stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_StopStream(micStream);
        Pa_CloseStream(micStream);
        Pa_CloseStream(refStream);
        Pa_CloseStream(outStream);
        Pa_Terminate();
        return 1;
    }
    
    err = Pa_StartStream(outStream);
    if (err != paNoError) {
        std::cerr << "Error starting output stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_StopStream(micStream);
        Pa_StopStream(refStream);
        Pa_CloseStream(micStream);
        Pa_CloseStream(refStream);
        Pa_CloseStream(outStream);
        Pa_Terminate();
        return 1;
    }

    // Initializing Speex Echo Canceller & Preprocessor
    SpeexEchoState *echo = speex_echo_state_init(FRAME_SIZE, FILTER_LENGTH);
    SpeexPreprocessState *preprocess = speex_preprocess_state_init(FRAME_SIZE, SAMPLE_RATE);
    speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
    int denoise = 1;
    speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);


    std::vector<short> micBuf(FRAME_SIZE), refBuf(FRAME_SIZE), outBuf(FRAME_SIZE);

    std::cout << "Starting real-time echo cancellation. Press Ctrl+C to stop.\n";

    while (!shutdown_requested.load()) {
        // Read from system (reference)
        Pa_ReadStream(refStream, refBuf.data(), FRAME_SIZE);
        // Read from mic (contains echo)
        Pa_ReadStream(micStream, micBuf.data(), FRAME_SIZE);

        // Run echo cancellation
        speex_echo_cancellation(echo, micBuf.data(), refBuf.data(), outBuf.data());
        // denoise
        speex_preprocess_run(preprocess, outBuf.data());

        // Playing cleaned mic audio
        Pa_WriteStream(outStream, outBuf.data(), FRAME_SIZE);
    }

    // Cleanup
    speex_echo_state_destroy(echo);
    speex_preprocess_state_destroy(preprocess);
    Pa_StopStream(micStream);
    Pa_StopStream(refStream);
    Pa_StopStream(outStream);
    Pa_CloseStream(micStream);
    Pa_CloseStream(refStream);
    Pa_CloseStream(outStream);
    Pa_Terminate();

    return 0;
}
