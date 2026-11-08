# SPDX-License-Identifier: Apache-2.0
# The standard ssl module (CPython / MicroPython API) for Tab5 OS sockets.
#
# TLS itself is t5_net's (mbedTLS through esp-tls). Unlike MicroPython's
# own ssl, servers are checked against the built-in CA bundle by default:
# pass cert_reqs=CERT_NONE, or set context.verify_mode = CERT_NONE, to talk
# to a server with a self-signed certificate (or give its CA as cadata).
# Certificates and keys may be PEM text, DER bytes or file paths.
import binascii

PROTOCOL_TLS_CLIENT = 0
PROTOCOL_TLS_SERVER = 1
CERT_NONE = 0
CERT_OPTIONAL = 1
CERT_REQUIRED = 2
MBEDTLS_VERSION = "mbedTLS (ESP-IDF)"
SSLError = OSError


def _pem(data, kind):
    """PEM text from PEM text, DER bytes or a file path; None stays None."""
    if data is None:
        return None
    if isinstance(data, str):
        if "-----BEGIN" in data:
            return data
        with open(data, "rb") as f:            # a path
            data = f.read()
    data = bytes(data)
    if b"-----BEGIN" in data:
        return data.decode()
    b64 = binascii.b2a_base64(data).decode().replace("\n", "")
    body = "\n".join(b64[i : i + 64] for i in range(0, len(b64), 64))
    return "-----BEGIN %s-----\n%s\n-----END %s-----\n" % (kind, body, kind)


def _pem_key(data):
    if data is None:
        return None
    k = _pem(data, "PRIVATE KEY")
    return k


class SSLContext:
    def __init__(self, protocol=PROTOCOL_TLS_CLIENT):
        self.protocol = protocol
        client = protocol == PROTOCOL_TLS_CLIENT
        self.verify_mode = CERT_REQUIRED if client else CERT_NONE
        self.check_hostname = client
        self._ca = self._cert = self._key = self._alpn = None

    def load_verify_locations(self, cafile=None, capath=None, cadata=None):
        self._ca = _pem(cafile if cafile is not None else cadata, "CERTIFICATE")

    def load_cert_chain(self, certfile, keyfile=None, password=None):
        self._cert = _pem(certfile, "CERTIFICATE")
        self._key = _pem_key(keyfile if keyfile is not None else certfile)

    def load_default_certs(self, purpose=None):
        pass                                     # the bundle is always there

    set_default_verify_paths = load_default_certs

    def set_alpn_protocols(self, protocols):
        self._alpn = list(protocols)

    def set_ciphers(self, ciphers):
        pass

    def wrap_socket(self, sock, server_side=False, do_handshake_on_connect=True, server_hostname=None):
        server = server_side or self.protocol == PROTOCOL_TLS_SERVER
        verify = not server and self.verify_mode != CERT_NONE
        check_name = verify and self.check_hostname and server_hostname is not None
        return sock._tls(server_hostname=server_hostname, verify=verify, check_name=check_name or not verify,
                         ca=self._ca, cert=self._cert, key=self._key, server_side=server, alpn=self._alpn)


def create_default_context(purpose=None, cafile=None, capath=None, cadata=None):
    ctx = SSLContext(PROTOCOL_TLS_CLIENT)
    if cafile is not None or cadata is not None:
        ctx.load_verify_locations(cafile=cafile, cadata=cadata)
    return ctx


def wrap_socket(sock, server_side=False, key=None, cert=None, cert_reqs=CERT_REQUIRED, cadata=None,
                server_hostname=None, do_handshake=True, keyfile=None, certfile=None, ca_certs=None):
    """MicroPython's legacy call. Without server_hostname the chain is
    still checked against the bundle, but not the name."""
    ctx = SSLContext(PROTOCOL_TLS_SERVER if server_side else PROTOCOL_TLS_CLIENT)
    ctx.verify_mode = cert_reqs
    ca = cadata if cadata is not None else ca_certs
    if ca is not None:
        ctx.load_verify_locations(cadata=ca)
    c, k = (cert if cert is not None else certfile), (key if key is not None else keyfile)
    if c is not None:
        ctx.load_cert_chain(c, k)
    return ctx.wrap_socket(sock, server_side=server_side, server_hostname=server_hostname)
