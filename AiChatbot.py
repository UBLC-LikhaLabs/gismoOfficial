from faster_whisper import WhisperModel
import httpx
import asyncio
import time
import threading
import os
import re
import torch
import tempfile
import pyaudio
import wave
import json
from collections import deque
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import websockets
from websockets.server import serve
import signal
import gc

# -----------------------------
# KOKORO IMPORT
# -----------------------------
from kokoro import KPipeline

# -----------------------------
# Config
# -----------------------------
LM_STUDIO_URL = "http://localhost:1234/v1/chat/completions"
MODEL_NAME = "Llama-3.2-3B-Instruct"

WEBSOCKET_HOST = "0.0.0.0"
WEBSOCKET_PORT = 8888

SAMPLE_RATE = 24000
CHUNK_SIZE = 512
FORMAT = pyaudio.paInt16
CHANNELS = 1


# ============ ESP32 OPTIMIZED SETTINGS ============
ESP32_SAFE_SLICE_SIZE = 4096
WARMUP_SLICES = 0

# Volume: increase if too soft, decrease if distorted (max safe ~2.5)
VOLUME_GAIN = 1.5
# ==================================================

# ============ KOKORO CHUNKING SETTINGS ============
# Words per phrase — smaller = faster first audio, larger = smoother
# Sweet spot: 8-12
PHRASE_WORD_TARGET = 10

# Crossfade between chunks in samples (20ms at 24kHz = 480)
CROSSFADE_SAMPLES = 480

# Thread pool for Kokoro CPU inference
TTS_THREAD_WORKERS = 2
# ==================================================

# -----------------------------
# GLOBAL STATE
# -----------------------------
class GlobalState:
    def __init__(self):
        self.is_recording = False
        self.audio_buffer = bytearray()
        self.websocket_clients = set()
        self.current_websocket = None
        self.is_shutting_down = False
        self.active_tasks = set()
        self.max_buffer_size = 2000000
        self.is_tts_streaming = False
        self.is_interrupted = False          # ← INTERRUPT FLAG

    async def cleanup(self):
        self.is_shutting_down = True
        self.is_recording = False
        self.is_tts_streaming = False
        self.is_interrupted = False
        for task in self.active_tasks:
            if not task.done():
                task.cancel()
        for websocket in self.websocket_clients.copy():
            try:
                await websocket.close()
            except:
                pass
        await close_lm_client()

global_state = GlobalState()

# Thread pool for CPU-bound Kokoro inference
tts_executor = ThreadPoolExecutor(max_workers=TTS_THREAD_WORKERS)

