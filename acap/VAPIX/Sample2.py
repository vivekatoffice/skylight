import argparse
import requests
import urllib3
from requests.auth import HTTPBasicAuth

# Ignore self-signed certificate warnings
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

def send_notification(ip, username, password, message):
    url = (
        f"https://{ip}"
        "/config/rest/speaker-display-notification/v1/simple"
    )

    payload = {
        "data": {
            "message": message,
            "textColor": "#FFFFFF",
            "backgroundColor": "#000000",
            "textSize": "large",
            "scrollDirection": "fromRightToLeft",
            "scrollSpeed": 5,
            "duration": {
                "type": "time",
                "value": 10000
            }
        }
    }

    response = requests.post(
        url,
        auth=HTTPBasicAuth(username, password),
        json=payload,
        verify=False,
        timeout=10,
    )

    print(f"Status: {response.status_code}")
    print(response.text)

    response.raise_for_status()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Send notification to Axis Speaker Display"
    )

    parser.add_argument("--ip", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--message", required=True)

    args = parser.parse_args()

    send_notification(
        args.ip,
        args.user,
        args.password,
        args.message,
    )