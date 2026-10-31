package com.mrpiclu.guitarmonitor

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import android.webkit.JavascriptInterface
import android.webkit.WebView
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.log10
import kotlin.math.max

class AudioBridge(private val activity: Activity, private val web: WebView) {
    private val main = Handler(Looper.getMainLooper())
    private val am = activity.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    @Volatile private var polling = false
    private var smooth = 0f

    private val poll = object : Runnable {
        override fun run() {
            if (!polling) return
            val rms = NativeAudio.nativeGetRms()
            val peak = NativeAudio.nativeGetPeak()
            val db = 20f * log10(max(rms, 1e-6f))
            val target = ((db + 60f) / 60f).coerceIn(0f, 1f)
            smooth = if (target > smooth) target else smooth * 0.9f + target * 0.1f
            web.evaluateJavascript("__nativeLevel($smooth,${peak > 1f})", null)
            main.postDelayed(this, 33)
        }
    }

    init {
        am.registerAudioDeviceCallback(object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>?) = notifyDevices()
            override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>?) = notifyDevices()
        }, main)
    }

    private fun notifyDevices() {
        web.evaluateJavascript("window.__nativeDevicesChanged && __nativeDevicesChanged()", null)
    }

    private fun typeName(t: Int) = when (t) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "내장 마이크"
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB"
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "유선"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "블루투스"
        else -> "입력"
    }

    @JavascriptInterface
    fun listDevices(): String {
        val arr = JSONArray()
        for (d in am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            arr.put(
                JSONObject()
                    .put("id", d.id.toString())
                    .put("label", "${d.productName} (${typeName(d.type)})")
            )
        }
        return arr.toString()
    }

    @JavascriptInterface
    fun start(json: String): String {
        if (activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            return JSONObject()
                .put("error", "마이크 권한이 필요합니다. 앱 설정에서 허용해 주세요.")
                .toString()
        }
        val o = JSONObject(json)
        val inId = o.optString("deviceId", "").toIntOrNull() ?: 0
        // 출력은 태블릿 내장 스피커로 고정합니다 (USB 장치로 새지 않게)
        val speaker = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
        val outId = speaker?.id ?: 0

        val rc = NativeAudio.nativeStart(
            inId, outId,
            o.optDouble("volume", 1.0).toFloat(),
            o.optBoolean("mono", true)
        )
        if (rc != 0) {
            return JSONObject()
                .put("error", "오디오를 시작할 수 없습니다. " + NativeAudio.nativeLastError())
                .toString()
        }
        polling = true
        main.post(poll)
        return JSONObject()
            .put("latencyMs", Math.round(NativeAudio.nativeLatencyMs()))
            .toString()
    }

    @JavascriptInterface
    fun stop() {
        polling = false
        main.removeCallbacks(poll)
        NativeAudio.nativeStop()
        main.post {
            smooth = 0f
            web.evaluateJavascript("__nativeLevel(0,false)", null)
        }
    }

    @JavascriptInterface
    fun setVolume(v: Double) = NativeAudio.nativeSetVolume(v.toFloat())

    @JavascriptInterface
    fun setMono(b: Boolean) = NativeAudio.nativeSetMono(b)
}