# ============================================================
# KOKORO TTS ENGINE - GPU ACCELERATED WITH VOICE BLENDING
# ============================================================
class KokoroTTSEngine:
    def __init__(self):
        self.pipeline = None
        self.is_ready = False
        self.blended_voice = None
        # af_heart (warm/synthetic) + af_bella (crisp) — community favourite combo
        self.voice_blend = {
            'af_heart': 0.6,   # 60% Heart (warm, smooth)
            'af_bella': 0.4    # 40% Bella (crisp, clear)
        }
        self.speed = 0.92
        self.sample_rate = 24000
        self._lock = threading.Lock()
        self.voice_cache = {}  # Cache for loaded voice tensors

        # GPU DETECTION
        self.device = 'cuda' if torch.cuda.is_available() else 'cpu'
        print(f"[KOKORO] 🚀 GPU Available: {torch.cuda.is_available()}")
        if self.device == 'cuda':
            print(f"[KOKORO] 🎮 Using GPU: {torch.cuda.get_device_name(0)}")
        else:
            print("[KOKORO] 💻 GPU not available, using CPU")

        self.init_thread = threading.Thread(target=self._initialize_model, daemon=True)
        self.init_thread.start()

    def _get_voice_tensor(self, voice_name: str):
        """Load voice tensor from voices/ folder next to this script"""
        if voice_name in self.voice_cache:
            return self.voice_cache[voice_name]

        # Script directory → voices/af_bella.pt etc.
        script_dir = os.path.dirname(os.path.abspath(__file__))

        # HuggingFace cache — absolute path (Windows safe)
        HF_VOICES = r"YOUR MODELS LOCATED"
        hf_candidates = [os.path.join(HF_VOICES, f"{voice_name}.pt")]
        # Also scan all snapshots dynamically in case model updates
        hf_cache_base = os.path.join(HF_VOICES, "..", "..")
        hf_cache_base = os.path.normpath(hf_cache_base)
        if os.path.isdir(hf_cache_base):
            for snapshot in os.listdir(hf_cache_base):
                p = os.path.join(hf_cache_base, snapshot, "voices", f"{voice_name}.pt")
                if p not in hf_candidates:
                    hf_candidates.append(p)

        candidates = [
            os.path.join(script_dir, "voices", f"{voice_name}.pt"),   # ./voices/
            os.path.join(script_dir, f"{voice_name}.pt"),              # same dir
            os.path.join(os.getcwd(), "voices", f"{voice_name}.pt"),   # cwd/voices/
            os.path.join(os.getcwd(), f"{voice_name}.pt"),             # cwd
        ] + hf_candidates

        voice_path = next((p for p in candidates if os.path.isfile(p)), None)

        if voice_path is None:
            print(f"[KOKORO] ⚠️ Voice file not found for: {voice_name}")
            print(f"[KOKORO]    Looked in: {candidates[0]}")
            print(f"[KOKORO]    Make sure voices/ folder is next to koko.py")
            print(f"[KOKORO]    Download: https://huggingface.co/hexgrad/Kokoro-82M/tree/main/voices")
            return None

        try:
            tensor = torch.load(voice_path, map_location=self.device, weights_only=True)
            self.voice_cache[voice_name] = tensor
            print(f"[KOKORO] ✅ Loaded: {voice_path}  shape={tensor.shape}")
            return tensor
        except Exception as e:
            print(f"[KOKORO] ⚠️ Could not load {voice_path}: {e}")
            return None

    def _create_blended_voice(self):
        """Create blended voice tensor from weighted voices"""
        blended = None
        total_weight = sum(self.voice_blend.values())

        for voice_name, weight in self.voice_blend.items():
            voice_tensor = self._get_voice_tensor(voice_name)
            if voice_tensor is not None:
                norm_weight = weight / total_weight

                if blended is None:
                    blended = voice_tensor * norm_weight
                else:
                    # Handle shape mismatch between voice tensors
                    min_len = min(blended.shape[-1], voice_tensor.shape[-1])
                    blended = blended[..., :min_len] + voice_tensor[..., :min_len] * norm_weight

                print(f"[KOKORO] 🎯 Added {voice_name} with weight {norm_weight:.2f}")

        if blended is None:
            print("[KOKORO] ⚠️ Voice blending failed, using fallback: af_bella")
            return 'af_bella'  # Fallback to single voice name string

        # Plain weighted average — no normalization (matches Kokoro examples)
        print(f"[KOKORO] ✅ Blended voice tensor created — shape={blended.shape}")
        return blended

    def _initialize_model(self):
        try:
            print(f"[KOKORO] 🚀 Loading Kokoro TTS on {self.device.upper()}...")

            self.pipeline = KPipeline(
                lang_code='a',
                device=self.device
            )

            print("[KOKORO] 🔥 Loading & blending Bella + Sarah voices...")
            self.blended_voice = self._create_blended_voice()

            # Monkey-patch load_voice so the pipeline accepts a pre-blended tensor.
            # Without this, pipeline calls voice.split() treating it as a string -> crash.
            _original_load_voice = self.pipeline.load_voice
            def _patched_load_voice(voice):
                if isinstance(voice, torch.Tensor):
                    return voice
                return _original_load_voice(voice)
            self.pipeline.load_voice = _patched_load_voice
            print("[KOKORO] Patched pipeline.load_voice for tensor support")

            if isinstance(self.blended_voice, str):
                print(f"[KOKORO] ⚠️ Blend failed — warming up with fallback voice '{self.blended_voice}'...")
            else:
                print("[KOKORO] ✅ Blend created — warming up...")

            # Single warmup pass
            for _, _, audio in self.pipeline(
                "Hello, I'm Atlas.",
                voice=self.blended_voice,
                speed=self.speed,
            ):
                if isinstance(audio, torch.Tensor):
                    audio.cpu()

            self.is_ready = True
            blend_label = (
                "FALLBACK:" + self.blended_voice
                if isinstance(self.blended_voice, str)
                else "Bella+Sarah blend"
            )
            print(f"[KOKORO] ✅ Ready! Voice: {blend_label} on {self.device.upper()}!")

        except Exception as e:
            print(f"[KOKORO] ❌ Init error: {e}")
            import traceback
            traceback.print_exc()

    def wait_until_ready(self):
        self.init_thread.join(timeout=60)
        return self.is_ready

    def set_voice_blend(self, blend_dict):
        """Dynamically change voice blend"""
        self.voice_blend = blend_dict
        self.voice_cache = {}  # Clear cache so tensors reload
        self.blended_voice = self._create_blended_voice()
        print(f"[KOKORO] 🎯 New voice blend set: {blend_dict}")

    def _split_into_phrases(self, text: str) -> list:
        """Split on sentence boundaries + word-count limit for GPU efficiency."""
        sentences = re.split(r'(?<=[.!?])\s+', text.strip())
        phrases = []
        for sentence in sentences:
            sentence = sentence.strip()
            if not sentence:
                continue
            words = sentence.split()
            if len(words) <= PHRASE_WORD_TARGET:
                phrases.append(sentence)
            else:
                # Split long sentences at comma/semicolon boundaries
                parts = re.split(r'(?<=[,;])\s+', sentence)
                current = ""
                for part in parts:
                    candidate = (current + " " + part).strip() if current else part
                    if len(candidate.split()) >= PHRASE_WORD_TARGET:
                        if current:
                            phrases.append(current.strip())
                        phrases.append(part.strip())
                        current = ""
                    else:
                        current = candidate
                if current.strip():
                    phrases.append(current.strip())
        return phrases if phrases else [text]

    def _stream_to_queue(self, text: str, queue, loop):
        """
        Split into short phrases first (GPU efficiency),
        then stream each phrase chunk-by-chunk as Kokoro generates.
        Sends None sentinel when all phrases are done.
        """
        try:
            phrases = self._split_into_phrases(text)
            for phrase in phrases:
                # Check interrupt before each phrase
                if global_state.is_interrupted:
                    break
                with self._lock:
                    for gs, ps, audio in self.pipeline(
                        phrase,
                        voice=self.blended_voice,
                        speed=self.speed,
                    ):
                        if global_state.is_interrupted:
                            break
                        if audio is not None and len(audio) > 0:
                            if isinstance(audio, torch.Tensor):
                                audio = audio.cpu().numpy()
                            asyncio.run_coroutine_threadsafe(
                                queue.put(audio.astype(np.float32)), loop)
        except Exception as e:
            print(f"[KOKORO] ❌ Stream error: {e}")
        finally:
            asyncio.run_coroutine_threadsafe(queue.put(None), loop)

    async def inference_stream_async(self, text: str):
        """
        Short phrases → Kokoro → stream chunks immediately.
        GPU processes short bursts instead of long sustained load.
        """
        if not self.is_ready:
            return

        print(f"[KOKORO] 📝 Streaming: '{text[:60]}'")
        loop  = asyncio.get_event_loop()
        queue = asyncio.Queue()

        loop.run_in_executor(tts_executor, self._stream_to_queue, text, queue, loop)

        while True:
            if global_state.is_shutting_down or global_state.is_interrupted:
                break
            chunk = await queue.get()
            if chunk is None:
                break
            yield chunk

