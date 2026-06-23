import argparse
import math
import time
import requests
import urllib3

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
                flight = aircraft.get(
                    "flight",
                    "UNKNOWN"
                ).strip()

                aircraft_type = aircraft.get(
                    "t",
                    "N/A"
                )

                altitude = aircraft.get(
                    "alt_baro",
                    "N/A"
                )

                message = (
                    f"✈ {flight} | "
                    f"{aircraft_type} | "
                    f"{distance:.1f}km | "
                    f"{altitude}ft"
                )

                print(message)

                if message != last_message:
                    send_to_axis(
                        args.ip,
                        args.user,
                        args.password,
                        message,
                    )
                    last_message = message

            else:
                print("No aircraft found")

        except Exception as e:
            print("ERROR:", e)

        print("Sleeping 5 minutes...")
        time.sleep(300)


if __name__ == "__main__":
    main()