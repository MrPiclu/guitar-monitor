package com.mrpiclu.guitarmonitor

object NativeAudio {
    init { System.loadLibrary("guitarmonitor") }

    external fun nativeStart(inputDeviceId: Int, outputDeviceId: Int, volume: Float, mono: Boolean): Int
    external fun nativeStop()
    external fun nativeSetVolume(volume: Float)
    external fun nativeSetMono(mono: Boolean)
    external fun nativeGetRms(): Float
    external fun nativeGetPeak(): Float
    external fun nativeLatencyMs(): Float
    external fun nativeLastError(): String
}