#!/usr/bin/python3
import sys
import os
import o11
import base64
import json
import datetime
import pytz
import uuid

from curl_cffi import requests
import re

from traceback import format_exc
user=o11.parse_params(sys.argv, 'user')
password=o11.parse_params(sys.argv, 'password')
device=o11.parse_params(sys.argv, 'device')
pin=o11.parse_params(sys.argv, 'pin')

id=o11.parse_params(sys.argv, 'id')
action=o11.parse_params(sys.argv, 'action')

bind=o11.parse_params(sys.argv, 'bind')
proxy=o11.parse_params(sys.argv, 'proxy')
doh=o11.parse_params(sys.argv, 'doh')
worker=o11.parse_params(sys.argv, 'worker')

cdm=o11.parse_params(sys.argv, 'cdm')
drm=o11.parse_params(sys.argv, 'drm')
kid=o11.parse_params(sys.argv, 'kid')
pssh=o11.parse_params(sys.argv, 'pssh')
challenge=o11.parse_params(sys.argv, 'challenge')

heartbeaturl=o11.parse_params(sys.argv, 'heartbeaturl')
heartbeatparams=o11.parse_params(sys.argv, 'heartbeatparams')

o11Session = o11.session(bind=bind, proxy=proxy, worker=worker)
req = o11Session.get_session()
if doh != "":
    o11.dns(doh)

if challenge == "cert":
    challenge = "CAQ="

authFile = '/disney_' + user + '.json'

headers = {}


proxies = {
    "http": proxy,
    "https": proxy
}


impersonation = 'chrome99_android'

def pair():
    print("pairing Failed", flush=True)

    # response = req.get()
    # try:
    #     code = response.json()['code']
    #     print("Please go to ... and enter code " + code, flush=True)
    # except:
    #     print(response.text)
    #     sys.exit()

    # while True:
    #     response = req.get()
    #     try:
    #         response.json()['token']
    #         json.dump(response.json(), open(os.path.abspath(os.path.dirname(__file__)) + authFile, 'w'))
    #         print("pairing done", flush=True)
    #         break
    #     except:
    #         pass

    #     time.sleep(1)

def login():
    print("logging in...", file=sys.stderr)

    # Get token
    # response = req.get()
    # try:
    #     response.json()['token']
    #     json.dump(response.json(), open(os.path.abspath(os.path.dirname(__file__)) + authFile, 'w'))
    # except:
    #     print(response.text)
    #     sys.exit()

    print("logged in Failed", file=sys.stderr)



