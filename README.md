ESP32-S3 AI Voice Assistant with Triple Servo Control
Project Overview
This system turns your ESP32-S3 into a voice‑controlled AI assistant with expressive servo‑driven movements and a cute OLED eye animation.
It connects to a Python backend that performs:

Speech‑to‑Text (Whisper)

AI conversation (Llama‑3.2‑3B via LM Studio)

Text‑to‑Speech (Kokoro with blended voices)

The ESP32 streams audio from a microphone, receives AI responses as speech, and controls three servos to give physical personality to your assistant.

Hardware Requirements
Component	Quantity	Notes
ESP32-S3 development board	1	(e.g., ESP32-S3-DevKitC-1 or any with PSRAM)
I2S Microphone (e.g., INMP441)	1	PDM/I2S, 3.3V
I2S Audio Amplifier (MAX98357A)	1	For speaker output
Small speaker (4Ω–8Ω)	1	Connect to amplifier
OLED Display (SH1106 128x64)	1	I²C address 0x3C
3x Servo motors (SG90/MG995)	3	For head waggle, base oscillation, etc.
Push button	1	Pull‑up, connected to GPIO3
Power supply (5V 2A+)	1	For ESP32 + servos
Optionally:

3.3V regulator if powering servos from ESP32’s 5V pin (do not exceed current limits).

Wiring Diagram (Pin Connections)
ESP32‑S3 Pin	Component	Wire / Signal
GND	All GND	Common ground
3V3	Microphone VDD, OLED VCC	3.3V supply
5V	Servo VCC	5V supply
GPIO 6	Microphone DOUT (SD)	I2S data in
GPIO 4	Microphone WS (LRCLK)	I2S word clock
GPIO 5	Microphone BCLK	I2S bit clock
GPIO 7	Amplifier DIN (DOUT)	I2S data out
GPIO 15	Amplifier BCLK	I2S bit clock
GPIO 16	Amplifier LRC	I2S LR clock
GPIO 41	OLED SDA	I²C data
GPIO 42	OLED SCL	I²C clock
GPIO 3	Push button (other to GND)	Input with pull‑up
GPIO 2	Servo 1 (Head waggle)	PWM signal
GPIO 1	Servo 2 (Continuous oscillation)	PWM signal
GPIO 10	Servo 3 (Base, stops when AI speaks)	PWM signal
Note: Some GPIOs may differ on your board – update the #define lines in the code accordingly.

How the System Works
1. Boot & Configuration
On power‑up, the ESP32 loads saved WiFi and server credentials from flash.

If no credentials exist, or you hold the button during boot for 3 seconds, it enters Configuration Mode – it creates a WiFi access point named ESP32-AI-Config.

Connect your phone/PC to that AP, open 192.168.4.1, and enter your WiFi SSID, password, and the IP address of the machine running the Python backend.

After saving, the ESP32 restarts and connects to your WiFi and the WebSocket server.

2. Normal Operation (After Setup)
The OLED shows eye animations (blinking, moving, etc.) when idle.

Press and hold the button → starts recording audio from the microphone.

Release the button → stops recording and sends the audio to the Python backend via WebSocket.

The backend transcribes your speech (Whisper), sends it to Llama (via LM Studio) for a response, and streams back the AI’s answer as both:

Text – displayed on the OLED (with auto‑scrolling or page flipping).

Audio – synthesised with Kokoro TTS and streamed back as raw PCM (16‑bit, 24 kHz, mono).

While the AI speaks:

Servo 1 waggles left/right (expressive “talking” motion).

Servo 2 continuously oscillates (never stops, gives a natural “living” feel).

Servo 3 (base) stops oscillating and moves to neutral position (so the head looks forward when listening).

When the speech ends, Servo 1 returns to neutral, Servo 3 resumes its oscillation, and the OLED shows the full AI response text.

3. Interrupt Feature
If you press the button while the AI is speaking, the ESP32 sends an INTERRUPT command to the backend.

The backend immediately stops TTS generation, clears its buffer, and sends an INTERRUPT_ACK.

