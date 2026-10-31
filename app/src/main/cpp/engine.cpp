#include <jni.h>
#include <oboe/Oboe.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

class Engine : public oboe::AudioStreamDataCallback {
public:
    int start(int inputId, int outputId, float volume, bool mono) {
        stop();
        volume_ = volume;
        mono_ = mono;
        rms_ = 0.f;
        peak_ = 0.f;

        // 출력(스피커) 스트림: 이 콜백이 오디오를 끌고 갑니다
        oboe::AudioStreamBuilder ob;
        ob.setDirection(oboe::Direction::Output)
          ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
          ->setSharingMode(oboe::SharingMode::Exclusive)
          ->setFormat(oboe::AudioFormat::Float)
          ->setChannelCount(2)
          ->setUsage(oboe::Usage::Media)
          ->setDeviceId(outputId)
          ->setDataCallback(this);
        oboe::Result r = ob.openStream(out_);
        if (r != oboe::Result::OK) return fail("출력 열기 실패: ", r);

        // 입력 스트림: 출력과 같은 샘플레이트로 열어 변환 지연을 피합니다
        auto openIn = [&](int channels) {
            oboe::AudioStreamBuilder ib;
            ib.setDirection(oboe::Direction::Input)
              ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
              ->setSharingMode(oboe::SharingMode::Exclusive)
              ->setFormat(oboe::AudioFormat::Float)
              ->setSampleRate(out_->getSampleRate())
              ->setChannelCount(channels)
              ->setInputPreset(oboe::InputPreset::Unprocessed)
              ->setDeviceId(inputId);
            return ib.openStream(in_);
        };
        r = openIn(2);
        if (r != oboe::Result::OK) r = openIn(oboe::kUnspecified);
        if (r != oboe::Result::OK) return fail("입력 열기 실패: ", r);

        inCh_ = std::max(1, in_->getChannelCount());
        cap_ = std::max<int32_t>(out_->getBufferCapacityInFrames(), 4096);
        inBuf_.assign(static_cast<size_t>(cap_) * inCh_, 0.f);

        out_->setBufferSizeInFrames(out_->getFramesPerBurst() * 2);
        drain_ = 10;

        r = in_->requestStart();
        if (r != oboe::Result::OK) return fail("입력 시작 실패: ", r);
        r = out_->requestStart();
        if (r != oboe::Result::OK) return fail("출력 시작 실패: ", r);
        return 0;
    }

    void stop() {
        if (out_) { out_->requestStop(); out_->close(); out_.reset(); }
        if (in_)  { in_->requestStop();  in_->close();  in_.reset(); }
    }

    void setVolume(float v) { volume_ = v; }
    void setMono(bool m) { mono_ = m; }
    float rms() const { return rms_.load(); }
    float takePeak() { return peak_.exchange(0.f); }
    const std::string& lastError() const { return lastError_; }

    float latencyMs() {
        if (!out_ || !in_) return 0.f;
        float outMs;
        auto lat = out_->calculateLatencyMillis();
        if (lat.error() == oboe::Result::OK) {
            outMs = static_cast<float>(lat.value());
        } else {
            outMs = out_->getBufferSizeInFrames() * 1000.f / out_->getSampleRate();
        }
        float inMs = in_->getFramesPerBurst() * 1000.f / in_->getSampleRate();
        return outMs + inMs;  // 대략적인 추정치
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* audioData,
                                          int32_t numFrames) override {
        float* o = static_cast<float*>(audioData);
        if (!in_ || numFrames > cap_) {
            std::memset(o, 0, sizeof(float) * 2 * numFrames);
            return oboe::DataCallbackResult::Continue;
        }

        // 시작 직후 쌓인 입력을 비워서 지연이 쌓이지 않게 합니다
        if (drain_ > 0) {
            drain_--;
            int32_t n;
            do {
                auto rr = in_->read(inBuf_.data(), numFrames, 0);
                n = (rr.error() == oboe::Result::OK) ? rr.value() : 0;
            } while (n == numFrames);
            std::memset(o, 0, sizeof(float) * 2 * numFrames);
            return oboe::DataCallbackResult::Continue;
        }

        int32_t got = 0;
        auto rr = in_->read(inBuf_.data(), numFrames, 0);
        if (rr.error() == oboe::Result::OK) got = rr.value();

        const float vol = volume_.load();
        const bool mono = mono_.load();
        const int ch = inCh_;
        float sumSq = 0.f, prePeak = 0.f;

        for (int32_t f = 0; f < numFrames; f++) {
            float l = 0.f, r = 0.f;
            if (f < got) {
                const float* s = inBuf_.data() + static_cast<size_t>(f) * ch;
                if (ch == 1) {
                    l = r = s[0];
                } else if (mono) {
                    float a = 0.f;
                    for (int c = 0; c < ch; c++) a += s[c];
                    l = r = a / ch;
                } else {
                    l = s[0];
                    r = s[1];
                }
            }
            l *= vol;
            r *= vol;
            prePeak = std::max(prePeak, std::max(std::fabs(l), std::fabs(r)));
            l = std::tanh(l);  // 부드러운 클리핑으로 스피커 보호
            r = std::tanh(r);
            o[2 * f] = l;
            o[2 * f + 1] = r;
            sumSq += l * l + r * r;
        }

        rms_.store(std::sqrt(sumSq / (2.f * numFrames)));
        if (prePeak > peak_.load()) peak_.store(prePeak);
        return oboe::DataCallbackResult::Continue;
    }

private:
    int fail(const char* what, oboe::Result r) {
        lastError_ = std::string(what) + oboe::convertToText(r);
        stop();
        return -1;
    }

    std::shared_ptr<oboe::AudioStream> out_, in_;
    std::vector<float> inBuf_;
    std::atomic<float> volume_{1.f}, rms_{0.f}, peak_{0.f};
    std::atomic<bool> mono_{true};
    std::string lastError_;
    int inCh_ = 2;
    int32_t cap_ = 4096;
    int drain_ = 0;
};

static Engine gEngine;

extern "C" {

JNIEXPORT jint JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeStart(JNIEnv*, jobject, jint inId, jint outId,
                                                       jfloat volume, jboolean mono) {
    return gEngine.start(inId, outId, volume, mono == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeStop(JNIEnv*, jobject) { gEngine.stop(); }

JNIEXPORT void JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeSetVolume(JNIEnv*, jobject, jfloat v) {
    gEngine.setVolume(v);
}

JNIEXPORT void JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeSetMono(JNIEnv*, jobject, jboolean m) {
    gEngine.setMono(m == JNI_TRUE);
}

JNIEXPORT jfloat JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeGetRms(JNIEnv*, jobject) { return gEngine.rms(); }

JNIEXPORT jfloat JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeGetPeak(JNIEnv*, jobject) {
    return gEngine.takePeak();
}

JNIEXPORT jfloat JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeLatencyMs(JNIEnv*, jobject) {
    return gEngine.latencyMs();
}

JNIEXPORT jstring JNICALL
Java_com_mrpiclu_guitarmonitor_NativeAudio_nativeLastError(JNIEnv* env, jobject) {
    return env->NewStringUTF(gEngine.lastError().c_str());
}

}  // extern "C"