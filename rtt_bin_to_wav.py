import sys, struct, wave, numpy as np, zlib

path = sys.argv[1] if len(sys.argv) > 1 else "capture.bin"
with open(path, "rb") as f:
    data = f.read()

MAGIC = b"PCM1"
idx = data.find(MAGIC)
if idx < 0:
    raise SystemExit("MAGIC 'PCM1' not found in file. Did you log RTT channel #1?")

# 解析头部
if len(data) < idx + 16 + 4:
    raise SystemExit("File too short.")

magic = data[idx:idx+4]
fs, n_samp, reserved = struct.unpack_from("<III", data, idx+4)
payload_off = idx + 16
pcm_bytes   = n_samp * 2
end_off     = payload_off + pcm_bytes
if len(data) < end_off + 4:
    raise SystemExit("File truncated: PCM/CRC not complete.")

pcm = data[payload_off:end_off]
crc_recv = struct.unpack_from("<I", data, end_off)[0]

crc_calc = zlib.crc32(data[idx:end_off]) & 0xFFFFFFFF
if crc_calc != crc_recv:
    print(f"[WARN] CRC mismatch: recv=0x{crc_recv:08X}, calc=0x{crc_calc:08X}")

samples = np.frombuffer(pcm, dtype="<i2")
print(f"fs={fs}, n={len(samples)}, dur={len(samples)/fs:.2f}s")

with wave.open("capture.wav", "wb") as wf:
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(fs)
    wf.writeframes(samples.tobytes())

print("Saved capture.wav")