The ESP32 stops playback, clears its audio buffer, and returns to idle – ready for a new recording.

4. Servo Behaviour Summary
Servo	Pin	Behaviour
Servo 1 (Head)	GPIO 2	Waggles (left‑right) while AI speaks. Returns to neutral (facing forward) when silent.
Servo 2 (Neck / base)	GPIO 1	Continuously oscillates between 30° and 150°. Never stops – even when AI speaks.
Servo 3 (Body / base)	GPIO 10	Oscillates normally (45°–135°) when idle. Goes to neutral (90°) when AI speaks (so the robot “listens” attentively).
Setting Up the Backend (Python)
The Python backend runs on a PC (or Raspberry Pi) with a GPU for best performance.
It uses:

faster-whisper (GPU‑accelerated STT)

LM Studio (runs Llama‑3.2‑3B locally – ensure the server is running on http://localhost:1234)

Kokoro TTS (with blended voices – Bella + Sarah)

WebSockets for real‑time communication

Prerequisites
Python 3.9+

CUDA‑capable GPU (optional but recommended)

LM Studio with a compatible model loaded (e.g., Llama‑3.2‑3B‑Instruct)

The voices folder from Kokoro (place next to the Python script)

Installation
bash
pip install faster-whisper httpx websockets kokoro pyaudio numpy torch
Running the Backend
Start LM Studio and load your model, enable the local API server (default port 1234).

Save the Python code as backend.py.

Ensure the voices directory is in the same folder (download from HuggingFace).

Run:

bash
python backend.py
The backend will display its IP address – note it (e.g., 192.168.1.100).

On the ESP32 configuration page, enter this IP and port 8888.

Configuring the ESP32
First‑Time Setup
Power the ESP32.

Hold the push button for 3 seconds during boot – the OLED shows “CONFIG MODE”.

Connect your phone/PC to the WiFi network ESP32-AI-Config (no password).

Open a browser and go to 192.168.4.1.

Fill in your home WiFi SSID, password, and the backend IP/port.

Click Save – the ESP32 restarts and connects.

Updating Settings
Repeat the above process to change WiFi or server details.

Using the Device
Idle state: Eyes animate, servos oscillate (except Servo 2 always moves).

To ask a question: Press and hold the button. Speak clearly towards the microphone.

Release the button to send your query.

Wait – you’ll see “Processing…” on the OLED, then your transcribed text, then the AI’s spoken response and text display.

To interrupt the AI mid‑speech: press the button again – speech stops, and you can ask a new question immediately.

Troubleshooting
Symptom	Possible Fix
ESP32 doesn’t connect to WiFi	Check SSID/password in config portal. Ensure 2.4 GHz network.
No audio output	Check amplifier wiring and I2S pins. Adjust VOLUME_GAIN in backend (increase if too soft).
Servos jitter or don’t move	Check power supply (servos need enough current). Verify pin assignments.
No speech recognition	Microphone not connected or I2S pins wrong. Check I2S_SD, I2S_WS, I2S_SCK.
Backend not receiving data	Firewall may block port 8888 – allow inbound connections. Ensure both devices on same network.
OLED shows “Disconnected”	Backend not running or IP/port wrong. Check backend console for errors.
Kokoro fails to load	Ensure voices folder exists and contains .pt files. GPU memory may be insufficient – switch to CPU in code.
Customisation Ideas
Change voice blend: Edit voice_blend dictionary in the Python backend (e.g., more Bella for clarity).

Adjust servo speed/range: Modify SERVO1_SPEED, OSCILLATE2_MIN_ANGLE, etc., in the ESP32 code.

Add more eye animations: Extend the launchNextAnimation() function.

Use different AI model: Change MODEL_NAME in the backend and ensure LM Studio serves it.

Credits
Built with ArduinoWebsockets

Eye animations inspired by common robot eye libraries

TTS: Kokoro

STT: faster-whisper

LLM: LM Studio

Enjoy your new AI assistant – and have fun customising it!
If you encounter issues, double‑check your wiring and power supply first – most problems are hardware‑related.

Happy building! 🚀

