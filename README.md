# ESP32-S3 INMP441 UDP Microphone Stream

## Overview

This project turns an **ESP32-S3** and an **INMP441 I2S digital microphone** into a low-latency wireless microphone. The ESP32 creates its own Wi-Fi access point, captures audio from the microphone, and streams raw PCM audio to a computer over UDP. A Python application running on the computer receives the stream and plays it in real time.

The project is designed for low latency while maintaining stable audio playback.

---

<table>
<tr>
<td align="center" width="50%">
<img width="660" height="1280" alt="esp32s3" src="https://github.com/user-attachments/assets/2d37504d-cc0c-4cc1-b8d7-22a594ad3d33" />
<b>ESP32-S3 Development Board</b>
</td>

<td align="center" width="50%">
<img width="992" height="1280" alt="mic" src="https://github.com/user-attachments/assets/d35464f4-9198-4abe-9450-8345e3f8217f" />
<b>INMP441 I2S MEMS Microphone</b>
</td>
</tr>
</table>





## Features

* ESP32-S3 Wi-Fi Access Point
* INMP441 I2S microphone support
* 16 kHz, 16-bit mono PCM audio
* Low-latency UDP streaming
* Automatic Gain Control (AGC)
* High-pass filter to reduce low-frequency noise
* Small ring buffer to minimize delay
* Simple Python receiver for Windows, Linux, or macOS

---

## Hardware Required

* ESP32-S3 development board
* INMP441 I2S microphone
* USB cable
* Computer with Python 3 installed

---

## Wiring

| INMP441 | ESP32-S3           |
| ------- | ------------------ |
| VDD     | 3.3V               |
| GND     | GND                |
| SCK     | GPIO 5             |
| WS      | GPIO 6             |
| SD      | GPIO 4             |
| L/R     | GND (Left Channel) |

---

## Software Requirements

### ESP32

* Arduino IDE 2.x
* ESP32 Arduino Core

### Computer

Install Python packages:

```bash
pip install sounddevice numpy
```

---

## Wi-Fi Configuration

The ESP32 creates its own Wi-Fi network.

**SSID**

```
mystream
```

**Password**

```
12345678
```

Once the computer connects to this network, it can receive the audio stream.
You can also use "esp32s3_mic_udp_sta.ino" instead of "esp32s3_mic_udp_ap.ino". "esp32s3_mic_udp_sta.ino" can be configured with your routers SSID and password. 

---

## Audio Configuration

| Setting     |     Value |
| ----------- | --------: |
| Sample Rate |  16000 Hz |
| Bit Depth   |    16-bit |
| Channels    |      Mono |
| Protocol    |       UDP |
| Packet Size | 512 bytes |

---

## How It Works

1. ESP32 starts a Wi-Fi access point.
2. Python sends a small "hello" packet every second.
3. ESP32 stores the sender's IP address.
4. INMP441 continuously captures audio.
5. Audio is converted from 32-bit I2S to 16-bit PCM.
6. A high-pass filter removes DC offset and low-frequency rumble.
7. Automatic Gain Control adjusts microphone volume.
8. Audio packets are streamed over UDP.
9. The Python player immediately plays received packets.

---

## Running the Project

### 1. Flash the ESP32

Upload the Arduino sketch to the ESP32-S3.

---

### 2. Connect to Wi-Fi

Connect the computer to:

```
SSID: mystream
Password: 12345678
```

---

### 3. Install Python Packages

```bash
pip install sounddevice numpy
```

---

### 4. Start the Player

```bash
python play.py
```

or

```bash
py play.py
```

When connected successfully, the terminal will display:

```
Streaming.
```

---

## Project Structure

```
project/
│
├── esp32s3_mic_udp_ap.ino
├── esp32s3_mic_udp_sta.ino
├── play.py
└── README.md
```

---

## Notes

* Keep the microphone away from the speaker to avoid acoustic feedback.
* Lower speaker volume if feedback occurs.
* Headphones provide the best experience.
* The project streams uncompressed PCM audio for minimum latency.

---

## License

This project is provided for educational and personal use. Feel free to modify and improve it for your own applications.