def get_fresh_token(auth=json.load(open(os.path.abspath(os.path.dirname(__file__)) + authFile)), force=False):

    url = "https://disney.api.edge.bamgrid.com/v1/public/graphql"
    headers = {
        "Connection": "Keep-Alive",
        "Content-Type": "application/json",
        "User-Agent": "BAMSDK/v22.0.0 (disney-svod-3d9324fc 26.10.0+rc3-2026.06.13.0; v7.0/v22.0.0; android; tv) google sdk_google_atv64_arm64 (TTT5.221206.003 dev-keys; Linux; 13; API 33)",
        "accept": "application/json",
        # "authorization": f"Bearer "+ auth['access_token'] if 'accessToken' not in auth else auth['accessToken'],
       "authorization": f"Bearer " + auth['extensions']['sdk']['token']['accessToken'],
        "x-application-version": "26.10.0+rc3-2026.06.13.0",
        "x-bamsdk-client-id": "disney-svod-3d9324fc",
        "x-bamsdk-platform": "android/google/tv",
        "x-bamsdk-platform-id": "android-tv",
        "x-bamsdk-transaction-id": str(uuid.uuid4()), #"d58942b0-7964-4835-97d6-d6fce7eef403",
        "x-bamsdk-version": "22.0.0",
        "x-dss-edge-accept": "vnd.dss.edge+json; version=2",
        "x-request-id": str(uuid.uuid4()),#"675ae0a4-77ae-4912-9938-9f286b093310",
        "x-request-yp-id": "624b805dafc5c73635b1a216"
    }
    payload ={
    "query": "\n            mutation exchangeAccountDelegationRefreshToken($exchangeAccountDelegationRefreshToken: ExchangeAccountDelegationRefreshTokenInput!) {\n              exchangeAccountDelegationRefreshToken(exchangeAccountDelegationRefreshToken: $exchangeAccountDelegationRefreshToken) {\n                activeSession {\n                  sessionId\n                }\n              }\n            }\n        ",
    "variables": {
        "exchangeAccountDelegationRefreshToken": {
            "accountDelegationRefreshToken": auth['account_delegation_refresh_token'],
            "deviceRefreshToken": auth['refresh_token'] if 'refreshToken' not in auth else auth['refreshToken']
        }
    },
    "operationName": "exchangeAccountDelegationRefreshToken"
}
    # print(headers)
    try:
        response = requests.post(
                        url, 
                        headers=headers, 
                        json=payload, 
                        # impersonate=impersonation,
                        proxies=proxies,
                        # 
                    )
        if response.ok and not force:
            print('Token Obtained', file=sys.stderr)
            # auth.update(response.json())
            json.dump(response.json(), open(os.path.abspath(os.path.dirname(__file__)) + authFile, 'w'))
            return response.json()['extensions']['sdk']['token']['accessToken']
    except:
        print(format_exc(), file=sys.stderr)
        # return auth['accessToken']

    

    url = "https://disney.api.edge.bamgrid.com/graph/v1/device/graphql"
    headers = {
        "Connection": "Keep-Alive",
        "Content-Type": "application/json",
        "User-Agent": "BAMSDK/v22.0.0 (disney-svod-3d9324fc 26.10.0+rc3-2026.06.13.0; v7.0/v22.0.0; android; tv) google sdk_google_atv64_arm64 (TTT5.221206.003 dev-keys; Linux; 13; API 33)",
        "accept": "application/json",
        "authorization": "Bearer ZGlzbmV5JmFuZHJvaWQmMS4wLjA.bkeb0m230uUhv8qrAXuNu39tbE_mD5EEhM_NAcohjyA",
        "x-application-version": "26.10.0+rc3-2026.06.13.0",
        "x-bamsdk-client-id": "disney-svod-3d9324fc",
        "x-bamsdk-platform": "android/google/tv",
        "x-bamsdk-platform-id": "android-tv",
        "x-bamsdk-transaction-id": str(uuid.uuid4()),#"4b9ea91f-fa7f-4b7c-9c81-b7c288ee2907",
        "x-bamsdk-version": "22.0.0",
        "x-dss-edge-accept": "vnd.dss.edge+json; version=2",
        "x-request-id": str(uuid.uuid4()), #"1de513e3-7841-4045-b419-4cb2f33706f3",
        "x-request-yp-id": "624b805dafc5c73635b1a216"
    }

    payload = {
        "query": "\n     mutation refreshToken($refreshToken: RefreshTokenInput!) {\n       refreshToken(refreshToken: $refreshToken) {\n         activeSession {\n           sessionId\n         }\n       }\n     }\n     ",
        "variables": {
            "refreshToken": {
            # "refreshToken": auth.get('refresh_token') if not 'refreshToken' in auth else auth.get('refreshToken')
                "refreshToken": auth['extensions']['sdk']['token']['refreshToken'] 
            }
        },
        "operationName": "refreshToken"
        }

    response = requests.post(
                url, 
                headers=headers, 
                json=payload, 
                # impersonate=impersonation,
                proxies=proxies,
                
            )
    if response.ok:
        if response.json()['extensions']['sdk']['token']:
            print('new refresh token obtained', file=sys.stderr)
            # auth['accessToken'] = response.json()['extensions']['sdk']['token']['accessToken']
            # auth['refreshToken'] = response.json()['extensions']['sdk']['token']['refreshToken']
            json.dump(response.json(), open(os.path.abspath(os.path.dirname(__file__)) + authFile, 'w'))



