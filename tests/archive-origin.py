#!/usr/bin/env python3
"""Small local archive-origin fixture; test decisions belong to the Bash driver."""
import http.server
from pathlib import Path
import sys

payload, portfile, requests = map(Path, sys.argv[1:])

class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        with requests.open('a') as log:
            log.write(self.path + '\n')
        data = payload.read_bytes()
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_):
        pass

server = http.server.HTTPServer(('127.0.0.1', 0), Origin)
portfile.write_text(str(server.server_port))
server.serve_forever()