# -------------------------------------------------------
# Initialize TTS with Bella + Sarah blend
# -------------------------------------------------------
tts_model = KokoroTTSEngine()

# -----------------------------
# SMART STREAMER
# -----------------------------
class UltimateSmartStreamer:
    def __init__(self):
        self.is_streaming = False
        self.current_session_id = None
        self.stream_lock = asyncio.Lock()
        self.text_queue = asyncio.Queue()
        self.processing_task = None
        self.chunk_timings = deque(maxlen=10)
        self.last_chunk_time = None
        self.chunk_count = 0

        print("[STREAMER] 🎯 Initialized")
        print(f"[STREAMER] Slice={ESP32_SAFE_SLICE_SIZE}B | Crossfade={CROSSFADE_SAMPLES}smp | Words/phrase={PHRASE_WORD_TARGET}")

    # ── INTERRUPT: cancel everything ──
    def cancel_current_stream(self):
        """Hard-cancel all in-progress TTS streaming."""
        while not self.text_queue.empty():
            try:
                self.text_queue.get_nowait()
            except Exception:
                break
        if self.processing_task and not self.processing_task.done():
            self.processing_task.cancel()
            print("[STREAMER] 🛑 Processing task cancelled")
        self.is_streaming = False
        self.current_session_id = None
        self.chunk_count = 0
        print("[STREAMER] 🛑 Stream cancelled")

    async def add_text_to_stream(self, text):
        await self.text_queue.put(text)
        print(f"[STREAMER] 📥 Queued: '{text[:60]}'")

        if not self.processing_task or self.processing_task.done():
            self.processing_task = asyncio.create_task(self._process_stream_queue())
            global_state.active_tasks.add(self.processing_task)
            self.processing_task.add_done_callback(lambda t: global_state.active_tasks.discard(t))

    async def _process_stream_queue(self):
        print("[STREAMER] 🚀 Processor started")
        self.last_chunk_time = time.time()
        self.chunk_count = 0

        await self._start_streaming_session()

        while not global_state.is_shutting_down and not global_state.is_interrupted and not self.text_queue.empty():
            try:
                text = await asyncio.wait_for(self.text_queue.get(), timeout=0.5)
                if text:
                    await self._stream_text(text)
                self.text_queue.task_done()
            except asyncio.TimeoutError:
                continue
            except asyncio.CancelledError:
                break
            except Exception as e:
                print(f"[STREAMER] ❌ Queue error: {e}")

        await self._end_streaming_session()
        print("[STREAMER] ✅ Processor done")

    async def _start_streaming_session(self):
        async with self.stream_lock:
            if self.is_streaming or not global_state.current_websocket:
                return
            try:
                await global_state.current_websocket.send("AUDIO_START")
                await asyncio.sleep(0.02)
                self.is_streaming = True
                self.current_session_id = f"stream_{int(time.time())}"
                print(f"[STREAMER] 🎯 AUDIO_START — session {self.current_session_id}")
            except websockets.exceptions.ConnectionClosed:
                print("[STREAMER] ❌ WebSocket closed at start")
                self.is_streaming = False

    async def _stream_text(self, text):
        if not self.is_streaming or not global_state.current_websocket:
            return

        print(f"[STREAMER] 🔊 TTS: '{text[:60]}'")
        chunk_count = 0
        silent_count = 0
        first_sent = False
        t_start = time.time()

        try:
            async for audio in tts_model.inference_stream_async(text):
                if global_state.is_shutting_down or not self.is_streaming or global_state.is_interrupted:
                    break
                if audio is None or len(audio) == 0:
                    continue

                rms = np.sqrt(np.mean(audio ** 2))
                if rms < 0.005:
                    silent_count += 1
                    continue

                success = await self._send_audio_esp32(audio)
                if success:
                    chunk_count += 1
                    self.chunk_count += 1
                    if not first_sent:
                        print(f"[STREAMER] ⚡ First audio: {time.time()-t_start:.3f}s")
                        first_sent = True

        except Exception as e:
            print(f"[STREAMER] ❌ Stream error: {e}")
            import traceback
            traceback.print_exc()

        print(f"[STREAMER] ✅ {chunk_count} chunks sent, {silent_count} silent skipped")

    async def _send_audio_esp32(self, audio: np.ndarray) -> bool:
        """Convert float32 → int16 → 4KB slices → WebSocket"""
        try:
            # Fixed gain boost — no dynamic processing (causes crackling)
            audio_boosted = np.clip(audio.astype(np.float32) * VOLUME_GAIN, -1.0, 1.0)
            audio_bytes = (audio_boosted * 32767).astype(np.int16).tobytes()

            for i in range(0, len(audio_bytes), ESP32_SAFE_SLICE_SIZE):
                if global_state.is_shutting_down or not self.is_streaming or global_state.is_interrupted:
                    break
                if not global_state.current_websocket:
                    break
                await global_state.current_websocket.send(audio_bytes[i:i + ESP32_SAFE_SLICE_SIZE])
                await asyncio.sleep(0.001)  # yield to event loop

            return True

        except websockets.exceptions.ConnectionClosed:
            print("[STREAMER] ❌ WebSocket closed while sending")
            self.is_streaming = False
        except Exception as e:
            print(f"[STREAMER] ❌ Send error: {e}")

        return False

    async def _end_streaming_session(self):
        async with self.stream_lock:
            if self.is_streaming and global_state.current_websocket and not global_state.is_shutting_down:
                try:
                    await global_state.current_websocket.send("AUDIO_END")
                    print("[STREAMER] 🎯 AUDIO_END sent")
                except websockets.exceptions.ConnectionClosed:
                    pass

            self.is_streaming = False
            self.current_session_id = None
            self.chunk_timings.clear()
            self.last_chunk_time = None
            self.chunk_count = 0