def get_manifest(id, auth, token):
    url = 'https://disney.playback.edge.bamgrid.com/v7/playback/ctr-high'
    payload = {
        "playbackId": id,
        "playback": {
            "attributes": {
                "resolution": {
                    "max": [
                        "1280x720"
                    ]
                },
                "protocol": "HTTPS",
                "assetInsertionStrategies": {
                    "point": "SGAI",
                    "range": "SGAI"
                },
                "playbackInitiationContext": "ONLINE",
                "slugDuration": "SLUG_500_MS",
                "maxSlideDuration": "4_HOUR"
            },
            "tracking": {
                # "playbackSessionId": auth['extensions']['sdk']['session']['sessionId']
                'playbackSessionId': auth['data']['exchangeAccountDelegationRefreshToken']['activeSession']['sessionId']
            },
            "adTracking": {
                "limitAdTrackingEnabled": "NO",
                "deviceAdId": "144f496b-579d-438c-bcec-c3300e14369b",
                "privacyOptOut": "NO"
            }
        },
        "allowedCreatives": [],
        "allowedInsertionVisuals": [
            "PROMO_FULL_TEXT",
            "PROMO_TEXT",
            "TITLE_TREATMENT",
            "ON_SCREEN_RATING",
            "ON_SCREEN_ADVISORY"
        ]
    }
    headers = {
        "accept": "application/vnd.media-service+json",
        "accept-encoding": "gzip",
        "authorization": f"Bearer {token}",
        "connection": "Keep-Alive",
        "content-length": "928",
        "content-type": "application/json",
        "host": "disney.playback.edge.bamgrid.com",
        "user-agent": "BAMSDK/v22.0.0 (disney-svod-3d9324fc 26.10.0+rc3-2026.06.13.0; v7.0/v22.0.0; android; tv) google sdk_google_atv64_arm64 (TTT5.221206.003 dev-keys; Linux; 13; API 33)",
        "x-application-version": "26.10.0+rc3-2026.06.13.0",
        "x-bamsdk-client-id": "disney-svod-3d9324fc",
        "x-bamsdk-platform": "android/google/tv",
        "x-bamsdk-transaction-id": str(uuid.uuid4()),
        "x-bamsdk-version": "22.0.0",
        "x-dss-edge-accept": "vnd.dss.edge+json; version=2",
        "x-dss-feature-filtering": "true",
        "x-request-id": str(uuid.uuid4()),
        "x-request-yp-id": "624b805dafc5c73635b1a216"
    }

    res = requests.post(
            url, 
            headers=headers, 
            json=payload, 
            impersonate="chrome",
            proxies=proxies,
            
        )
    print(res.json(), file=sys.stderr)
    return res


