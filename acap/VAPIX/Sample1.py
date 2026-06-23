import requests
import math

# Replace with your coordinates
MY_LAT = 12.97
MY_LON = 77.75

# Search radius in nautical miles
RADIUS_NM = 25

url = (
    f"https://api.adsb.lol/v2/lat/{MY_LAT}/lon/{MY_LON}/dist/{RADIUS_NM}"
)

response = requests.get(url, timeout=10)
response.raise_for_status()

data = response.json()

aircraft = data.get("ac", [])

def haversine(lat1, lon1, lat2, lon2):
    R = 6371.0

    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)

    a = (
        math.sin(dlat / 2) ** 2
        + math.cos(math.radians(lat1))
        * math.cos(math.radians(lat2))
        * math.sin(dlon / 2) ** 2
    )

    return R * (2 * math.atan2(math.sqrt(a), math.sqrt(1 - a)))

print(f"Found {len(aircraft)} aircraft\n")

for ac in aircraft:
    lat = ac.get("lat")
    lon = ac.get("lon")

    if lat is None or lon is None:
        continue

    distance_km = haversine(MY_LAT, MY_LON, lat, lon)

    print(
        f"{ac.get('flight', 'UNKNOWN').strip():<10} "
        f"{ac.get('t', 'N/A'):<8} "
        f"{distance_km:6.1f} km "
        f"Alt: {ac.get('alt_baro', 'N/A'):>6} ft "
        f"Heading: {ac.get('track', 'N/A')}"
    )