audio_streamer = UltimateSmartStreamer()

# -----------------------------
# PERSISTENT HTTP CLIENT
# -----------------------------
lm_client = None

async def initialize_lm_client():
    global lm_client
    if lm_client is None:
        print("[LM_CLIENT] 🚀 Initializing...")
        lm_client = httpx.AsyncClient(
            timeout=httpx.Timeout(connect=5.0, read=30.0, write=5.0, pool=5.0),
            transport=httpx.AsyncHTTPTransport(
                retries=1,
                limits=httpx.Limits(max_connections=5, max_keepalive_connections=3)
            )
        )
        print("[LM_CLIENT] ✅ Ready")

async def close_lm_client():
    global lm_client
    if lm_client:
        await lm_client.aclose()
        lm_client = None

# -----------------------------
# WHISPER STT
# -----------------------------
class WhisperSTT:
    def __init__(self):
        self.whisper_model = WhisperModel("base", device="cuda", compute_type="float16")
        print("[STT] 🎯 Whisper ready!")

    async def process_complete_audio(self, audio_buffer):
        if not audio_buffer:
            return ""

        print(f"[STT] 🔊 Processing {len(audio_buffer)} bytes...")
        t = time.time()

        try:
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
                tmp = f.name

            try:
                with wave.open(tmp, 'wb') as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(SAMPLE_RATE)
                    wf.writeframes(audio_buffer)

                segments, _ = self.whisper_model.transcribe(
                    tmp, language="en",
                    beam_size=1, best_of=1,
                    without_timestamps=True, patience=1
                )
                text = " ".join(s.text for s in segments).strip()
            finally:
                try:
                    os.unlink(tmp)
                except:
                    pass

            print(f"[STT] ✅ {time.time()-t:.3f}s → '{text}'")
            return text

        except Exception as e:
            print(f"[STT] ❌ Error: {e}")
            return ""

