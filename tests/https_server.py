#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Serve a directory over https, for the tests that make Pkg on AROS read a
channel with TLS.

    https_server.py <cert.pem> <directory> <port> [bind]

<cert.pem> holds the certificate and its key, as tests/tls-certs.sh writes
them. The port is printed once the socket listens, so a caller can wait for
that line before it starts."""

import functools
import http.server
import ssl
import sys

cert, directory, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
bind = sys.argv[4] if len(sys.argv) > 4 else "127.0.0.1"

handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
httpd = http.server.HTTPServer((bind, port), handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert)
httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
print("listening %d" % httpd.server_address[1], flush=True)
httpd.serve_forever()
