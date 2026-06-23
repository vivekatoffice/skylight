
import socket
import time
import re
import hashlib
import os
import io
from gtts import gTTS
from pydub import AudioSegment
import audioop
import requests


class AxisAudioStreamerTTS:
    def __init__(self, axis_ip, username=None, password=None,
                 http_port=80, chunk_size=800):
        self.host = axis_ip
        self.http_port = http_port   # only one port for HTTP control + audio
        self.username = username
        self.password = password
        self.chunk_size = chunk_size
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    def getAuthChallenge(self):
        url = f"http://{self.host}:{self.http_port}/axis-cgi/audio/transmit.cgi?audiotransmitmode=live"
        response = requests.get(url)
        if response.status_code == 401:
            auth_header = response.headers.get("WWW-Authenticate")
            if auth_header and "Digest" in auth_header:
                return auth_header
        print(f"no digest auth header found in response: {response.headers}")
        return None

    def generateDigestResponse(self, auth_header, method="POST",
                               uri="/axis-cgi/audio/transmit.cgi?audiotransmitmode=live"):
        realm = re.search(r'realm="([^"]+)"', auth_header).group(1)
        nonce = re.search(r'nonce="([^"]+)"', auth_header).group(1)
        qop = re.search(r'qop="([^"]+)"', auth_header).group(1)
        algorithm = re.search(r"algorithm=([^,]+)", auth_header).group(1)

        ha1 = hashlib.md5(f"{self.username}:{realm}:{self.password}".encode()).hexdigest()
        ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
        nc = "00000001"
        cnonce = hashlib.md5(os.urandom(8)).hexdigest()
        response = hashlib.md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}".encode()).hexdigest()

        return (
            f'Digest username="{self.username}", realm="{realm}", nonce="{nonce}", '
            f'uri="{uri}", algorithm={algorithm}, qop={qop}, nc={nc}, '
            f'cnonce="{cnonce}", response="{response}"'
        )

    def _build_request_headers(self):
        auth_header = self.getAuthChallenge()
        digest_auth = self.generateDigestResponse(auth_header)
        headers = [
            "POST /axis-cgi/audio/transmit.cgi?audiotransmitmode=live HTTP/1.1",
            f"Host: {self.host}:{self.http_port}",
            f"Authorization: {digest_auth}",
            "Content-Type: audio/basic",
            "",
            "",
        ]
        return "\r\n".join(headers)

    def _stream_audio(self, data, sample_rate=8000):
        try:
            self.socket.connect((self.host, self.http_port))  # connect to HTTP port
            self.socket.send(self._build_request_headers().encode())

            position = 0
            while position < len(data):
                chunk = data[position: position + self.chunk_size]
                position += self.chunk_size
                print(f"Sending chunk of {len(chunk)} bytes")
                self.socket.send(chunk)

                # pacing to avoid buffer overrun
                time.sleep(len(chunk) / sample_rate)
        except Exception as e:
            print(f"Streaming error: {str(e)}")
        finally:
            self.socket.close()

    def speak_and_stream(self, text):
        # Generate TTS MP3 in memory
        with io.BytesIO() as buf:
            tts = gTTS(text=text, lang="en")
            tts.write_to_fp(buf)
            buf.seek(0)
            audio = AudioSegment.from_file(buf, format="mp3")

        pcm_audio = audio.raw_data
        if audio.frame_rate != 8000:
            print(f"Resampling from {audio.frame_rate} Hz to 8000 Hz")
            pcm_audio, _ = audioop.ratecv(audio.raw_data, 2, 1,
                                          audio.frame_rate, 8000, None)

        # Convert to μ-law
        mulaw = audioop.lin2ulaw(pcm_audio, 2)

        # Stream to Axis
        self._stream_audio(mulaw, 8000)


# ---- Usage Example ----
if __name__ == "__main__":
    streamer = AxisAudioStreamerTTS(
        axis_ip="195.60.68.14",   # change to your Axis device IP
        username="VLTuser",
        password="XXXXXXXXX",
        http_port=13047,             # Axis HTTP(S) port (80 or 443 typically)
        chunk_size=800            # 100ms chunks at 8kHz μ-law
        #chunk_size=160            # 20ms chunks at 8kHz μ-law
    )

    streamer.speak_and_stream(
        "Hello Vivek! Attention. Flight IGO6535. Flying from Bangalore to Bhubaneswar. Currently 21 kilometers away at 8400 feet."
    )