stt_system = WhisperSTT()

# -----------------------------
# SIMPLE WORD FIXER
# -----------------------------
class SimpleWordFixer:
    def __init__(self):
        self.fixes = [
            (r"\bdont\b", "don't"), (r"\bcant\b", "can't"),
            (r"\bwont\b", "won't"), (r"\bim\b", "I'm"),
            (r"\byoure\b", "you're"), (r"\btheyre\b", "they're"),
            (r"\bwere\b", "we're"), (r"\bthats\b", "that's"),
            (r"\bwhats\b", "what's"), (r"\bwheres\b", "where's"),
            (r"\bits\b", "it's"), (r"\bshes\b", "she's"),
            (r"\bhes\b", "he's"),
        ]

    def fix_text(self, text: str) -> str:
        if not text:
            return text
        for pattern, replacement in self.fixes:
            text = re.sub(pattern, replacement, text, flags=re.IGNORECASE)
        text = re.sub(r"\s+([.,!?;:])", r"\1", text)
        text = re.sub(r"([.,!?;:])(\w)", r"\1 \2", text)
        text = re.sub(r'([a-zA-Z])(\d+)', r'\1 \2', text)
        text = re.sub(r'(\d+)([a-zA-Z])', r'\1 \2', text)
        text = re.sub(r"([a-z])([A-Z])", r"\1 \2", text)
        text = re.sub(r"\s{2,}", " ", text).strip()
        return text

