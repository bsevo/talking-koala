# Talking Koala

[Demo](https://youtu.be/sjhLlgn_NK4)

We wanted to have a toy we can play with together with our kids when kids want that we tell them a story (e.g. before going to bed).
Toy needs to be fun, be creative and spark interest in kids for technology. At the same time, there should be no screen to which kids will stare.
As a form of protection, we used Llama guard rails, for question and response.

# How we did

1. esp32 microcontroller (low power, WiFi, mic, speaker) is put into Koala toy. 
2. Koala listens the question and sends it to the server.
3. Server transribes the audio using Whisper.
4. Text is checked using Llama Guards. We used the model hosted by Groq.
5. Kids story is generated using Llama hosted by Groq.
6. Response is checked using Llama Guards.
7. Audio response is generated using Eleven Labs.
8. Audio is streamed back to Koala and played back by Koala.
