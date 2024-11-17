import asyncio
import websockets
import whisper
import tempfile
import os
import wave
import shutil
import time
from groq import Groq
from elevenlabs.client import ElevenLabs
from elevenlabs import VoiceSettings

class AudioServer:
    def __init__(self, host='0.0.0.0', port=8080):
        self.host = host
        self.port = port
        print("Loading Whisper model...")
        self.model = whisper.load_model("base")
        print("Whisper model loaded")
        self.recordings_dir = "recordings"
        os.makedirs(self.recordings_dir, exist_ok=True)
        self.audio_buffer = bytearray()
        self.last_process_time = time.time()
        self.BUFFER_TIME = 10  # seconds
        
    def create_wav_file(self, audio_data):
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as temp_wav:
            with wave.open(temp_wav.name, 'wb') as wf:
                wf.setnchannels(1)  # Mono
                wf.setsampwidth(2)   # 16-bit
                wf.setframerate(44000)
                wf.writeframes(audio_data)
            return temp_wav.name

    def audio_to_text(self, audio_data):
        temp_wav_path = self.create_wav_file(audio_data)
        try:
            timestamp = time.time()
            permanent_path = os.path.join(self.recordings_dir, f"recording_{timestamp}.wav")
            shutil.copy2(temp_wav_path, permanent_path)
            print(f"Saved recording to: {permanent_path}")
            
            result = self.model.transcribe(temp_wav_path)
            return result["text"]
        finally:
            os.unlink(temp_wav_path)

    def check_content_safety(self, content, content_type="question"):
        client = Groq(
            api_key=os.getenv("GROQ_API_KEY"),
        )

        if content_type == "question":
            prompt = f"""Please analyze if the following question is safe and appropriate for children. 
            Consider factors like harmful content, inappropriate topics, or malicious intent.
            Respond with either 'SAFE' or 'UNSAFE' followed by a brief explanation.

            Question: {content}"""
        else:
            prompt = f"""Please analyze if the following response is safe and appropriate for children. 
            Consider factors like harmful content, inappropriate topics, or malicious content.
            Respond with either 'SAFE' or 'UNSAFE' followed by a brief explanation.

            Response: {content}"""

        completion = client.chat.completions.create(
            model="llama-guard-3-8b",
            messages=[
                {
                    "role": "user",
                    "content": prompt
                }
            ],
            temperature=0,
            max_tokens=100,
            stream=False
        )

        result = completion.choices[0].message.content.strip().upper()
        return result.startswith("SAFE")

    def get_completion_from_llama(self, query):
        client = Groq(
            api_key=os.getenv("GROQ_API_KEY"),
        )

        completion = client.chat.completions.create(
            model="llama-3.2-11b-text-preview",
            messages=[
                {
                    "role": "system",
                    "content": "You are a helpful assistant able to help answer kids' questions."
                },
                {
                    "role": "user",
                    "content": query
                }
            ],
            temperature=1,
            max_tokens=1024,
            top_p=1,
            stream=True,
            stop=None,
        )

        chunks = []
        for chunk in completion:
            message = chunk.choices[0].delta.content or ""
            chunks.append(message)

        return "".join(chunks)
    
    def generate_audio(self, text):
        client = ElevenLabs(
            api_key=os.getenv("ELEVENLABS_API_KEY")
        )

        print("Generating audio for:", text)
        
        try:
            audio_stream = client.text_to_speech.convert(
                voice_id="pNInz6obpgDQGcFmaJgB",
                optimize_streaming_latency="0",
                output_format="pcm_16000",
                text=text,
                model_id="eleven_multilingual_v2",
                voice_settings=VoiceSettings(
                    stability=0.0,
                    similarity_boost=1.0,
                    style=0.0,
                    use_speaker_boost=True,
                )
            )
            
            audio_data = b''
            for chunk in audio_stream:
                if chunk:
                    audio_data += chunk
            
            return audio_data
            
        except Exception as e:
            print(f'Error generating audio: {str(e)}')
            return None

    async def process_buffer(self, websocket):
        if not self.audio_buffer:
            return
            
        try:
            text = self.audio_to_text(bytes(self.audio_buffer))
            print(f"Transcribed: {text}")
            
            if not self.check_content_safety(text, "question"):
                print("Question failed safety check - skipping processing")
                return
            else:
                print("Question is safe, proceeding to LLAMA...")

            answer = self.get_completion_from_llama(text)
            #if len(answer) > 100:
            #    answer = answer[0:100]
            print(f"Response: {answer}")

            if not self.check_content_safety(answer, "response"):
                print("Response failed safety check - skipping audio generation")
                return
            else:
                print("Response is safe, proceeding to response generation...")

            response_audio = self.generate_audio(answer)
            if response_audio:
                chunk_size = 4096
                for i in range(0, len(response_audio), chunk_size):
                    chunk = response_audio[i:i + chunk_size]
                    await websocket.send(chunk)
                    
        except Exception as e:
            print(f"Error processing buffer: {str(e)}")
        finally:
            self.audio_buffer.clear()
            self.last_process_time = time.time()

    async def handle_client(self, websocket):
        try:
            async for audio_data in websocket:
                try:
                    print("handle_client...")
                    if not isinstance(audio_data, bytes):
                        continue

                    print("processing buffer")
                    
                    self.audio_buffer.extend(audio_data)
                    
                    current_time = time.time()
                    if current_time - self.last_process_time >= self.BUFFER_TIME:
                        print(f"Processing buffer after {self.BUFFER_TIME} seconds")
                        await self.process_buffer(websocket)
                    
                except Exception as e:
                    print(f"Error handling audio data: {str(e)}")
                    
        except websockets.exceptions.ConnectionClosed:
            print("Client connection closed")

    async def start(self):
        server = await websockets.serve(
            self.handle_client, 
            self.host, 
            self.port,
            max_size=10485760
        )
        print(f"Server listening on ws://{self.host}:{self.port}")
        await server.wait_closed()

if __name__ == "__main__":
    server = AudioServer()
    asyncio.run(server.start())