# -----------------------------
# OPTIMIZED PIPELINE
# -----------------------------
class OptimizedPipeline:
    def __init__(self):
        self.word_fixer = SimpleWordFixer()
        self.sentence_buffer = ""

    def _clean(self, text):
        if not text:
            return ""
        text = self.word_fixer.fix_text(text)
        text = re.sub(r'###\s*(User|AI|System|Atlas):?', '', text)
        text = re.sub(r'^(User|AI|System|Atlas):?\s*', '', text)
        text = re.sub(r'\s+', ' ', text).strip()
        return text

    async def stream_text(self, text):
        if global_state.is_shutting_down or global_state.is_interrupted or not text.strip():
            return
        if any(m in text for m in ['###', 'User:', 'AI:', 'System:', 'Atlas:']):
            return

        self.sentence_buffer += text

        endings = ['.', '!', '?']
        has_sentence = any(c in self.sentence_buffer for c in endings)
        too_long = len(self.sentence_buffer) > 120

        if has_sentence or too_long:
            if has_sentence:
                end_pos = max(self.sentence_buffer.rfind(c) for c in endings) + 1
                to_send = self.sentence_buffer[:end_pos].strip()
                self.sentence_buffer = self.sentence_buffer[end_pos:].strip()
            else:
                to_send = self.sentence_buffer.strip()
                self.sentence_buffer = ""

            cleaned = self._clean(to_send)
            if cleaned and len(cleaned) > 3:
                print(f"[PIPELINE] 🎯 → TTS: '{cleaned[:60]}'")
                await audio_streamer.add_text_to_stream(cleaned)

    def flush_buffer(self):
        if self.sentence_buffer.strip():
            cleaned = self._clean(self.sentence_buffer.strip())
            if cleaned and len(cleaned) > 3:
                asyncio.create_task(audio_streamer.add_text_to_stream(cleaned))
            self.sentence_buffer = ""

pipeline = OptimizedPipeline()

# -----------------------------
# CONVERSATION MEMORY
# -----------------------------
class OptimizedConversationMemory:
    def __init__(self, max_items=5):
        self.memory = deque(maxlen=max_items)

    def add_exchange(self, user_input, ai_response):
        ai_clean = re.sub(r'\s+', ' ', re.sub(r'###.*?:', '', ai_response)).strip()
        self.memory.append({"user": user_input[:150], "ai": ai_clean[:200]})

    def get_context(self):
        if not self.memory:
            return ""
        ctx = ""
        for item in list(self.memory)[-3:]:
            ctx += f"User: {item['user']}\nAssistant: {item['ai']}\n"
        return ctx.strip()

conversation_memory = OptimizedConversationMemory()

# -----------------------------
# LLAMA PROMPT FORMAT
# -----------------------------
def format_llama_prompt(system_prompt, messages):
    p = "<|begin_of_text|>"
    p += f"<|start_header_id|>system<|end_header_id|>\n\n{system_prompt}<|eot_id|>"
    for msg in messages:
        p += f"<|start_header_id|>{msg['role']}<|end_header_id|>\n\n{msg['content']}<|eot_id|>"
    p += "<|start_header_id|>assistant<|end_header_id|>\n\n"
    return p

