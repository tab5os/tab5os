# SPDX-License-Identifier: Apache-2.0
# requests (the CPython "requests" / MicroPython "urequests" API) on t5.net:
# HTTPS checked against the built-in CA bundle, redirects, chunked and
# streamed bodies, any method.
#
#   import requests
#   r = requests.get("https://api.example.com/x", params={"q": 1}, timeout=5)
#   r.raise_for_status(); data = r.json()
#   requests.post(url, json={"a": 1}, headers={"Authorization": "Bearer ..."})
import binascii
import t5


class RequestException(OSError):
    pass


class HTTPError(RequestException):
    pass


ConnectionError = RequestException
Timeout = RequestException


class _Exceptions:
    RequestException = RequestException
    HTTPError = HTTPError
    ConnectionError = ConnectionError
    Timeout = Timeout


exceptions = _Exceptions


def _quote(s):
    s = str(s)
    out = []
    for ch in s.encode():
        c = chr(ch)
        if c.isalpha() or c.isdigit() or c in "-._~":
            out.append(c)
        else:
            out.append("%%%02X" % ch)
    return "".join(out)


def urlencode(params):
    items = params.items() if isinstance(params, dict) else params
    return "&".join(_quote(k) + "=" + _quote(v) for k, v in items)


class Response:
    def __init__(self, r):
        self.raw = r
        self.status_code = r.status
        self.headers = r.headers
        self.url = r.url
        self.encoding = "utf-8"
        self.reason = ""
        self._content = None

    @property
    def content(self):
        if self._content is None:
            self._content = self.raw.content
        return self._content

    @property
    def text(self):
        return str(self.content, self.encoding)

    @property
    def ok(self):
        return 200 <= self.status_code < 400

    def json(self):
        import json

        return json.loads(self.content)

    def iter_content(self, chunk_size=1024):
        while True:
            b = self.raw.read(chunk_size)
            if not b:
                break
            yield b

    def raise_for_status(self):
        if self.status_code >= 400:
            raise HTTPError("%d for %s" % (self.status_code, self.url))

    def close(self):
        self.raw.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def request(method, url, data=None, json=None, headers=None, stream=False, auth=None, timeout=None, params=None,
            allow_redirects=True, verify=True, cert=None, parse_headers=True):
    h = dict(headers or {})
    if params:
        url += ("&" if "?" in url else "?") + urlencode(params)
    if auth is not None:
        cred = binascii.b2a_base64(("%s:%s" % auth).encode()).decode().strip()
        h["Authorization"] = "Basic " + cred
    if isinstance(data, (dict, list, tuple)):
        data = urlencode(data)
        h.setdefault("Content-Type", "application/x-www-form-urlencoded")
    if isinstance(timeout, (tuple, list)):
        timeout = max(t for t in timeout if t is not None) if any(t is not None for t in timeout) else None
    ms = int(timeout * 1000) if timeout else 10000
    ca = None
    if isinstance(verify, str):
        with open(verify) as f:
            ca = f.read()
    try:
        r = t5.net.request(method, url, data, json=json, headers=h, timeout=ms, redirects=allow_redirects,
                           verify=bool(verify), ca=ca, stream=bool(stream))
    except OSError as e:
        raise RequestException(*e.args)
    return Response(r)


def get(url, params=None, **kw):
    return request("GET", url, params=params, **kw)


def head(url, **kw):
    return request("HEAD", url, **kw)


def post(url, data=None, json=None, **kw):
    return request("POST", url, data=data, json=json, **kw)


def put(url, data=None, **kw):
    return request("PUT", url, data=data, **kw)


def patch(url, data=None, **kw):
    return request("PATCH", url, data=data, **kw)


def delete(url, **kw):
    return request("DELETE", url, **kw)
