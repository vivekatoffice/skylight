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



urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


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
    url = (
        f"https://{ip}"
        "/config/rest/speaker-display-notification/v1/simple"
    )

    payload = {
        "data": {
            "message": message,
            "textSize": "large",
            "scrollDirection": "fromRightToLeft",
            "scrollSpeed": 5,
            "duration": {
                "type": "time",
                "value": 300000
            }
        }
    }

    response = requests.post(
        url,
        auth=(user, password),
        json=payload,
        verify=False,
        timeout=15,
    )

    print(f"[Axis] {response.status_code}")
    response.raise_for_status()


def get_route(callsign):
    try:
        callsign = callsign.strip()

        response = requests.get(
            f"https://api.adsbdb.com/v0/callsign/{callsign}",
            timeout=10,
        )

        if response.status_code != 200:
            return None

        data = response.json()

        route = (
            data.get("response", {})
            .get("flightroute")
        )

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
    url = (
        f"https://api.adsb.lol/v2/lat/{lat}/lon/{lon}/dist/{radius_nm}"
    )

    response = requests.get(url, timeout=15)
    response.raise_for_status()

    aircraft = response.json().get("ac", [])

    nearest = None
    nearest_distance = float("inf")

    for ac in aircraft:
        ac_lat = ac.get("lat")
        ac_lon = ac.get("lon")

        if ac_lat is None or ac_lon is None:
            continue

        distance = haversine(
            lat,
            lon,
            ac_lat,
            ac_lon,
        )

        if distance < nearest_distance:
            nearest = ac
            nearest_distance = distance

    return nearest, nearest_distance


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument("--ip", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)

    parser.add_argument(
        "--lat",
        type=float,
        default=12.97,
        help="Observer latitude"
    )

    parser.add_argument(
        "--lon",
        type=float,
        default=77.75,
        help="Observer longitude"
    )

    parser.add_argument(
        "--radius",
        type=int,
        default=25,
        help="Radius in nautical miles"
    )

    args = parser.parse_args()

    last_message = None

    while True:
        try:
            aircraft, distance = get_closest_aircraft(
                args.lat,
                args.lon,
                args.radius,
            )

            if aircraft:
                flight = (
                    aircraft.get("flight", "UNKNOWN")
                    .strip()
                )

                aircraft_type = aircraft.get(
                    "t",
                    "N/A"
                )

                altitude = aircraft.get(
                    "alt_baro",
                    "N/A"
                )

                route = get_route(flight)

                lines = [f"✈ {flight}"]

                if route:
                    origin, destination = route
                    lines.append(origin)
                    lines.append("→")
                    lines.append(destination)
                else:
                    lines.append(aircraft_type)

                lines.append(
                    f"{distance:.1f}km {altitude}ft"
                )

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