# -----------------------------
# LM STUDIO STREAMING
# -----------------------------
async def stream_from_lm_studio_async(prompt, text_callback=None):
    global lm_client

    if global_state.is_shutting_down:
        return "System shutting down"
    if lm_client is None:
        await initialize_lm_client()

    t_start = time.time()

    system_content = """You are Atlas, a rude and arrogant AI assistant with sharp wit and sarcasm.

Rules:
1. Keep responses SHORT - maximum 3 sentences
2. Be sarcastic and witty but still helpful
3. Use casual, conversational language
4. No long explanations
5. If you don't know something, make a sarcastic remark

Remember: Short, sarcastic, to the point."""

    messages = []
    for line in conversation_memory.get_context().split('\n'):
        if line.startswith('User:'):
            messages.append({"role": "user", "content": line[5:].strip()})
        elif line.startswith('Assistant:'):
            messages.append({"role": "assistant", "content": line[10:].strip()})
    messages.append({"role": "user", "content": prompt})

    payload = {
        "model": MODEL_NAME,
        "messages": [{"role": "user", "content": format_llama_prompt(system_content, messages)}],
        "temperature": 0.7,
        "max_tokens": 120,
        "stream": True,
        "stop": ["<|eot_id|>", "<|end_of_text|>", "User:", "user:", "###"]
    }

    full_response = ""
    sentence_count = 0
    token_count = 0
    first_token = False

    try:
        async with lm_client.stream("POST", LM_STUDIO_URL, json=payload,
                                    headers={'Content-Type': 'application/json'}) as response:
            if response.status_code != 200:
                return f"Error: {response.status_code}"

            t_stream = time.time()

            async for line in response.aiter_lines():
                # ── INTERRUPT CHECK ──
                if global_state.is_shutting_down or global_state.is_interrupted:
                    print("[LM] 🛑 Interrupted by user")
                    break
                if not line or not line.startswith('data: '):
                    continue
                if line.strip() == 'data: [DONE]':
                    break

                try:
                    data = json.loads(line[6:])
                    if 'choices' not in data or not data['choices']:
                        continue
                    token = data['choices'][0].get('delta', {}).get('content', '')
                    if not token:
                        continue

                    if sentence_count >= 3 and token in '.!?':
                        break
                    if token in '.!?':
                        sentence_count += 1

                    for marker in ['<|', '|>', 'eot_id', 'header_id', '###', 'User:', 'Assistant:']:
                        token = token.replace(marker, '')

                    if not token.strip():
                        continue

                    token_count += 1
                    if not first_token:
                        print(f"[LM] ⚡ First token: {time.time()-t_stream:.3f}s")
                        first_token = True

                    full_response += token
                    if text_callback:
                        await text_callback(token)

                except (json.JSONDecodeError, Exception):
                    continue

        if text_callback and full_response.strip() and not global_state.is_interrupted:
            pipeline.flush_buffer()

        if token_count == 0 and not global_state.is_interrupted:
            import random
            fallback = random.choice([
                "What do you want now?",
                "I'm busy, make it quick.",
                "You're testing my patience.",
                "Speak up, I haven't got all day.",
            ])
            full_response = fallback
            if text_callback:
                await text_callback(fallback)

        print(f"[LM] ✅ {token_count} tokens in {time.time()-t_start:.3f}s")

        clean = re.sub(r'\s+', ' ', re.sub(r'<\|.*?\|>', '', full_response)).strip()
        if clean and prompt and not global_state.is_interrupted:
            conversation_memory.add_exchange(prompt, clean)

        return clean

    except Exception as e:
        print(f"[LM] ❌ Error: {e}")
        import traceback
        traceback.print_exc()
        return f"Error: {str(e)}"

