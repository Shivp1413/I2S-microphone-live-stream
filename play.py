import socket
import sys
import threading
import time

import numpy as np
import sounddevice as sd

# Must match the sketch.
ESP32_IP    = "192.168.4.1"
UDP_PORT    = 4210
SAMPLE_RATE = 16000
CHANNELS    = 1

# Playback buffer. Smaller = lower latency but more prone to gaps.
# 1024 frames at 16 kHz = 64 ms. Try 512 for less lag, 2048 if it stutters.
BLOCKSIZE = 1024


def hello_loop(sock):
    """Keep telling the ESP32 where to send audio."""
    while True:
        try:
            sock.sendto(b"MIC?", (ESP32_IP, UDP_PORT))
        except OSError:
            pass
        time.sleep(1.0)


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))          # any local port; the ESP32 replies to it
    sock.settimeout(2.0)

    threading.Thread(target=hello_loop, args=(sock,), daemon=True).start()

    print(f"Asking {ESP32_IP}:{UDP_PORT} to stream. Ctrl+C to stop.")

    stream = sd.RawOutputStream(
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype="int16",
        blocksize=BLOCKSIZE,
        latency="low",
    )
    stream.start()

    got_audio = False
    try:
        while True:
            try:
                data, _ = sock.recvfrom(4096)
            except socket.timeout:
                print("No audio yet. Are you on the 'mystream' WiFi?")
                continue

            if not got_audio:
                print("Streaming.")
                got_audio = True

            # Guard against a stray odd-length packet.
            if len(data) % 2:
                data = data[:-1]

            # Blocking write paces us to the sound card's real clock.
            stream.write(data)
    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        stream.stop()
        stream.close()
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
