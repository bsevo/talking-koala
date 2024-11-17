# Talking Koala

[YT Demo Video](https://youtu.be/sjhLlgn_NK4)

An interactive, screen-free smart toy that tells AI-generated bedtime stories while keeping children safe through content moderation.

# Tech Stack 
- [M5Stack Core 2](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit?srsltid=AfmBOooeX2IEIAfY55qHhm9GF2HbXkUpD46qQuF5tvjUqDMXEVCNNbaR) embedded in koala toy
- Whisper model for speech-to-text
- Llama Guard for content moderation
- Llama 3.2 for story generation
- Eleven Labs for text-to-speech
- Groq for AI model hosting

# How it works

- Core 2 device in the koala captures audio input
- Audio is sent to server via WiFi
- Whisper transcribes audio to text
- Llama Guard validates input for safety
- Llama generates an age-appropriate story
- Llama Guard reviews generated content
- Eleven Labs converts story to speech
- Audio streams back to koala for playback