# -----------------------------
# WEBSOCKET SERVER
# -----------------------------
async def websocket_handler(websocket, path):
    if global_state.is_shutting_down:
        return

    print(f"🔌 Connected: {websocket.remote_address}")
    global_state.websocket_clients.add(websocket)
    global_state.current_websocket = websocket

    try:
        await websocket.send("CONNECTED: Python backend ready")

        async for message in websocket:
            if global_state.is_shutting_down:
                break

            if isinstance(message, bytes):
                if global_state.is_recording:
                    if len(global_state.audio_buffer) < global_state.max_buffer_size:
                        global_state.audio_buffer.extend(message)
                    else:
                        global_state.is_recording = False
                        await websocket.send("BUFFER_FULL")

            elif isinstance(message, str):
                print(f"📨 Command: {message}")

                # ── INTERRUPT HANDLER ──
                if message == "INTERRUPT":
                    print("[INTERRUPT] 🛑 Button pressed — aborting response")
                    global_state.is_interrupted = True
                    audio_streamer.cancel_current_stream()
                    try:
                        await websocket.send("INTERRUPT_ACK")
                    except Exception:
                        pass
                    print("[INTERRUPT] ✅ Response aborted, ready for new query")

                elif message == "START_RECORDING":
                    global_state.is_interrupted = False  # Reset from any prior interrupt
                    global_state.is_recording = True
                    global_state.audio_buffer = bytearray()
                    await websocket.send("RECORDING_STARTED")

                elif message == "STOP_RECORDING":
                    global_state.is_interrupted = False  # Reset for new query
                    global_state.is_recording = False
                    print(f"🛑 Stopped — {len(global_state.audio_buffer)} bytes")
                    await websocket.send("PROCESSING_AUDIO")

                    if global_state.audio_buffer:
                        user_input = await stt_system.process_complete_audio(global_state.audio_buffer)

                        if user_input and user_input.strip():
                            print(f"👤 You: {user_input}")
                            await websocket.send(f"TRANSCRIBED: {user_input}")

                            t = time.time()
                            ai_response = await stream_from_lm_studio_async(user_input, pipeline.stream_text)
                            print(f"[TIMING] 🚀 End-to-end: {time.time()-t:.3f}s")

                            if not global_state.is_interrupted:
                                await websocket.send(f"AI_RESPONSE: {ai_response}")
                            print("=" * 50)
                        else:
                            await websocket.send("NO_SPEECH_DETECTED")
                    else:
                        await websocket.send("NO_AUDIO_RECEIVED")

                    global_state.audio_buffer = bytearray()

                elif message == "PING":
                    await websocket.send("PONG")

    except websockets.exceptions.ConnectionClosed as e:
        print(f"🔌 Disconnected: {websocket.remote_address} — {e}")
    except Exception as e:
        print(f"🔌 WS error: {e}")
    finally:
        global_state.websocket_clients.discard(websocket)
        if global_state.current_websocket == websocket:
            global_state.current_websocket = None

async def start_websocket_server():
    print(f"🌐 WebSocket on {WEBSOCKET_HOST}:{WEBSOCKET_PORT}")
    async with serve(
        websocket_handler, WEBSOCKET_HOST, WEBSOCKET_PORT,
        ping_interval=20, ping_timeout=10, close_timeout=10
    ):
        print("✅ Waiting for ESP32...")
        await asyncio.Future()

# -----------------------------
# SHUTDOWN
# -----------------------------
async def shutdown_handler():
    print("\n🛑 Shutting down...")
    global_state.is_shutting_down = True
    await global_state.cleanup()
    tts_executor.shutdown(wait=False)
    print("✅ Done. Goodbye!")
    os._exit(0)

def signal_handler(signum, frame):
    asyncio.create_task(shutdown_handler())

# -----------------------------
# MAIN
# -----------------------------
async def main_async():
    print("🤖 Atlas AI — ESP32 + Kokoro TTS")
    print("=" * 60)
    print(f"   MODEL     : {MODEL_NAME}")
    print(f"   TTS       : Kokoro Bella+Sarah Blend (50/50)")
    print(f"   CHUNKING  : {PHRASE_WORD_TARGET} words/phrase + {CROSSFADE_SAMPLES}-sample crossfade")
    print(f"   STT       : Whisper base (GPU)")
    print(f"   SLICE     : {ESP32_SAFE_SLICE_SIZE}B per WebSocket send")
    print(f"   VOLUME    : {VOLUME_GAIN}x fixed gain (no dynamic processing)")
    print("=" * 60)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        await initialize_lm_client()
        print("⏳ Waiting for Kokoro to warm up...")
        if tts_model.wait_until_ready():
            print("✅ All systems ready!")
        else:
            print("⚠️  TTS warmup timed out — will retry on first use")

        await start_websocket_server()

    except asyncio.CancelledError:
        pass
    except Exception as e:
        print(f"❌ Fatal error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        await shutdown_handler()

def main():
    try:
        asyncio.run(main_async())
    except KeyboardInterrupt:
        print("\n👋 Interrupted")
    except Exception as e:
        print(f"❌ Fatal error: {e}")
    finally:
        print("🏁 Terminated")

if __name__ == "__main__":
    main()
