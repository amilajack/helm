#!/usr/bin/env python3
"""Local development server with the isolation required by AudioWorklet Wasm."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--port',type=int,default=8080)
parser.add_argument('--directory',default=str(Path(__file__).resolve().parents[2]/'build/wasm/web'))
args=parser.parse_args()
class Handler(SimpleHTTPRequestHandler):
    def __init__(self,*a,**kw): super().__init__(*a,directory=args.directory,**kw)
    def end_headers(self):
        for key,value in {'Cross-Origin-Opener-Policy':'same-origin','Cross-Origin-Embedder-Policy':'require-corp','Cross-Origin-Resource-Policy':'same-origin','Cache-Control':'no-cache'}.items():
            self.send_header(key,value)
        super().end_headers()
class Server(ThreadingHTTPServer):
    request_queue_size=128
    daemon_threads=True
Server(('127.0.0.1',args.port),Handler).serve_forever()
