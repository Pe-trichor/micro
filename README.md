# RTT Audio Capture → WAV (nRF5340)
ps：流程有点麻烦的，可能有更简便的，待改进

## 1. Flash firmware / 烧录固件

确保固件会在启动后仅发送约 3 s 的 PCM 数据到 RTT 通道 #1。

```bash
# 烧录示例
west build -b nrf5340dk_nrf5340_cpuapp -p auto
west flash
```

---

## 2. Capture with JLinkRTTLogger / 用 JLinkRTTLogger 抓取

在主机侧执行（最后一个参数是输出文件名）：

```bash
JLinkRTTLogger -Device nRF5340_xxAA -If SWD -Speed 4000 -RTTChannel 1 capture.bin
```


---

## 3. Convert BIN → WAV / 转换为 WAV

使用 Python 脚本将捕获的二进制文件转换为 WAV：

```bash
python3 rtt_bin_to_wav.py capture.bin
```

转换后会生成 `capture.wav` 文件（默认采样率 16 kHz，16 位，单声道）。



---

## 快速试听

macOS：`afplay capture5s.wav`

Linux：`aplay capture5s.wav`

Windows：双击 `capture5s.wav` 用系统播放器打开。