def do_action():

    if action == "pair":
        pair()
        sys.exit()
    elif action == "login":
        # login()
        try:
            auth = json.load(open(os.path.abspath(os.path.dirname(__file__)) + authFile))
        except:
            print(format_exc(), file=sys.stderr)
            return "error"
        token = get_fresh_token(auth)
        print(token, file=sys.stderr)
        sys.exit()

    try:
        auth = json.load(open(os.path.abspath(os.path.dirname(__file__)) + authFile))
        token = auth['extensions']['sdk']['token']['accessToken']
        # token = auth['access_token']
    except:
        print(format_exc(), file=sys.stderr)
        return "error"
    
    # token = get_fresh_token(auth)
    # token =  auth['access_token'] if 'access_token' in auth else auth['accessToken']
    
    print(token, file=sys.stderr)
    # print(requests.get('https://ipinfo.io/json',proxies=proxies, ).text)
    if action == "channels":
        output = {}
        output['Channels'] = []

        response = req.get()
        try:
            for chan in response.json():
                channel  = {}
                channel['Name'] = chan['Name']
                channel['Mode'] = "live"
                channel['SessionManifest'] = True
                channel['ManifestScript'] = 'id=' + str(chan['Id'])
                channel['CdmType'] = "widevine"
                channel['UseCdm'] = True
                channel['Cdm'] = 'id=' + str(chan['Id'])
                channel['Video'] = 'best'
                channel['OnDemand'] = True
                channel['SpeedUp'] = True
                output['Channels'].append(channel)

            print(json.dumps(output, indent=2))
        except:
            print(format_exc(), file=sys.stderr)
            return "error"

    elif action == "vod":
        output = {}
        output['Vod'] = []

        response = req.get()
        try:
            for item in response.json():
                vod  = {}
                vod['Name'] = item['Name']
                vod['Mode'] = "vod"
                vod['SessionManifest'] = True
                vod['ManifestScript'] = 'id=' + str(item['Id'])
                vod['CdmType'] = "widevine"
                vod['UseCdm'] = True
                
                vod['Cdm'] = 'id=' + str(item['Id'])
                vod['Video'] = 'best'
                output['Vod'].append(vod)

            print(json.dumps(output, indent=2))
        except:
            print(format_exc(), file=sys.stderr)
            return "error"

    elif action == "events":
        output = {}
        output['Events'] = []
        url = "https://disney.api.edge.bamgrid.com/explore/v1.18/page/page-367196c8-5316-4fee-aa7a-89a766840d7b"
        headers = {
            'accept':'application/json',
            # "Connection": "Keep-Alive",
            "User-Agent": "BAMSDK/v22.0.0 (disney-svod-3d9324fc 26.10.0+rc3-2026.06.13.0; v7.0/v22.0.0; android; tv) google sdk_google_atv64_arm64 (TTT5.221206.003 dev-keys; Linux; 13; API 33)",
            # "accept": "application/json",
            "authorization": f"Bearer {token}",
            "x-application-version": "26.10.0+rc3-2026.06.13.0",
            "x-bamsdk-client-id": "disney-svod-3d9324fc",
            "x-bamsdk-platform": "android/google/tv",
            "x-bamsdk-transaction-id": 'ab8daea4-135c-4537-9bd7-02c2dc4b7b93', #str(uuid.uuid4()),
            "x-bamsdk-version": "22.0.0",
            "x-dss-edge-accept": "vnd.dss.edge+json; version=2",
            "x-request-id": 'f1138038-beb6-45b0-8bc0-fff3a75c04d6',#str(uuid.uuid4()),
            "x-request-yp-id": "624b805dafc5c73635b1a216"
        }
    
        response = requests.get(url, headers=headers, proxies=proxies, )
        # response = req.get(url, headers=headers,)
        print(response.json(), file=sys.stderr)
        try:
            for containers in response.json()['data']['page']['containers']:
                for items in containers['items']:
                    for actions in items['actions']:
                        if 'modalType' in actions:
                            if actions['modalType'] == 'live':
                                event  = {}
                                if 'visuals' in actions:
                                    if 'airingEventState' in actions['visuals']:
                                        if 'timeline' in actions['visuals']['airingEventState']:
                                            timeline = actions['visuals']['airingEventState']['timeline']
                                    
                                            date_object = datetime.datetime.strptime(timeline['startTime'], "%Y-%m-%dT%H:%M:%SZ")
                                            date_utc = date_object.replace(tzinfo=pytz.UTC)
                                            event['Start'] = int(date_utc.timestamp())
                                            date_object = datetime.datetime.strptime(timeline['endTime'], "%Y-%m-%dT%H:%M:%SZ")
                                            date_utc = date_object.replace(tzinfo=pytz.UTC)
                                            event['End'] = int(date_utc.timestamp())


                                for ev in actions['actions']:
                                    print(ev, file=sys.stderr)
                                    if 'resourceId' in ev:
                                        if 'visuals' in actions:
                                            if 'title' in actions['visuals']:
                                                event['Name'] = actions['visuals']['title']
                                        else:
                                            if 'internalTitle' in ev:
                                                event['Name'] = ev['internalTitle']
                                            else:
                                                event['Name'] = ev['visuals']['title']
                                        event['Mode'] = "live"
                                        event['SessionManifest'] = True
                                        event['ManifestScript'] = 'id=' + str(ev['resourceId'])
                                        event['CdmType'] = "widevine"
                                        event['CdmMode'] = "external"
                                        event['OnDemand'] = True
                                        event['UseCdm'] = True
                                        event['Cdm'] = 'id=' + str(ev['resourceId'])
                                        event['Video'] = 'best'
                                        event['Autostart'] = False
                                       

                                        output['Events'].append(event)

            print(json.dumps(output, indent=2))
        except:
            print(format_exc(), file=sys.stderr)
            return "error"

    elif action == "heartbeat":
        sys.exit()

    elif action == "manifest":
        try:
            
          
            res = get_manifest(id, auth, token)
            print(res.json(), file=sys.stderr)
            output = {
                    "Cdn" : [],
                    "ManifestUrl" : "",
                    "Headers": {
                        "Manifest": {
                            'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
                            },
                        "Media": {
                            'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
                            }
                        },
                    "Heartbeat": {
                        "Url" : '',
                        "Params": '',
                        "PeriodMs" : 5*60*1000
                        }
                    }
            i = 'I'
            for cdn in res.json()['stream']['sources']:
            
                output['Cdn'].append({ "Name": i, "ManifestUrl": cdn['slide']['url'] })
                i+='I'
            del i
            print(json.dumps(output))
        except:
            print(format_exc(), file=sys.stderr)
            return "error"

    elif action == "cdm" and cdm == "internal":
        # return license from challenge
        response = req.post(response.json()['licUrl'], headers=headers, data=base64.b64decode(challenge))
        response_b64 = str(base64.b64encode(response.content), 'ascii')
        if response_b64.startswith('CA'):
            print(response_b64)
        else:
            print(format_exc(), file=sys.stderr)
            return "error"

    elif action == "cdm" and cdm == "external":

        from pywidevine.cdm import Cdm
        from pywidevine.device import Device
        from pywidevine.pssh import PSSH

        device = Device.load(os.path.abspath(os.path.dirname(__file__)) + "/L3.wvd")
        pssh2 = PSSH(pssh)
        cdm2 = Cdm.from_device(device)

        session_id = cdm2.open()
        challenge = cdm2.get_license_challenge(session_id, pssh2)
        
        headers = {
                "authorization": f"Bearer {token}",
                "user-agent": "BAMSDK/v22.0.0 (disney-svod-3d9324fc 26.10.0+rc3-2026.06.13.0; v7.0/v22.0.0; android; tv) google sdk_google_atv64_arm64 (TTT5.221206.003 dev-keys; Linux; 13; API 33)",
                "x-application-version": "26.10.0+rc3-2026.06.13.0",
                "x-bamsdk-client-id": "disney-svod-3d9324fc",
                "x-bamsdk-platform": "android/google/tv",
                "x-bamsdk-transaction-id": str(uuid.uuid4()),
                "x-bamsdk-version": "22.0.0",
                "x-dss-edge-accept": "vnd.dss.edge+json; version=2",
                "x-dss-feature-filtering": "true",
                "x-request-id": str(uuid.uuid4()),
                "x-request-yp-id": "624b805dafc5c73635b1a216"
            }
        print(headers, file=sys.stderr)
        licence = requests.post("https://disney.playback.edge.bamgrid.com/widevine/v1/channel/obtain-license", data=challenge, headers=headers, proxies=proxies,)
        # print(licence.text, file=sys.stderr)
        # licence.raise_for_status()
        if not licence.ok:
            cdm2.close(session_id)
            res = get_manifest(id, auth, token)
            print(res.json(), file=sys.stderr)
            m3u8 = res.json()['stream']['sources'][0]['slide']['url']
            res = req.get(m3u8)
            m = re.search(r'base64,([^"]+)', res.text)
            pssh3 = m.group(1)
            device = Device.load(os.path.abspath(os.path.dirname(__file__)) + "/L3.wvd")
            pssh2 = PSSH(pssh3)
            cdm2 = Cdm.from_device(device)
            session_id = cdm2.open()
            challenge = cdm2.get_license_challenge(session_id, pssh2)
            licence = requests.post("https://disney.playback.edge.bamgrid.com/widevine/v1/channel/obtain-license", 
                                    data=challenge, headers=headers,
                                    impersonate=impersonation,
                                    proxies=proxies,
                                    
                                    )
            print(licence.text, file=sys.stderr)


        cdm2.parse_license(session_id, licence.content)

        for key in cdm2.get_keys(session_id):
            print(f"{key.kid.hex}:{key.key.hex()}")

        cdm2.close(session_id)
    else:
        print("invalid action: " + action)

if do_action() == "error":
    get_fresh_token()
    do_action()
