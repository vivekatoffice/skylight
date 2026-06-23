
# FlightDisplayAnnouncer.py
import argparse
import math
import time
import requests
import urllib3
import socket
import re
import hashlib
import os
import io
import audioop

from gtts import gTTS
from pydub import AudioSegment

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


class AxisAudioStreamerTTS:
    def __init__(self, axis_ip, username=None, password=None,
                 http_port=80, chunk_size=160):
        self.host = axis_ip
        self.http_port = http_port
        self.username = username
        self.password = password
        self.chunk_size = chunk_size

    def getAuthChallenge(self):
        url = f"http://{self.host}:{self.http_port}/axis-cgi/audio/transmit.cgi?audiotransmitmode=live"
        response = requests.get(url, timeout=10)
        if response.status_code == 401:
            auth_header = response.headers.get("WWW-Authenticate")
            if auth_header and "Digest" in auth_header:
                return auth_header
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
        if not auth_header:
            raise RuntimeError("Digest auth challenge not received")
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
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.connect((self.host, self.http_port))
            sock.send(self._build_request_headers().encode())

            pos = 0
            while pos < len(data):
                chunk = data[pos:pos + self.chunk_size]
                pos += self.chunk_size
                sock.send(chunk)
                time.sleep(len(chunk) / sample_rate)
        finally:
            sock.close()

    def speak_and_stream(self, text):
        with io.BytesIO() as buf:
            tts = gTTS(text=text, lang="en")
            tts.write_to_fp(buf)
            buf.seek(0)
            audio = AudioSegment.from_file(buf, format="mp3")

        pcm = audio.raw_data
        if audio.frame_rate != 8000:
            pcm, _ = audioop.ratecv(
                audio.raw_data, 2, 1,
                audio.frame_rate, 8000, None
            )

        mulaw = audioop.lin2ulaw(pcm, 2)
        self._stream_audio(mulaw, 8000)


def haversine(lat1, lon1, lat2, lon2):
    r = 6371.0
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)

    a = (
        math.sin(dlat / 2) ** 2
        + math.cos(math.radians(lat1))
        * math.cos(math.radians(lat2))
        * math.sin(dlon / 2) ** 2
    )
    return r * (2 * math.atan2(math.sqrt(a), math.sqrt(1 - a)))


def send_to_axis(ip, user, password, message):
    url = f"https://{ip}/config/rest/speaker-display-notification/v1/simple"

    payload = {
        "data": {
            "message": message,
            "textSize": "large",
            "scrollDirection": "fromRightToLeft",
            "scrollSpeed": 5,
            "duration": {"type": "time", "value": 300000}
        }
    }

    r = requests.post(
        url,
        auth=(user, password),
        json=payload,
        verify=False,
        timeout=15,
    )
    print("[Axis Display]", r.status_code)
    r.raise_for_status()


def get_route(callsign):
    try:
        r = requests.get(
            f"https://api.adsbdb.com/v0/callsign/{callsign.strip()}",
            timeout=10,
        )

        if r.status_code != 200:
            return None

        route = r.json().get("response", {}).get("flightroute")
        if not route:
            return None

        origin = route.get("origin", {})
        destination = route.get("destination", {})

        origin_text = (
            f"{origin.get('municipality', 'Unknown')} "
            f"({origin.get('name', 'Unknown Airport')})"
        )

        destination_text = (
            f"{destination.get('municipality', 'Unknown')} "
            f"({destination.get('name', 'Unknown Airport')})"
        )

        return origin_text, destination_text

    except Exception as ex:
        print("Route lookup failed:", ex)
        return None


def get_closest_aircraft(lat, lon, radius_nm):
    r = requests.get(
        f"https://api.adsb.lol/v2/lat/{lat}/lon/{lon}/dist/{radius_nm}",
        timeout=15,
    )
    r.raise_for_status()

    nearest = None
    nearest_distance = float("inf")

    for ac in r.json().get("ac", []):
        if ac.get("lat") is None or ac.get("lon") is None:
            continue

        d = haversine(lat, lon, ac["lat"], ac["lon"])

        if d < nearest_distance:
            nearest_distance = d
            nearest = ac

    return nearest, nearest_distance


def announce_flight(audio_ip, audio_port, user, password,
                    flight, route, distance, altitude):
    try:
        if route:
            origin, destination = route
            text = (
                f"Attention Vivek. Flight {flight}. "
                f"Flying from {origin.split('(')[0].strip()} "
                f"to {destination.split('(')[0].strip()}. "
                f"Currently {distance:.0f} kilometers away "
                f"at {altitude} feet."
            )
        else:
            text = (
                f"Attention. Flight {flight}. "
                f"Currently {distance:.0f} kilometers away "
                f"at {altitude} feet."
            )

        print("[TTS]", text)

        AxisAudioStreamerTTS(
            axis_ip=audio_ip,
            username=user,
            password=password,
            http_port=audio_port,
            chunk_size=800,
        ).speak_and_stream(text)

    except Exception as ex:
        print("Announcement failed:", ex)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--audio-port", type=int, default=13047)
    parser.add_argument("--lat", type=float, default=12.97)
    parser.add_argument("--lon", type=float, default=77.75)
    parser.add_argument("--radius", type=int, default=25)

    args = parser.parse_args()

    last_message = None
    last_announced_flight = None

    while True:
        try:
            aircraft, distance = get_closest_aircraft(
                args.lat, args.lon, args.radius
            )

            if aircraft:
                flight = aircraft.get("flight", "UNKNOWN").strip()
                altitude = aircraft.get("alt_baro", "N/A")
                aircraft_type = aircraft.get("t", "N/A")

                route = get_route(flight)

                lines = [f"✈ {flight}"]

                if route:
                    lines.extend([route[0], "→", route[1]])
                else:
                    lines.append(aircraft_type)

                lines.append(f"{distance:.1f}km {altitude}ft")

                message = "\n".join(lines)

                print("\n" + "=" * 60)
                print(message)
                print("=" * 60)

                if message != last_message:
                    send_to_axis(
                        args.ip,
                        args.user,
                        args.password,
                        message,
                    )
                    last_message = message

                    if flight != last_announced_flight:
                        announce_flight(
                            args.ip.split(":")[0],
                            args.audio_port,
                            args.user,
                            args.password,
                            flight,
                            route,
                            distance,
                            altitude,
                        )
                        last_announced_flight = flight
                else:
                    print("No change, skipping update")
            else:
                print("No aircraft found")

        except Exception as ex:
            print("ERROR:", ex)

        print("Sleeping 5 minutes...")
        time.sleep(300)


if __name__ == "__main__":
    main()
