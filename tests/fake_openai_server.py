#!/usr/bin/env python3
"""仅供本地 AI 插件测试使用的最小 OpenAI 兼容 HTTP 服务（SSE 流式输出）。"""

import time

from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        chunks = [
            b'data: {"choices":[{"delta":{"content":" return 0;"}}]}\n\n',
            b'data: {"choices":[{"delta":{"content":"\\n}"}}]}\n\n',
            b"data: [DONE]\n\n",
        ]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for chunk in chunks:
            self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
            self.wfile.flush()
            time.sleep(0.05)

    def log_message(self, format, *args):
        return


HTTPServer(("127.0.0.1", 18080), Handler).serve_forever()
