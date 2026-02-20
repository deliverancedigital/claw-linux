"""
agent/main.py — Entry point for the claw-linux autonomous agent.

Usage:
    python3 main.py              # interactive REPL
    python3 main.py --api        # start REST API server (requires api.enabled in config)
    python3 main.py "do a thing" # run a single turn and exit
"""
from __future__ import annotations

import argparse
import sys

from core.config import Config
from core.agent import Agent


def main() -> None:
    parser = argparse.ArgumentParser(
        description="claw-linux autonomous agent (OpenClaw-compatible)"
    )
    parser.add_argument(
        "prompt",
        nargs="?",
        help="Run a single prompt and exit instead of starting the REPL",
    )
    parser.add_argument(
        "--api",
        action="store_true",
        help="Start the REST API server instead of the interactive REPL",
    )
    args = parser.parse_args()

    cfg   = Config()
    agent = Agent(cfg)

    if args.api:
        _run_api(cfg, agent)
    elif args.prompt:
        response = agent.run_once(args.prompt)
        print(response)
    else:
        agent.run_interactive()


def _run_api(cfg: Config, agent: Agent) -> None:
    """Minimal HTTP API server for remote control via JSON POST requests."""
    import json
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):  # noqa: N802
            pass  # suppress default access log; agent logger handles it

        def do_POST(self):  # noqa: N802
            length = int(self.headers.get("Content-Length", 0))
            body   = self.rfile.read(length)
            try:
                data   = json.loads(body)
                prompt = data.get("prompt", "")
            except (json.JSONDecodeError, KeyError):
                self._respond(400, {"ok": False, "error": "Invalid JSON body"})
                return

            response = agent.run_once(prompt)
            self._respond(200, {"ok": True, "response": response})

        def _respond(self, code: int, payload: dict) -> None:
            body = json.dumps(payload).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    host = cfg.get("api", "host", default="127.0.0.1")
    port = int(cfg.get("api", "port", default=8080))
    server = HTTPServer((host, port), Handler)
    print(f"API server listening on http://{host}:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nAPI server stopped.")


if __name__ == "__main__":
    main()
